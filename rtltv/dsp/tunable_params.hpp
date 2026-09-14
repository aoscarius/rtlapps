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
    std::atomic<float> h_shift_frac{0.180f};
    std::atomic<float> h_scale{0.902f};

    // Vertical alignment: rotates which output row a captured scanline
    // lands on (mod lines_per_field), independent of the field-length
    // lock itself. Useful for fixing a picture that's split/rolled
    // top-to-bottom without having to touch lines_per_field.
    std::atomic<int> v_shift{0};

    // Color hue trim (degrees), used only by PalColorSeparator (--c64).
    // Our chroma demod has no absolute phase reference to the real
    // broadcast subcarrier -- only a self-consistent *relative* one
    // recovered from the color burst -- so a real captured signal will
    // generally come out globally hue-rotated by some fixed but unknown
    // amount (this is exactly why old NTSC sets had a physical "hue"
    // knob; PAL sets didn't need one for this specific issue since the
    // V-switch cancels *line-to-line* phase errors, not a constant
    // offset). Nudge this until colors look right.
    std::atomic<float> hue_trim_deg{0.0f};

    // Contrast/gain applied to the normalized envelope before mapping
    // to brightness, in SyncSeparator (mono path only). Real captured
    // signals can have much weaker native black/white separation than
    // this project's own clean synthetic test signals -- multiplying
    // the normalized envelope by a gain >1 before clamping stretches
    // that separation back out. 1.0 = no change (matches a clean
    // signal); found empirically via a real capture that ~4.0 gave
    // good black/white separation on a particular weak/noisy signal --
    // there's no universally "correct" value, it depends on how much
    // native contrast the actual signal has.
    std::atomic<float> contrast_gain{1.0f};

    explicit TunableParams(const Config& cfg) { resetFrom(cfg); }

    // Restores the values a fresh run would start with. h_shift_frac /
    // h_scale / v_shift / hue_trim_deg have no CLI equivalent (there
    // was nothing to misalign before this menu existed), so they reset
    // to the same hand-picked (but now signal-accurate, see above)
    // defaults rather than to a Config field.
    void resetFrom(const Config& cfg) {
        sync_threshold_frac.store(cfg.sync_threshold_frac);
        invert.store(cfg.invert);
        lines_per_field.store(cfg.lines_per_field);
        h_shift_frac.store(0.180f);
        h_scale.store(0.902f);
        v_shift.store(0);
        hue_trim_deg.store(0.0f);
        contrast_gain.store(3.0f);
    }
};