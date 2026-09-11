#pragma once
// CompositeColorBarsGenerator: synthesizes a fake RTL-SDR IQ stream
// carrying classic 8-bar color bars (white/yellow/cyan/green/magenta/
// red/blue/black), correctly modulated for whichever standard the
// Config selects (NTSC or PAL -- see isNtscTiming() in config.hpp),
// so CompositeColorSeparator can be validated end-to-end against known
// ground-truth colors on *both* standards without any real hardware.
// Not used by the normal --dry-run path (that stays on StreamGenerator's
// Pong/Clock/Joke simulation); wired in only for --dry-run --c64, where
// a Pong game loop wouldn't mean anything anyway.
//
// This generator and CompositeColorSeparator are two independent
// implementations of the same standard(s) -- not mirror images of each
// other -- so a passing round-trip test here is actually checking
// standard-compliance, not just self-consistency. See
// composite_separator.hpp's header comment for the modulation
// convention both sides agree on: PAL flips V's sign every line (U is
// constant), NTSC flips neither.

#include "dsp/composite_separator.hpp" // for kSubcarrierHzPal/Ntsc
#include "dsp/config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

class CompositeColorBarsGenerator {
public:
    struct Bar { float y, u, v; }; // ground-truth YUV per bar, for tests to check against

    CompositeColorBarsGenerator(const Config& cfg, uint64_t transmitterFrequencyHz)
        : cfg_(cfg), transmitterFrequencyHz_(transmitterFrequencyHz), rng_(0xC64u),
          ntsc_(isNtscTiming(cfg)),
          subcarrierHz_(ntsc_ ? CompositeColorSeparator::kSubcarrierHzNtsc
                               : CompositeColorSeparator::kSubcarrierHzPal) {}

    void setTransmitterFrequency(uint64_t frequencyHz) { transmitterFrequencyHz_ = frequencyHz; }

    // Standard 100%-amplitude 8-bar YUV values (Rec.601-ish coefficients),
    // white to black, left to right -- exposed so tests can check the
    // decoder recovers each bar's hue correctly.
    static const std::array<Bar, 8>& referenceBars() {
        static const std::array<Bar, 8> bars = {{
            {1.000f,  0.000f,  0.000f},  // white
            {0.886f, -0.436f,  0.100f},  // yellow
            {0.701f,  0.147f, -0.615f},  // cyan
            {0.587f, -0.289f, -0.515f},  // green
            {0.413f,  0.289f,  0.515f},  // magenta
            {0.299f, -0.147f,  0.615f},  // red
            {0.114f,  0.436f, -0.100f},  // blue
            {0.000f,  0.000f,  0.000f},  // black
        }};
        return bars;
    }

