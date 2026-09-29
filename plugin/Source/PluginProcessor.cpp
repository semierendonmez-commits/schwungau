#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstdio>
#include <cstring>

namespace {

} // namespace

juce::AudioProcessor::BusesProperties SchwungProcessor::makeBuses() {
    auto b = BusesProperties()
                 .withOutput("Output", juce::AudioChannelSet::stereo(), true);
   #if SCHWUNG_IS_FX
    b = b.withInput("Input", juce::AudioChannelSet::stereo(), true);
   #else
    // Instruments can still take a sidechain-style input for linein & co.
    b = b.withInput("Input", juce::AudioChannelSet::stereo(), false);
   #endif
    return b;
}

SchwungProcessor::SchwungProcessor() : AudioProcessor(makeBuses()) {
    for (int i = 0; i < kNumKnobs; ++i) {
        auto* k = new juce::AudioParameterFloat(
            juce::ParameterID { "knob" + juce::String(i + 1), 1 },
            "Knob " + juce::String(i + 1),
            juce::NormalisableRange<float>(0.f, 1.f), 0.5f);
        addParameter(k);
        knobs_[(size_t)i] = k;
        lastKnob_[(size_t)i] = -1.f;
        suppressKnob_[(size_t)i] = false;
    }
    midiIn_.reserve(2048);
    midiOut_.reserve(2048);
    openEngine();
}

SchwungProcessor::~SchwungProcessor() {
    cancelPendingUpdate();
    engine_.close();
}

void SchwungProcessor::openEngine() {
    data_ = schwung::locateData();
    setStatus(schwung::installBundledModules(data_));
    schwung::Engine::Config cfg;
    cfg.dataRoot = data_.dataRoot.getFullPathName().toStdString();
    cfg.schwungDir = data_.schwungDir.getFullPathName().toStdString();
    std::string err;
    engineOk_ = engine_.open(cfg, err);
    if (!engineOk_) setStatus(juce::String("Engine: ") + err);
    engine_.setPreRenderHook([](void* ctx, schwung::Engine& e) {
        auto* self = static_cast<SchwungProcessor*>(ctx);
        for (int i = 0; i < kNumKnobs; ++i) {
            const float v = self->knobs_[(size_t)i]->get();
            if (std::abs(v - self->lastKnob_[(size_t)i]) < 1e-6f) continue;
            const bool first = self->lastKnob_[(size_t)i] < 0.f;
            self->lastKnob_[(size_t)i] = v;
            // A change that came FROM the chain (syncKnobsFromChain) or the
            // initial value must not be written back into it.
            if (first || self->suppressKnob_[(size_t)i].exchange(false)) continue;
            self->applyKnob(i, v);
        }
        (void)e;
    }, this);
    rescanModules();
   #if SCHWUNG_IS_FX
    // An insert effect must pass its input through. On Schwung the track's
    // audio enters a chain through the `linein` sound generator, so a new FX
    // instance starts with linein in the synth position (a restored state
    // replaces it).
    if (engineOk_) engine_.setParam("synth:module", "linein");
   #endif
    // Optional starting slot (a Schwung slot_N.json / patch document).
    if (auto* init = std::getenv("SCHWUNG_INITIAL_STATE"); engineOk_ && init && *init) {
        const juce::File f(juce::String::fromUTF8(init));
        if (f.existsAsFile()) { pendingState_ = f.loadFileAsString(); triggerAsyncUpdate(); }
    }
}

void SchwungProcessor::rescanModules() {
    catalog_.scan(data_.schwungDir.getFullPathName().toStdString(),
                  data_.schwungDir.getChildFile("module-catalog.json").getFullPathName().toStdString());
}

bool SchwungProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

void SchwungProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    engine_.prepare(sampleRate, juce::jmax(samplesPerBlock, 32));
    setLatencySamples(engine_.latencySamples());
    for (auto& v : lastKnob_) v = -1.f;
    if (pendingState_.isNotEmpty()) triggerAsyncUpdate();
}

// Audio thread. Map a DAW knob (0..1) onto whatever the chain's knob N is
// mapped to, with the target parameter's own range, allocation-free.
void SchwungProcessor::applyKnob(int i, float v) {
    char key[64], target[64], param[96], buf[64];
    std::snprintf(key, sizeof key, "knob_%d_target", i + 1);
    if (engine_.getParamRaw(key, target, sizeof target) <= 0) return;
    std::snprintf(key, sizeof key, "knob_%d_param", i + 1);
    if (engine_.getParamRaw(key, param, sizeof param) <= 0) return;
    float mn = 0.f, mx = 1.f;
    std::snprintf(key, sizeof key, "knob_%d_min", i + 1);
    if (engine_.getParamRaw(key, buf, sizeof buf) > 0) mn = (float)std::atof(buf);
    std::snprintf(key, sizeof key, "knob_%d_max", i + 1);
    if (engine_.getParamRaw(key, buf, sizeof buf) > 0) mx = (float)std::atof(buf);
    std::snprintf(key, sizeof key, "knob_%d_value", i + 1);
    const bool isInt = engine_.getParamRaw(key, buf, sizeof buf) > 0 && std::strchr(buf, '.') == nullptr;
    const float val = mn + v * (mx - mn);
    if (isInt) std::snprintf(buf, sizeof buf, "%d", (int)std::lround(val));
    else       std::snprintf(buf, sizeof buf, "%.4f", val);
    char full[192];
    std::snprintf(full, sizeof full, "%s:%s", target, param);
    engine_.setParamRaw(full, buf);
}

