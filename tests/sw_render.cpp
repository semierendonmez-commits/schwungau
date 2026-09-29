// sw_render — headless driver for the Schwung shell engine.
//
//   sw_render --data <dataRoot> [--rate 48000] [--block 512]
//             [--synth id] [--midi-fx id]... [--fx id]... [--out file.wav]
//   sw_render --data <dataRoot> --sweep [--rate R] [--block B] [--json out.json]
//
// Single mode renders a short phrase through one chain and writes a WAV.
// Sweep mode loads EVERY installed chainable module in its own forked child
// (a crashing module cannot take the sweep down), plays notes / feeds audio,
// and reports: loaded, error text, RMS, peak, NaN, crash signal, timeout,
// state round-trip.
#include "schwung/Engine.h"
#include "schwung/Catalog.h"

#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace schwung;

struct Opts {
    std::string data, out, json, synth;
    std::vector<std::string> fx, mfx;
    double rate = 48000;
    int block = 512;
    double seconds = 3.0;
    bool sweep = false;
    std::string only;
};

static void writeWav(const std::string& path, const std::vector<float>& lr, int rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)lr.size(), bytes = n * 2;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (float v : lr) { int16_t s = (int16_t)std::lrintf(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f); u16((uint16_t)s); }
    std::fclose(f);
}

struct Result {
    bool loaded = false;
    std::string error;
    double rms = 0, peak = 0;
    bool nan = false;
    bool stateOk = false;
    double renderMsPerSec = 0;   // CPU cost: ms of wall time per second of audio
};

// Render `seconds` of audio through `e` while playing a phrase.
static Result runChain(Engine& e, const Opts& o, bool feedInput, std::vector<float>* capture) {
    Result r;
    const int n = o.block;
    std::vector<float> L(n), R(n), inL(n), inR(n);
    float* outs[2] = { L.data(), R.data() };
    const float* ins[2] = { inL.data(), inR.data() };
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
    const int total = (int)(o.seconds * o.rate);
    Transport t; t.valid = true; t.playing = true; t.bpm = 120;
    std::vector<MidiEvent> midiOut; midiOut.reserve(1024);
    // Phrase: chord at 0.1s, notes change every 0.5s, all off at 75%.
    const int notes[4][3] = {{60, 64, 67}, {62, 65, 69}, {57, 60, 64}, {55, 59, 62}};
    double sum = 0; long cnt = 0; double phase = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int pos = 0; pos < total; pos += n) {
        std::vector<MidiEvent> ev;
        for (int k = 0; k < n; ++k) {
            const int s = pos + k;
            const int step = (int)(o.rate * 0.5);
            if (s >= (int)(o.rate * 0.1) && (s - (int)(o.rate * 0.1)) % step == 0 && s < total * 3 / 4) {
                const int idx = ((s - (int)(o.rate * 0.1)) / step) % 4;
                const int prev = (idx + 3) % 4;
                if (s > (int)(o.rate * 0.1))
                    for (int m : notes[prev]) { MidiEvent x; x.offset = k; x.data[0] = 0x80; x.data[1] = (uint8_t)m; x.data[2] = 0; x.len = 3; ev.push_back(x); }
                for (int m : notes[idx]) { MidiEvent x; x.offset = k; x.data[0] = 0x90; x.data[1] = (uint8_t)m; x.data[2] = 100; x.len = 3; ev.push_back(x); }
                // Drum machines listen on Move's pad notes (36-51): kick, snare, hats.
                for (int m : {36 + idx, 38 + idx, 42 + idx}) {
                    MidiEvent on; on.offset = k; on.data[0] = 0x90; on.data[1] = (uint8_t)m; on.data[2] = 110; on.len = 3; ev.push_back(on);
                }
            }
            if (s == total * 3 / 4)
                for (int ch = 0; ch < 1; ++ch) { MidiEvent x; x.offset = k; x.data[0] = 0xB0; x.data[1] = 123; x.data[2] = 0; x.len = 3; ev.push_back(x); }
            if (feedInput) {
                phase += 220.0 / o.rate;
                inL[k] = 0.3f * (float)std::sin(2 * M_PI * phase) + noise(rng) * 0.2f;
                inR[k] = inL[k];
            } else { inL[k] = inR[k] = 0; }
        }
        t.ppq = pos / o.rate * 2.0;
        midiOut.clear();
        e.process(ins, 2, outs, 2, n, ev.data(), (int)ev.size(), t, &midiOut);
        for (int k = 0; k < n; ++k) {
            if (!std::isfinite(L[k]) || !std::isfinite(R[k])) r.nan = true;
            sum += (double)L[k] * L[k] + (double)R[k] * R[k]; cnt += 2;
            r.peak = std::max(r.peak, (double)std::max(std::fabs(L[k]), std::fabs(R[k])));
            if (capture) { capture->push_back(L[k]); capture->push_back(R[k]); }
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    r.renderMsPerSec = std::chrono::duration<double, std::milli>(t1 - t0).count() / o.seconds;
    r.rms = cnt ? std::sqrt(sum / cnt) : 0;
    return r;
}

static bool openEngine(Engine& e, const Opts& o, std::string& err) {
    Engine::Config c;
    c.dataRoot = o.data;
    c.schwungDir = o.data + "/schwung";
    if (!e.open(c, err)) return false;
    e.prepare(o.rate, o.block);
    return true;
}

static std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c < 0x20) { o += ' '; continue; } o += c; }
    return o;
}

