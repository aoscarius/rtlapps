#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct RawChunk { std::vector<uint8_t> data; };

class RawQueue {
public:
    void push(std::vector<uint8_t>&& data) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.size() > 64) q_.pop_front(); // drop oldest if consumer falls behind
        q_.push_back(RawChunk{std::move(data)});
        cv_.notify_one();
    }
    bool pop(RawChunk& out, std::atomic<bool>& running) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&] { return !q_.empty() || !running.load(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
private:
    std::deque<RawChunk> q_;
    std::mutex mtx_;
    std::condition_variable cv_;
};
