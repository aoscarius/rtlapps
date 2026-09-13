#!/usr/bin/env python3
"""
rtlradio.py - minimal single-frequency WFM broadcast receiver (mono).
Asynchronous version with fixed Ctrl+C signal management.

Same signal chain as the C version, expressed with numpy/scipy so the
whole thing fits in one small, mostly-vectorized loop:

  raw IQ -> DC blocker -> FM discriminator -> anti-alias FIR -> decimate
         -> de-emphasis -> volume -> int16 PCM -> stdout

Install:
    pip install pyrtlsdr numpy scipy --break-system-packages

pyrtlsdr is a ctypes wrapper around librtlsdr -- the SAME library used by
the C version. If you built rtl-sdr-blog by hand without `make install`,
just point the dynamic loader at it before running this script:

    export LD_LIBRARY_PATH=~/rtl-sdr-blog/build/src:$LD_LIBRARY_PATH

If librtlsdr was installed system-wide (`make install` / apt), you don't
need that line at all.

Run:
    python3 rtlradio.py -f 92.6M -g auto -v 1.5 | paplay --rate=48000 --format=s16le --channels=1
"""
import sys
import argparse
import signal
import numpy as np
from scipy.signal import lfilter, firwin
from rtlsdr import RtlSdr

CAPTURE_RATE = 240_000          # IQ sample rate from the dongle, Hz
AUDIO_RATE = 48_000             # output PCM rate, Hz
DECIM = CAPTURE_RATE // AUDIO_RATE       # 5
PEAK_DEVIATION = 75_000.0       # standard broadcast FM peak deviation, Hz
DC_BLOCK_ALPHA = 0.0005         # running-average DC removal rate

FIR_TAPS = 81
FIR_CUTOFF_HZ = 15_000.0        # audio bandwidth for mono broadcast
CHUNK_SAMPLES = 65536           # Optimal power-of-two buffer size for libusb

# Global handle for signal cleanup
sdr_device = None
output_file = None

def signal_handler(sig, frame):
    """Explicitly intercept Ctrl+C and gracefully terminate the async loop."""
    print("\n[INFO] Ctrl+C detected. Stopping receiver cleanly...", file=sys.stderr)
    if sdr_device:
        try:
            sdr_device.cancel_read_async()
        except Exception:
            pass
    # We do NOT exit here; canceling async will naturally return execution to main()

def parse_freq(s):
    """Accepts 92.6M, 92600000, 92,600,000, 92_600_000, etc."""
    s = s.replace(',', '').replace('_', '').replace(' ', '').replace("'", '')
    mult = 1.0
    if s and s[-1] in 'Mm':
        mult, s = 1e6, s[:-1]
    elif s and s[-1] in 'Kk':
        mult, s = 1e3, s[:-1]
    return float(s) * mult

def pick_nearest_gain(sdr, requested_db):
    """rtlsdr tuners only support a fixed list of gain steps; snap to
    the closest one actually supported, same as the C version does."""
    gains = sdr.valid_gains_db
    print(f"Supported tuner gains (dB): {gains}", file=sys.stderr)
    nearest = min(gains, key=lambda g: abs(g - requested_db))
    if nearest != requested_db:
        print(f"Requested {requested_db} dB not supported; "
              f"using closest match: {nearest} dB", file=sys.stderr)
    return nearest

