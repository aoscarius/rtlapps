// pong_tv -- RTL-SDR Blog V4 decoder for analog "Pong on a chip" RF video,
// rendered in an SDL2 window with an old-CRT treatment.
//
// See README.md for the full pipeline explanation, tuning guidance, and
// honest limitations (this is a simplified field-counting decoder, not a
// broadcast-accurate NTSC/PAL demodulator).
//
// Build: cmake -B build && cmake --build build
// Run:   ./build/pong_tv --freq 61250000 --gain 300

#include "dsp/config.hpp"
#include "dsp/frame_buffer.hpp"
#include "dsp/raw_queue.hpp"
#include "dsp/sync_separator.hpp"
#include "dsp/tunable_params.hpp"
#include "display/crt_display.hpp"
#include "display/font5x7.hpp"
#include "display/osd_menu.hpp"

#include <rtl-sdr.h>
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static RawQueue g_rawQueue;
static std::atomic<bool> g_running{true};

static void rtlsdr_callback(unsigned char* buf, uint32_t len, void* /*ctx*/) {
    if (!g_running.load()) return;
    std::vector<uint8_t> copy(buf, buf + len);
    g_rawQueue.push(std::move(copy));
}

static void sdrThreadFn(rtlsdr_dev_t* dev) {
    rtlsdr_reset_buffer(dev);
    rtlsdr_read_async(dev, rtlsdr_callback, nullptr, 16, 16384 * 4);
}

static void processingThreadFn(Config* cfg, TunableParams* params, FrameBuffer* fb,
                                std::atomic<bool>* running) {
    SyncSeparator sync(*cfg, *params, *fb);
    std::vector<float> env;
    while (running->load()) {
        RawChunk chunk;
        if (!g_rawQueue.pop(chunk, *running)) break;
        size_t nSamples = chunk.data.size() / 2;
        env.resize(nSamples);
        for (size_t i = 0; i < nSamples; ++i) {
            float I = (float)chunk.data[2 * i]     - 127.5f;
            float Q = (float)chunk.data[2 * i + 1] - 127.5f;
            env[i] = std::sqrt(I * I + Q * Q);
        }
        sync.process(env);
    }
}

static void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --freq HZ             video carrier frequency (default 61250000, NTSC ch3)\n"
        "  --rate HZ             sample rate (default 3200000)\n"
        "  --gain TENTH_DB       manual RF gain *10 (e.g. 400 = 40.0 dB), or -1 for AGC\n"
        "  --ppm N               tuner PPM correction\n"
        "  --channel N           cosmetic channel number for the OSD (default 3)\n"
        "  --ntsc                NTSC line timing (63.5556us / 245 lines/field) [default]\n"
        "  --pal                 PAL line timing (64us / 288 lines/field)\n"
        "  --lines-per-field N   override field length used for frame flip\n"
        "  --width N / --height N  output framebuffer size (default 320x240)\n"
        "  --sync-thresh F       sync detector threshold fraction 0..1 (default 0.72)\n"
        "  --invert              invert picture polarity\n"
        "  --device N            rtl-sdr device index (default 0)\n"
        "  --list                list rtl-sdr devices and exit\n"
        "\n"
        "Runtime keys:\n"
        "  Tab / M     open/close the tuning menu\n"
        "  (menu open) Up/Down select, Left/Right adjust, Shift+arrow = fine step,\n"
        "              Enter toggles/activates, Esc closes, mouse: click -/+ or\n"
        "              scroll wheel over a row, click a row to select it\n"
        "  (menu closed) r CRT fx, [ ] sync thresh, , . freq, g h gain\n",
        argv0);
}

