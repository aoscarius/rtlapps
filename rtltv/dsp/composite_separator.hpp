#pragma once
// CompositeColorSeparator: a genuine (if simplified) composite color
// decoder for sources that actually carry chroma -- e.g. a Commodore
// 64's RF modulator output -- unlike the monochrome "Pong on a chip"
// signal SyncSeparator targets. Activated via --c64; works for BOTH
// NTSC and PAL, selected the same way the mono decoder already picks a
// standard: from lines_per_field (see isNtscTiming() in config.hpp),
// i.e. whatever --ntsc/--pal set. There's no PAL-specific assumption
// baked in here anymore -- the only two things that actually differ
// between the standards are handled explicitly below (subcarrier
// frequency, and PAL's V-switch not existing in NTSC).
//
// ============================================================================
// READ THIS FIRST: sample rate is not optional here.
// ============================================================================
// The color subcarrier sits well above the video carrier -- 3.579545 MHz
// for NTSC, 4.43361875 MHz for PAL. To represent either without aliasing
// you need roughly sample_rate > 2*(subcarrier + half the chroma
// bandwidth), which works out to about 8 MS/s (NTSC) to 10 MS/s (PAL).
// At the 3.2 MS/s this project's mono path uses by default, the
// subcarrier is simply outside the captured bandwidth -- there is no
// amount of clever DSP that recovers a signal that was never sampled.
// --c64 therefore raises the default --rate to 10'000'000 (safely covers
// both standards) unless you override it explicitly. Whether an RTL-SDR
// Blog V4 sustains that reliably over your particular USB link is
// genuinely hardware/host dependent -- this code does the
// mathematically correct thing at whatever rate it's actually given
// (gracefully degrading, not crashing, if you're under-sampled), but it
// can't manufacture bandwidth that wasn't captured.
//
// ============================================================================
// What's implemented, and what's deliberately left out
// ============================================================================
// Implemented: per-line color-burst detection via correlation against a
// local oscillator at the selected standard's subcarrier frequency;
// PAL's V-switch (line-alternating chroma phase), resolved
// *differentially* between consecutive bursts (robust to an unknown
// constant phase offset -- it doesn't need to know "true" broadcast
// phase, only that consecutive lines swing by +/-45 degrees around a
// common axis); synchronous quadrature chroma demod referenced to that
// per-line burst phase; a luma low-pass tuned to notch out the
// subcarrier; and a standard YUV->RGB matrix.
//
// NTSC has no V-switch -- its burst phase is constant line to line, not
// alternating. ntscMode() below explicitly disables the V-flip in that
// case rather than relying on the differential math "naturally" landing
// on no-op (which it would, in the noise-free case: two identical burst
// vectors summed just reinforce the same axis with double magnitude,
// giving a swing of exactly 0). The problem is *real* noise: with the
// flip left enabled, a swing that's supposed to sit at exactly 0 will
// jitter across 0 and randomly flip V line-to-line, adding visible
// chroma noise that a real NTSC decoder (which never even looks for a
// switch) wouldn't have. So the flip is a PAL-only code path, not just
// a PAL-only signal property.
//
// NTSC broadcast technically uses I/Q axes (rotated ~33 degrees from
// U/V) rather than PAL's U/V directly, weighted for human color-error
// sensitivity. This decoder uses the same U/V-style matrix for both
// standards rather than a separate I/Q rotation -- the practical effect
// is a fixed extra hue offset on NTSC sources, which the HUE menu
// control (see TunableParams::hue_trim_deg) absorbs the same way it
// already has to absorb this decoder's unknown constant phase offset
// on any real capture. One manual trim covers both causes.
//
// Not implemented (either standard): NONE of the classic 1-line delay
// comb filter is skipped anymore -- see below -- but be aware of what it
// does and doesn't fix. It averages this line's (u,v) with the same
// column's (u,v) from the immediately preceding line, which cancels
// residual that *flips* between adjacent lines (this is exactly what
// PAL's V-switch guarantees for V-linked cross-talk into U, and vice
// versa) and gives a modest noise/SNR improvement from the averaging
// itself. It does NOT cancel a channel's own self-generated harmonic
// distortion from an imprecise box-filter notch (the same U value
// produces the same self-residual on every line, comb-averaging or not)
// -- if you're seeing visible ripple *within* a single saturated color
// region rather than at edges/cross-talk, the fix for that is a longer/
// better-designed low-pass on U,V after demod, or more sample rate (see
// the top of this file), not this filter.

