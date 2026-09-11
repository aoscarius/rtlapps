# rtlradio

A minimal, single-file, command-line WFM (broadcast FM) receiver for RTL-SDR
dongles. It tunes to one center frequency, demodulates it to mono audio, and
streams raw 16-bit PCM to stdout so you can pipe it into any audio player.

It is intentionally simple (no stereo/RDS decoding, no waterfall, no GUI) but
implements a professional DSP pipeline for pristine audio: an intermediate
frequency (IF) bandpass stage before demodulation to block out-of-band RF noise,
a DC-spike removal filter, a secondary anti-alias filter before decimation,
broadcast de-emphasis, and a 3-stage threaded design that prevents audio dropouts.

## Building
Use the `https://github.com` corrected drivers in
order to correctly work with the device. 

### Option A — system-installed librtlsdr

```bash
sudo apt install librtlsdr-dev cmake build-essential
mkdir build && cd build
cmake ..
make -j\$(nproc)
```

### Option B — a manually built (not installed) rtl-sdr-blog checkout

If you built [rtl-sdr-blog](https://github.com)
in-place without `make install` (recommended if you also run other SDR tools
that might reinstall an older `librtlsdr` as a dependency and silently
override your build):

```bash
mkdir build && cd build
cmake .. -DRTLSDR_ROOT=~/rtl-sdr-blog
make -j\$(nproc)
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

## How the code works

```
RTL-SDR USB callback thread
──────────────────────────
raw IQ bytes (uint8, offset-binary)
        │
        ▼
   iq_ring_push() ───────────────┐
  (never blocks)                 │
                                 ▼ IQ Ring Buffer (32 USB chunks capacity)
                         Demodulator worker thread
                         ─────────────────────────
                           iq_ring_pop() (blocks on condvar)
                                 │
                                 ▼
                         center at 127.5, split into I/Q floats
                                 │
                                 ▼
                         31-tap intermediate frequency (IF) FIR low-pass
                         (limits IQ bandwidth to 200 kHz channel)
                                 │
                                 ▼
                         DC blocker (removes LO-leakage spike)
                                 │
                                 ▼
                         FM discriminator: angle(z[n]·conj(z[n-1]))
                           = instantaneous frequency, at 960 kHz
                                 │
                                 ▼
                         81-tap windowed-sinc low-pass FIR audio filter
                         (circular buffer, O(1) push, 15 kHz cutoff)
                                 │
                                 ▼
                         decimate ×20 → 48 kHz
                                 │
                                 ▼
                         de-emphasis (single-pole RC low-pass)
                                 │
                                 ▼
                         volume, clip to int16
                                 │
                                 ▼
                           audio_ring_push() ────────────┐
                           (never blocks)                │
                                                         ▼ Audio Ring Buffer (4 sec capacity)
                                                 Writer thread
                                                 ─────────────
                                                   audio_ring_pop() (blocks on condvar)
                                                         │
                                                         ▼
                                                   fwrite() to stdout/file
                                                   (can block freely — doesn't
                                                    affect USB capture or DSP)
```

### Signal chain details

1. **IF Filtering (Noise Reduction).** The architecture implements an intermediate
   frequency (IF) processing stage before demodulation. A 31-tap low-pass FIR filter
   is applied directly onto the complex IQ samples, restricting the RF bandwidth
   to a strict $\pm 100\text{ kHz}$ window (200 kHz total WFM channel width). This ensures that
   adjacent channel interference and high-frequency RF noise are stripped out *before* reaching
   the phase discriminator, mimicking advanced SDR platforms like SDR++ and ensuring high audio purity.

2. **DC blocking.** RTL-SDR's zero-IF architecture produces a small LO-leakage spike
   exactly at the tuned center frequency. Since that falls right in the middle of the wanted
   channel, it would badly corrupt the phase discriminator. A running-average high-pass filter
   continuously subtracts it from the clean, IF-filtered IQ stream.

3. **FM discriminator.** For consecutive complex baseband samples `z[n]` and `z[n-1]`, the
   angle of `z[n] · conj(z[n-1])` represents the phase change, which is proportional to the
   instantaneous frequency. Implemented with pure real arithmetic:
   ```c
   re = si*prev_i + sq*prev_q;
   im = sq*prev_i - si*prev_q;
   theta = atan2(im, re);
   ```

4. **Anti-alias filter + decimation.** The raw discriminator output runs at the stable native
   960 kHz hardware capture rate. To convert this down to the 48 kHz target audio rate,
   the signal undergoes a $\times 20$ decimation factor. An 81-tap Hamming-windowed-sinc low-pass FIR
   (15 kHz cutoff) removes high-frequency components (like the 19 kHz stereo pilot tone),
   safeguarding against aliasing noise before downsampling.

5. **De-emphasis.** Broadcast FM pre-emphasizes high frequencies during transmission; the
   receiver reverses this via a matching single-pole RC low-pass filter (50 µs for EU, 75 µs for US)
   to eliminate harsh high-frequency hiss.

### 3-Stage Threaded Architecture

To achieve zero audio dropouts, the application utilizes a decoupled producer-consumer model divided into three asynchronous stages:

*   **Stage 1 (USB Capture):** The `librtlsdr` event loop invokes `rtlsdr_callback` on its own hardware management thread. This callback performs an immediate, non-blocking memory transfer (`iq_ring_push`) of the raw bytes into a massive IQ ring buffer, returning instantly to prevent hardware-level packet drops.
*   **Stage 2 (DSP & Demodulation):** A separate `demod_thread` waits for raw data, pulls chunks out of the IQ buffer, and runs the entire heavy mathematical pipeline (IF filtering, DC blocking, `atan2f`, FIR audio filtering, decimation, and de-emphasis) entirely outside the USB interrupt context.
*   **Stage 3 (Audio Writer):** The processed 16-bit PCM samples are pushed into a secondary audio queue. The `writer_thread` blocks until a 300 ms prebuffer safety cushion accumulates, then continuously writes the streams to `stdout` or a file. If the media player downstream stalls or exhibits system scheduling jitter, the writer can block safely without bottlenecking the DSP thread or dropping USB packets.

### Why digital AGC is off by default

`rtlsdr_set_agc_mode()` enables the RTL2832's own digital AGC, separate from the tuner's analog gain. Running that at the same time as tuner auto-gain (`-g auto`) means two independent automatic gain loops fighting each other, which is a well-known source of garbled/noisy WFM audio. This program defaults it off; pass `-a` to turn it on if you specifically want to experiment with it.

### Gain snapping

`rtlsdr_set_tuner_gain()` only accepts exact values from the tuner's supported gain list. This program queries `rtlsdr_get_tuner_gains()` and snaps your requested `-g` value to the closest one actually supported, printing both the full list and what got applied.

## Known hardware gotchas (RTL-SDR Blog V4 / R828D)

If you see `[R82XX] PLL not locked!` on startup:

- It's sometimes a benign artifact of the tuner's internal calibration sweep and reception still works.
- It's also commonly caused by an outdated Osmocom `librtlsdr` (the one from `apt install librtlsdr-dev`) mishandling the R828D's PLL configuration. The actively maintained [rtl-sdr-blog fork](https://github.com) fixes this for V4 boards specifically.
- Make sure the in-kernel DVB-T driver isn't holding the device: `echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtl.conf`, then reattach the device.
- If you install other SDR tools later (GQRX, GNU Radio, etc.) via `apt`, double-check they haven't silently reinstalled the old `librtlsdr` as a dependency and shadowed your working build.

## Limitations

- Mono only — no stereo pilot/MPX (19 kHz pilot + 38 kHz L−R subcarrier) decoding.
- No RDS decoding.
- No squelch.