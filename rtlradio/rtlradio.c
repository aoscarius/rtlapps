/*
 * rtl_fm_simple.c — minimal single-frequency WFM broadcast receiver (mono)
 *
 * See README.md for a full explanation of the signal chain and the
 * producer/consumer threading model used to avoid audio dropouts.
 *
 * Build (see CMakeLists.txt for the recommended way):
 *   gcc -O2 -o rtl_fm_simple rtl_fm_simple.c -lrtlsdr -lm -lpthread
 *
 * Run:
 *   ./rtl_fm_simple -f 92.6M -g auto -v 1.5 | paplay --rate=48000 --format=s16le --channels=1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <rtl-sdr.h>

/* ---- tunables ------------------------------------------------------- */
#define CAPTURE_RATE     960000    /* IQ sample rate from the dongle, Hz  */
#define AUDIO_RATE       48000     /* output PCM rate, Hz                 */
#define DECIMATION       (CAPTURE_RATE / AUDIO_RATE)   /* = 20             */
#define BUF_LEN          (4 * 16384)                   /* USB read chunk */
#define PEAK_DEVIATION   75000.0   /* standard broadcast FM peak dev, Hz */
#define DC_BLOCK_ALPHA   0.0005f   /* running-average DC removal rate    */

/* anti-alias low-pass FIR applied before decimation */
#define FIR_TAPS         81
#define FIR_CUTOFF_HZ    15000.0   /* audio bandwidth for mono broadcast */

/* audio ring buffer: decouples the time-critical USB callback thread
 * from the (potentially blocking) write to the output pipe. Sized as
 * several seconds of audio so that transient stalls on either side
 * (USB/usbipd jitter, or the downstream audio player/pipe stalling)
 * get absorbed instead of causing an audible dropout. */
#define RING_SECONDS     4
#define RING_CAPACITY    (AUDIO_RATE * RING_SECONDS)
#define PREBUFFER_MS     300       /* wait this much audio before first write */
#define PREBUFFER_SAMPLES ((AUDIO_RATE * PREBUFFER_MS) / 1000)

static rtlsdr_dev_t *dev = NULL;
static FILE *out = NULL;
static volatile int do_exit = 0;

/* previous (DC-corrected) IQ sample, for the FM discriminator */
static float prev_i = 0.0f, prev_q = 0.0f;
static int have_prev = 0;

/* DC-blocker running averages, tracked separately for I and Q */
static float dc_i = 0.0f, dc_q = 0.0f;

/* FIR decimation filter state: circular buffer, O(1) push, avoids the
 * O(N) memmove-per-sample that used to run 240,000 times/sec. */
static float fir_coeffs[FIR_TAPS];
static float fir_hist[FIR_TAPS];
static int fir_pos = 0;
static uint32_t sample_counter = 0;

/* de-emphasis filter state (single-pole RC low-pass at audio rate) */
static float deemph_alpha = 0.0f;   /* 0 = disabled */
static float deemph_prev = 0.0f;

/* volume control, linear multiplier applied to the final audio sample */
static float volume = 1.0f;

/* diagnostics: running stats, reported to stderr roughly once/2sec */
static double diag_in_sumsq = 0.0;
static double diag_out_sumsq = 0.0;
static double diag_out_peak = 0.0;
static uint64_t diag_in_count = 0;
static uint64_t diag_out_count = 0;
static int digital_agc = 0;

/* ---- ring buffer (single producer: USB callback thread;
 *                   single consumer: writer thread) -------------------- */
static int16_t ring_buf[RING_CAPACITY];
static size_t ring_read = 0, ring_write = 0, ring_count = 0;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_t writer_thread_id;

/* Called only from the USB callback thread. Never blocks: if the ring
 * is full (writer thread starved for too long), oldest samples are
 * overwritten rather than stalling the USB event loop -- losing a
 * little audio is far less audible than a hard xrun/dropout. */
