#pragma once
// FileRecorder: writes raw IQ bytes to disk as they're produced by
// whichever source thread is active (real RTL-SDR callback, the
// --dry-run synthetic generator, or even a --play-file source, if
// someone wants to re-save/trim a capture). Same interleaved unsigned
// 8-bit I/Q format the real rtl_sdr command-line tool writes, so files
// recorded here are also usable directly in GNU Radio, other SDR
// tools, or re-played with this program's own --play-file.
//
// Thread-safety: write() is called from whatever thread owns the
// signal source (the RTL-SDR async callback thread for live capture,
// or a dedicated source thread for --dry-run/--play-file), which is
// not the main thread -- hence the mutex, even though there's only
// ever one writer at a time in practice.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

class FileRecorder {
public:
    // maxSeconds <= 0 means "record until the program exits or stop()
    // is called" (no automatic cutoff).
    bool open(const std::string& path, uint32_t sampleRate, double maxSeconds) {
        std::lock_guard<std::mutex> lk(mtx_);
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_.is_open()) return false;
        path_ = path;
        sampleRate_ = sampleRate;
        maxBytes_ = (maxSeconds > 0.0)
            ? (uint64_t)(maxSeconds * sampleRate * 2.0 /*I+Q bytes per sample*/)
            : 0;
        bytesWritten_ = 0;
        finished_ = false;
        return true;
    }

    void write(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (finished_ || !file_.is_open()) return;
        file_.write(reinterpret_cast<const char*>(data), (std::streamsize)len);
        bytesWritten_ += len;
        if (maxBytes_ > 0 && bytesWritten_ >= maxBytes_) {
            finished_ = true;
            file_.flush();
            file_.close();
            std::printf("[record] reached %.1fs, stopped recording to %s\n",
                        (double)bytesWritten_ / (2.0 * sampleRate_), path_.c_str());
        }
    }

    // Convenience overload for call sites already holding a vector.
    void write(const std::vector<uint8_t>& data) { write(data.data(), data.size()); }

    bool active() {
        std::lock_guard<std::mutex> lk(mtx_);
        return file_.is_open() && !finished_;
    }

    double secondsRecorded() {
        std::lock_guard<std::mutex> lk(mtx_);
        return sampleRate_ > 0 ? (double)bytesWritten_ / (2.0 * sampleRate_) : 0.0;
    }

private:
    std::ofstream file_;
    std::mutex mtx_;
    std::string path_;
    uint64_t bytesWritten_ = 0;
    uint64_t maxBytes_ = 0; // 0 == unlimited
    uint32_t sampleRate_ = 0;
    bool finished_ = false;
};
