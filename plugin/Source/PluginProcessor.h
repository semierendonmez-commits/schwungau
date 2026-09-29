#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "schwung/Engine.h"
#include "schwung/Catalog.h"
#include "ModuleInstaller.h"

class SchwungProcessor : public juce::AudioProcessor, private juce::AsyncUpdater {
public:
    static constexpr int kNumKnobs = 8;   // Move's eight encoders

    SchwungProcessor();
    ~SchwungProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- used by the editor ----------------------------------------------
    schwung::Engine& engine() { return engine_; }
    const schwung::Catalog& catalog() const { return catalog_; }
    void rescanModules();
    const schwung::DataLocation& data() const { return data_; }
    juce::String statusLine() const { const juce::ScopedLock l(statusLock_); return status_; }
    void setStatus(const juce::String& s) { const juce::ScopedLock l(statusLock_); status_ = s; }
    juce::AudioParameterFloat* knob(int i) { return knobs_[(size_t)i]; }

    // Load a module into a chain position ("synth", "fx2", "midi_fx1" ...).
    // Empty id unloads. Runs on the calling (message) thread.
    void loadModule(const juce::String& position, const juce::String& moduleId);
    // Called by the editor when a knob mapping or target value may have moved,
    // so the DAW-facing knob follows (without echoing back to the chain).
    void syncKnobsFromChain();

    std::atomic<int> chainRevision { 0 };   // bumped when the chain shape changes

private:
    static BusesProperties makeBuses();
    void handleAsyncUpdate() override;
    void applyKnob(int i, float normalised);   // audio thread, lock held
    void openEngine();

    schwung::DataLocation data_;
    schwung::Engine engine_;
    schwung::Catalog catalog_;
    bool engineOk_ = false;

    std::array<juce::AudioParameterFloat*, kNumKnobs> knobs_ {};
    std::array<float, kNumKnobs> lastKnob_ {};
    std::array<std::atomic<bool>, kNumKnobs> suppressKnob_ {};

    std::vector<schwung::MidiEvent> midiIn_, midiOut_;
    juce::String pendingState_;            // state arriving before the engine is ready

    juce::CriticalSection statusLock_;
    juce::String status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SchwungProcessor)
};