int main(int argc, char** argv) {
    Config cfg;
    int deviceIndex = 0;
    bool listOnly = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (a == "--freq") cfg.center_freq = std::stoull(next("--freq"));
        else if (a == "--rate") cfg.sample_rate = (uint32_t)std::stoul(next("--rate"));
        else if (a == "--gain") cfg.gain_tenth_db = std::stoi(next("--gain"));
        else if (a == "--ppm") cfg.ppm_correction = std::stoi(next("--ppm"));
        else if (a == "--channel") cfg.channel_number = std::stoi(next("--channel"));
        else if (a == "--ntsc") { cfg.line_period_us = 63.5556; cfg.lines_per_field = 245; }
        else if (a == "--pal")  { cfg.line_period_us = 64.0;    cfg.lines_per_field = 288; }
        else if (a == "--lines-per-field") cfg.lines_per_field = std::stoi(next("--lines-per-field"));
        else if (a == "--width") cfg.out_width = std::stoi(next("--width"));
        else if (a == "--height") cfg.out_height = std::stoi(next("--height"));
        else if (a == "--sync-thresh") cfg.sync_threshold_frac = std::stof(next("--sync-thresh"));
        else if (a == "--invert") cfg.invert = true;
        else if (a == "--device") deviceIndex = std::stoi(next("--device"));
        else if (a == "--list") listOnly = true;
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "Unknown option: %s\n", a.c_str()); print_usage(argv[0]); return 1; }
    }

    int devCount = rtlsdr_get_device_count();
    if (devCount == 0) {
        std::fprintf(stderr, "No RTL-SDR devices found. Check USB connection/permissions (udev rules).\n");
        return 1;
    }
    if (listOnly) {
        for (int i = 0; i < devCount; ++i) {
            char manuf[256], product[256], serial[256];
            rtlsdr_get_device_usb_strings(i, manuf, product, serial);
            std::printf("Device %d: %s %s SN:%s\n", i, manuf, product, serial);
        }
        return 0;
    }

    rtlsdr_dev_t* dev = nullptr;
    if (rtlsdr_open(&dev, deviceIndex) != 0) {
        std::fprintf(stderr, "Failed to open rtl-sdr device %d\n", deviceIndex);
        return 1;
    }
    rtlsdr_set_sample_rate(dev, cfg.sample_rate);
    rtlsdr_set_center_freq(dev, cfg.center_freq);
    rtlsdr_set_freq_correction(dev, cfg.ppm_correction);
    if (cfg.gain_tenth_db < 0) {
        rtlsdr_set_tuner_gain_mode(dev, 0);
    } else {
        rtlsdr_set_tuner_gain_mode(dev, 1);
        rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db);
    }

    std::printf("Tuned to %.4f MHz, %.3f MS/s\n", cfg.center_freq / 1e6, cfg.sample_rate / 1e6);

    // Snapshot of the settings we started with, so "RESET ALL" in the
    // tuning menu has something sane to go back to.
    const Config defaults = cfg;

    TunableParams params(cfg);
    FrameBuffer fb(cfg.out_width, cfg.out_height);
    std::thread sdrThread(sdrThreadFn, dev);
    std::thread procThread(processingThreadFn, &cfg, &params, &fb, &g_running);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    CrtDisplay display(cfg.window_w, cfg.window_h, cfg.out_width, cfg.out_height);
    display.setChannelLabel("CH" + std::to_string(cfg.channel_number));

    // ------------------------- Tuning menu -------------------------
    //
    // Every item here targets exactly the two things a person actually
    // needs to adjust to get a picture: locking onto the carrier
    // (FREQ/PPM/GAIN/SYNC LVL/LINES-FLD) and aligning the decoded image
    // once locked (H-POS/H-WIDTH/V-SHIFT/INVERT). Hardware settings
    // (FREQ/GAIN/PPM) push straight to the tuner via rtlsdr_set_*();
    // decode-side settings write into the atomics in `params` that
    // SyncSeparator reads on the processing thread.
    OsdMenu menu;

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
            rtlsdr_set_center_freq(dev, cfg.center_freq);
        },
        nullptr
    });

    menu.addItem(MenuItem{
        "PPM",
        [&]() { return std::to_string(cfg.ppm_correction); },
        [&](int dir, bool fine) {
            int step = fine ? 1 : 5;
            cfg.ppm_correction += dir * step;
            rtlsdr_set_freq_correction(dev, cfg.ppm_correction);
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
            rtlsdr_set_tuner_gain_mode(dev, 1);
            rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db);
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
            cfg.gain_tenth_db = defaults.gain_tenth_db;
            cfg.ppm_correction = defaults.ppm_correction;
            rtlsdr_set_center_freq(dev, cfg.center_freq);
            rtlsdr_set_freq_correction(dev, cfg.ppm_correction);
            if (cfg.gain_tenth_db < 0) {
                rtlsdr_set_tuner_gain_mode(dev, 0);
            } else {
                rtlsdr_set_tuner_gain_mode(dev, 1);
                rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db);
            }
            params.resetFrom(defaults);
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
                        case SDLK_LEFTBRACKET:
                            params.sync_threshold_frac.store(
                                std::max(0.05f, params.sync_threshold_frac.load() - 0.02f));
                            break;
                        case SDLK_RIGHTBRACKET:
                            params.sync_threshold_frac.store(
                                std::min(0.95f, params.sync_threshold_frac.load() + 0.02f));
                            break;
                        case SDLK_COMMA:
                            cfg.center_freq -= 25000; rtlsdr_set_center_freq(dev, cfg.center_freq); break;
                        case SDLK_PERIOD:
                            cfg.center_freq += 25000; rtlsdr_set_center_freq(dev, cfg.center_freq); break;
                        case SDLK_g:
                            cfg.gain_tenth_db = std::max(0, cfg.gain_tenth_db - 25);
                            rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db); break;
                        case SDLK_h:
                            cfg.gain_tenth_db += 25;
                            rtlsdr_set_tuner_gain(dev, cfg.gain_tenth_db); break;
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
            }
        });
    }

    g_running = false;
    rtlsdr_cancel_async(dev);
    sdrThread.join();
    g_rawQueue.push({});
    procThread.join();

    SDL_Quit();
    rtlsdr_close(dev);
    return 0;
}
