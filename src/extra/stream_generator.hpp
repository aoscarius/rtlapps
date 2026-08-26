#pragma once

#include "dsp/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <random>

enum class vEffect {
    Pong,
    Clock,
    Joke
};

class StreamGenerator {
public:
    StreamGenerator(const Config& cfg, uint64_t transmitterFrequencyHz, vEffect effect = vEffect::Pong)
        : cfg_(cfg),
          transmitterFrequencyHz_(transmitterFrequencyHz),
          currentEffect_(effect),
          rng_(0x504F4E47u),
          noise_(0.0f, 1.0f) {}

    void setTransmitterFrequency(uint64_t frequencyHz) {
        transmitterFrequencyHz_ = frequencyHz;
    }

    void setVisualEffect(vEffect effect) {
        currentEffect_ = effect;
    }

    std::vector<uint8_t> nextChunk(std::size_t sampleCount) {
        std::vector<uint8_t> iq(sampleCount * 2);

        const double sampleRate =
            static_cast<double>(cfg_.sample_rate);

        const long double samplesPerLine =
            static_cast<long double>(cfg_.sample_rate) *
            static_cast<long double>(cfg_.line_period_us) /
            1'000'000.0L;

        const int linesPerField =
            std::max(1, cfg_.lines_per_field);

        const double frequencyOffset =
            static_cast<double>(transmitterFrequencyHz_) -
            static_cast<double>(cfg_.center_freq);

        const double phaseStep =
            2.0 * kPi * frequencyOffset / sampleRate;

        // Simulated tuner/channel response.
        constexpr double channelBandwidthHz = 1'500'000.0;
        const double normalizedOffset =
            frequencyOffset / channelBandwidthHz;
        const float rfGain = static_cast<float>(
            std::exp(-0.5 * normalizedOffset * normalizedOffset));

        constexpr float noiseSigma = 1.5f;
        constexpr float carrierAmplitude = 110.0f;

        for (std::size_t n = 0; n < sampleCount; ++n) {
            const uint64_t absoluteSample = sampleIndex_++;

            // Fractional line timing: do not round this to an integer.
            const uint64_t lineNumber = static_cast<uint64_t>(
                std::floor(static_cast<long double>(absoluteSample) /
                           samplesPerLine));

            const uint64_t lineStart = static_cast<uint64_t>(
                std::floor(static_cast<long double>(lineNumber) *
                           samplesPerLine));

            const uint64_t lineEnd = static_cast<uint64_t>(
                std::floor(static_cast<long double>(lineNumber + 1) *
                           samplesPerLine));

            const uint64_t lineLength =
                std::max<uint64_t>(1, lineEnd - lineStart);

            const float t = static_cast<float>(
                absoluteSample - lineStart) /
                static_cast<float>(lineLength);

            const int line = static_cast<int>(
                lineNumber % static_cast<uint64_t>(linesPerField));

            float envelope = videoBlack();

            if (t < 0.075f) {
                envelope = syncTip();       // highest envelope
            } else if (t < 0.180f) {
                envelope = videoBlack();
            } else if (t < 0.920f) {
                const float x = (t - 0.180f) / 0.740f;
                const float y =
                    static_cast<float>(line) /
                    static_cast<float>(linesPerField);

                envelope = dispatchRender(x, y, lineNumber);
            }

            const double phase =
                phaseStep * static_cast<double>(absoluteSample);

            const float amplitude =
                carrierAmplitude * rfGain * envelope;

            const float i =
                127.5f + amplitude *
                static_cast<float>(std::cos(phase)) +
                noiseSigma * noise_(rng_);

            const float q =
                127.5f + amplitude *
                static_cast<float>(std::sin(phase)) +
                noiseSigma * noise_(rng_);

            iq[2 * n] = sampleByte(i);
            iq[2 * n + 1] = sampleByte(q);
        }

        return iq;
    }

private:
    static constexpr double kPi =
        3.14159265358979323846;

    static constexpr float videoBlack() { return 0.60f; }
    static constexpr float videoWhite() { return 0.025f; }
    static constexpr float syncTip() { return 0.95f; }
    static constexpr float carrierAmplitude() { return 120.0f; }

    static uint8_t sampleByte(float value) {
        return static_cast<uint8_t>(std::clamp(
            static_cast<int>(std::lround(value)), 0, 255));
    }

    float dispatchRender(float x, float y, uint64_t lineNumber) const {
        switch (currentEffect_) {
            case vEffect::Clock:
                return renderClock(x, y, lineNumber);
            case vEffect::Joke:
                return renderJoke(x, y, lineNumber);
            case vEffect::Pong:
            default:
                return renderPong(x, y, lineNumber);
        }
    }