// One module test in a child process; prints a JSON line on stdout.
static void childTest(const Opts& o, const ModuleInfo& m, int fd) {
    Engine e;
    std::string err;
    Result r;
    if (!openEngine(e, o, err)) { r.error = err; }
    else {
        bool input = true;     // modules that listen to audio in get a test signal
        if (m.componentType == "sound_generator") {
            e.setParam("synth:module", m.id);
            r.error = e.getParam("synth_error", 512);
            r.loaded = e.getParam("synth_module", 256) == m.id;
        } else if (m.componentType == "audio_fx") {
            e.setParam("synth:module", "linein");         // audio in -> FX
            e.setParam("fx1:module", m.id);
            r.loaded = e.getParam("fx1_module", 256) == m.id;
            input = true;
        } else if (m.componentType == "midi_fx") {
            e.setParam("midi_fx1:module", m.id);
            e.setParam("synth:module", "moog");
            r.loaded = e.getParam("midi_fx1_module", 256) == m.id;
        }
        // Why a module did not load, in the chain host's own words
        // ("dlopen failed: ... symbol not found ...").
        if (!r.loaded)
            for (auto& l : e.drainLog())
                if (l.find("failed") != std::string::npos || l.find("rror") != std::string::npos) { r.error = l; }
        // Let asynchronous loaders (worker threads) land.
        for (int i = 0; i < 20; ++i) { Opts q = o; q.seconds = 0.05; runChain(e, q, false, nullptr); }
        Result rr = runChain(e, o, input, nullptr);
        r.rms = rr.rms; r.peak = rr.peak; r.nan = rr.nan; r.renderMsPerSec = rr.renderMsPerSec;
        // State round-trip: save, reload into a fresh engine, compare modules.
        const std::string st = e.saveSlotState();
        Engine e2;
        std::string err2;
        if (openEngine(e2, o, err2)) {
            std::string le;
            if (e2.loadSlotState(st, le)) {
                const char* key = m.componentType == "sound_generator" ? "synth_module"
                                : m.componentType == "audio_fx" ? "fx1_module" : "midi_fx1_module";
                r.stateOk = e2.getParam(key, 256) == m.id;
            }
        }
    }
    char line[2048];
    std::snprintf(line, sizeof line,
        "{\"id\":\"%s\",\"type\":\"%s\",\"loaded\":%s,\"rms\":%.5f,\"peak\":%.4f,\"nan\":%s,"
        "\"state\":%s,\"cpu_ms_per_s\":%.1f,\"error\":\"%s\"}\n",
        m.id.c_str(), m.componentType.c_str(), r.loaded ? "true" : "false", r.rms, r.peak,
        r.nan ? "true" : "false", r.stateOk ? "true" : "false", r.renderMsPerSec, jsonEsc(r.error).c_str());
    (void)!write(fd, line, std::strlen(line));
}