#include "dsp/config.hpp"
#include "dsp/frame_buffer.hpp"
#include "dsp/tunable_params.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <vector>

class CompositeColorSeparator {
public:
    static constexpr double kSubcarrierHzPal  = 4'433'618.75;
    static constexpr double kSubcarrierHzNtsc = 3'579'545.0;

    CompositeColorSeparator(const Config& cfg, TunableParams& params, FrameBuffer& fb)
        : cfg_(cfg), params_(params), fb_(fb),
          ntsc_(isNtscTiming(cfg)),
          subcarrierHz_(ntsc_ ? kSubcarrierHzNtsc : kSubcarrierHzPal) {
        lineBuf_.reserve(4096);
    }

    void process(const std::vector<float>& env) {
        float threshold = params_.sync_threshold_frac.load(std::memory_order_relaxed);
        for (float v : env) {
            updateMinMax(v);
            float range = std::max(envMax_ - envMin_, 1e-6f);
            float norm = (v - envMin_) / range;

            bool isSyncNow = norm > threshold;
            if (isSyncNow && !inSync_) onLineBoundary();
            inSync_ = isSyncNow;

            // Keep the *raw* (non-normalized) sample for chroma/luma
            // work -- normalizing per-sample the way the sync detector
            // does would distort the AM envelope's actual amplitude
            // ratios, which is exactly what color demodulation depends on.
            lineBuf_.push_back(v);
        }
    }

    bool locked() const { return locked_; }

private:
    void updateMinMax(float v) {
        const float alpha = 0.0005f;
        if (!haveRange_) { envMin_ = v; envMax_ = v; haveRange_ = true; return; }
        if (v < envMin_) envMin_ = v; else envMin_ += alpha * (v - envMin_);
        if (v > envMax_) envMax_ = v; else envMax_ += alpha * (v - envMax_);
    }

    void onLineBoundary() {
        size_t len = lineBuf_.size();
        if (!lineBuf_.empty()) emitLine(lineBuf_);
        lineBuf_.clear();
        updateLockHeuristic(len);
    }

    void updateLockHeuristic(size_t lastLineLen) {
        if (lastLineLen == 0) return;
        recentLens_.push_back(lastLineLen);
        if (recentLens_.size() > 30) recentLens_.pop_front();
        if (recentLens_.size() < 10) { locked_ = false; return; }

        double mean = 0;
        for (auto l : recentLens_) mean += (double)l;
        mean /= recentLens_.size();
        double var = 0;
        for (auto l : recentLens_) { double d = (double)l - mean; var += d * d; }
        var /= recentLens_.size();
        double relStdDev = std::sqrt(var) / std::max(mean, 1.0);

        locked_ = relStdDev < 0.08;
        fb_.markLock(locked_);
    }

    // Correlates `samples[from,to)` against a local oscillator at
    // subcarrierHz_ to get that window's (I,Q) burst/chroma vector.
    // The oscillator's phase 0 is fixed at the start of the current
    // line (sample index 0 within `samples`).
    std::complex<double> correlateAtSubcarrier(const std::vector<float>& samples,
                                                size_t from, size_t to,
                                                double sampleRate) const {
        double sumI = 0.0, sumQ = 0.0;
        for (size_t k = from; k < to && k < samples.size(); ++k) {
            double t = (double)k / sampleRate;
            double phase = 2.0 * kPi * subcarrierHz_ * t;
            sumI += samples[k] * std::cos(phase);
            sumQ += samples[k] * std::sin(phase);
        }
        size_t n = (to > from) ? (to - from) : 1;
        return std::complex<double>(sumI / (double)n, sumQ / (double)n);
    }

