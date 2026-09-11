# rtlradio

A minimal, single-file, command-line WFM (broadcast FM) receiver for RTL-SDR
dongles. It tunes to one center frequency, demodulates it to mono audio, and
streams raw 16-bit PCM to stdout so you can pipe it into any audio player.

It is intentionally simple (no stereo/RDS decoding, no waterfall, no GUI) but
implements the pieces that actually matter for clean audio: DC-spike removal,
a real anti-alias filter before decimation, broadcast de-emphasis, and a
threaded design that avoids audio dropouts.

## Building
Use the `https://github.com/rtlsdrblog/rtl-sdr-blog.git` corrected drivers in
order to correctly works with device. 

### Option A — system-installed librtlsdr

```bash
sudo apt install librtlsdr-dev cmake build-essential
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Option B — a manually built (not installed) rtl-sdr-blog checkout

If you built [rtl-sdr-blog](https://github.com/rtlsdrblog/rtl-sdr-blog)
in-place without `make install` (recommended if you also run other SDR tools
that might reinstall an older `librtlsdr` as a dependency and silently
override your build):

```bash
mkdir build && cd build
cmake .. -DRTLSDR_ROOT=~/rtl-sdr-blog
make -j$(nproc)
```

This links directly against `~/rtl-sdr-blog/build/src/librtlsdr.so` and bakes
that path into the binary (via `rpath`), so no `LD_LIBRARY_PATH` juggling is
needed at run time.

### Option C — plain gcc, no CMake

```bash
gcc -O2 -o rtlradio rtlradio.c -lrtlsdr -lm -lpthread
```

(add `-I ~/rtl-sdr-blog/include -L ~/rtl-sdr-blog/build/src
-Wl,-rpath,~/rtl-sdr-blog/build/src` if using an in-tree, not-installed
build).

## Usage

```bash
./rtlradio -f <freq_hz> [-d <device_index>] [-g <gain_tenth_db|auto>] [-e <us>] [-v <volume>] [-p <ppm>] [-a] [-o <file>]
```

| Flag | Meaning |
|---|---|
| `-f` | Center frequency in Hz. Accepts `k`/`M` suffixes and common separators, e.g. `92.6M`, `92600000`, `92,600,000`. |
| `-d` | RTL-SDR device index (default `0`). |
| `-g` | Tuner gain in tenths of dB (e.g. `400` = 40.0 dB), or `auto` (default). Snapped to the nearest value the hardware actually supports. |
| `-e` | De-emphasis time constant, microseconds: `50` (EU/most of the world, default), `75` (US/Korea), or `0` to disable. |
| `-v` | Volume multiplier, linear (default `1.0`). |
| `-p` | Frequency correction in ppm (default `0`). |
| `-a` | Enable the RTL2832's digital AGC (default off — see "Why digital AGC is off by default" below). |
| `-o` | Write PCM to a file instead of stdout. |

Output is raw signed 16-bit little-endian mono PCM at 48000 Hz. Pipe it into:

```bash
# PipeWire
./rtlradio -f 92.6M -v 2.0 | pw-play --raw --rate=48000 --format=s16 --channels=1 --latency-msec=100

# PulseAudio-compatible (works with PipeWire's pulse server too — most reliable under WSLg)
./rtlradio -f 92.6M -v 2.0 | paplay --raw --rate=48000 --format=s16le --channels=1 --latency-msec=100

# ffplay
./rtlradio -f 92.6M -v 2.0 | ffplay -f s16le -ar 48000 -ac 1 -nodisp -af "aresample=async=1" -i -
```

Every 2 seconds a `[level]` diagnostic line is printed to stderr:

```
[level] input RMS: 18.4/127 (14%)  audio RMS: 3200/32767  audio peak: 21000/32767
```

- Input RMS under ~5% → signal too weak (raise gain, check antenna, check tuning).
- Input RMS over ~60% → likely overloaded (lower gain).
- Audio RMS near 0 with healthy input RMS → not demodulating (wrong frequency? PLL not locked?).

## How the code works

```
RTL-SDR USB callback thread                         Writer thread
──────────────────────────                          ─────────────
raw IQ bytes (uint8, offset-binary)
        │
        ▼
center at 127.5, split into I/Q floats
        │
        ▼
DC blocker (removes LO-leakage spike)
        │
        ▼
FM discriminator: angle(z[n]·conj(z[n-1]))
  = instantaneous frequency, at 240 kHz
        │
        ▼
81-tap windowed-sinc low-pass FIR
(circular buffer, O(1) push)
        │
        ▼
decimate ×5 → 48 kHz
        │
        ▼
de-emphasis (single-pole RC low-pass)
        │
        ▼
volume, clip to int16
        │
        ▼
   ring_push()  ──────────────────────────▶  ring_pop() (blocks on condvar)
   (never blocks)          ring buffer              │
                          (4 sec capacity)           ▼
                                              fwrite() to stdout/file
                                              (can block freely — doesn't
                                               affect USB capture)
