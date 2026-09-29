// sw_latency — regression test for the engine's timing contract.
//
//   sw_latency --data <dataRoot>
//
// 1. Audio: impulses through `linein` at several host rates, with the host
//    block size changing randomly on every call. Every impulse must come out
//    exactly latencySamples() later (±1 for rounding), with no underruns.
// 2. MIDI: notes into `moog`. Onset must never precede the reported latency
//    and must not depend on the host block size (identical across sizes).
//
// Exit code 0 = contract holds. Needs a data root with linein and moog built.
#include "schwung/Engine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace schwung;

static bool openAt(Engine& e, const std::string& data, double sr, int maxBlock, const char* synth) {
    Engine::Config c{data, data + "/schwung"};
    std::string err;
    if (!e.open(c, err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return false; }
    e.prepare(sr, maxBlock);
    e.setParam("synth:module", synth);
    return e.getParam("synth_module", 64) == synth;
}

int main(int argc, char** argv) {
    std::string data;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--data" && i + 1 < argc) data = argv[++i];
    if (data.empty()) { std::fprintf(stderr, "usage: sw_latency --data <dataRoot>\n"); return 2; }
    int failures = 0;
    std::mt19937 rng(7);

    std::printf("audio path (linein), random host block sizes\n");
    for (double sr : {44100.0, 48000.0, 88200.0, 96000.0}) {
        for (int maxb : {64, 512, 1024}) {
            Engine e;
            if (!openAt(e, data, sr, maxb, "linein")) { std::printf("  linein missing\n"); return 2; }
            const int total = (int)(sr * 1.4);
            std::vector<float> in(total, 0.f), out(total, 0.f), L(maxb), R(maxb);
            std::vector<int> imp;
            for (int k = 0; k < 6; ++k) { const int p = (int)(sr * 0.2) + k * (int)(sr * 0.17) + k * 37; imp.push_back(p); in[p] = 0.8f; }
            std::uniform_int_distribution<int> bs(1, maxb);
            Transport t;
            for (int pos = 0; pos < total;) {
                const int n = std::min(bs(rng), total - pos);
                const float* ins[2] = {&in[pos], &in[pos]};
                float* outs[2] = {L.data(), R.data()};
                e.process(ins, 2, outs, 2, n, nullptr, 0, t, nullptr);
                for (int i = 0; i < n; ++i) out[pos + i] = L[i];
                pos += n;
            }
            int mn = 1 << 30, mx = -1;
            for (int p : imp) {
                int best = p; float bv = 0;
                for (int i = p; i < std::min(total, p + 2000); ++i) if (std::fabs(out[i]) > bv) { bv = std::fabs(out[i]); best = i; }
                mn = std::min(mn, best - p); mx = std::max(mx, best - p);
            }
            const int rep = e.latencySamples();
            const bool ok = mn == mx && std::abs(mn - rep) <= 1 && e.underruns() == 0;
            failures += !ok;
            std::printf("  %s sr=%6.0f maxBlock=%5d reported=%4d measured=[%d..%d] underruns=%d\n",
                        ok ? "ok  " : "FAIL", sr, maxb, rep, mn, mx, e.underruns());
        }
    }

    std::printf("MIDI path (moog): onset after event, per host block size\n");
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        int refMin = -1, refMax = -1;
        for (int maxb : {32, 256, 1024}) {
            Engine e;
            if (!openAt(e, data, sr, maxb, "moog")) { std::printf("  moog missing\n"); return 2; }
            const int total = (int)(sr * 3.2);
            std::vector<float> out(total, 0.f), z(maxb, 0.f), L(maxb), R(maxb);
            std::vector<int> ev;
            for (int k = 0; k < 8; ++k) ev.push_back((int)(sr * 0.3) + k * (int)(sr * 0.37) + k * 53);
            std::uniform_int_distribution<int> bs(1, maxb);
            Transport t;
            for (int pos = 0; pos < total;) {
                const int n = std::min(bs(rng), total - pos);
                std::vector<MidiEvent> m;
                for (int on : ev) {
                    const int off = on + (int)(sr * 0.15);
                    if (on >= pos && on < pos + n) { MidiEvent x; x.offset = on - pos; x.data[0] = 0x90; x.data[1] = 48; x.data[2] = 120; x.len = 3; m.push_back(x); }
                    if (off >= pos && off < pos + n) { MidiEvent x; x.offset = off - pos; x.data[0] = 0x80; x.data[1] = 48; x.data[2] = 0; x.len = 3; m.push_back(x); }
                }
                const float* ins[2] = {z.data(), z.data()};
                float* outs[2] = {L.data(), R.data()};
                e.process(ins, 2, outs, 2, n, m.data(), (int)m.size(), t, nullptr);
                for (int i = 0; i < n; ++i) out[pos + i] = L[i];
                pos += n;
            }
            int mn = 1 << 30, mx = -1;
            for (int p : ev) { int i = p; while (i < total && std::fabs(out[i]) < 0.01f) ++i; mn = std::min(mn, i - p); mx = std::max(mx, i - p); }
            const int blockHost = (int)std::lround(128 * sr / 44100.0);
            bool ok = mn >= e.latencySamples() && (mx - mn) <= blockHost + 2;
            if (refMin < 0) { refMin = mn; refMax = mx; } else ok = ok && mn == refMin && mx == refMax;
            failures += !ok;
            std::printf("  %s sr=%6.0f maxBlock=%5d latency=%4d onset=[%d..%d]\n",
                        ok ? "ok  " : "FAIL", sr, maxb, e.latencySamples(), mn, mx);
        }
    }
    std::printf(failures ? "FAILED (%d)\n" : "all timing checks passed\n", failures);
    return failures ? 1 : 0;
}