static void ring_push(const int16_t *data, size_t n) {
    pthread_mutex_lock(&ring_lock);
    if (n > RING_CAPACITY) {
        data += (n - RING_CAPACITY);
        n = RING_CAPACITY;
    }
    if (ring_count + n > RING_CAPACITY) {
        size_t drop = (ring_count + n) - RING_CAPACITY;
        ring_read = (ring_read + drop) % RING_CAPACITY;
        ring_count -= drop;
    }
    for (size_t i = 0; i < n; i++) {
        ring_buf[ring_write] = data[i];
        ring_write = (ring_write + 1) % RING_CAPACITY;
    }
    ring_count += n;
    pthread_cond_signal(&ring_not_empty);
    pthread_mutex_unlock(&ring_lock);
}

/* Called only from the writer thread. Blocks (cheaply, via condvar)
 * until at least one sample is available or shutdown is requested. */
static size_t ring_pop(int16_t *dst, size_t max_n) {
    pthread_mutex_lock(&ring_lock);
    while (ring_count == 0 && !do_exit) {
        pthread_cond_wait(&ring_not_empty, &ring_lock);
    }
    size_t n = ring_count < max_n ? ring_count : max_n;
    for (size_t i = 0; i < n; i++) {
        dst[i] = ring_buf[ring_read];
        ring_read = (ring_read + 1) % RING_CAPACITY;
    }
    ring_count -= n;
    pthread_mutex_unlock(&ring_lock);
    return n;
}

/* Writer thread: the only thread that ever touches `out`. It can
 * block on write() as long as it wants without affecting USB capture. */
static void *writer_thread_fn(void *arg) {
    (void)arg;
    int16_t chunk[4096];

    /* prebuffer: wait for a small cushion of audio before the first
     * write, so playback starts smoothly instead of stuttering while
     * the pipeline is still spinning up. */
    for (;;) {
        pthread_mutex_lock(&ring_lock);
        size_t c = ring_count;
        pthread_mutex_unlock(&ring_lock);
        if (c >= PREBUFFER_SAMPLES || do_exit) break;
        usleep(5000);
    }

    while (!do_exit) {
        size_t n = ring_pop(chunk, sizeof(chunk) / sizeof(chunk[0]));
        if (n > 0 && out) {
            fwrite(chunk, sizeof(int16_t), n, out);
        }
    }
    /* drain whatever's left after do_exit was set */
    size_t n;
    while ((n = ring_pop(chunk, sizeof(chunk) / sizeof(chunk[0]))) > 0) {
        if (out) {
            fwrite(chunk, sizeof(int16_t), n, out);
        }
    }
    return NULL;
}

static void handle_sigint(int sig) {
    (void)sig;
    do_exit = 1;
    pthread_cond_broadcast(&ring_not_empty);
    if (dev) rtlsdr_cancel_async(dev);
}

/* Design a windowed-sinc low-pass FIR (linear phase, symmetric). */
static void design_fir_lowpass(float *coeffs, int ntaps, double fs, double fc) {
    double fc_norm = fc / fs;
    int mid = (ntaps - 1) / 2;
    double sum = 0.0;

    for (int n = 0; n < ntaps; n++) {
        int m = n - mid;
        double sinc;
        if (m == 0) {
            sinc = 2.0 * fc_norm;
        } else {
            sinc = sin(2.0 * M_PI * fc_norm * m) / (M_PI * m);
        }
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * n / (ntaps - 1));
        double v = sinc * w;
        coeffs[n] = (float)v;
        sum += v;
    }
    for (int n = 0; n < ntaps; n++) {
        coeffs[n] = (float)(coeffs[n] / sum);
    }
}

/* rtlsdr_set_tuner_gain() only accepts exact values from the device's
 * supported gain list; anything else silently fails and leaves the
 * gain wherever it was (often near minimum). Snap the requested value
 * to the closest one the hardware actually supports. */
