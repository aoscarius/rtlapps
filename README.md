# RTLTV — RTL-SDR Blog V4 decoder for "Pong on a chip" RF video and similar (SDL2 window)

Cheap plug-and-play "TV Games" (Pong/Tennis/Breakout-on-a-chip consoles) don't
transmit a digital protocol. They generate analog composite video (NTSC or
PAL, monochrome) and RF-modulate it onto a VHF channel, exactly like a
console plugged into a TV antenna input. This program tunes an RTL-SDR
Blog V4 to that carrier, AM-demodulates it, slices out horizontal sync to
recover scanlines, and renders the picture in an SDL2 window with an
old-CRT treatment: barrel distortion, scanlines, vignette, a retro
channel/status on-screen display, and snow while unsynced.

## Project layout

```
CMakeLists.txt              top-level build, automatic dependency discovery
cmake/FindRTLSDR.cmake      custom find-module (librtlsdr has no CMake config upstream)
src/main.cpp                CLI parsing, RTL-SDR setup, capture/processing threads, event loop
src/dsp/config.hpp          tunable parameters
src/dsp/raw_queue.hpp       thread-safe queue: SDR callback -> processing thread
src/dsp/sync_separator.hpp  AM envelope -> horizontal sync -> scanlines -> frame buffer
src/dsp/frame_buffer.hpp    double-buffered frame handoff to the render thread
src/dsp/tunable_params.hpp  atomics for the parameters the menu/keys adjust live, shared
                             safely between the main thread and the DSP processing thread
src/dsp/composite_separator.hpp  NTSC/PAL COLOR decoder (--c64): burst-locked
                             synchronous chroma demod + notch-filtered luma + YUV->RGB,
                             works for either standard (selected the same way the mono
                             path already does, via lines_per_field -- see its header
                             comment for the sample-rate requirement, the one NTSC-vs-PAL
                             difference that needs actual code, i.e. PAL's V-switch, and
                             what's simplified vs a real broadcast decoder)
src/dsp/file_recorder.hpp   thread-safe raw-IQ-to-file writer, tees whatever source is active
src/extra/stream_generator.hpp        --dry-run synthetic signal: three simultaneous
                             mono carriers 6MHz apart (matching real NTSC channel
                             spacing) -- a CPU-vs-CPU Pong match on the tuned
                             frequency, an analog clock face 6MHz up, a glitch/static
                             pattern 6MHz down -- so channel-scanning behavior is
                             actually exercisable against --dry-run, not just a
                             single fixed signal
src/extra/composite_generator.hpp --dry-run --c64 synthetic NTSC or PAL
                             color-bars signal (whichever --ntsc/--pal selected), used
                             to validate composite_separator.hpp against known
                             ground truth on both standards
src/display/crt_display.*   SDL2 renderer: barrel-distortion LUT, scanlines, vignette,
                             snow, and a hand-rolled 5x7 font OSD (no SDL_ttf dependency).
                             Handles both grayscale (mono) and RGB (--c64) source frames.
src/display/osd_menu.*      keyboard+mouse tuning menu, drawn on top of the picture
src/display/font5x7.hpp     the shared bitmap font used by both OSDs
```

The CRT look is intentionally inspired by the CRT-emulation feature
described in GOROman's [famicom-rf-hackrf-decoder](https://github.com/GOROman/famicom-rf-hackrf-decoder)
(barrel distortion + scanlines + vignette, a retro channel-number/sync-status
overlay, and snow while unlocked). That project targets a HackRF + Famicom
over `libhackrf`; this one targets an RTL-SDR Blog V4 over `librtlsdr`, and
the display code here is an independent reimplementation of the same idea
rather than a port — GitHub's robots.txt blocks automated browsing of their
file tree, so I wasn't able to pull their actual source to adapt directly.

## Build

Only two real dependencies: `SDL2` and `librtlsdr`. CMake finds both
automatically:

- `SDL2` via its own CMake package config (installed by `libsdl2-dev` /
  Homebrew / vcpkg), exposing an `SDL2::SDL2` target — with a legacy
  `SDL2_INCLUDE_DIRS`/`SDL2_LIBRARIES` fallback for older distro packages.
- `librtlsdr` via `cmake/FindRTLSDR.cmake`, which tries `pkg-config` first
  (covers essentially every packaged install) and falls back to a manual
  header/library search for source installs or non-standard prefixes.