    float renderPong(float x, float y, uint64_t lineNumber) const {
        if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return videoBlack();

        const float time = static_cast<float>(lineNumber) * static_cast<float>(cfg_.line_period_us) / 1'000'000.0f;

        const float ballX = 0.12f + 0.76f * std::abs(std::fmod(time * 0.23f, 2.0f) - 1.0f);
        const float ballY = 0.18f + 0.64f * std::abs(std::fmod(time * 0.31f, 2.0f) - 1.0f);
        const float leftY = 0.50f + 0.25f * std::sin(time * 1.7f);
        const float rightY = 0.50f + 0.25f * std::sin(time * 1.35f + 1.2f);

        const bool border = std::abs(x - 0.06f) < 0.012f || std::abs(x - 0.94f) < 0.012f ||
                            std::abs(y - 0.08f) < 0.012f || std::abs(y - 0.92f) < 0.012f;

        const bool centreLine = std::abs(x - 0.50f) < 0.006f && (static_cast<int>(y * 32.0f) % 2 == 0);
        const bool leftPaddle = x > 0.09f && x < 0.12f && std::abs(y - leftY) < 0.09f;
        const bool rightPaddle = x > 0.88f && x < 0.91f && std::abs(y - rightY) < 0.09f;
        const bool ball = std::abs(x - ballX) < 0.025f && std::abs(y - ballY) < 0.035f;

        return border || centreLine || leftPaddle || rightPaddle || ball ? videoWhite() : videoBlack();
    }

    float renderClock(float x, float y, uint64_t lineNumber) const {
        if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return videoBlack();

        const float time = static_cast<float>(lineNumber) * static_cast<float>(cfg_.line_period_us) / 1'000'000.0f;

        const float cx = x - 0.50f;
        const float cy = (y - 0.50f) * 0.75f; 
        const float distSq = cx * cx + cy * cy;
        const float radius = std::sqrt(distSq);
        const float angle = std::atan2(cy, cx);

        const bool outerRing = std::abs(radius - 0.35f) < 0.008f;

        bool hourTicks = false;
        if (radius > 0.30f && radius < 0.34f) {
            float tickAngle = angle + kPi / 2.0f; // Ruota di 90 gradi per far partire lo zero dall'alto
            if (tickAngle < 0.0f) tickAngle += 2.0f * kPi;
            
            float sector = (tickAngle / (2.0f * kPi)) * 12.0f;
            float sectorDist = std::abs(sector - std::round(sector));
            
            if (sectorDist < 0.04f) {
                hourTicks = true;
            }
        }

        const float speedMultiplier = 1.0f; 
        const float simulatedSeconds = time * speedMultiplier;
        
        const float angleSec = (kPi / 2.0f) - (simulatedSeconds * (2.0f * kPi / 60.0f));
        const float angleMin = (kPi / 2.0f) - ((simulatedSeconds / 60.0f) * (2.0f * kPi / 60.0f));
        const float angleHour = (kPi / 2.0f) - ((simulatedSeconds / 3600.0f) * (2.0f * kPi / 12.0f));

        auto isHand = [cx, cy, radius](float handAngle, float maxRadius, float thickness) -> bool {
            if (radius > maxRadius) return false;
            float distanceToLine = std::abs(cx * std::sin(handAngle) - cy * std::cos(handAngle));
            float dotProduct = cx * std::cos(handAngle) + cy * std::sin(handAngle);
            return distanceToLine < thickness && dotProduct > 0.0f;
        };

        const bool hourHand = isHand(angleHour, 0.18f, 0.012f);
        const bool minuteHand = isHand(angleMin, 0.28f, 0.008f);
        const bool secondHand = isHand(angleSec, 0.32f, 0.003f);
        const bool centerPin = distSq < 0.0002f;

        return outerRing || hourTicks || hourHand || minuteHand || secondHand || centerPin 
            ? videoWhite() 
            : videoBlack();
    }
 
    float renderJoke(float x, float y, uint64_t lineNumber) const {
        if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) return videoBlack();

        const float time = static_cast<float>(lineNumber) * static_cast<float>(cfg_.line_period_us) / 1'000'000.0f;
        const float glitchTrigger = std::sin(time * 4.5f) * std::cos(time * 12.0f);
        
        float distortedX = x;
        if (glitchTrigger > 0.4f && std::abs(y - std::fmod(time * 0.7f, 1.0f)) < 0.15f) {
            distortedX += std::sin(y * 150.0f) * 0.08f;
        }

        const int barIndex = static_cast<int>(distortedX * 7.0f);
        bool pixelOn = false;

        if (static_cast<int>(time) % 2 == 0) {
            pixelOn = (barIndex % 2 == 0) ^ (y > 0.7f);
        } else {
            const unsigned int pseudoRandom = static_cast<unsigned int>(distortedX * 800.0f) ^ 
                                             static_cast<unsigned int>(y * 600.0f) ^ 
                                             static_cast<unsigned int>(time * 100.0f);
            pixelOn = (pseudoRandom % 7) == 0;
        }
        const float scanlineDrop = std::fmod(time * 1.5f, 1.1f);
        if (std::abs(y - scanlineDrop) < 0.02f) {
            return videoBlack();
        }
        
        return pixelOn ? videoWhite() : videoBlack();
    }

    const Config& cfg_;
    uint64_t transmitterFrequencyHz_;
    vEffect currentEffect_;
    uint64_t sampleIndex_ = 0;
    std::mt19937 rng_;
    std::normal_distribution<float> noise_;
};