static int pick_nearest_gain(rtlsdr_dev_t *d, int requested_tenth_db) {
    int n = rtlsdr_get_tuner_gains(d, NULL);
    if (n <= 0) return requested_tenth_db;

    int *gains = malloc((size_t)n * sizeof(int));
    if (!gains) return requested_tenth_db;
    rtlsdr_get_tuner_gains(d, gains);

    int best = gains[0];
    int best_diff = abs(gains[0] - requested_tenth_db);
    fprintf(stderr, "Supported tuner gains (tenths of dB):");
    for (int i = 0; i < n; i++) {
        fprintf(stderr, " %d", gains[i]);
        int diff = abs(gains[i] - requested_tenth_db);
        if (diff < best_diff) { best_diff = diff; best = gains[i]; }
    }
    fprintf(stderr, "\n");
    free(gains);
    return best;
}

/* Called by librtlsdr for every USB packet of raw IQ bytes. This is
 * the time-critical path: it must never block. It demodulates and
 * pushes finished audio into the ring buffer, then returns. */
static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx) {
    (void)ctx;
    if (do_exit || len < 2) return;

    uint32_t n_samples = len / 2;
    int16_t pcm[BUF_LEN / 2 / DECIMATION + 2];
    size_t pcm_n = 0;

    const float theta_to_hz = (float)CAPTURE_RATE / (2.0f * (float)M_PI);
    const float hz_to_fullscale = 32767.0f / (float)PEAK_DEVIATION;

    for (uint32_t k = 0; k < n_samples; k++) {
        float raw_i = (float)buf[2 * k]     - 127.5f;
        float raw_q = (float)buf[2 * k + 1] - 127.5f;

        /* DC blocker: removes the zero-IF LO-leakage spike that
         * otherwise corrupts the atan2-based discriminator. */
        dc_i += DC_BLOCK_ALPHA * (raw_i - dc_i);
        dc_q += DC_BLOCK_ALPHA * (raw_q - dc_q);
        float si = raw_i - dc_i;
        float sq = raw_q - dc_q;

        diag_in_sumsq += (double)(si * si + sq * sq);
        diag_in_count++;

        if (have_prev) {
            /* z[n] * conj(z[n-1]); its angle is proportional to
             * instantaneous frequency (the FM-demodulated signal). */
            float re = si * prev_i + sq * prev_q;
            float im = sq * prev_i - si * prev_q;
            float theta = atan2f(im, re);

            fir_hist[fir_pos] = theta;
            fir_pos = (fir_pos + 1) % FIR_TAPS;

            sample_counter++;
            if (sample_counter >= DECIMATION) {
                sample_counter = 0;

                /* fir_pos now points at the oldest sample in the
                 * history; walk forward from there (oldest->newest). */
                float acc = 0.0f;
                int idx = fir_pos;
                for (int t = 0; t < FIR_TAPS; t++) {
                    acc += fir_coeffs[t] * fir_hist[idx];
                    idx = (idx + 1) % FIR_TAPS;
                }

                float audio = acc * theta_to_hz * hz_to_fullscale;

                if (deemph_alpha > 0.0f) {
                    deemph_prev += deemph_alpha * (audio - deemph_prev);
                    audio = deemph_prev;
                }

                audio *= volume;

                if (audio > 32767.0f) audio = 32767.0f;
                if (audio < -32768.0f) audio = -32768.0f;
                pcm[pcm_n++] = (int16_t)audio;

                diag_out_sumsq += (double)audio * (double)audio;
                if (fabs((double)audio) > diag_out_peak) diag_out_peak = fabs((double)audio);
                diag_out_count++;

                if (diag_out_count >= AUDIO_RATE * 2) {
                    double in_rms = sqrt(diag_in_sumsq / (double)diag_in_count);
                    double out_rms = sqrt(diag_out_sumsq / (double)diag_out_count);
                    char line[192];
                    int llen = snprintf(line, sizeof(line),
                        "[level] input RMS: %.1f/127 (%.0f%%)  "
                        "audio RMS: %.0f/32767  audio peak: %.0f/32767\n",
                        in_rms, 100.0 * in_rms / 127.0, out_rms, diag_out_peak);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
                    if (llen > 0) write(2, line, (size_t)llen);
#pragma GCC diagnostic pop 
                    diag_in_sumsq = 0.0; diag_in_count = 0;
                    diag_out_sumsq = 0.0; diag_out_count = 0; diag_out_peak = 0.0;
                }
            }
        }
        prev_i = si;
        prev_q = sq;
        have_prev = 1;
    }

    if (pcm_n > 0) {
        ring_push(pcm, pcm_n);   /* non-blocking; returns immediately */
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -f <freq_hz> [-d <device_index>] [-g <gain_tenth_db|auto>] "
        "[-e <us>] [-v <volume>] [-p <ppm>] [-a] [-o <file>]\n"
        "\n"
        "  -f  Center frequency, Hz. Accepts suffixes k/M, e.g. 92.6M\n"
        "  -d  RTL-SDR device index (default 0)\n"
        "  -g  Tuner gain in tenths of dB (e.g. 400 = 40.0 dB), or 'auto' (default)\n"
        "  -e  De-emphasis time constant in microseconds: 50 (EU/most of world,\n"
        "      default), 75 (US/Korea), or 0 to disable\n"
        "  -v  Volume multiplier, linear (default 1.0). Try 1.5-3.0 if quiet\n"
        "  -p  Frequency correction, ppm (default 0)\n"
        "  -a  Enable the RTL2832's digital AGC (default off -- stacking it with\n"
        "      tuner auto-gain often causes garbled/noisy audio)\n"
        "  -o  Output file for raw PCM (default: stdout)\n"
        "\n"
        "Every 2 seconds, a [level] line is printed to stderr showing input\n"
        "signal strength and output audio level -- use it to check gain is sane:\n"
        "  input RMS under ~5%%   -> signal too weak, raise gain\n"
        "  input RMS over ~60%%   -> likely overloaded/clipping, lower gain\n"
        "  audio RMS near 0      -> not demodulating (wrong freq? no signal?)\n"
        "\n"
        "Output is raw signed 16-bit little-endian mono PCM at 48000 Hz.\n"
        "Pipe to an audio player, e.g.:\n"
        "  %s -f 92.6M -v 2.0 -g auto -v 0.5 | paplay --raw --rate=48000 --format=s16le --channels=1 --latency-msec=500\n"
        "  %s -f 92.6M -v 2.0 -g auto -v 0.5 | ffplay -f s16le -ar 48000 -ac 1 -nodisp -af ""aresample=async=1"" -i -\n",
        prog, prog, prog);
}

