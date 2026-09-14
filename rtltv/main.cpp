// rtltv -- RTL-SDR Blog V4 decoder for analog composite-video-over-RF
// signals ("Pong on a chip" monochrome by default, or real NTSC/PAL
// color sources like a Commodore 64's RF modulator output via --c64),
// rendered in an SDL2 window with an old-CRT treatment.
//
// See README.md for the full pipeline explanation, tuning guidance, and
// honest limitations (this is a simplified field-counting decoder, not a
// broadcast-accurate NTSC/PAL demodulator; see composite_separator.hpp 
// for --c64's specific caveats, chiefly that it needs a much higher
// sample rate than the mono path to see any color at all).
//
// Build: cmake -B build && cmake --build build
// Run:   ./build/rtltv --freq 61250000 --gain 300
//        ./build/rtltv --c64 --freq 591250000          (PAL color, needs ~10 MS/s)
//        ./build/rtltv --c64 --ntsc --freq 61250000    (NTSC color, needs ~10 MS/s)
//        ./build/rtltv --record capture.iq --record-seconds 30
//        ./build/rtltv --play-file capture.iq

#include "dsp/config.hpp"
#include "dsp/composite_separator.hpp"
#include "dsp/file_recorder.hpp"
#include "dsp/frame_buffer.hpp"
#include "dsp/raw_queue.hpp"
#include "dsp/sync_separator.hpp"
#include "dsp/tunable_params.hpp"
#include "display/crt_display.hpp"
#include "display/font5x7.hpp"
#include "display/osd_menu.hpp"
#include "extra/composite_generator.hpp"
#include "extra/stream_generator.hpp"

#include <rtl-sdr.h>
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>

static RawQueue g_rawQueue;
static std::atomic<bool> g_running{true};

// NTSC/PAL detection lives in config.hpp (isNtscTiming) so this file and
// composite_separator.hpp agree on exactly one heuristic.

static uint64_t channelFrequencyHz(bool ntsc, int channel) {
    if (ntsc) {
        if (channel >= 2 && channel <= 4)
            return 55'250'000ULL + (channel - 2) * 6'000'000ULL;
        if (channel >= 5 && channel <= 6)
            return 77'250'000ULL + (channel - 5) * 6'000'000ULL;
        if (channel >= 7 && channel <= 13)
            return 175'250'000ULL + (channel - 7) * 6'000'000ULL;
        if (channel >= 14 && channel <= 69)
            return 471'250'000ULL + (channel - 14) * 6'000'000ULL;
    } else {
        if (channel >= 2 && channel <= 4)
            return 48'250'000ULL + (channel - 2) * 7'000'000ULL;
        if (channel >= 5 && channel <= 12)
            return 175'250'000ULL + (channel - 5) * 7'000'000ULL;
        if (channel >= 21 && channel <= 69)
            return 471'250'000ULL + (channel - 21) * 8'000'000ULL;
    }

    return 0;
}

// `usingRealHardware` is what `dryRun` alone used to guard: with
// --play-file added, there are now *two* non-live sources (--dry-run
// and --play-file), and neither should touch rtlsdr_* calls.
static bool tuneChannel(Config& cfg, rtlsdr_dev_t* dev, bool usingRealHardware,
                        int channel) {
    const uint64_t frequency =
        channelFrequencyHz(isNtscTiming(cfg), channel);

    if (frequency == 0)
        return false;

    cfg.channel_number = channel;
    cfg.center_freq = frequency;

    if (usingRealHardware && dev != nullptr)
        rtlsdr_set_center_freq(dev, frequency);

    return true;
}

// ctx carries an optional FileRecorder* so a live capture can be teed
// to disk without a global variable.
static void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    if (!g_running.load()) return;
    FileRecorder* recorder = static_cast<FileRecorder*>(ctx);
    if (recorder) recorder->write(buf, len);
    std::vector<uint8_t> copy(buf, buf + len);
    g_rawQueue.push(std::move(copy));
}

static void sdrThreadFn(rtlsdr_dev_t* dev, FileRecorder* recorder) {
    rtlsdr_reset_buffer(dev);
    rtlsdr_read_async(dev, rtlsdr_callback, recorder, 16, 16384 * 4);
}