    void emitLine(const std::vector<float>& samples) {
        size_t n = samples.size();
        if (n < 16) return;

        const double sampleRate = (double)cfg_.sample_rate;

        // Same horizontal alignment controls as the mono decoder (menu:
        // H-POS/H-WIDTH), applied identically so both paths respond the
        // same way to those controls.
        float shift = std::clamp(params_.h_shift_frac.load(std::memory_order_relaxed), 0.0f, 0.45f);
        float scale = std::clamp(params_.h_scale.load(std::memory_order_relaxed), 0.3f, 2.0f);
        size_t start = (size_t)(shift * n);
        size_t maxLen = n > start ? n - start : 0;
        if (maxLen < 8) return;
        size_t activeLen = (size_t)std::clamp((float)maxLen * scale, 8.0f, (float)maxLen);
        size_t activeEnd = std::min(n, start + activeLen);

        // --- Color burst: a short window inside the back porch/breezeway
        // region, i.e. after sync but clearly before active picture
        // starts. Expressed as a fraction of the *whole* line (not the
        // shifted active region) since the burst always sits right after
        // sync regardless of how H-POS/H-WIDTH are tuned for the picture.
        // NTSC and PAL line periods differ by under 1% (63.5556us vs
        // 64us), so reusing the same fractional window for both is a
        // reasonable simplification rather than a broadcast-precise
        // per-standard burst gate. ---
        size_t burstFrom = (size_t)(0.140 * n);
        size_t burstTo   = (size_t)(0.175 * n);
        if (burstTo <= burstFrom || burstTo > n) { emitFallbackGray(samples, start, activeEnd); return; }

        std::complex<double> burst = correlateAtSubcarrier(samples, burstFrom, burstTo, sampleRate);
        double burstMag = std::abs(burst);

        if (burstMag < 1e-4) {
            // No usable burst (unlocked/noisy/no chroma present at all --
            // e.g. this decoder pointed at a monochrome source). Fall
            // back to a plain grayscale line rather than emitting
            // meaningless color noise.
            haveLastBurst_ = false;
            prevLineValid_ = false; // no continuous chroma to comb against next time
            emitFallbackGray(samples, start, activeEnd);
            return;
        }

        // Differential axis estimate: two consecutive unit burst vectors
        // summed point along their common reference axis regardless of
        // any constant phase offset our arbitrary local oscillator has
        // relative to the real transmitter -- true whether or not that
        // axis is swinging (PAL) or fixed (NTSC). Also acts as a cheap
        // 2-line running average of the reference either way.
        std::complex<double> axisEstimate = burst;
        if (haveLastBurst_ && std::abs(lastBurst_) > 1e-6) {
            std::complex<double> u1 = burst / burstMag;
            std::complex<double> u2 = lastBurst_ / std::abs(lastBurst_);
            std::complex<double> sum = u1 + u2;
            if (std::abs(sum) > 1e-6) axisEstimate = sum;
        }
        double refPhase = std::arg(axisEstimate);

        // PAL only: which side of the axis is *this* line's burst on?
        // That sign is the V-switch state we need to undo below. NTSC
        // has no switch -- forcing vSign to +1 avoids letting sampling
        // noise (which would otherwise make a swing that's "supposed"
        // to sit at exactly 0 jitter across 0) randomly flip V line to
        // line on a standard that never had a flip to begin with.
        float vSign = 1.0f;
        if (!ntsc_) {
            double swing = std::arg(burst * std::polar(1.0, -refPhase));
            vSign = (swing >= 0.0) ? 1.0f : -1.0f;
        }

        lastBurst_ = burst;
        haveLastBurst_ = true;

        // --- Luma: box filter with a window length equal to one
        // subcarrier period. A moving average's frequency response has
        // nulls at multiples of 1/window_duration, so sizing the window
        // to exactly one subcarrier cycle puts a null right at
        // subcarrierHz_ -- a cheap, classic trick to keep most of the
        // chroma energy out of the luma channel without a dedicated
        // low-pass filter design. ---
        size_t lumaWindow = std::max<size_t>(1, (size_t)std::llround(sampleRate / subcarrierHz_));
        // Chroma demod uses a wider box filter (several subcarrier
        // cycles) to suppress the 2x-subcarrier image term that
        // synchronous AM demodulation produces alongside the wanted
        // baseband U/V. This helps but doesn't fully solve it at
        // typical sample rates: near Nyquist (e.g. 10 MS/s, ~2.3
        // samples/subcarrier-cycle for PAL), part of that image term
        // *aliases* onto the same frequency as the wanted signal, and
        // no amount of post-sampling filtering can separate two things
        // that already overlap in frequency -- only a higher sample
        // rate prevents the aliasing in the first place (see the top of
        // this file). Empirically, 4 cycles cuts residual ripple by
        // roughly a third at 10 MS/s without blurring adjacent color
        // regions together; wider than that gives rapidly diminishing
        // returns since the *aliased* portion doesn't shrink no matter
        // how long the filter is.
        size_t chromaWindow = std::max<size_t>(2, lumaWindow * 4);

        bool invertLuma = params_.invert.load(std::memory_order_relaxed);
        int lpf = std::max(1, params_.lines_per_field.load(std::memory_order_relaxed));
        int vShift = params_.v_shift.load(std::memory_order_relaxed);
        int outLine = ((curLine_ + vShift) % lpf + lpf) % lpf;

        // 1-line delay comb filter setup: no vertically-adjacent
        // previous line exists at the top of a field, and the buffer
        // needs (re)sizing if this is the first line ever or out_width
        // changed.
        if (curLine_ == 0) prevLineValid_ = false;
        if (prevLineU_.size() != (size_t)cfg_.out_width) {
            prevLineU_.assign(cfg_.out_width, 0.0f);
            prevLineV_.assign(cfg_.out_width, 0.0f);
            prevLineValid_ = false;
        }

        std::vector<uint8_t> rgb((size_t)cfg_.out_width * 3);
        float range = std::max(envMax_ - envMin_, 1e-6f);

        for (int px = 0; px < cfg_.out_width; ++px) {
            double t = (double)px / (cfg_.out_width - 1);
            size_t span = (activeEnd > start) ? (activeEnd - start - 1) : 0;
            size_t centerIdx = start + (size_t)(t * span);
            if (centerIdx >= n) centerIdx = n - 1;

            // Luma: average over the notch window centered on this pixel.
            size_t lumaFrom = centerIdx > lumaWindow / 2 ? centerIdx - lumaWindow / 2 : 0;
            size_t lumaTo = std::min(n, lumaFrom + lumaWindow);
            double lumaSum = 0.0;
            for (size_t k = lumaFrom; k < lumaTo; ++k) lumaSum += samples[k];
            double lumaRaw = lumaSum / std::max<size_t>(1, lumaTo - lumaFrom);

            // Chroma: synchronous quadrature demod referenced to this
            // line's burst-derived axis, box-filtered to baseband.
            size_t chromaFrom = centerIdx > chromaWindow / 2 ? centerIdx - chromaWindow / 2 : 0;
            size_t chromaTo = std::min(n, chromaFrom + chromaWindow);
            double sumU = 0.0, sumV = 0.0;
            for (size_t k = chromaFrom; k < chromaTo; ++k) {
                double tk = (double)k / sampleRate;
                double phase = 2.0 * kPi * subcarrierHz_ * tk - refPhase;
                sumU += 2.0 * samples[k] * std::cos(phase);
                sumV += 2.0 * samples[k] * std::sin(phase);
            }
            size_t cn = std::max<size_t>(1, chromaTo - chromaFrom);
            // Only V flips with vSign (forced to +1 above on NTSC); U's
            // sign is always constant, matching real PAL's convention.
            double u = (sumU / cn);
            double v = (sumV / cn) * vSign;

            // Manual hue correction (see TunableParams::hue_trim_deg):
            // rotate the recovered (u,v) vector by a user-set angle to
            // compensate both this decoder's unknown constant phase
            // offset (any real capture) and, on NTSC, the fixed ~33
            // degree I/Q-vs-U/V axis difference this decoder doesn't
            // model separately (see file header).
            float hueDeg = params_.hue_trim_deg.load(std::memory_order_relaxed);
            if (hueDeg != 0.0f) {
                double hueRad = hueDeg * kPi / 180.0;
                double ch = std::cos(hueRad), sh = std::sin(hueRad);
                double ru = u * ch - v * sh;
                double rv = u * sh + v * ch;
                u = ru;
                v = rv;
            }

            // 1-line delay comb filter: average this line's (u,v) with
            // the same pixel column's (u,v) from the immediately
            // preceding line, the standard technique real PAL/NTSC
            // decoders use to cancel residual cross-luma/cross-color
            // rather than a fixed-frequency notch. Only the *previous*
            // raw (pre-comb) line is kept -- this is a simple two-tap
            // comb, not a recursive/cascading average.
            {
                float uRaw = (float)u, vRaw = (float)v;
                if (prevLineValid_) {
                    u = (u + prevLineU_[px]) * 0.5;
                    v = (v + prevLineV_[px]) * 0.5;
                }
                prevLineU_[px] = uRaw;
                prevLineV_[px] = vRaw;
            }

            // Normalize luma the same way the mono decoder does (adaptive
            // envelope min/max), then apply a standard YUV->RGB matrix.
            // U/V are left in raw correlation units scaled by a
            // hand-picked gain (kChromaGain) rather than a physically
            // calibrated one -- there's no absolute reference for "100%
            // saturation" on an arbitrary composite source, so if colors
            // look over/under-saturated, kChromaGain is the knob.
            double yNorm = (lumaRaw - envMin_) / range;   // 0..1, high = sync/black
            double y = 1.0 - yNorm;                        // invert: low carrier = bright
            if (invertLuma) y = 1.0 - y;
            y = std::clamp(y, 0.0, 1.0) * 255.0;

            constexpr double kChromaGain = 2.2;
            double uu = u * kChromaGain;
            double vv = v * kChromaGain;

            double r = y + 1.140 * vv;
            double g = y - 0.395 * uu - 0.581 * vv;
            double b = y + 2.032 * uu;

            rgb[(size_t)px * 3 + 0] = clampByte(r);
            rgb[(size_t)px * 3 + 1] = clampByte(g);
            rgb[(size_t)px * 3 + 2] = clampByte(b);
        }
        prevLineValid_ = true; // this line's (u,v) are now in prevLineU_/V_ for the next line

        fb_.writeLine(outLine, rgb);
        curLine_++;
        if (curLine_ >= lpf) {
            curLine_ = 0;
            fb_.commitFrame();
        }
    }

