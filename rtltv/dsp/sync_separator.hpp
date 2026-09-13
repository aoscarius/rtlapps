#pragma once
// Consumes AM-envelope samples, finds horizontal sync pulse edges, and
// resamples the active-picture portion of each line into the shared
// FrameBuffer. A field boundary is declared every `lines_per_field`
// lines -- a deliberate simplification (no true vsync serration
// detection); see the project README for the honest limitations.
//
// All the fields a person would want to nudge live while chasing lock
// (sync threshold, polarity, field length, horizontal position/width,
// vertical shift) live in TunableParams and are read here as atomics,
// since the menu/keyboard shortcuts write them from the main thread
// while this class runs on the processing thread.
//
// Also tracks a rough "locked" state from how consistent recent line
// lengths are, purely to drive the OSD status text and the on-screen
// snow effect when nothing sensible is being received.

#include "dsp/config.hpp"
#include "dsp/frame_buffer.hpp"
#include "dsp/tunable_params.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

class SyncSeparator {
public:
    SyncSeparator(const Config& cfg, TunableParams& params, FrameBuffer& fb)
        : cfg_(cfg), params_(params), fb_(fb) {
        lineBuf_.reserve(4096);
    }

    void process(const std::vector<float>& env) {
        float threshold = params_.sync_threshold_frac.load(std::memory_order_relaxed);
        for (float v : env) {
            updateMinMax(v);
            float range = std::max(envMax_ - envMin_, 1e-6f);
            float norm = (v - envMin_) / range; // 0..1, higher = sync/black

            bool isSyncNow = norm > threshold;
            if (isSyncNow && !inSync_) onLineBoundary();
            inSync_ = isSyncNow;

            lineBuf_.push_back(norm);
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

    // Locked == recent line lengths are consistent (low relative jitter).
    // That's a decent proxy for "actually seeing a real horizontal sync
    // train" versus noise triggering the threshold randomly.
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

        locked_ = relStdDev < 1.00; // <100% line-to-line jitter looks like a real sync train
        fb_.markLock(locked_);
    }

    void emitLine(const std::vector<float>& samples, float s_gain = 4.0f) {
        size_t n = samples.size();
        if (n < 8) return;

        // Horizontal alignment: h_shift_frac skips sync tip + back porch
        // (and lets the user nudge left/right); h_scale trims how much
        // of what's left counts as "active picture" (zoom in/out to fix
        // a picture that looks too wide/narrow).
        float shift = std::clamp(params_.h_shift_frac.load(std::memory_order_relaxed), 0.0f, 0.45f);
        float scale = std::clamp(params_.h_scale.load(std::memory_order_relaxed), 0.3f, 2.0f);

        size_t start = (size_t)(shift * n);
        size_t maxLen = n > start ? n - start : 0;
        if (maxLen < 4) return;
        size_t activeLen = (size_t)std::clamp((float)maxLen * scale, 4.0f, (float)maxLen);

        bool invert = params_.invert.load(std::memory_order_relaxed);

        std::vector<uint8_t> px(cfg_.out_width);
        for (int x = 0; x < cfg_.out_width; ++x) {
            double t = (double)x / (cfg_.out_width - 1);
            size_t idx = start + (size_t)(t * (activeLen - 1));
            if (idx >= n) idx = n - 1;
            float norm = samples[idx] * s_gain;
            float brightness = 1.0f - norm; // invert: low carrier = bright
            if (invert) brightness = 1.0f - brightness;
            brightness = std::clamp(brightness, 0.0f, 1.0f);
            px[x] = (uint8_t)(brightness * 255.0f);
        }

        // Vertical alignment: v_shift rotates which output row this
        // scanline lands on, independent of the lines_per_field lock.
        int lpf = std::max(1, params_.lines_per_field.load(std::memory_order_relaxed));
        int vShift = params_.v_shift.load(std::memory_order_relaxed);
        int line = ((curLine_ + vShift) % lpf + lpf) % lpf;

        fb_.writeLine(line, px);
        curLine_++;
        if (curLine_ >= lpf) {
            curLine_ = 0;
            fb_.commitFrame();
        }
    }

    const Config& cfg_;
    TunableParams& params_;
    FrameBuffer& fb_;
    std::vector<float> lineBuf_;
    bool inSync_ = false;
    bool haveRange_ = false;
    float envMin_ = 0, envMax_ = 0;
    int curLine_ = 0;

    std::deque<size_t> recentLens_;
    bool locked_ = false;
};