// Shared by both --dry-run generators (StreamGenerator for the
// mono Pong/Clock/Joke multi-carrier simulation, CompositeColorBarsGenerator
// for --c64's color validation pattern) -- they expose the same
// constructor/nextChunk surface, so one template covers both instead
// of duplicating the pacing loop.
template <typename Generator>
static void fakeStreamThreadFnT(const Config* cfg, FileRecorder* recorder) {
    constexpr std::size_t samplesPerChunk = 16384;

    const uint64_t transmitterFrequency = cfg->center_freq;
    Generator generator(*cfg, transmitterFrequency);

    while (g_running.load()) {
        auto chunk = generator.nextChunk(samplesPerChunk);
        if (recorder) recorder->write(chunk);
        g_rawQueue.push(std::move(chunk));

        std::this_thread::sleep_for(std::chrono::duration<double>(
            static_cast<double>(samplesPerChunk) / static_cast<double>(cfg->sample_rate)));
    }
}

// Plays back a raw IQ file recorded by FileRecorder (or by the real
// rtl_sdr tool -- same format) instead of connecting to a dongle.
// Paces itself the same way the live/dry-run sources do, so the
// decoder sees realistic timing. Loops back to the start at EOF unless
// `loop` is false, in which case it stops (leaving the last decoded
// picture on screen -- the UI keeps running, it just stops receiving
// new samples).
static void fileStreamThreadFn(const Config* cfg, std::string path, bool loop, FileRecorder* recorder) {
    constexpr std::size_t samplesPerChunk = 16384;
    constexpr std::size_t chunkBytes = samplesPerChunk * 2;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "Failed to open playback file: %s\n", path.c_str());
        g_running = false;
        return;
    }

    std::vector<uint8_t> buf(chunkBytes);
    while (g_running.load()) {
        in.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)chunkBytes);
        std::streamsize got = in.gcount();

        if (got <= 0) {
            if (loop) {
                in.clear();
                in.seekg(0, std::ios::beg);
                continue;
            }
            std::printf("[playback] end of file reached, stopping playback "
                        "(picture will freeze; pass a fresh --play-file or restart to replay)\n");
            break;
        }

        std::vector<uint8_t> chunk(buf.begin(), buf.begin() + got);
        if (recorder) recorder->write(chunk);
        g_rawQueue.push(std::move(chunk));

        std::this_thread::sleep_for(std::chrono::duration<double>(
            static_cast<double>(got / 2) / static_cast<double>(cfg->sample_rate)));
    }
}

static void processingThreadFn(Config* cfg, TunableParams* params, FrameBuffer* fb,
                                std::atomic<bool>* running, bool colorMode) {
    std::vector<float> env;
    auto computeEnvelope = [&](const std::vector<uint8_t>& data) {
        size_t nSamples = data.size() / 2;
        env.resize(nSamples);
        for (size_t i = 0; i < nSamples; ++i) {
            float I = (float)data[2 * i]     - 127.5f;
            float Q = (float)data[2 * i + 1] - 127.5f;
            env[i] = std::sqrt(I * I + Q * Q);
        }
    };

    if (colorMode) {
        CompositeColorSeparator sep(*cfg, *params, *fb);
        while (running->load()) {
            RawChunk chunk;
            if (!g_rawQueue.pop(chunk, *running)) break;
            computeEnvelope(chunk.data);
            sep.process(env);
        }
    } else {
        SyncSeparator sep(*cfg, *params, *fb);
        while (running->load()) {
            RawChunk chunk;
            if (!g_rawQueue.pop(chunk, *running)) break;
            computeEnvelope(chunk.data);
            sep.process(env);
        }
    }
}

