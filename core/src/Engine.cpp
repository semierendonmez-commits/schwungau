// Schwung shell engine — see Engine.h for the contract.
#include "schwung/Engine.h"
#include "Resampler.h"

// plugin_api_v1.h is a C header and pins its ABI with C11 _Static_assert;
// in C++ the same check is static_assert.
#ifndef _Static_assert
#define _Static_assert static_assert
#define SW_UNDEF_STATIC_ASSERT
#endif
#include "host/plugin_api_v1.h"      // from the Schwung source tree
#ifdef SW_UNDEF_STATIC_ASSERT
#undef _Static_assert
#endif
#include "nlohmann/json.hpp"

#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

using json = nlohmann::json;

namespace schwung {

// ===========================================================================
// Process-global state.
//
// Every plugin instance dlopen()s the same chain dsp.so and the same module
// images, and dlopen of an already-loaded path returns the SAME image: module
// globals (their stored host_api pointer, lazily built tables, shared scratch
// buffers) are shared by every instance in the process. On Move that is safe
// because one SPI thread makes every call. We reproduce that exactly: one
// process-wide lock serialises every call into Schwung code, and one shared
// host_api answers on behalf of whichever engine currently holds the lock.
// ===========================================================================
namespace {

std::mutex& globalLock() { static std::mutex m; return m; }
Engine* g_current = nullptr;              // valid only while globalLock() is held

// Move's SPI mailbox. Modules read audio input at audio_in_offset.
constexpr size_t kMailboxSize = 4096;
alignas(64) uint8_t g_mailbox[kMailboxSize];

// Log ring (RT-safe: fixed slots, no allocation).
constexpr int kLogSlots = 256, kLogLen = 240;
char g_log[kLogSlots][kLogLen];
std::atomic<uint32_t> g_logWrite{0};
uint32_t g_logRead = 0;
std::mutex g_logReadMutex;

struct ChainLib {
    void* handle = nullptr;
    plugin_api_v2_t* api = nullptr;
    int refs = 0;
    std::string path;
};
ChainLib g_chain;

// Chain instances are POOLED, never destroyed. The chain host initialises each
// sub-plugin with a host_api that lives INSIDE the chain instance
// (chain_instance_t::subplugin_host_api), and modules keep that pointer in a
// process-global. Destroying a chain instance would leave every module that
// was last initialised through it holding a dangling host_api — the next
// host->log() is a use-after-free (reproduced: moog's destroy log after a
// second slot is freed). On Move the four slots are created at boot and never
// destroyed, so that never happens; pooling gives a DAW the same lifetime.
std::vector<void*> g_pool;
host_api_v1_t g_host;                     // zero-initialised: reserved tail stays NULL

} // namespace

// Per-engine data the host_api trampolines reach through g_current.
struct Engine::HostState {
    Transport t;               // transport at the current engine block
    double beatAtBlock = -1.0;
    std::vector<MidiEvent>* midiOut = nullptr;
    int midiOutOffset = 0;     // host offset to stamp outgoing events with
};

namespace {

void hostLog(const char* msg) {
    if (!msg) return;
    const uint32_t i = g_logWrite.fetch_add(1, std::memory_order_relaxed) % kLogSlots;
    std::strncpy(g_log[i], msg, kLogLen - 1);
    g_log[i][kLogLen - 1] = '\0';
}

int queueOut(const uint8_t* msg, int len) {
    // USB-MIDI packet: [cable|CIN, status, d1, d2]
    if (!g_current || !g_current->hs_ || !g_current->hs_->midiOut || len < 4) return 0;
    auto* out = g_current->hs_->midiOut;
    if (out->size() >= out->capacity()) return 0;          // never allocate here
    MidiEvent e;
    e.offset = g_current->hs_->midiOutOffset;
    e.data[0] = msg[1]; e.data[1] = msg[2]; e.data[2] = msg[3];
    const uint8_t st = msg[1];
    e.len = (st >= 0xF8) ? 1 : ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 2 : 3;
    out->push_back(e);
    return len;
}

int hostMidiInternal(const uint8_t*, int len) { return len; }   // Move's own surface: no-op
int hostMidiExternal(const uint8_t* msg, int len) { return queueOut(msg, len); }
int hostMidiInject(const uint8_t* msg, int len) { return queueOut(msg, len); }

float hostBpm() {
    if (!g_current || !g_current->hs_) return 120.0f;
    const auto& t = g_current->hs_->t;
    return (float)(t.valid && t.bpm > 0 ? t.bpm : 120.0);
}

double hostBeat() {
    if (!g_current || !g_current->hs_) return -1.0;
    return g_current->hs_->beatAtBlock;
}

int hostClockStatus() {
    if (!g_current || !g_current->hs_) return MOVE_CLOCK_STATUS_UNAVAILABLE;
    const auto& t = g_current->hs_->t;
    if (!t.valid) return MOVE_CLOCK_STATUS_UNAVAILABLE;
    return t.playing ? MOVE_CLOCK_STATUS_RUNNING : MOVE_CLOCK_STATUS_STOPPED;
}

int hostSlotRecv(void*) {
    if (!g_current) return -2;
    return g_current->routing().receiveChannel;
}

void initHostApi() {
    static bool done = false;
    if (done) return;
    std::memset(&g_host, 0, sizeof g_host);
    g_host.api_version = MOVE_PLUGIN_API_VERSION;
    g_host.sample_rate = MOVE_SAMPLE_RATE;
    g_host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    g_host.mapped_memory = g_mailbox;
    g_host.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    g_host.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    g_host.log = hostLog;
    g_host.midi_send_internal = hostMidiInternal;
    g_host.midi_send_external = hostMidiExternal;
    g_host.get_clock_status = hostClockStatus;
    g_host.get_bpm = hostBpm;
    g_host.midi_inject_to_move = hostMidiInject;
    g_host.slot_recv_channel = hostSlotRecv;
    g_host.get_beat_position = hostBeat;
    done = true;
}

// Scoped "this engine is the one Schwung is talking to".
struct Current {
    explicit Current(Engine* e) : prev(g_current) { g_current = e; }
    ~Current() { g_current = prev; }
    Engine* prev;
};

bool fileExists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }

void mkdirs(const std::string& p) {
    std::string cur;
    std::stringstream ss(p);
    std::string part;
    if (!p.empty() && p[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part + "/";
        ::mkdir(cur.c_str(), 0755);
    }
}

inline int16_t toS16(float v) {
    v = std::max(-1.f, std::min(1.f, v));
    return (int16_t)std::lrintf(v * 32767.f);
}

} // namespace

// ===========================================================================
Engine::Engine() : hs_(std::make_unique<HostState>()) {
    std::memset(activeNote_, 0xFF, sizeof activeNote_);
    pending_.resize(4096);
}

Engine::~Engine() { close(); }

bool Engine::open(const Config& cfg, std::string& error) {
    close();
    cfg_ = cfg;
    // /data/UserData -> dataRoot for every module (see sw_fsredirect.h).
    ::setenv("SCHWUNG_DATA_ROOT", cfg.dataRoot.c_str(), 1);
    mkdirs(cfg.schwungDir + "/patches");
    mkdirs(cfg.dataRoot + "/UserLibrary/Samples");

    std::lock_guard<std::mutex> lk(globalLock());
    initHostApi();

    // Streaming modules (webstream, radiogarden, airplay...) talk to child
    // processes through pipes. A write to a pipe whose reader has gone raises
    // SIGPIPE, whose default action terminates the process — here, the DAW.
    // Schwung's own daemons ignore SIGPIPE for the same reason. Only take that
    // decision if the host has not: leave any installed handler alone.
    {
        struct sigaction cur;
        if (sigaction(SIGPIPE, nullptr, &cur) == 0 && cur.sa_handler == SIG_DFL) {
            struct sigaction ign {};
            ign.sa_handler = SIG_IGN;
            sigemptyset(&ign.sa_mask);
            sigaction(SIGPIPE, &ign, nullptr);
        }
    }

    const std::string chainDir = cfg.schwungDir + "/modules/chain";
    const std::string so = chainDir + "/dsp.so";
    if (!g_chain.handle) {
        if (!fileExists(so)) { error = "Signal Chain not found: " + so; return false; }
        void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { error = std::string("dlopen chain: ") + dlerror(); return false; }
        auto init = (move_plugin_init_v2_fn)dlsym(h, MOVE_PLUGIN_INIT_V2_SYMBOL);
        if (!init) { dlclose(h); error = "chain: move_plugin_init_v2 missing"; return false; }
        plugin_api_v2_t* api = init(&g_host);
        if (!api || !api->create_instance) { dlclose(h); error = "chain: init failed"; return false; }
        g_chain.handle = h; g_chain.api = api; g_chain.path = so;
    }
    Current cur(this);
    void* inst = nullptr;
    if (!g_pool.empty()) {
        inst = g_pool.back();
        g_pool.pop_back();
    } else {
        // The shim passes NULL config (shadow_chain_mgmt.c).
        inst = g_chain.api->create_instance(chainDir.c_str(), nullptr);
    }
    if (!inst) { error = "chain: create_instance failed"; return false; }
    g_chain.refs++;
    chainHandle_ = g_chain.handle;
    chain_ = g_chain.api;
    instance_ = inst;
    return true;
}

void Engine::close() {
    if (!instance_) return;
    std::lock_guard<std::mutex> lk(globalLock());
    Current cur(this);
    // Return the slot to the pool empty, as a freshly created one would be.
    if (chain_->set_param) {
        chain_->set_param(instance_, "clear", "1");
        chain_->set_param(instance_, "knob_cc_out", "0");
        chain_->set_param(instance_, "midi_fx_pre_mode", "0");
    }
    g_pool.push_back(instance_);
    instance_ = nullptr;
    if (--g_chain.refs == 0) {
        // Keep the image mapped: modules' static destructors and threads may
        // outlive the last instance by a moment, and reopening is cheap.
    }
}

void Engine::prepare(double hostRate, int maxHostBlock) {
    std::lock_guard<std::mutex> lk(globalLock());
    hostRate_ = hostRate > 0 ? hostRate : 44100.0;
    ratio_ = kEngineRate / hostRate_;
    maxHostBlock_ = std::max(1, maxHostBlock);
    const int maxIn = (int)std::ceil(maxHostBlock_ * ratio_) + kBlock * 2 + 64;
    inRs_ = std::make_unique<Resampler>();
    outRs_ = std::make_unique<Resampler>();
    inRs_->setup(hostRate_, kEngineRate, maxHostBlock_ + 64);
    outRs_->setup(kEngineRate, hostRate_, maxIn + kBlock * 4);
    inFifo_.assign((size_t)(maxIn + kBlock * 4) * 2, 0.f);
    scratchIn_.assign((size_t)(maxHostBlock_ + 64) * 2, 0.f);
    scratchOut_.assign((size_t)(maxIn + kBlock * 8) * 2, 0.f);
    inFifoFrames_ = 0;

    // CONSTANT LATENCY BY CONSTRUCTION ("push" model).
    // Every complete 128-frame block of engine-rate input is rendered as soon
    // as it exists; the output stream is pre-filled with D frames of silence.
    // Engine frame e then sits at output-stream position D + e, read at a
    // fixed rate, so every sample (and every MIDI event scheduled on the
    // same timeline) leaves exactly L host samples after it arrived, whatever
    // the host block size. D is the least that never underruns: a block can
    // be up to 127 frames short of complete, plus both filters' look-ahead.
    // Where host input sample h lands on the engine timeline: MIDI and the
    // transport are scheduled with the same mapping, so a note and the audio
    // it plays against stay aligned.
    inDelayEngine_ = inRs_->delayIn() * ratio_;
    // Pre-roll: a block may be up to 127 frames short of complete, and each
    // filter needs its look-ahead before it can produce output.
    const int D = kBlock + (int)std::ceil(inRs_->lookaheadIn() * ratio_ + outRs_->lookaheadIn()) + 2;
    std::vector<float> silence((size_t)D * 2, 0.f);
    outRs_->push(silence.data(), D);
    engineLatencyFrames_ = D;
    latency_.store((int)std::lround(inRs_->delayIn() + (D + outRs_->delayIn()) / ratio_));

    pendingCount_ = 0;
    engineFrame_ = 0;
    hostFrame_ = 0;
    lastClockTick_ = INT64_MIN;
    wasPlaying_ = false;
}

void Engine::reset() {
    if (inRs_) prepare(hostRate_, maxHostBlock_);
}

// ---------------------------------------------------------------------------
// MIDI, the way the shim does it.
// ---------------------------------------------------------------------------
void Engine::deliver(const uint8_t* msg, int len, int source) {
    if (chain_ && chain_->on_midi) chain_->on_midi(instance_, msg, len, source);
}

void Engine::dispatchVoice(const uint8_t* in, int len) {
    uint8_t msg[3] = { in[0], len > 1 ? in[1] : (uint8_t)0, len > 2 ? in[2] : (uint8_t)0 };
    const uint8_t status = msg[0];
    if (status >= 0xF0) {                       // system messages pass untouched
        deliver(msg, len, MOVE_MIDI_SOURCE_EXTERNAL);
        return;
    }
    const int ch = status & 0x0F;
    const int recv = rtRecv_.load(std::memory_order_relaxed);
    if (recv >= 0 && recv != ch) return;        // slot receive filter

    // Forward channel (forward_channel.h: forward_channel_resolve)
    int fwd = rtFwd_.load(std::memory_order_relaxed);
    const int modDef = moduleDefaultFwd_.load(std::memory_order_relaxed);
    if (fwd == SlotRouting::kAuto && (modDef == SlotRouting::kThru || (modDef >= 0 && modDef <= 15)))
        fwd = modDef;
    int outCh;
    if (fwd == SlotRouting::kThru) outCh = ch;
    else if (fwd >= 0 && fwd <= 15) outCh = fwd;
    else outCh = (recv < 0) ? ch : recv;
    msg[0] = (uint8_t)((status & 0xF0) | outCh);

    // Transpose with note tracking (shadow_chain_apply_transpose)
    const uint8_t type = status & 0xF0;
    const int tr = rtTranspose_.load(std::memory_order_relaxed);
    if (type == 0x90 || type == 0x80 || type == 0xA0) {
        const int orig = msg[1];
        if (type == 0x90 && msg[2] > 0) {
            const int n = orig + tr;
            if (n < 0 || n > 127) return;
            activeNote_[ch][orig] = (uint8_t)n;
            msg[1] = (uint8_t)n;
        } else if (type == 0xA0) {
            const uint8_t n = activeNote_[ch][orig];
            if (n == 0xFF) { const int m = orig + tr; if (m < 0 || m > 127) return; msg[1] = (uint8_t)m; }
            else msg[1] = n;
        } else {                                   // note off
            const uint8_t n = activeNote_[ch][orig];
            if (n != 0xFF) { msg[1] = n; activeNote_[ch][orig] = 0xFF; }
            else { const int m = orig + tr; if (m < 0 || m > 127) return; msg[1] = (uint8_t)m; }
        }
    }
    deliver(msg, len, MOVE_MIDI_SOURCE_EXTERNAL);
    // shadow_midi.c also broadcasts every voice message to the slot's audio FX.
    uint8_t raw[3] = { in[0], len > 1 ? in[1] : (uint8_t)0, len > 2 ? in[2] : (uint8_t)0 };
    deliver(raw, len, MOVE_MIDI_SOURCE_FX_BROADCAST);
}

// 24 PPQN clock from the DAW transport, delivered like Move's cable-0 clock:
// single-byte messages, source EXTERNAL.
void Engine::emitClock(const Transport& t, double hostOffsetSamples) {
    const bool playing = t.valid && t.playing;
    if (playing && !wasPlaying_) {
        const uint8_t start[1] = { 0xFA };
        deliver(start, 1, MOVE_MIDI_SOURCE_EXTERNAL);
        const double ppq = t.ppq + hostOffsetSamples / hostRate_ * t.bpm / 60.0;
        lastClockTick_ = (int64_t)std::floor(ppq * 24.0) - 1;
    } else if (!playing && wasPlaying_) {
        const uint8_t stop[1] = { 0xFC };
        deliver(stop, 1, MOVE_MIDI_SOURCE_EXTERNAL);
    }
    wasPlaying_ = playing;
    if (!playing) { hs_->beatAtBlock = -1.0; return; }

    // Ticks whose time falls inside this engine block.
    const double ppqStart = t.ppq + hostOffsetSamples / hostRate_ * t.bpm / 60.0;
    const double ppqEnd = ppqStart + (kBlock / kEngineRate) * t.bpm / 60.0;
    hs_->beatAtBlock = ppqStart;
    const int64_t lastTick = (int64_t)std::floor(ppqEnd * 24.0 - 1e-9);
    if (lastClockTick_ == INT64_MIN || lastTick - lastClockTick_ > 96 || lastTick < lastClockTick_)
        lastClockTick_ = lastTick - 1;                  // relocate / loop: resync, no burst
    const uint8_t tick[1] = { 0xF8 };
    while (lastClockTick_ < lastTick) {
        deliver(tick, 1, MOVE_MIDI_SOURCE_EXTERNAL);
        ++lastClockTick_;
    }
}

// ---------------------------------------------------------------------------
// One Move block: 128 frames @ 44.1 kHz.
// ---------------------------------------------------------------------------
void Engine::renderEngineBlock(const Transport& t, std::vector<MidiEvent>* midiOut) {
    // Audio in -> mailbox (int16 interleaved, like MOVE_AUDIO_IN_OFFSET).
    int16_t* ain = reinterpret_cast<int16_t*>(g_mailbox + MOVE_AUDIO_IN_OFFSET);
    const size_t take = std::min<size_t>(inFifoFrames_, kBlock);
    for (size_t i = 0; i < (size_t)kBlock * 2; ++i)
        ain[i] = (i < take * 2) ? toS16(inFifo_[i]) : 0;
    if (take > 0) {
        std::memmove(inFifo_.data(), inFifo_.data() + take * 2, (inFifoFrames_ - take) * 2 * sizeof(float));
        inFifoFrames_ -= take;
    }

    // Host time this engine block corresponds to, relative to the start of
    // the current host block (engine frame e <-> host input time
    // (e - inDelayEngine_) / ratio_).
    const double hostOffset = ((double)engineFrame_ - inDelayEngine_) / ratio_ - (double)hostFrame_;
    hs_->t = t;
    hs_->midiOut = midiOut;
    hs_->midiOutOffset = std::max(0, std::min(currentBlockSize_ - 1, (int)std::lround(hostOffset)));

    emitClock(t, hostOffset);

    // MIDI due by the START of this block. As on Move, an event that falls
    // inside a block is heard from the next block boundary: never early,
    // at most one block (2.9 ms) late — so audio never precedes the
    // reported latency and the host's delay compensation stays exact.
    size_t w = 0;
    for (size_t i = 0; i < pendingCount_; ++i) {
        if (pending_[i].engineFrame <= engineFrame_) dispatchVoice(pending_[i].data, pending_[i].len);
        else pending_[w++] = pending_[i];
    }
    pendingCount_ = w;

    int16_t out[kBlock * 2];
    std::memset(out, 0, sizeof out);
    chain_->render_block(instance_, out, kBlock);

    float fo[kBlock * 2];
    for (int i = 0; i < kBlock * 2; ++i) fo[i] = out[i] * (1.0f / 32768.0f);
    outRs_->push(fo, kBlock);
    engineFrame_ += kBlock;
}

void Engine::process(const float* const* in, int numIn, float* const* out, int numOut, int n,
                     const MidiEvent* events, int numEvents, const Transport& transport,
                     std::vector<MidiEvent>* midiOut) {
    if (!instance_ || !outRs_) {
        for (int c = 0; c < numOut; ++c) std::memset(out[c], 0, sizeof(float) * (size_t)n);
        return;
    }
    currentBlockSize_ = n;

    // 1) MIDI onto the engine timeline: host sample h <-> engine frame
    //    h * ratio + inDelayEngine_ (the same mapping audio input gets).
    for (int e = 0; e < numEvents; ++e) {
        if (pendingCount_ >= pending_.size()) break;
        Pending& p = pending_[pendingCount_++];
        p.engineFrame = (int64_t)std::floor((double)(hostFrame_ + events[e].offset) * ratio_ + inDelayEngine_);
        std::memcpy(p.data, events[e].data, 3);
        p.len = events[e].len;
    }

    // 2) host input -> engine-rate FIFO. Needs no lock: it is this engine's.
    for (int i = 0; i < n; ++i) {
        const float l = numIn > 0 && in[0] ? in[0][i] : 0.f;
        const float r = numIn > 1 && in[1] ? in[1][i] : l;
        scratchIn_[2 * i] = l; scratchIn_[2 * i + 1] = r;
    }
    inRs_->push(scratchIn_.data(), n);
    auto drainInput = [&] {
        for (;;) {
            size_t room = inFifo_.size() / 2 - inFifoFrames_;
            if (room < (size_t)kBlock) {
                // Backlog beyond capacity (the lock was held for a long module
                // load): count the oldest block as rendered silence, so the
                // timeline and the latency stay exactly what we reported.
                std::memmove(inFifo_.data(), inFifo_.data() + kBlock * 2, (inFifoFrames_ - kBlock) * 2 * sizeof(float));
                inFifoFrames_ -= kBlock;
                float z[kBlock * 2] = {};
                outRs_->push(z, kBlock);
                engineFrame_ += kBlock;
                room += kBlock;
            }
            const int got = inRs_->pull(inFifo_.data() + inFifoFrames_ * 2, (int)room);
            inFifoFrames_ += (size_t)got;
            if (got < (int)room) break;
        }
    };
    drainInput();

    // 3) Render every complete Move block, under the process-wide Schwung
    //    lock. Other instances' renders are short, so wait a little; a module
    //    load on the UI thread is not, and then the blocks simply wait in the
    //    FIFO for the next callback (the pre-roll covers the gap).
    auto& gl = globalLock();
    bool locked = gl.try_lock();
    if (!locked) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::microseconds((int64_t)(0.25e6 * n / hostRate_));
        while (!locked && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            locked = gl.try_lock();
        }
    }
    if (locked) {
        std::lock_guard<std::mutex> lk(gl, std::adopt_lock);
        Current cur(this);
        if (hook_) hook_(hookCtx_, *this);
        Transport t = transport;
        while (inFifoFrames_ >= (size_t)kBlock)
            renderEngineBlock(t, midiOut);
    }