```bash
sudo apt install librtlsdr-dev libsdl2-dev cmake pkg-config g++   # Debian/Ubuntu
# brew install hackrf sdl2 cmake pkg-config                        # macOS (rtl-sdr via brew too)

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

CMake prints which SDL2/RTLSDR it found during configure, so a missing
dependency fails loudly with a clear message rather than a mysterious link
error.

## Run

```bash
./build/rtltv --list                 # list connected dongles

./build/rtltv                        # NTSC channel 3 (61.25 MHz), default settings
./build/rtltv --freq 67250000        # NTSC channel 4
./build/rtltv --pal --freq 55250000  # PAL region

./build/rtltv --freq 61250000 --gain 300 --sync-thresh 0.65 --channel 3
```

Full flag list: `./build/rtltv --help`

`--dry-run` (no hardware needed) transmits three simulated stations 6MHz
apart, matching real NTSC channel spacing: a live Pong CPU-vs-CPU match
on the tuned frequency, an analog clock face 6MHz up, and a glitch/static
pattern 6MHz down. Use `CHANNEL` in the tuning menu (or `--channel N`) to
actually exercise channel-scanning against it instead of only ever
seeing one fixed test signal.

## Color decode: --c64

By default this decodes **monochrome** composite video (Pong-on-a-chip
consoles never had color). `--c64` switches to a real **color** decoder
instead, for sources that do carry chroma — a Commodore 64's RF modulator
output being the obvious one, but this applies to any NTSC or PAL
composite-over-RF source. It works with whichever standard `--ntsc`/
`--pal` already selects (NTSC by default, same as the mono path) — `--c64`
only changes *whether* color is decoded, not which broadcast standard.

```bash
./build/rtltv --c64 --freq 61250000            # NTSC color (default standard)
./build/rtltv --c64 --pal --freq 591250000     # PAL color, e.g. UK UHF ch36-ish
```

**Read this before expecting it to work:** the color subcarrier sits well
above the video carrier — 3.579545 MHz for NTSC, 4.43361875 MHz for PAL.
Capturing either without aliasing needs roughly **8-10+ MS/s** — far more
than the 3.2 MS/s the mono path uses. `--c64` raises the default `--rate`
to 10000000 automatically (safely covers both standards; override with
an explicit `--rate` if you want), but whether an RTL-SDR Blog V4
actually sustains that reliably over your particular USB connection is
genuinely hardware/host dependent — you may need to experiment. Full
technical details, including exactly what's simplified compared to a
real broadcast decoder (no delay-line comb filter, so expect more
dot-crawl/color-fringing than a real TV), the one real difference
between the two standards this decoder actually implements (PAL's
V-switch; NTSC has no such thing and the code explicitly avoids letting
noise fake one), and why there's a `HUE` control in the menu (no
absolute phase reference to the real broadcast subcarrier — same reason
old NTSC sets had a hue knob, and it also absorbs NTSC's fixed I/Q-vs-U/V
axis offset since this decoder doesn't model that separately), are in
`src/dsp/composite_separator.hpp`'s header comment.

Test it without hardware: `--dry-run --c64` generates a standard 8-bar
color-bar test pattern (white/yellow/cyan/green/magenta/red/blue/black,
NTSC or PAL depending on `--ntsc`/`--pal`) instead of the mono Pong simulation, so you can confirm the
color path is working (and see how `HUE`/`H-POS`/`SYNC LVL` etc. affect
a known reference image) before pointing it at a real signal.

## Recording and playback

Two flags let you decouple "capture the RF" from "look at the picture":

```bash
# Grab 30 seconds of whatever's currently being decoded (live, --dry-run,
# or even a --play-file source) to a raw IQ file:
./build/rtltv --record capture.iq --record-seconds 30