static void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --freq HZ             video carrier frequency (default 61250000, NTSC ch3)\n"
        "  --rate HZ             sample rate (default 3200000; --c64 raises this to\n"
        "                        10000000 unless you set --rate explicitly)\n"
        "  --gain TENTH_DB       manual RF gain *10 (e.g. 400 = 40.0 dB), or -1 for AGC\n"
        "  --ppm N               tuner PPM correction\n"
        "  --channel N           tune TV channel N using the selected NTSC/PAL table\n"
        "  --ntsc                NTSC line timing (63.5556us / 245 lines/field) [default]\n"
        "  --pal                 PAL line timing (64us / 288 lines/field)\n"
        "  --c64                 decode COLOR instead of monochrome, using whichever\n"
        "                        standard --ntsc/--pal selected (NTSC by default, same\n"
        "                        as the mono path) -- needs a much higher sample rate,\n"
        "                        see composite_separator.hpp.\n"
        "  --lines-per-field N   override field length used for frame flip\n"
        "  --width N / --height N  output framebuffer size (default 320x240)\n"
        "  --sync-thresh F       sync detector threshold fraction 0..1 (default 0.75)\n"
        "  --invert              invert picture polarity\n"
        "  --device N            rtl-sdr device index (default 0)\n"
        "  --list                list rtl-sdr devices and exit\n"
        "  --dry-run             use a synthetic test stream (simulation); no RTL-SDR\n"
        "                        required (Pong/Clock/Joke multi-carrier normally,\n"
        "                        PAL/NTSC color bars with --c64)\n"
        "  --record FILE         tee the raw IQ stream to FILE as it's received/\n"
        "                        generated (live, --dry-run, or even --play-file).\n"
        "                        Same interleaved u8 I/Q format rtl_sdr itself writes.\n"
        "  --record-seconds N    stop recording automatically after N seconds\n"
        "                        (default: record until the program exits)\n"
        "  --play-file FILE      read raw IQ from FILE instead of a live dongle\n"
        "                        (mutually exclusive with --dry-run)\n"
        "  --no-loop             with --play-file, stop at end-of-file instead of\n"
        "                        looping back to the start\n"
        "\n"
        "Runtime keys:\n"
        "  Tab / M     open/close the tuning menu\n"
        "  (menu open) Up/Down select, Left/Right adjust, Shift+arrow = fine step,\n"
        "              Enter toggles/activates, Esc closes, mouse: click -/+ or\n"
        "              scroll wheel over a row, click a row to select it\n"
        "  (menu closed) r CRT fx, f fullscreen, [ ] sync thresh, , . freq, g h gain\n",
        argv0);
}