void SchwungProcessor::syncKnobsFromChain() {
    if (!engineOk_) return;
    std::array<float, kNumKnobs> norm {};
    std::array<bool, kNumKnobs> valid {};
    engine_.call([&](schwung::Engine& e) {
        for (int i = 0; i < kNumKnobs; ++i) {
            const std::string n = std::to_string(i + 1);
            const std::string t = e.getParamUnlocked("knob_" + n + "_target", 64);
            const std::string p = e.getParamUnlocked("knob_" + n + "_param", 96);
            if (t.empty() || p.empty()) continue;
            const float mn = (float)std::atof(e.getParamUnlocked("knob_" + n + "_min", 32).c_str());
            const float mx = (float)std::atof(e.getParamUnlocked("knob_" + n + "_max", 32).c_str());
            const std::string cur = e.getParamUnlocked(t + ":" + p, 64);
            if (cur.empty() || mx <= mn) continue;
            norm[(size_t)i] = juce::jlimit(0.f, 1.f, ((float)std::atof(cur.c_str()) - mn) / (mx - mn));
            valid[(size_t)i] = true;
        }
    });
    for (int i = 0; i < kNumKnobs; ++i) {
        if (!valid[(size_t)i]) continue;
        auto* k = knobs_[(size_t)i];
        if (std::abs(k->get() - norm[(size_t)i]) > 0.002f) {
            suppressKnob_[(size_t)i] = true;
            k->setValueNotifyingHost(norm[(size_t)i]);
        }
    }
}

void SchwungProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    midiIn_.clear();
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        const int len = m.getRawDataSize();
        if (len < 1 || len > 3) continue;                 // SysEx is not routed to chain slots
        if (midiIn_.size() == midiIn_.capacity()) break;
        schwung::MidiEvent e;
        e.offset = juce::jlimit(0, n - 1, meta.samplePosition);
        std::memcpy(e.data, m.getRawData(), (size_t)len);
        e.len = (uint8_t)len;
        midiIn_.push_back(e);
    }
    midi.clear();

    schwung::Transport t;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            t.valid = true;
            t.playing = pos->getIsPlaying();
            if (auto bpm = pos->getBpm()) t.bpm = *bpm;
            if (auto ppq = pos->getPpqPosition()) t.ppq = *ppq;
        }
    }

    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const float* ins[2] = { numIn > 0 ? buffer.getReadPointer(0) : nullptr,
                            numIn > 1 ? buffer.getReadPointer(1) : nullptr };
    // The engine reads all input before writing output, so in-place is fine.
    float* outs[2] = { buffer.getWritePointer(0), numOut > 1 ? buffer.getWritePointer(1) : buffer.getWritePointer(0) };

    midiOut_.clear();
    engine_.process(ins, numIn, outs, juce::jmin(numOut, 2), n,
                    midiIn_.data(), (int)midiIn_.size(), t, &midiOut_);

    for (const auto& e : midiOut_)
        midi.addEvent(e.data, e.len, juce::jlimit(0, n - 1, e.offset));
}

void SchwungProcessor::loadModule(const juce::String& position, const juce::String& moduleId) {
    engine_.setParam((position + ":module").toStdString(), moduleId.isEmpty() ? "none" : moduleId.toStdString());
    ++chainRevision;
    const auto err = juce::String(engine_.getParam("synth_error", 512));
    setStatus(err.isNotEmpty() ? err : (moduleId.isEmpty() ? position + " cleared" : "Loaded " + moduleId));
}

// ---- state ------------------------------------------------------------------
// The DAW chunk IS a Schwung slot document (the same JSON Move autosaves as
// slot_N.json), plus the eight knob values. A slot saved in a project can be
// copied to a Move and vice versa.
void SchwungProcessor::getStateInformation(juce::MemoryBlock& dest) {
    juce::String doc = engineOk_ ? juce::String(engine_.saveSlotState()) : pendingState_;
    juce::var v = juce::JSON::parse(doc);
    if (auto* obj = v.getDynamicObject()) {
        juce::Array<juce::var> ks;
        for (auto* k : knobs_) ks.add(k->get());
        obj->setProperty("schwung_au_knobs", ks);
        doc = juce::JSON::toString(v, false);
    }
    dest.replaceAll(doc.toRawUTF8(), doc.getNumBytesAsUTF8());
}

void SchwungProcessor::setStateInformation(const void* data, int size) {
    const juce::String doc = juce::String::fromUTF8(static_cast<const char*>(data), size);
    juce::var v = juce::JSON::parse(doc);
    if (auto* ks = v["schwung_au_knobs"].getArray())
        for (int i = 0; i < juce::jmin(kNumKnobs, ks->size()); ++i) {
            suppressKnob_[(size_t)i] = true;
            knobs_[(size_t)i]->setValueNotifyingHost((float)(double)(*ks)[i]);
        }
    pendingState_ = doc;
    if (juce::MessageManager::getInstance()->isThisTheMessageThread()) handleAsyncUpdate();
    else triggerAsyncUpdate();
}

void SchwungProcessor::handleAsyncUpdate() {
    if (!engineOk_ || pendingState_.isEmpty()) return;
    std::string err;
    if (!engine_.loadSlotState(pendingState_.toStdString(), err))
        setStatus("Could not restore slot: " + juce::String(err));
    else
        setStatus("Slot restored");
    pendingState_.clear();
    ++chainRevision;
}

juce::AudioProcessorEditor* SchwungProcessor::createEditor() { return new SchwungEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SchwungProcessor(); }