    // 4) engine rate -> host rate. The pre-roll D guarantees n frames
    //    whenever the renders above kept up.
    const int got = outRs_->pull(scratchOut_.data(), n);
    for (int c = 0; c < numOut; ++c) {
        float* o = out[c];
        const int src = std::min(c, 1);
        for (int i = 0; i < n; ++i) o[i] = i < got ? scratchOut_[2 * i + src] : 0.f;
    }
    if (got < n) {
        // Keep the output stream's clock: account for what we could not read.
        underruns_.fetch_add(1, std::memory_order_relaxed);
        float z[kBlock * 2] = {};
        int missing = n - got;
        while (missing > 0) { const int k = std::min(missing, kBlock); outRs_->push(z, k); missing -= k; }
        outRs_->pull(scratchOut_.data(), n - got);
    }
    hostFrame_ += n;
    hs_->midiOut = nullptr;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
void Engine::setParamUnlocked(const std::string& key, const std::string& value) {
    if (!instance_ || !chain_->set_param) return;
    Current cur(this);
    chain_->set_param(instance_, key.c_str(), value.c_str());
    if (key == "synth:module") refreshModuleDefaultForward();
}

std::string Engine::getParamUnlocked(const std::string& key, int maxLen) {
    if (!instance_ || !chain_->get_param) return {};
    Current cur(this);
    std::string buf((size_t)maxLen, '\0');
    const int n = chain_->get_param(instance_, key.c_str(), buf.data(), maxLen);
    if (n <= 0) return {};
    buf.resize((size_t)std::min(n, maxLen - 1));
    buf.resize(std::strlen(buf.c_str()));
    return buf;
}

int Engine::getParamRaw(const char* key, char* buf, int len) {
    if (!instance_ || !chain_->get_param || len <= 0) return -1;
    buf[0] = '\0';
    const int n = chain_->get_param(instance_, key, buf, len);
    buf[len - 1] = '\0';
    return n <= 0 ? -1 : (int)std::strlen(buf);
}

void Engine::setParamRaw(const char* key, const char* value) {
    if (instance_ && chain_->set_param) chain_->set_param(instance_, key, value);
}

void Engine::setParam(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(globalLock());
    setParamUnlocked(key, value);
}

std::string Engine::getParam(const std::string& key, int maxLen) {
    std::lock_guard<std::mutex> lk(globalLock());
    return getParamUnlocked(key, maxLen);
}

void Engine::call(const std::function<void(Engine&)>& fn) {
    std::lock_guard<std::mutex> lk(globalLock());
    Current cur(this);
    fn(*this);
}

// A module's module.json may carry capabilities.default_forward_channel
// (-2 THRU, 1..16). Cached at load like the shim does; never read per event.
void Engine::refreshModuleDefaultForward() {
    int def = SlotRouting::kAuto;
    const std::string id = getParamUnlocked("synth_module", 256);
    if (!id.empty()) {
        std::ifstream f(cfg_.schwungDir + "/modules/sound_generators/" + id + "/module.json");
        if (f) {
            try {
                json j = json::parse(f, nullptr, false);
                const json* v = nullptr;
                if (j.contains("capabilities") && j["capabilities"].contains("default_forward_channel"))
                    v = &j["capabilities"]["default_forward_channel"];
                else if (j.contains("default_forward_channel"))
                    v = &j["default_forward_channel"];
                if (v && v->is_number_integer()) {
                    const int c = v->get<int>();
                    if (c == -2) def = SlotRouting::kThru;
                    else if (c >= 1 && c <= 16) def = c - 1;
                }
            } catch (...) {}
        }
    }
    moduleDefaultFwd_.store(def);
}

SlotRouting Engine::routing() const {
    std::lock_guard<std::mutex> lk(routingMutex_);
    return routing_;
}

void Engine::setRouting(const SlotRouting& r) {
    std::lock_guard<std::mutex> lk(routingMutex_);
    routing_ = r;
    rtRecv_.store(r.receiveChannel);
    rtFwd_.store(r.forwardChannel);
    rtTranspose_.store(r.transpose);
}

std::vector<std::string> Engine::drainLog() {
    std::lock_guard<std::mutex> lk(g_logReadMutex);
    std::vector<std::string> out;
    const uint32_t w = g_logWrite.load();
    if (w - g_logRead > (uint32_t)kLogSlots) g_logRead = w - kLogSlots;
    while (g_logRead != w) out.emplace_back(g_log[g_logRead++ % kLogSlots]);
    return out;
}

// ---------------------------------------------------------------------------
// Slot state: Shadow UI's buildSlotPatchJson + autosave wrapper, in C++.
// ---------------------------------------------------------------------------
std::string Engine::saveSlotState() {
    // KEY ORDER IS PART OF THE FORMAT. chain_patch.c scans each object span
    // for the first "type"/"module" it meets, so a component's own state
    // containing a "type" key (chord: "type":"major") must come AFTER the
    // component's type. JavaScript objects keep insertion order, and this
    // mirrors Shadow UI's buildSlotPatchJson key for key; ordered_json keeps
    // it (plain json would sort "params" ahead of "type").
    using ojson = nlohmann::ordered_json;
    ojson patch;
    std::lock_guard<std::mutex> lk(globalLock());
    Current cur(this);

    auto componentEntry = [&](const std::string& id) {
        ojson config = ojson::object();
        const std::string st = getParamUnlocked(id + ":state", 1 << 20);
        if (!st.empty()) {
            ojson parsed = ojson::parse(st, nullptr, false);
            config = ojson{{"state", parsed.is_discarded() ? ojson(st) : parsed}};
        }
        const std::string byp = getParamUnlocked(id + ":bypassed", 16);
        return std::make_pair(config, (!byp.empty() && std::atoi(byp.c_str()) == 1) ? 1 : 0);
    };

    patch["custom_name"] = "Schwung AU";
    patch["input"] = "both";
    patch["synth"] = nullptr;
    patch["audio_fx"] = ojson::array();

    const std::string synth = getParamUnlocked("synth_module", 256);
    if (!synth.empty()) {
        auto [cfg, byp] = componentEntry("synth");
        ojson s; s["module"] = synth; s["config"] = cfg; s["bypassed"] = byp;
        patch["synth"] = s;
    }
    for (int i = 1; i <= 8; ++i) {
        const std::string m = getParamUnlocked("midi_fx" + std::to_string(i) + "_module", 256);
        if (m.empty()) continue;
        auto [cfg, byp] = componentEntry("midi_fx" + std::to_string(i));
        if (!patch.contains("midi_fx")) patch["midi_fx"] = ojson::array();
        ojson e; e["type"] = m; e["params"] = cfg; e["bypassed"] = byp;
        patch["midi_fx"].push_back(e);
    }
    for (int i = 1; i <= 8; ++i) {
        const std::string m = getParamUnlocked("fx" + std::to_string(i) + "_module", 256);
        if (m.empty()) continue;
        auto [cfg, byp] = componentEntry("fx" + std::to_string(i));
        ojson e; e["type"] = m; e["params"] = cfg; e["bypassed"] = byp;
        patch["audio_fx"].push_back(e);
    }
    const SlotRouting r = routing();
    patch["receive_channel"] = r.receiveChannel;
    patch["forward_channel"] = r.forwardChannel;
    const std::string pre = getParamUnlocked("midi_fx_pre_mode", 16);
    if (!pre.empty()) patch["midi_fx_pre_mode"] = std::atoi(pre.c_str()) ? 1 : 0;
    const std::string cc = getParamUnlocked("knob_cc_out", 16);
    if (!cc.empty()) patch["knob_cc_out"] = std::atoi(cc.c_str()) ? 1 : 0;
    const std::string km = getParamUnlocked("knob_mappings", 1 << 16);
    if (!km.empty()) {
        ojson j = ojson::parse(km, nullptr, false);
        if (j.is_array() && !j.empty()) patch["knob_mappings"] = j;
    }
    const std::string lfo = getParamUnlocked("lfo_config", 1 << 16);
    if (!lfo.empty()) {
        ojson j = ojson::parse(lfo, nullptr, false);
        if (!j.is_discarded() && !j.is_null()) patch["lfos"] = j;
    }

    ojson wrapper;
    wrapper["name"] = "Schwung AU";
    wrapper["version"] = 1;
    wrapper["modified"] = getParamUnlocked("dirty", 8) == "1";
    wrapper["chain"] = patch;
    wrapper["schwung_au"] = ojson{{"transpose", r.transpose}};
    return wrapper.dump(2);
}

bool Engine::loadSlotState(const std::string& text, std::string& error) {
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.contains("chain")) { error = "not a Schwung slot document"; return false; }

    SlotRouting r;
    const json& c = doc["chain"];
    if (c.contains("receive_channel") && c["receive_channel"].is_number_integer()) r.receiveChannel = c["receive_channel"];
    if (c.contains("forward_channel") && c["forward_channel"].is_number_integer()) r.forwardChannel = c["forward_channel"];
    if (doc.contains("schwung_au") && doc["schwung_au"].contains("transpose")) r.transpose = doc["schwung_au"]["transpose"];
    setRouting(r);

    // Same path the shim uses to restore autosave: a file + "load_file".
    const std::string dir = cfg_.schwungDir + "/.au_state";
    mkdirs(dir);
    char name[64];
    std::snprintf(name, sizeof name, "/slot_%p.json", (void*)this);
    const std::string path = dir + name;
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) { error = "cannot write " + path; return false; }
        f << text;
    }
    std::lock_guard<std::mutex> lk(globalLock());
    Current cur(this);
    setParamUnlocked("clear", "1");
    setParamUnlocked("load_file", path);
    refreshModuleDefaultForward();
    std::remove(path.c_str());
    return true;
}

} // namespace schwung
