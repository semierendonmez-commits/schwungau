// Streaming stereo resampler: polyphase windowed-sinc (Kaiser), arbitrary
// fixed ratio, allocation-free after setup(). Used on both sides of the
// engine: host-rate input -> 44100 Hz, and 44100 Hz output -> host rate.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace schwung {

class Resampler {
public:
    // maxPush: largest number of frames ever pushed between pulls.
    void setup(double inRate, double outRate, int maxPush, int halfTaps = 24, int phases = 512) {
        inRate_ = inRate; outRate_ = outRate;
        step_ = inRate / outRate;
        identity_ = std::fabs(step_ - 1.0) < 1e-12;
        half_ = identity_ ? 0 : halfTaps;
        phases_ = phases;
        const double cutoff = std::min(1.0, outRate / inRate) * 0.94;
        const int taps = 2 * half_;
        table_.assign((size_t)(phases_ + 1) * (size_t)std::max(taps, 1), 0.f);
        if (!identity_) {
            const double beta = 8.6;
            for (int p = 0; p <= phases_; ++p) {
                const double frac = (double)p / phases_;
                double sum = 0.0;
                for (int t = 0; t < taps; ++t) {
                    const double x = (t - half_ + 1) - frac;        // distance from center
                    const double sx = cutoff * x;
                    const double sinc = (std::fabs(sx) < 1e-9) ? 1.0 : std::sin(M_PI * sx) / (M_PI * sx);
                    const double w = x / half_;
                    const double win = (std::fabs(w) >= 1.0) ? 0.0
                                      : bessel0(beta * std::sqrt(1.0 - w * w)) / bessel0(beta);
                    const double v = cutoff * sinc * win;
                    table_[(size_t)p * taps + t] = (float)v;
                    sum += v;
                }
                // normalise DC gain per phase
                if (sum != 0.0)
                    for (int t = 0; t < taps; ++t) table_[(size_t)p * taps + t] = (float)(table_[(size_t)p * taps + t] / sum);
            }
        }
        cap_ = (size_t)(maxPush + 4 * half_ + 64);
        buf_.assign(cap_ * 2, 0.f);
        reset();
    }

    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.f);
        // Start with `half_` frames of silence of history so the first output
        // is centred on real input: this is the filter's group delay.
        frames_ = (size_t)half_;
        pos_ = (double)(half_ > 0 ? half_ - 1 : 0);
    }

    // Latency in INPUT frames (group delay of the filter).
    double latencyIn() const { return identity_ ? 0.0 : (double)half_; }
    // Group delay actually added, in INPUT frames: reset() pre-rolls half a
    // kernel of silence and centres the first output on it, so the filter's
    // look-ahead is paid by availability (see available()), not by delay.
    double delayIn() const { return identity_ ? 0.0 : 1.0; }
    // Input frames that must arrive before an output can be produced.
    double lookaheadIn() const { return identity_ ? 0.0 : (double)half_; }

    bool identity() const { return identity_; }

    void push(const float* interleaved, int n) {
        // Amortised compaction, forced whenever the incoming block would not
        // fit: dropping input here would be silent audio loss.
        compact(frames_ + (size_t)n > cap_);
        if (frames_ + (size_t)n > cap_) n = (int)(cap_ - frames_);   // only if setup() was undersized
        std::memcpy(&buf_[frames_ * 2], interleaved, sizeof(float) * 2 * (size_t)n);
        frames_ += (size_t)n;
    }

    // Input frames buffered ahead of the current read position.
    double bufferedIn() const { return (double)frames_ - pos_ - (identity_ ? 0.0 : half_); }

    int available() const {
        if (identity_) return (int)(frames_ - (size_t)pos_);
        // need floor(pos)+half_ < frames_
        const double last = (double)frames_ - half_ - 1;
        if (last < pos_) return 0;
        return (int)std::floor((last - pos_) / step_) + 1;
    }

    int pull(float* out, int n) {
        int produced = 0;
        if (identity_) {
            const int avail = (int)(frames_ - (size_t)pos_);
            produced = std::min(n, avail);
            std::memcpy(out, &buf_[(size_t)pos_ * 2], sizeof(float) * 2 * (size_t)produced);
            pos_ += produced;
            return produced;
        }
        const int taps = 2 * half_;
        while (produced < n) {
            const double fl = std::floor(pos_);
            const int ip = (int)fl;
            if (ip + half_ >= (int)frames_) break;
            const double frac = pos_ - fl;
            const double pf = frac * phases_;
            const int p0 = (int)pf;
            const float a = (float)(pf - p0);
            const float* k0 = &table_[(size_t)p0 * taps];
            const float* k1 = &table_[(size_t)(p0 + 1) * taps];
            const float* src = &buf_[(size_t)(ip - half_ + 1) * 2];
            float l = 0.f, r = 0.f;
            for (int t = 0; t < taps; ++t) {
                const float k = k0[t] + a * (k1[t] - k0[t]);
                l += k * src[2 * t];
                r += k * src[2 * t + 1];
            }
            out[2 * produced] = l;
            out[2 * produced + 1] = r;
            ++produced;
            pos_ += step_;
        }
        return produced;
    }

private:
    static double bessel0(double x) {
        double s = 1.0, t = 1.0;
        for (int k = 1; k < 50; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-12 * s) break; }
        return s;
    }
    void compact(bool force = false) {
        // Drop frames that can no longer be referenced.
        const long keepFrom = identity_ ? (long)std::floor(pos_)
                                        : (long)std::floor(pos_) - half_ + 1;
        if (keepFrom <= 0) return;
        const size_t drop = (size_t)std::min<long>(keepFrom, (long)frames_);
        if (!force && drop < cap_ / 4 && frames_ + 256 < cap_) return;   // amortise
        std::memmove(&buf_[0], &buf_[drop * 2], sizeof(float) * 2 * (frames_ - drop));
        frames_ -= drop;
        pos_ -= (double)drop;
    }

    double inRate_ = 44100, outRate_ = 44100, step_ = 1.0, pos_ = 0.0;
    bool identity_ = true;
    int half_ = 0, phases_ = 0;
    std::vector<float> table_, buf_;
    size_t cap_ = 0, frames_ = 0;
};

} // namespace schwung
