#pragma once
// Parameters the user can fine-tune live (via keyboard shortcuts or the
// OSD tuning menu) to help the decoder acquire lock and align the
// picture correctly. Kept as std::atomic because these are written from
// the main thread (menu/keys) and read every single sample by the DSP
// processing thread -- plain Config fields would be a data race here.
//
// (center_freq / gain / ppm live in Config instead: they're hardware
// tuner settings only ever touched by the main thread, which owns the
// rtlsdr_dev_t*, so no cross-thread access occurs for those.)

#include "dsp/config.hpp"
#include <atomic>

struct TunableParams {
    // Sync separator: how much above the adaptive noise floor counts as
    // a horizontal sync pulse. Too low -> false triggers on noise
    // (torn/rolling picture or snow). Too high -> misses real sync
    // pulses (same symptom, other direction).
    std::atomic<float> sync_threshold_frac{0.72f};

    // Flips picture polarity if it looks like a photographic negative.
    std::atomic<bool> invert{false};

    // Simplified field length (see sync_separator.hpp) -- the number of
    // horizontal lines counted before a field is considered complete
    // and the frame flips. Wrong value makes the picture roll/tear
    // vertically at a steady rate.
    std::atomic<int> lines_per_field{245};

    // Horizontal alignment. h_shift_frac is the fraction of each
    // captured line skipped before the active picture starts (covers
    // the sync tip + back porch); nudging it shifts the image
    // left/right. h_scale trims how much of the remaining line is
    // treated as "active picture" -- <1 zooms in (corrects a picture
    // that looks horizontally stretched), >1 zooms out.
    std::atomic<float> h_shift_frac{0.10f};
    std::atomic<float> h_scale{1.0f};

    // Vertical alignment: rotates which output row a captured scanline
    // lands on (mod lines_per_field), independent of the field-length
    // lock itself. Useful for fixing a picture that's split/rolled
    // top-to-bottom without having to touch lines_per_field.
    std::atomic<int> v_shift{0};

    explicit TunableParams(const Config& cfg) { resetFrom(cfg); }

    // Restores the values a fresh run would start with. h_shift_frac /
    // h_scale / v_shift have no CLI equivalent (there was nothing to
    // misalign before this menu existed), so they reset to sane
    // hand-picked defaults rather than to a Config field.
    void resetFrom(const Config& cfg) {
        sync_threshold_frac.store(cfg.sync_threshold_frac);
        invert.store(cfg.invert);
        lines_per_field.store(cfg.lines_per_field);
        h_shift_frac.store(0.10f);
        h_scale.store(1.0f);
        v_shift.store(0);
    }
};
