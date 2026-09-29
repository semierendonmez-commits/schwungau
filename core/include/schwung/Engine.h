// Schwung shell engine — runs one Schwung Signal Chain slot inside a plugin.
//
// This is the part of Schwung's shim that a DAW track needs, and nothing
// more: it loads the UNMODIFIED chain host (modules/chain/dsp.so), gives it a
// host_api_v1_t, feeds it MIDI and audio exactly the way the shim does on
// Move, and renders it in Move's native format — 44100 Hz, 128-frame blocks,
// int16 interleaved stereo — adapting to whatever rate and block size the
// DAW runs at.
//
// THREADING. Schwung's rule is "there is no control thread": every module
// entry point runs on one thread and nothing is ever concurrent. We keep that
// invariant with a single mutex around every call into the chain:
//   * the audio thread takes it with a bounded try-lock; if a slow operation
//     (a module load) holds it, that block renders silence instead of waiting;
//   * the UI / state threads take it normally through call()/setParam()/...
// So a module sees exactly one caller at a time, as on the device, while
// loads no longer run on the audio callback (on Move they do).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct host_api_v1;
struct plugin_api_v2;

namespace schwung {

class Resampler;

struct MidiEvent {
    int     offset = 0;      // sample offset within the host block
    uint8_t data[3] = {0, 0, 0};
    uint8_t len = 0;
};

struct Transport {
    bool   valid   = false;  // host supplied transport info
    bool   playing = false;
    double bpm     = 120.0;
    double ppq     = 0.0;    // quarter notes since song start, at block start
};

// Slot-level MIDI routing, owned by the shim on Move (not by the chain).
struct SlotRouting {
    static constexpr int kAll  = -1;   // receive: all channels
    static constexpr int kAuto = -1;   // forward: module default / receive ch
    static constexpr int kThru = -2;   // forward: keep original channel
    int receiveChannel = kAll;         // -1 or 0..15
    int forwardChannel = kAuto;        // -2, -1 or 0..15
    int transpose      = 0;            // semitones
};

class Engine {
public:
    static constexpr int    kBlock      = 128;
    static constexpr double kEngineRate = 44100.0;

    struct Config {
        std::string dataRoot;    // stands in for Move's /data/UserData
        std::string schwungDir;  // <dataRoot>/schwung (modules/, patches/)
    };

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- lifecycle (non-realtime) ---------------------------------------
    bool open(const Config& cfg, std::string& error);
    void close();
    bool isOpen() const { return instance_ != nullptr; }
    void prepare(double hostRate, int maxHostBlock);
    int  latencySamples() const { return latency_.load(); }  // at host rate
    void reset();

    // ---- audio thread ---------------------------------------------------
    // in/out: planar float, numIn/numOut channels (mono inputs are duplicated,
    // missing inputs are silence). midiOut receives what modules send out.
    void process(const float* const* in, int numIn,
                 float* const* out, int numOut, int numFrames,
                 const MidiEvent* events, int numEvents,
                 const Transport& transport,
                 std::vector<MidiEvent>* midiOut);

    // ---- control (any non-audio thread) -----------------------------------
    void        setParam(const std::string& key, const std::string& value);
    std::string getParam(const std::string& key, int maxLen = 65536);
    // Allocation-free access for the audio thread, valid ONLY inside the
    // pre-render hook (the lock is already held there).
    int  getParamRaw(const char* key, char* buf, int len);
    void setParamRaw(const char* key, const char* value);
    // Called on the audio thread, with the Schwung lock held, before any
    // Move block of this host block is rendered (Move services parameter
    // requests at exactly this point: shim_pre_transfer).
    using PreRenderHook = void (*)(void* ctx, Engine& e);
    void setPreRenderHook(PreRenderHook fn, void* ctx) { hook_ = fn; hookCtx_ = ctx; }

    // Run several chain calls under one lock acquisition.
    void call(const std::function<void(Engine&)>& fn);
    // Unlocked variants for use inside call().
    void        setParamUnlocked(const std::string& key, const std::string& value);
    std::string getParamUnlocked(const std::string& key, int maxLen = 65536);

    // ---- slot state -------------------------------------------------------
    // Serialises the slot exactly as Shadow UI's buildSlotPatchJson does, so
    // the document is a valid Schwung patch file.
    std::string saveSlotState();
    bool        loadSlotState(const std::string& json, std::string& error);

    SlotRouting routing() const;
    void        setRouting(const SlotRouting& r);

    const Config& config() const { return cfg_; }

    // Log lines emitted by modules (drained by the UI; bounded ring).
    std::vector<std::string> drainLog();

    // ---- host_api callbacks (static trampolines need access) -------------
    struct HostState;

private:
    void renderEngineBlock(const Transport& t, std::vector<MidiEvent>* midiOut);
    void dispatchVoice(const uint8_t* msg, int len);
    void deliver(const uint8_t* msg, int len, int source);
    void emitClock(const Transport& t, double hostOffsetSamples);
    void refreshModuleDefaultForward();

    Config cfg_;
    void* chainHandle_ = nullptr;
    plugin_api_v2* chain_ = nullptr;
    void* instance_ = nullptr;
    std::unique_ptr<host_api_v1> hostApi_;
    std::vector<uint8_t> mailbox_;

    std::mutex chainMutex_;

    // rate adaptation
    double hostRate_ = 44100.0;
    double ratio_ = 1.0;                 // engine / host
    std::unique_ptr<Resampler> inRs_, outRs_;
    std::vector<float> inFifo_;          // interleaved stereo @44.1k
    size_t inFifoFrames_ = 0;
    std::vector<float> outFifo_;         // interleaved stereo @44.1k
    size_t outFifoFrames_ = 0, outFifoRead_ = 0;
    std::vector<float> scratchIn_, scratchOut_;
    std::atomic<int> latency_{0};
    int engineLatencyFrames_ = kBlock;
    double inDelayEngine_ = 0.0;         // input filter delay, engine frames
    int maxHostBlock_ = 512;
    int currentBlockSize_ = 1;
    std::atomic<int> underruns_{0};
public:
    int underruns() const { return underruns_.load(); }   // should stay 0
private:

    // timeline
    int64_t engineFrame_ = 0;            // frames rendered so far
    int64_t hostFrame_ = 0;              // host frames processed so far
    struct Pending { int64_t engineFrame; uint8_t data[3]; uint8_t len; };
    std::vector<Pending> pending_;       // fixed capacity, RT-safe
    size_t pendingCount_ = 0;

    // transport / clock
    Transport blockTransport_;
    bool wasPlaying_ = false;
    int64_t lastClockTick_ = INT64_MIN;

    // routing
    mutable std::mutex routingMutex_;
    SlotRouting routing_;
    std::atomic<int> rtRecv_{SlotRouting::kAll}, rtFwd_{SlotRouting::kAuto},
                     rtTranspose_{0}, moduleDefaultFwd_{SlotRouting::kAuto};
    uint8_t activeNote_[16][128];

    std::vector<MidiEvent>* currentMidiOut_ = nullptr;
    PreRenderHook hook_ = nullptr;
    void* hookCtx_ = nullptr;

public:
    std::unique_ptr<HostState> hs_;
};

} // namespace schwung