# Later (no dongle needed), decode from that file instead of a live one:
./build/rtltv --play-file capture.iq
```

- `--record FILE` tees the raw interleaved-u8-I/Q stream to `FILE` as
  it's produced. Omit `--record-seconds` to record until you quit the
  program instead of stopping automatically. This is the *exact* format
  the `rtl_sdr` command-line tool itself writes, so files recorded here
  also work with other SDR tools (GNU Radio, `rtl_sdr`-compatible
  utilities, etc.), and files from those tools work with `--play-file`.
- `--play-file FILE` replaces the live dongle with a file, paced at the
  same rate real capture would be so decode timing behaves identically.
  It loops back to the start at end-of-file by default; pass `--no-loop`
  to stop there instead (the picture just freezes — the window stays
  open, nothing crashes).
- Recording and playback both work in mono and `--c64` color mode.
- While recording, a small red `REC 12S` indicator shows in the
  bottom-right of the window (hidden while the tuning menu is open).

## Tuning menu

Press `Tab` (or `M`) to open an on-screen menu for dialing in reception
and picture alignment without restarting the program:

| Item        | What it does                                                             |
|-------------|---------------------------------------------------------------------------|
| `CHANNEL`   | steps through the standard NTSC/PAL broadcast channel table               |
| `FREQ`      | video carrier frequency -- the main thing for *acquiring* the signal      |
| `PPM`       | tuner frequency correction (crystal drift)                                |
| `GAIN`      | manual RF gain                                                             |
| `SYNC LVL`  | sync detector threshold -- the main thing for *locking* horizontally      |
| `LINES/FLD` | simplified field length -- fixes steady vertical roll                     |
| `H-POS`     | shifts the picture left/right                                             |
| `H-WIDTH`   | zooms the picture in/out horizontally (fixes a stretched/squeezed image)  |
| `V-SHIFT`   | rotates the picture up/down without touching the vertical lock            |
| `INVERT`    | flips black/white polarity                                                 |
| `HUE`       | *(--c64 only)* rotates recovered color hue -- see "Color decode" above    |
| `CRT FX`    | toggles the barrel-distortion/scanline/vignette treatment                 |
| `RESET ALL` | restores every one of the above to the values the program started with    |

Controls:
- **Keyboard:** Up/Down to select a row, Left/Right to adjust, hold
  **Shift** while adjusting for a smaller/finer step, Enter to
  toggle/activate a row, Esc to close the menu.
- **Mouse:** click the `-`/`+` next to a row, scroll the wheel over a
  row to adjust it, click anywhere else on a row to select it (or
  trigger it, for toggles/`RESET ALL`). Hold **Shift** while
  clicking/scrolling for the fine step, same as the keyboard.

While the menu is open it captures all keyboard/mouse input (so the
global shortcuts below don't also fire underneath it); close it with
Esc or Tab/M to go back to those.

## Tuning tips (this is the part that actually matters)

1. **Find the real carrier first.** Cheap RF modulators drift a few
   hundred kHz off the "official" channel frequency. Open the signal in
   GQRX/SDR#/CubicSDR first, find the peak, and pass that as `--freq`.
2. **Gain**: start manual gain (`--gain`) around 200–400 (20–40 dB) and
   adjust with `g`/`h` at runtime until the picture isn't clipped/noisy.
3. **Sync threshold**: press `[`/`]` at runtime. Too low = false sync
   triggers → torn/rolling picture. Too high = misses real sync pulses →
   same symptom the other direction.
4. **Frequency nudging**: `,`/`.` step center frequency by 25 kHz live.
5. If the picture looks like a **photographic negative**, pass `--invert`.
6. If the image rolls vertically at a steady rate, `--lines-per-field`
   doesn't match the real field length for that unit; nudge it up/down.
7. `H-SYNC:OK`/`H-SYNC:NO` in the top-right OSD (and the snow effect) is
   driven by a simple line-jitter heuristic, not true broadcast vsync
   detection — see the big comment block in `src/dsp/sync_separator.hpp`
   for the honest limitations of this simplified decoder.

## Runtime keys (menu closed)

| Key       | Action                            |
|-----------|-------------------------------------|
| `Tab`/`M` | open the tuning menu                |
| `r`       | toggle CRT effect (distortion/scanlines/vignette) on/off |
| `f`       | toggle fullscreen                    |
| `[` `]`   | sync threshold down / up            |
| `,` `.`   | center frequency down / up 25 kHz   |
| `g` `h`   | tuner gain down / up                 |
| `Esc`/`q` | quit                                  |

These are the same quick shortcuts from before; the tuning menu (`Tab`)
covers the same ground plus horizontal/vertical alignment, with finer
control (mouse buttons, Shift for fine steps) -- use whichever's faster
for what you're adjusting.