def main():
    global sdr_device, output_file

    ap = argparse.ArgumentParser(description="Simple WFM mono receiver (RTL-SDR)")
    ap.add_argument('-f', '--freq', required=True, help="Center frequency, e.g. 92.6M")
    ap.add_argument('-g', '--gain', default='auto', help="Gain in dB, or 'auto'")
    ap.add_argument('-v', '--volume', type=float, default=1.0, help="Volume multiplier")
    ap.add_argument('-e', '--deemph', type=float, default=50.0, help="De-emphasis time constant (us)")
    ap.add_argument('-p', '--ppm', type=int, default=0, help="Frequency correction, ppm")
    ap.add_argument('-o', '--out', default=None, help="Output file (default: stdout)")
    args = ap.parse_args()

    freq = parse_freq(args.freq)

    # Register the custom signal handler for SIGINT (Ctrl+C)
    signal.signal(signal.SIGINT, signal_handler)

    sdr = RtlSdr()
    sdr_device = sdr  # Store in global for signal visibility
    
    sdr.sample_rate = CAPTURE_RATE
    sdr.center_freq = freq
    if args.ppm:
        sdr.freq_correction = args.ppm

    if args.gain == 'auto':
        sdr.gain = 'auto'
    else:
        sdr.gain = pick_nearest_gain(sdr, float(args.gain))

    out = open(args.out, 'wb') if args.out else sys.stdout.buffer
    output_file = out

    print(f"Tuned to {freq/1e6:.4f} MHz, capture {CAPTURE_RATE} Hz -> "
          f"audio {AUDIO_RATE} Hz, de-emphasis {args.deemph:.0f} us, "
          f"volume x{args.volume:.2f}. Ctrl+C to stop.", file=sys.stderr)

    # --- filter design -----------------------------------------------
    fir_taps = firwin(FIR_TAPS, cutoff=FIR_CUTOFF_HZ, fs=CAPTURE_RATE, window='hamming')
    
    state = {
        'fir_zi': np.zeros(len(fir_taps) - 1),
        'dc_zi': np.zeros(1, dtype=complex),
        'deemph_zi': np.zeros(1) if args.deemph > 0 else None,
        'prev_sample': np.complex128(1.0),
        'sample_count': 0
    }

    if args.deemph > 0:
        dt = 1.0 / AUDIO_RATE
        tau = args.deemph * 1e-6
        alpha = dt / (tau + dt)
        deemph_b, deemph_a = [alpha], [1.0, -(1.0 - alpha)]

    dc_b, dc_a = [DC_BLOCK_ALPHA], [1.0, -(1.0 - DC_BLOCK_ALPHA)]

    # Asynchronous Callback Function
    def streaming_callback(iq, context):
        dc_zi = state['dc_zi']
        fir_zi = state['fir_zi']
        deemph_zi = state['deemph_zi']
        prev_sample = state['prev_sample']

        # DC blocker: removes the zero-IF LO-leakage spike that
        # otherwise corrupts the phase-based discriminator below.        
        dc_est, dc_zi = lfilter(dc_b, dc_a, iq, zi=dc_zi)
        iq = iq - dc_est

        # FM discriminator: angle of z[n] * conj(z[n-1]) is
        # proportional to instantaneous frequency.
        shifted = np.concatenate(([prev_sample], iq[:-1]))
        theta = np.angle(iq * np.conj(shifted))
        state['prev_sample'] = iq[-1]

        # anti-alias low-pass (state carried across chunks -> no
        # per-chunk boundary clicks), then decimate to audio rate
        filtered, fir_zi = lfilter(fir_taps, [1.0], theta, zi=fir_zi)
        audio = filtered[::DECIM]

        # scale: rad/sample -> Hz deviation -> full-scale int16
        audio = audio * (CAPTURE_RATE / (2 * np.pi)) * (32767.0 / PEAK_DEVIATION)

        # De-emphasis
        if args.deemph > 0:
            audio, deemph_zi = lfilter(deemph_b, deemph_a, audio, zi=deemph_zi)
            state['deemph_zi'] = deemph_zi

        # Volume & Cast
        audio = audio * args.volume
        audio = np.clip(audio, -32768, 32767).astype('<i2')

        # Write to pipe immediately
        out.write(audio.tobytes())
        out.flush()

        state['dc_zi'] = dc_zi
        state['fir_zi'] = fir_zi

        # Metrics printing
        state['sample_count'] += len(iq)
        if state['sample_count'] >= CAPTURE_RATE:
            in_rms = np.sqrt(np.mean(np.abs(iq) ** 2)) * 127.0
            out_rms = np.sqrt(np.mean(audio.astype(np.float64) ** 2))
            print(f"[level] input RMS: {in_rms:.1f}/127 ({100*in_rms/127:.0f}%)  "
                      f"audio RMS: {out_rms:.0f}/32767", file=sys.stderr)
            sys.stderr.flush()
            state['sample_count'] = 0

    # Start streaming
    try:
        # This will block the main thread smoothly until cancel_read_async() is fired
        sdr.read_samples_async(streaming_callback, num_samples=CHUNK_SAMPLES)

    finally:
        # Guarantees device and descriptor release regardless of how the block finishes
        print("[INFO] Cleaning up hardware resources...", file=sys.stderr)
        sdr.close()
        if args.out:
            out.close()
        print("[INFO] Done.", file=sys.stderr)

if __name__ == '__main__':
    main()