    std::vector<uint8_t> nextChunk(std::size_t sampleCount) {
        std::vector<uint8_t> iq(sampleCount * 2);

        const double sampleRate = static_cast<double>(cfg_.sample_rate);
        const std::size_t lineSamples = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::llround(
                sampleRate * static_cast<double>(cfg_.line_period_us) / 1'000'000.0)));

        const double offsetHz =
            static_cast<double>(transmitterFrequencyHz_) - static_cast<double>(cfg_.center_freq);
        const double phaseStep = 2.0 * kPi * offsetHz / sampleRate;
        const double rolloffHz = sampleRate * 0.18;
        const float selectivity = static_cast<float>(
            std::exp(-(offsetHz * offsetHz) / (2.0 * rolloffHz * rolloffHz)));

        std::normal_distribution<float> noise(0.0f, kNoiseAmplitude);
        const auto& bars = referenceBars();

        for (std::size_t n = 0; n < sampleCount; ++n) {
            const uint64_t absoluteSample = sampleIndex_++;
            const uint64_t lineNumber = absoluteSample / lineSamples;
            const std::size_t position = static_cast<std::size_t>(absoluteSample % lineSamples);
            const float t = static_cast<float>(position) / static_cast<float>(lineSamples);

            // PAL only: V-switch alternates every line. Sign convention
            // only has to agree with CompositeColorSeparator's, which
            // treats whichever line the burst happens to swing "+45 deg"
            // on as vSign=+1 -- an arbitrary but consistent starting
            // point. NTSC has no switch: vSign is always +1 and the
            // burst axis is transmitted at a fixed (here: 0) phase every
            // line, matching the decoder's ntsc_ branch which never
            // looks for a swing at all.
            const float vSign = ntsc_ ? 1.0f : ((lineNumber % 2 == 0) ? +1.0f : -1.0f);
            // Reference axis is nominally 180 (NTSC) or 135 (PAL) degrees
            // from the U axis in real broadcast; the exact value doesn't
            // matter for our differential decoder (see
            // composite_separator.hpp), we just need *a* fixed
            // axis. Using 0 here keeps the synthetic math simple --
            // there's no cable/ADC delay in a simulation to introduce an
            // unknown offset the way there would be on a real capture,
            // so hue_trim_deg==0 should decode correctly on either
            // standard.
            constexpr double axis = 0.0;
            const double lineTheta = ntsc_ ? axis : (axis + vSign * kSwingRad);

            double tLine = static_cast<double>(position) / sampleRate;
            double subcarrierPhase = 2.0 * kPi * subcarrierHz_ * tLine;

            float envelope = videoBlack();

            if (t < 0.075f) {
                envelope = syncTip();
            } else if (t < 0.140f) {
                envelope = videoBlack();
            } else if (t < 0.175f) {
                // Color burst: ~10 cycles at (axis [+ line's swing on
                // PAL]), same window CompositeColorSeparator correlates
                // against.
                envelope = videoBlack() +
                    kBurstAmplitude * static_cast<float>(std::cos(subcarrierPhase - lineTheta));
            } else if (t < 0.920f) {
                const float x = (t - 0.180f) / 0.740f;
                if (x < 0.0f || x > 1.0f) {
                    envelope = videoBlack();
                } else {
                    int barIdx = std::min<int>(7, static_cast<int>(x * 8.0f));
                    const Bar& bar = bars[barIdx];
                    float lumaEnv = videoBlack() - bar.y * (videoBlack() - videoWhite());
                    // Only V (the sin-referenced component) flips sign
                    // with vSign on PAL (vSign is always +1 on NTSC); U
                    // (cos-referenced) never flips -- this must match
                    // CompositeColorSeparator's demod convention.
                    float chroma =
                        bar.u * static_cast<float>(std::cos(subcarrierPhase - axis)) +
                        vSign * bar.v * static_cast<float>(std::sin(subcarrierPhase - axis));
                    envelope = lumaEnv + kChromaAmplitude * chroma;
                }
            } else {
                envelope = videoBlack();
            }

            const double phase = phaseStep * static_cast<double>(absoluteSample);
            const float amplitude = carrierAmplitude() * envelope * selectivity;

            const float i = 127.5f + amplitude * static_cast<float>(std::cos(phase)) + noise(rng_);
            const float q = 127.5f + amplitude * static_cast<float>(std::sin(phase)) + noise(rng_);
            iq[2 * n]     = sampleByte(i);
            iq[2 * n + 1] = sampleByte(q);
        }
        return iq;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kSwingRad = 45.0 * kPi / 180.0;

    static constexpr float videoWhite() { return 0.05f; }
    static constexpr float videoBlack() { return 0.45f; }
    static constexpr float syncTip()    { return 1.00f; }
    static constexpr float carrierAmplitude() { return 120.0f; }
    static constexpr float kNoiseAmplitude = 1.0f; // lighter than the Pong generator: this
                                                    // signal is for precise color validation
    static constexpr float kBurstAmplitude = 0.12f;
    static constexpr float kChromaAmplitude = 0.16f;

    static uint8_t sampleByte(float value) {
        return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
    }

    const Config& cfg_;
    uint64_t transmitterFrequencyHz_;
    uint64_t sampleIndex_ = 0;
    std::mt19937 rng_;
    const bool ntsc_;
    const double subcarrierHz_;
};