static int sweep(const Opts& o) {
    Catalog cat;
    cat.scan(o.data + "/schwung", o.data + "/schwung/module-catalog.json");
    FILE* jf = o.json.empty() ? nullptr : std::fopen(o.json.c_str(), "w");
    if (jf) std::fprintf(jf, "[\n");
    int idx = 0, ok = 0, total = 0;
    for (auto& m : cat.all()) {
        if (!m.chainable) continue;
        if (!o.only.empty() && ("," + o.only + ",").find("," + m.id + ",") == std::string::npos) continue;
        ++total;
        int p[2]; if (pipe(p) != 0) return 1;
        pid_t pid = fork();
        if (pid == 0) { close(p[0]); alarm(60); childTest(o, m, p[1]); _exit(0); }
        close(p[1]);
        std::string outp; char buf[4096]; ssize_t k;
        while ((k = read(p[0], buf, sizeof buf)) > 0) outp.append(buf, (size_t)k);
        close(p[0]);
        int status = 0; waitpid(pid, &status, 0);
        std::string line = outp;
        if (line.empty()) {
            char b[512];
            const bool timeout = WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM;
            std::snprintf(b, sizeof b,
                "{\"id\":\"%s\",\"type\":\"%s\",\"loaded\":false,\"crash\":\"%s\"}\n",
                m.id.c_str(), m.componentType.c_str(),
                timeout ? "timeout" : WIFSIGNALED(status) ? strsignal(WTERMSIG(status)) : "exit");
            line = b;
        }
        if (line.find("\"loaded\":true") != std::string::npos && line.find("\"nan\":false") != std::string::npos) ++ok;
        std::fputs(line.c_str(), stdout); std::fflush(stdout);
        if (jf) { if (idx++) std::fprintf(jf, ",\n"); line.pop_back(); std::fputs(line.c_str(), jf); }
    }
    if (jf) { std::fprintf(jf, "\n]\n"); std::fclose(jf); }
    std::fprintf(stderr, "sweep: %d/%d loaded and rendered finite audio\n", ok, total);
    return 0;
}

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--data") o.data = next();
        else if (a == "--rate") o.rate = std::atof(next().c_str());
        else if (a == "--block") o.block = std::atoi(next().c_str());
        else if (a == "--synth") o.synth = next();
        else if (a == "--fx") o.fx.push_back(next());
        else if (a == "--midi-fx") o.mfx.push_back(next());
        else if (a == "--out") o.out = next();
        else if (a == "--json") o.json = next();
        else if (a == "--seconds") o.seconds = std::atof(next().c_str());
        else if (a == "--only") o.only = next();
        else if (a == "--sweep") o.sweep = true;
    }
    if (o.data.empty()) { std::fprintf(stderr, "--data required\n"); return 2; }
    if (o.sweep) return sweep(o);

    Engine e; std::string err;
    if (!openEngine(e, o, err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    std::printf("latency: %d samples @ %.0f Hz\n", e.latencySamples(), o.rate);
    for (size_t i = 0; i < o.mfx.size(); ++i) e.setParam("midi_fx" + std::to_string(i + 1) + ":module", o.mfx[i]);
    if (!o.synth.empty()) e.setParam("synth:module", o.synth);
    for (size_t i = 0; i < o.fx.size(); ++i) e.setParam("fx" + std::to_string(i + 1) + ":module", o.fx[i]);
    std::printf("synth=%s err=%s fx1=%s midi_fx1=%s\n", e.getParam("synth_module").c_str(),
                e.getParam("synth_error").c_str(), e.getParam("fx1_module").c_str(), e.getParam("midi_fx1_module").c_str());
    std::vector<float> cap;
    Result r = runChain(e, o, o.synth == "linein", &cap);
    std::printf("rms=%.5f peak=%.4f nan=%d cpu=%.1f ms/s\n", r.rms, r.peak, (int)r.nan, r.renderMsPerSec);
    for (auto& l : e.drainLog()) std::printf("log: %s\n", l.c_str());
    if (!o.out.empty()) writeWav(o.out, cap, (int)o.rate);
    const std::string st = e.saveSlotState();
    std::printf("state (%zu bytes): %.300s\n", st.size(), st.c_str());
    return 0;
}