```

### Signal chain details

1. **DC blocking.** RTL-SDR's zero-IF architecture produces a small
   LO-leakage spike exactly at the tuned center frequency. Since that's
   right in the middle of the wanted signal, it corrupts the phase-difference
   discriminator badly if not removed. A running-average high-pass
   (`dc_i`/`dc_q`, effectively a ~19 Hz cutoff) subtracts it out continuously.

2. **FM discriminator.** For consecutive complex baseband samples `z[n]` and
   `z[n-1]`, the angle of `z[n] · conj(z[n-1])` is the phase change between
   them, which is proportional to instantaneous frequency — exactly what FM
   encodes. Implemented directly with real arithmetic (no library complex
   type needed):
   ```
   re = si*prev_i + sq*prev_q
   im = sq*prev_i - si*prev_q
   theta = atan2(im, re)
   ```

3. **Anti-alias filter + decimation.** The raw discriminator output runs at
   the full 240 kHz capture rate, but we only want 48 kHz of audio. Simply
   dropping 4 out of every 5 samples would alias high-frequency content
   (the 19 kHz stereo pilot, 38 kHz subcarrier, etc., all still present in
   the raw discriminator output) back down into the audio band as noise.
   An 81-tap Hamming-windowed-sinc low-pass FIR (cutoff 15 kHz) is applied
   first to remove that content before decimating. The filter history is
   kept in a circular buffer for O(1) inserts, and the convolution is
   evaluated only once every 5th sample (right before decimating), not per
   input sample.

4. **De-emphasis.** Broadcast FM pre-emphasizes high audio frequencies before
   transmission (compensating for the fact that FM's noise floor rises with
   frequency); the receiver must undo this with a matching low-pass, or
   audio sounds harsh/hissy. A single-pole RC filter does this, with the
   standard 50 µs (EU/most of the world) or 75 µs (US/Korea) time constant.

5. **Volume.** A plain linear multiplier before clipping to `int16` range.

### Why there's a ring buffer and a separate writer thread

Earlier versions of this program wrote PCM directly to stdout from inside
the USB callback. That callback runs on the same thread librtlsdr uses to
service USB transfers — it must return quickly, or the internal transfer
queue backs up and samples get dropped (an audible "hole" in the audio).
Writing to a pipe is not guaranteed to be fast: if the downstream consumer
(`paplay`, PipeWire, etc.) stalls even briefly — which happens periodically
under WSLg — `fwrite()` blocks, and so does USB capture along with it.

The fix is the classic producer/consumer pattern:

- The USB callback (producer) only ever pushes finished audio samples into a
  4-second ring buffer (`ring_push`), which never blocks — if the buffer
  is ever completely full (writer starved for multiple seconds), it
  overwrites the oldest samples rather than stalling capture.
- A dedicated writer thread (consumer) pulls from the ring buffer
  (`ring_pop`, which blocks cheaply on a condition variable when empty) and
  does the actual `fwrite()` to stdout/file. It can stall as long as it
  needs to without ever affecting USB capture.
- On startup, the writer waits for a small prebuffer (300 ms of audio) to
  accumulate before writing anything, so playback starts smoothly instead
  of underrunning immediately while the pipeline is still spinning up.

### Why digital AGC is off by default

`rtlsdr_set_agc_mode()` enables the RTL2832's own digital AGC, separate from
the tuner's analog gain. Running that at the same time as tuner auto-gain
(`-g auto`) means two independent automatic gain loops fighting each other,
which is a well-known source of garbled/noisy WFM audio. This program
defaults it off; pass `-a` to turn it on if you specifically want to
experiment with it.

### Gain snapping

`rtlsdr_set_tuner_gain()` only accepts exact values from the tuner's
supported gain list (e.g. R820T/R828D tuners support specific steps like
0, 9, 14, 27, 37, 77 ... in tenths of dB) — passing an arbitrary value
silently fails and leaves the gain wherever it was (often near minimum,
which looks like "the tuner isn't receiving anything"). This program queries
`rtlsdr_get_tuner_gains()` and snaps your requested `-g` value to the
closest one actually supported, printing both the full list and what got
applied.

## Known hardware gotchas (RTL-SDR Blog V4 / R828D)

If you see `[R82XX] PLL not locked!` on startup:

- It's sometimes a benign artifact of the tuner's internal calibration sweep
  and reception still works — check the `[level]` line for a genuine signal
  reading before assuming it's fatal.
- It's also commonly caused by an outdated Osmocom `librtlsdr` (the one from
  `apt install librtlsdr-dev`) mishandling the R828D's PLL configuration.
  The actively maintained
  [rtl-sdr-blog fork](https://github.com/rtlsdrblog/rtl-sdr-blog) fixes
  this for V4 boards specifically.
- Make sure the in-kernel DVB-T driver isn't holding the device:
  `echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtl.conf`,
  then reattach the device.
- If you install other SDR tools later (GQRX, GNU Radio, etc.) via `apt`,
  double-check they haven't silently reinstalled the old `librtlsdr` as a
  dependency and shadowed your working build:
  `ldconfig -p | grep rtlsdr` should point at the fork's `.so`.

## Limitations

- Mono only — no stereo pilot/MPX (19 kHz pilot + 38 kHz L−R subcarrier)
  decoding.
- No RDS decoding.
- No squelch.