int main(int argc, char** argv) {
    Config cfg;
    int deviceIndex = 0;
    bool listOnly = false;
    bool dryRun = false;
    bool colorMode = false;
    bool noLoop = false;
    std::string playFile;
    std::string recordFile;
    double recordSeconds = 0.0;

    bool freqExplicit = false;
    bool channelExplicit = false;
    bool rateExplicit = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", flag); std::exit(1); }
            return argv[++i];
        };
        if      (a == "-f" || a == "--freq") {cfg.center_freq = std::stoull(next("--freq")); freqExplicit = true;}
        else if (a == "-r" || a == "--rate") { cfg.sample_rate = (uint32_t)std::stoul(next("--rate")); rateExplicit = true; }
        else if (a == "-g" || a == "--gain") cfg.gain_tenth_db = std::stoi(next("--gain"));
        else if (a == "-p" || a == "--ppm") cfg.ppm_correction = std::stoi(next("--ppm"));
        else if (a == "-c" || a == "--channel") {cfg.channel_number = std::stoi(next("--channel")); channelExplicit = true;}
        else if (a == "-N" || a == "--ntsc") { cfg.line_period_us = 63.5556; cfg.lines_per_field = 245; }
        else if (a == "-P" || a == "--pal")  { cfg.line_period_us = 64.0;    cfg.lines_per_field = 288; }
        else if (a == "--c64") colorMode = true;
        else if (a == "-L" || a == "--lines-per-field") { cfg.lines_per_field = std::stoi(next("--lines-per-field")); }
        else if (a == "-W" || a == "--width") cfg.out_width = std::stoi(next("--width"));
        else if (a == "-H" || a == "--height") cfg.out_height = std::stoi(next("--height"));
        else if (a == "-s" || a == "--sync-thresh") cfg.sync_threshold_frac = std::stof(next("--sync-thresh"));
        else if (a == "-i" || a == "--invert") cfg.invert = true;
        else if (a == "-D" || a == "--device") deviceIndex = std::stoi(next("--device"));
        else if (a == "-l" || a == "--list") listOnly = true;
        else if (a == "-d" || a == "--dry-run") dryRun = true;
        else if (a == "--record") recordFile = next("--record");
        else if (a == "--record-seconds") recordSeconds = std::stod(next("--record-seconds"));
        else if (a == "--play-file") playFile = next("--play-file");
        else if (a == "--no-loop") noLoop = true;
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "Unknown option: %s\n", a.c_str()); print_usage(argv[0]); return 1; }
    }

    if (colorMode) {
        // --c64 works with whichever standard --ntsc/--pal already
        // selected (NTSC by default, same as the mono path) -- it no
        // longer forces PAL timing. The only thing color decode
        // universally needs bumped is sample rate: both standards'
        // subcarriers (3.58MHz NTSC, 4.43MHz PAL) sit well outside the
        // mono path's default 3.2 MS/s, and 10 MS/s safely covers
        // either one (see composite_separator.hpp).
        if (!rateExplicit) cfg.sample_rate = 10'000'000;
    }

    if (!playFile.empty() && dryRun) {
        std::fprintf(stderr, "Cannot combine --play-file with --dry-run -- pick one signal source.\n");
        return 1;
    }

    // Neither --dry-run nor --play-file touches real hardware.
    const bool usingRealHardware = !dryRun && playFile.empty();

    if (channelExplicit && !freqExplicit) {
        if (!tuneChannel(cfg, nullptr, false, cfg.channel_number)) {
            std::fprintf(stderr, "Invalid channel %d for %s\n",
                         cfg.channel_number,
                         isNtscTiming(cfg) ? "NTSC" : "PAL");
            return 1;
        }
    }

    int devCount = 0;
    if (usingRealHardware) {
        devCount = rtlsdr_get_device_count();
        if (devCount == 0) {
            std::fprintf(stderr, "No RTL-SDR devices found.\n");
            return 1;
        }
    }

    if (listOnly) {
        if (!usingRealHardware) {
            std::printf(dryRun
                ? "--dry-run selected; no RTL-SDR devices queried.\n"
                : "--play-file selected; no RTL-SDR devices queried.\n");
        } else {
            for (int i = 0; i < devCount; ++i) {
                char manuf[256], product[256], serial[256];
                rtlsdr_get_device_usb_strings(i, manuf, product, serial);
                std::printf("Device %d: %s %s SN:%s\n",
                            i, manuf, product, serial);
            }
        }
        return 0;
    }

    rtlsdr_dev_t* dev = nullptr;
    if (usingRealHardware) {
        if (rtlsdr_open(&dev, deviceIndex) != 0) {
            std::fprintf(stderr, "Failed to open rtl-sdr device %d\n", deviceIndex);
            return 1;
        }
        rtlsdr_set_sample_rate(dev, cfg.sample_rate);

        // The RTL2832U derives its sample rate by dividing a fixed
        // reference clock by an integer, so not every requested rate is
        // achievable exactly -- the driver silently rounds to the
        // nearest one it can actually generate. Everything downstream
        // depends on cfg.sample_rate being the *actual* rate, not the
        // requested one, so read it back rather than trusting the
        // value we asked for.
        uint32_t actualRate = rtlsdr_get_sample_rate(dev);
        if (actualRate != 0 && actualRate != cfg.sample_rate) {
            std::printf("Note: requested %.3f MS/s, dongle actually running at %.3f MS/s "
                        "(nearest rate its clock divider can produce) -- using the actual rate.\n",
                        cfg.sample_rate / 1e6, actualRate / 1e6);
            cfg.sample_rate = actualRate;
        }

        rtlsdr_set_center_freq(dev, cfg.center_freq);
        rtlsdr_set_freq_correction(dev, cfg.ppm_correction);
        if (cfg.gain_tenth_db < 0) {
            rtlsdr_set_tuner_gain_mode(dev, 0);
        } else {
            rtlsdr_set_tuner_gain_mode(dev, 1);
            rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db);
        }

        std::printf("Tuned to %.4f MHz, %.3f MS/s\n",
                    cfg.center_freq / 1e6, cfg.sample_rate / 1e6);
    } else if (dryRun) {
        std::printf("Dry-run: using synthetic %s%s stream at %.3f MS/s\n",
                    colorMode ? (isNtscTiming(cfg) ? "NTSC" : "PAL") : "",
                    colorMode ? " color bars" : "Pong/Clock/Joke", cfg.sample_rate / 1e6);
    } else {
        std::printf("Playing back from file: %s (%s)\n", playFile.c_str(), noLoop ? "no loop" : "looping");
    }

    FileRecorder recorder;
    FileRecorder* recorderPtr = nullptr;
    if (!recordFile.empty()) {
        if (!recorder.open(recordFile, cfg.sample_rate, recordSeconds)) {
            std::fprintf(stderr, "Failed to open %s for recording\n", recordFile.c_str());
            return 1;
        }
        recorderPtr = &recorder;
        if (recordSeconds > 0.0)
            std::printf("Recording raw IQ to %s (max %.1fs)\n", recordFile.c_str(), recordSeconds);
        else
            std::printf("Recording raw IQ to %s (until program exits)\n", recordFile.c_str());
    }

    // Snapshot of the settings we started with, so "RESET ALL" in the
    // tuning menu has something sane to go back to.
    const Config defaults = cfg;

    TunableParams params(cfg);
    FrameBuffer fb(cfg.out_width, cfg.out_height, colorMode ? 3 : 1);

    std::thread sourceThread;
    if (dryRun) {
        void (*fn)(const Config*, FileRecorder*) = colorMode
            ? &fakeStreamThreadFnT<CompositeColorBarsGenerator>
            : &fakeStreamThreadFnT<StreamGenerator>;
        sourceThread = std::thread(fn, &cfg, recorderPtr);
    } else if (!playFile.empty()) {
        sourceThread = std::thread(fileStreamThreadFn, &cfg, playFile, !noLoop, recorderPtr);
    } else {
        sourceThread = std::thread(sdrThreadFn, dev, recorderPtr);
    }
    std::thread procThread(processingThreadFn, &cfg, &params, &fb, &g_running, colorMode);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    CrtDisplay display(cfg.window_w, cfg.window_h, cfg.out_width, cfg.out_height, colorMode ? 3 : 1);
    display.setChannelLabel("CH" + std::to_string(cfg.channel_number));

    // ------------------------- Tuning menu -------------------------
    //
    // Every item here targets exactly the two things a person actually
    // needs to adjust to get a picture: locking onto the carrier
    // (FREQ/PPM/GAIN/SYNC LVL/LINES-FLD) and aligning the decoded image
    // once locked (H-POS/H-WIDTH/V-SHIFT/INVERT/HUE/CONTRAST). Hardware
    // settings (FREQ/GAIN/PPM) push straight to the tuner via rtlsdr_set_*(),
    // guarded by `usingRealHardware`; decode-side settings write into
    // the atomics in `params` that SyncSeparator/CompositeColorSeparator read
    // on the processing thread.
    OsdMenu menu;

    menu.addItem(MenuItem{
        "CHANNEL",
        [&]() {
            return std::string("CH") +
                   std::to_string(cfg.channel_number);
        },
        [&](int dir, bool fine) {
            (void)fine;

            const bool ntsc = isNtscTiming(cfg);
            int channel = cfg.channel_number + (dir > 0 ? 1 : -1);

            while (channel >= 2 && channel <= 69 &&
                   channelFrequencyHz(ntsc, channel) == 0) {
                channel += dir > 0 ? 1 : -1;
            }

            if (channel < 2 || channel > 69)
                return;

            if (tuneChannel(cfg, dev, usingRealHardware, channel)) {
                display.setChannelLabel("CH" + std::to_string(cfg.channel_number));
            }
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "FREQ",
        [&]() {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4fMHZ", cfg.center_freq / 1e6);
            return std::string(buf);
        },
        [&](int dir, bool fine) {
            uint64_t step = fine ? 1000 : 100000; // 1kHz fine / 100kHz coarse
            long long nf = (long long)cfg.center_freq + (long long)dir * (long long)step;
            nf = std::clamp<long long>(nf, 24'000'000LL, 1'766'000'000LL);
            cfg.center_freq = (uint64_t)nf;
            if (usingRealHardware) rtlsdr_set_center_freq(dev, cfg.center_freq);
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "PPM",
        [&]() { return std::to_string(cfg.ppm_correction); },
        [&](int dir, bool fine) {
            int step = fine ? 1 : 5;
            cfg.ppm_correction += dir * step;
            if (usingRealHardware) rtlsdr_set_freq_correction(dev, cfg.ppm_correction);
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "GAIN",
        [&]() {
            if (cfg.gain_tenth_db < 0) return std::string("AGC");
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1fDB", cfg.gain_tenth_db / 10.0);
            return std::string(buf);
        },
        [&](int dir, bool fine) {
            if (cfg.gain_tenth_db < 0) cfg.gain_tenth_db = 0; // leaving AGC switches to manual
            int step = fine ? 5 : 25; // 0.5dB fine / 2.5dB coarse
            cfg.gain_tenth_db = std::clamp(cfg.gain_tenth_db + dir * step, 0, 500);
            if (usingRealHardware) {
                rtlsdr_set_tuner_gain_mode(dev, 1);
                rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db);
            }
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "SYNC LVL",
        [&]() {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.3f", params.sync_threshold_frac.load());
            return std::string(buf);
        },
        [&](int dir, bool fine) {
            float step = fine ? 0.005f : 0.02f;
            float v = std::clamp(params.sync_threshold_frac.load() + dir * step, 0.05f, 0.95f);
            params.sync_threshold_frac.store(v);
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "LINES/FLD",
        [&]() { return std::to_string(params.lines_per_field.load()); },
        [&](int dir, bool fine) {
            int step = fine ? 1 : 5;
            int v = std::clamp(params.lines_per_field.load() + dir * step, 100, 400);
            params.lines_per_field.store(v);
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "H-POS",
        [&]() {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.3f", params.h_shift_frac.load());
            return std::string(buf);
        },
        [&](int dir, bool fine) {
            float step = fine ? 0.002f : 0.01f;
            float v = std::clamp(params.h_shift_frac.load() + dir * step, 0.0f, 0.40f);
            params.h_shift_frac.store(v);
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "H-WIDTH",
        [&]() {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.3f", params.h_scale.load());
            return std::string(buf);
        },
        [&](int dir, bool fine) {
            float step = fine ? 0.005f : 0.02f;
            float v = std::clamp(params.h_scale.load() + dir * step, 0.5f, 1.5f);
            params.h_scale.store(v);
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "V-SHIFT",
        [&]() { return std::to_string(params.v_shift.load()); },
        [&](int dir, bool fine) {
            int step = fine ? 1 : 5;
            int lpf = std::max(1, params.lines_per_field.load());
            int v = ((params.v_shift.load() + dir * step) % lpf + lpf) % lpf;
            params.v_shift.store(v);
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "INVERT",
        [&]() { return params.invert.load() ? std::string("ON") : std::string("OFF"); },
        [&](int dir, bool) { (void)dir; params.invert.store(!params.invert.load()); },
        [&]() { params.invert.store(!params.invert.load()); }
    });

    menu.addItem(MenuItem{
        "CONTRAST",
        [&]() {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.2fX", params.contrast_gain.load());
            return std::string(buf);
        },
        [&](int dir, bool fine) {
            float step = fine ? 0.1f : 0.5f;
            float v = std::clamp(params.contrast_gain.load() + dir * step, 0.1f, 8.0f);
            params.contrast_gain.store(v);
        },
        nullptr
    });

    if (colorMode) {
        // Only meaningful for the color decoder -- see
        // TunableParams::hue_trim_deg for why this control has to
        // exist at all (no absolute broadcast phase reference).
        menu.addItem(MenuItem{
            "HUE",
            [&]() {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.0fDEG", params.hue_trim_deg.load());
                return std::string(buf);
            },
            [&](int dir, bool fine) {
                float step = fine ? 1.0f : 5.0f;
                float v = params.hue_trim_deg.load() + dir * step;
                while (v > 180.0f) v -= 360.0f;
                while (v < -180.0f) v += 360.0f;
                params.hue_trim_deg.store(v);
            },
            nullptr
        });
    }

    menu.addItem(MenuItem{
        "CRT FX",
        [&]() { return display.crtEffectOn() ? std::string("ON") : std::string("OFF"); },
        [&](int dir, bool) { (void)dir; display.toggleCrtEffect(); },
        [&]() { display.toggleCrtEffect(); }
    });

    menu.addItem(MenuItem{
        "RESET ALL",
        []() { return std::string(""); },
        nullptr,
        [&]() {
            cfg.center_freq = defaults.center_freq;
            cfg.channel_number = defaults.channel_number;
            cfg.gain_tenth_db = defaults.gain_tenth_db;
            cfg.ppm_correction = defaults.ppm_correction;
            if (usingRealHardware) {
                rtlsdr_set_center_freq(dev, cfg.center_freq);
                rtlsdr_set_freq_correction(dev, cfg.ppm_correction);
                if (cfg.gain_tenth_db < 0) {
                    rtlsdr_set_tuner_gain_mode(dev, 0);
                } else {
                    rtlsdr_set_tuner_gain_mode(dev, 1);
                    rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db);
                }
            }
            params.resetFrom(defaults);
            display.setChannelLabel("CH" + std::to_string(cfg.channel_number));
        }
    });

    std::vector<uint8_t> frame;
    bool quit = false;
    SDL_Event ev;
    while (!quit && g_running.load()) {
        while (SDL_PollEvent(&ev)) {
            bool shiftHeld = (SDL_GetModState() & KMOD_SHIFT) != 0;

            if (ev.type == SDL_QUIT) {
                quit = true;
            } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                display.handleResize(ev.window.data1, ev.window.data2);
            } else if (ev.type == SDL_MOUSEMOTION) {
                menu.handleMouseMotion(ev.motion.x, ev.motion.y);
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                if (ev.button.button == SDL_BUTTON_LEFT)
                    menu.handleMouseButtonDown(ev.button.x, ev.button.y, shiftHeld);
            } else if (ev.type == SDL_MOUSEWHEEL) {
                menu.handleMouseWheel(ev.wheel.y, shiftHeld);
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode key = ev.key.keysym.sym;
                if (key == SDLK_TAB || key == SDLK_m) {
                    menu.toggle();
                } else if (menu.handleKey(key, shiftHeld)) {
                    // consumed by the open menu
                } else {
                    // Global shortcuts, only reachable while the menu is closed.
                    switch (key) {
                        case SDLK_ESCAPE:
                        case SDLK_q: quit = true; break;
                        case SDLK_r: display.toggleCrtEffect(); break;
                        case SDLK_f: display.toggleFullscreen(); break;
                        case SDLK_LEFTBRACKET:
                            params.sync_threshold_frac.store(
                                std::max(0.05f, params.sync_threshold_frac.load() - 0.02f));
                            break;
                        case SDLK_RIGHTBRACKET:
                            params.sync_threshold_frac.store(
                                std::min(0.95f, params.sync_threshold_frac.load() + 0.02f));
                            break;
                        case SDLK_COMMA:
                            cfg.center_freq = cfg.center_freq >= 25'000 ? cfg.center_freq - 25'000 : 0;
                            if (usingRealHardware) rtlsdr_set_center_freq(dev, cfg.center_freq);
                            break;
                        case SDLK_PERIOD:
                            cfg.center_freq = std::min<uint64_t>(cfg.center_freq + 25'000, 1'766'000'000ULL);
                            if (usingRealHardware) rtlsdr_set_center_freq(dev, cfg.center_freq);
                            break;
                        case SDLK_g:
                            cfg.gain_tenth_db = std::max(0, cfg.gain_tenth_db - 25);
                            if (usingRealHardware) rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db);
                            break;
                        case SDLK_h:
                            cfg.gain_tenth_db += 25;
                            if (usingRealHardware) rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db);
                            break;
                    }
                }
            }
        }

        bool locked = false;
        if (fb.grab(frame, locked)) display.updateFrame(frame);
        bool lockedNow = fb.isLocked();
        display.setLocked(lockedNow);
        display.setStatusLine(lockedNow ? "H-SYNC:OK" : "H-SYNC:NO");
        char freqBuf[32];
        std::snprintf(freqBuf, sizeof(freqBuf), "%.2fMHZ", cfg.center_freq / 1e6);
        display.setFreqLine(freqBuf);

        display.render([&](std::vector<uint32_t>& px, int w, int h) {
            if (menu.visible()) {
                menu.render(px, w, h);
            } else {
                font5x7::draw_text(px, w, h, 24, h - 30, "TAB:MENU", 2, 0xFFAAAAAAu);
                if (recorderPtr && recorderPtr->active()) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "REC %.0fS", recorderPtr->secondsRecorded());
                    font5x7::draw_text(px, w, h, w - 160, h - 30, buf, 2, 0xFFFF4444u);
                }
            }
        });
    }

    g_running = false;
    if (usingRealHardware)
        rtlsdr_cancel_async(dev);
    sourceThread.join();
    g_rawQueue.push({});
    procThread.join();

    SDL_Quit();
    if (usingRealHardware)
        rtlsdr_close(dev);
    return 0;
}