static double parse_freq(const char *s) {
    char clean[64];
    size_t n = strlen(s);
    if (n >= sizeof(clean)) n = sizeof(clean) - 1;

    int dot_count = 0;
    for (size_t i = 0; i < n; i++) if (s[i] == '.') dot_count++;

    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ',' || c == '_' || c == ' ' || c == '\'') continue;
        if (c == '.' && dot_count > 1) continue;
        clean[j++] = c;
    }
    clean[j] = '\0';

    char *end;
    double val = strtod(clean, &end);

    if (*end == 'M' || *end == 'm') { val *= 1e6; end++; }
    else if (*end == 'k' || *end == 'K') { val *= 1e3; end++; }

    if (*end != '\0') {
        fprintf(stderr,
            "Warning: frequency '%s' has unrecognized trailing characters "
            "('%s') that were ignored. Parsed value: %.0f Hz (%.4f MHz).\n"
            "If that's not what you intended, pass a plain number of Hz "
            "(e.g. -f 92600000) or use an M suffix (e.g. -f 92.6M).\n",
            s, end, val, val / 1e6);
    }

    return val;
}

int main(int argc, char **argv) {
    int dev_index = 0;
    double freq_hz = 0;
    int gain_auto = 1;
    int gain_tenth_db = 0;
    double deemph_us = 50.0;   /* EU default */
    const char *outfile = NULL;
    int ppm = 0;
    int opt;

    while ((opt = getopt(argc, argv, "f:d:g:e:v:p:ao:h")) != -1) {
        switch (opt) {
            case 'f': freq_hz = parse_freq(optarg); break;
            case 'd': dev_index = atoi(optarg); break;
            case 'g':
                if (strcmp(optarg, "auto") == 0) { gain_auto = 1; }
                else { gain_auto = 0; gain_tenth_db = atoi(optarg); }
                break;
            case 'e': deemph_us = atof(optarg); break;
            case 'v': volume = (float)atof(optarg); break;
            case 'p': ppm = atoi(optarg); break;
            case 'a': digital_agc = 1; break;
            case 'o': outfile = optarg; break;
            default: usage(argv[0]); return 1;
        }
    }

    if (freq_hz <= 0) {
        fprintf(stderr, "Error: you must specify a center frequency with -f\n\n");
        usage(argv[0]);
        return 1;
    }

    if (deemph_us > 0.0) {
        double dt = 1.0 / (double)AUDIO_RATE;
        double tau = deemph_us * 1e-6;
        deemph_alpha = (float)(dt / (tau + dt));
    } else {
        deemph_alpha = 0.0f;
    }

    design_fir_lowpass(fir_coeffs, FIR_TAPS, CAPTURE_RATE, FIR_CUTOFF_HZ);

    int device_count = rtlsdr_get_device_count();
    if (device_count == 0) {
        fprintf(stderr, "No RTL-SDR devices found.\n");
        return 1;
    }

    if (rtlsdr_open(&dev, dev_index) < 0) {
        fprintf(stderr, "Failed to open RTL-SDR device #%d\n", dev_index);
        return 1;
    }

    rtlsdr_set_sample_rate(dev, CAPTURE_RATE);
    rtlsdr_set_center_freq(dev, (uint32_t)freq_hz);
    if (ppm != 0) rtlsdr_set_freq_correction(dev, ppm);

    if (gain_auto) {
        rtlsdr_set_tuner_gain_mode(dev, 0);
    } else {
        rtlsdr_set_tuner_gain_mode(dev, 1);
        int actual_gain = pick_nearest_gain(dev, gain_tenth_db);
        if (actual_gain != gain_tenth_db) {
            fprintf(stderr,
                "Requested gain %d (%.1f dB) isn't supported by this tuner; "
                "using closest match: %d (%.1f dB)\n",
                gain_tenth_db, gain_tenth_db / 10.0, actual_gain, actual_gain / 10.0);
        }
        rtlsdr_set_tuner_gain(dev, actual_gain);
        fprintf(stderr, "Tuner gain now reads: %.1f dB\n",
                rtlsdr_get_tuner_gain(dev) / 10.0);
    }

    rtlsdr_set_agc_mode(dev, digital_agc);

    out = outfile ? fopen(outfile, "wb") : stdout;
    if (!out) {
        fprintf(stderr, "Failed to open output file '%s'\n", outfile);
        rtlsdr_close(dev);
        return 1;
    }
    setvbuf(out, NULL, _IONBF, 1 << 16); /* 64KB stdio buffer in the writer thread */

    fprintf(stderr,
        "Tuned to %.4f MHz, capture %d Hz -> audio %d Hz, de-emphasis %.0f us, "
        "volume x%.2f. Ctrl+C to stop.\n",
        freq_hz / 1e6, CAPTURE_RATE, AUDIO_RATE, deemph_us, volume);

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    if (pthread_create(&writer_thread_id, NULL, writer_thread_fn, NULL) != 0) {
        fprintf(stderr, "Failed to start audio writer thread\n");
        rtlsdr_close(dev);
        return 1;
    }

    rtlsdr_reset_buffer(dev);
    rtlsdr_read_async(dev, rtlsdr_callback, NULL, 32, BUF_LEN);

    do_exit = 1;
    pthread_cond_broadcast(&ring_not_empty);
    pthread_join(writer_thread_id, NULL);

    if (out != stdout) fclose(out);
    rtlsdr_close(dev);
    return 0;
}