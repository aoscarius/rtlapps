# pong_tv — RTL-SDR Blog V4 decoder for "Pong on a chip" RF video → old-TV SDL2 window

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
src/display/crt_display.*   SDL2 renderer: barrel-distortion LUT, scanlines, vignette,
                             snow, and a hand-rolled 5x7 font OSD (no SDL_ttf dependency)
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
./build/pong_tv --list                 # list connected dongles

./build/pong_tv                        # NTSC channel 3 (61.25 MHz), default settings
./build/pong_tv --freq 67250000        # NTSC channel 4
./build/pong_tv --pal --freq 55250000  # PAL region

./build/pong_tv --freq 61250000 --gain 300 --sync-thresh 0.65 --channel 3
```

Full flag list: `./build/pong_tv --help`

## Tuning menu

Press `Tab` (or `M`) to open an on-screen menu for dialing in reception
and picture alignment without restarting the program:

| Item        | What it does                                                             |
|-------------|---------------------------------------------------------------------------|
| `FREQ`      | video carrier frequency -- the main thing for *acquiring* the signal      |
| `PPM`       | tuner frequency correction (crystal drift)                                |
| `GAIN`      | manual RF gain                                                             |
| `SYNC LVL`  | sync detector threshold -- the main thing for *locking* horizontally      |
| `LINES/FLD` | simplified field length -- fixes steady vertical roll                     |
| `H-POS`     | shifts the picture left/right                                             |
| `H-WIDTH`   | zooms the picture in/out horizontally (fixes a stretched/squeezed image)  |
| `V-SHIFT`   | rotates the picture up/down without touching the vertical lock            |
| `INVERT`    | flips black/white polarity                                                 |
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
| `[` `]`   | sync threshold down / up            |
| `,` `.`   | center frequency down / up 25 kHz   |
| `g` `h`   | tuner gain down / up                 |
| `Esc`/`q` | quit                                  |

These are the same quick shortcuts from before; the tuning menu (`Tab`)
covers the same ground plus horizontal/vertical alignment, with finer
control (mouse buttons, Shift for fine steps) -- use whichever's faster
for what you're adjusting.
