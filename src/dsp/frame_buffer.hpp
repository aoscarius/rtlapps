#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

class FrameBuffer {
public:
    FrameBuffer(int w, int h) : w_(w), h_(h), back_(w * h, 0), front_(w * h, 0) {}

    void writeLine(int lineIndex, const std::vector<uint8_t>& pixels) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (lineIndex < 0 || lineIndex >= h_) return;
        std::memcpy(&back_[(size_t)lineIndex * w_], pixels.data(), std::min((size_t)w_, pixels.size()));
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

private:
    int w_, h_;
    std::vector<uint8_t> back_, front_;
    std::mutex mtx_;
    bool newFrame_ = false;
    bool locked_ = false;
};
