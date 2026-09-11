#pragma once
// Generalized over a channel count so the same class serves both the
// grayscale mono path (channels=1, one byte/pixel) and the C64 color
// path (channels=3, interleaved RGB) without duplicating the
// double-buffering/locking logic.
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

class FrameBuffer {
public:
    FrameBuffer(int w, int h, int channels = 1)
        : w_(w), h_(h), channels_(channels),
          back_((size_t)w * h * channels, 0), front_((size_t)w * h * channels, 0) {}

    // `pixels` must hold width()*channels() bytes (e.g. RGB-interleaved
    // for channels()==3), matching one scanline.
    void writeLine(int lineIndex, const std::vector<uint8_t>& pixels) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (lineIndex < 0 || lineIndex >= h_) return;
        size_t rowBytes = (size_t)w_ * channels_;
        std::memcpy(&back_[(size_t)lineIndex * rowBytes], pixels.data(),
                    std::min(rowBytes, pixels.size()));
    }

    void commitFrame() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::swap(back_, front_);
        newFrame_ = true;
    }

    void markLock(bool locked) {
        std::lock_guard<std::mutex> lk(mtx_);
        locked_ = locked;
    }

    // Returns true and fills `out` if a new frame is available since the
    // last call. `lockedOut` always reflects the latest lock state.
    bool grab(std::vector<uint8_t>& out, bool& lockedOut) {
        std::lock_guard<std::mutex> lk(mtx_);
        lockedOut = locked_;
        if (!newFrame_) return false;
        out = front_;
        newFrame_ = false;
        return true;
    }

    // Lightweight status check that doesn't touch the frame/new-frame state.
    bool isLocked() {
        std::lock_guard<std::mutex> lk(mtx_);
        return locked_;
    }

    int width() const { return w_; }
    int height() const { return h_; }
    int channels() const { return channels_; }

private:
    int w_, h_, channels_;
    std::vector<uint8_t> back_, front_;
    std::mutex mtx_;
    bool newFrame_ = false;
    bool locked_ = false;
};
