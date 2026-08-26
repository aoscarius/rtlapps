#pragma once
#include <cstdint>

// See README for tuning guidance -- these defaults target NTSC channel 3.
struct Config {
    uint32_t sample_rate    = 3'200'000;  // S/s, IQ pairs
    uint64_t center_freq    = 61'250'000; // Hz, NTSC ch3 video carrier
    int      gain_tenth_db  = 400;        // manual gain in tenths of dB; -1 = AGC
    int      ppm_correction = 0;
    int      channel_number = 3;          // cosmetic, shown in OSD only

    double   line_period_us  = 63.5556;   // NTSC full horizontal line period
    int      lines_per_field = 245;       // simplified field length (see README)

    int      out_width  = 320;
    int      out_height = 240;

    float    sync_threshold_frac = 0.75f;
    bool     invert = false;

    int      window_w = 960;
    int      window_h = 720;
};