    // Used when a line has no usable burst (unlocked, or a genuinely
    // monochrome source pointed at the color decoder by mistake): emit
    // a plain gray line so the picture stays a recognizable B&W image
    // instead of colored noise.
    void emitFallbackGray(const std::vector<float>& samples, size_t start, size_t end) {
        int lpf = std::max(1, params_.lines_per_field.load(std::memory_order_relaxed));
        int vShift = params_.v_shift.load(std::memory_order_relaxed);
        int outLine = ((curLine_ + vShift) % lpf + lpf) % lpf;
        bool invertLuma = params_.invert.load(std::memory_order_relaxed);
        float range = std::max(envMax_ - envMin_, 1e-6f);

        std::vector<uint8_t> rgb((size_t)cfg_.out_width * 3);
        size_t len = end > start ? end - start : 1;
        for (int px = 0; px < cfg_.out_width; ++px) {
            double t = (double)px / (cfg_.out_width - 1);
            size_t idx = start + (size_t)(t * (len - 1));
            if (idx >= samples.size()) idx = samples.empty() ? 0 : samples.size() - 1;
            double norm = samples.empty() ? 0.0 : (samples[idx] - envMin_) / range;
            double y = 1.0 - norm;
            if (invertLuma) y = 1.0 - y;
            uint8_t g = clampByte(std::clamp(y, 0.0, 1.0) * 255.0);
            rgb[(size_t)px * 3 + 0] = g;
            rgb[(size_t)px * 3 + 1] = g;
            rgb[(size_t)px * 3 + 2] = g;
        }
        fb_.writeLine(outLine, rgb);
        curLine_++;
        if (curLine_ >= lpf) {
            curLine_ = 0;
            fb_.commitFrame();
        }
    }

    static uint8_t clampByte(double v) {
        return (uint8_t)std::clamp(v, 0.0, 255.0);
    }

    static constexpr double kPi = 3.14159265358979323846;

    const Config& cfg_;
    TunableParams& params_;
    FrameBuffer& fb_;

    const bool ntsc_;
    const double subcarrierHz_;

    std::vector<float> lineBuf_;
    bool inSync_ = false;
    bool haveRange_ = false;
    float envMin_ = 0, envMax_ = 0;
    int curLine_ = 0;

    std::complex<double> lastBurst_{0.0, 0.0};
    bool haveLastBurst_ = false;

    std::deque<size_t> recentLens_;
    bool locked_ = false;

    // 1-line delay comb filter state: the previous line's per-pixel
    // (u,v) *after* V-switch correction (i.e. exactly what that line
    // used for its own R,G,B matrix), so this line can average against
    // it the same way a real hardware delay-line PAL/NTSC decoder
    // combines the current and previous line's chroma. Invalidated at
    // the start of each field (line 0) and whenever lock is lost, since
    // neither case has a *vertically adjacent* previous line to combine
    // with.
    std::vector<float> prevLineU_;
    std::vector<float> prevLineV_;
    bool prevLineValid_ = false;
};
