/* ============================================================================
 * shazam_data.c  —  Synthesised test signals for the FFT spectrum analyzer.
 *
 * Four signals, each composed of four sinusoidal tones at different
 * frequencies.  The blue button cycles through them; the STFT output lets
 * the Python UI display the spectral peaks in real time.
 *
 *   Signal 0 "Quad-Low"      —  100 / 300 / 600 / 1200 Hz
 *   Signal 1 "Odd Harmonics" —  150 / 450 / 900 / 1800 Hz
 *   Signal 2 "Mid-Crystal"   —  125 / 375 / 625 / 1125 Hz
 *   Signal 3 "Concert-A"     —  220 / 440 / 880 / 1760 Hz
 *
 * Only one signal lives in RAM at a time (synthesised on demand) to stay
 * within the F407's 192 KB SRAM budget.
 * ============================================================================
 */

#include "shazam.h"
#include <math.h>

/* 2 s of audio at 8 kHz — enough frames to fill the Python chart nicely. */
#define SIGNAL_SAMPLES  (AUDIO_SAMPLE_RATE * 2u)

static float signal_buf[SIGNAL_SAMPLES];
static int   current_signal = -1;   /* which signal is in signal_buf */

/* tone bank: [signal_idx][tone_idx] = { frequency_Hz, amplitude } */
static const float signal_tones[NUM_TEST_SIGNALS][4][2] = {
    { {100.f, 0.80f}, {300.f, 0.60f}, {600.f, 0.50f}, {1200.f, 0.40f} },
    { {150.f, 0.70f}, {450.f, 0.50f}, {900.f, 0.50f}, {1800.f, 0.30f} },
    { {125.f, 0.80f}, {375.f, 0.60f}, {625.f, 0.50f}, {1125.f, 0.40f} },
    { {220.f, 0.70f}, {440.f, 0.60f}, {880.f, 0.50f}, {1760.f, 0.40f} },
};

static const char * const signal_labels[NUM_TEST_SIGNALS] = {
    "100 / 300 / 600 / 1200 Hz",
    "150 / 450 / 900 / 1800 Hz",
    "125 / 375 / 625 / 1125 Hz",
    "220 / 440 / 880 / 1760 Hz",
};

/* Synthesise signal idx into signal_buf (normalised amplitude sum = 1). */
static void synthesise(uint8_t idx)
{
    const float fs      = (float)AUDIO_SAMPLE_RATE;
    const float two_pi  = 6.28318530718f;

    float amp_sum = 0.0f;
    for (int k = 0; k < 4; k++) amp_sum += signal_tones[idx][k][1];
    const float norm = 1.0f / amp_sum;

    for (size_t n = 0; n < SIGNAL_SAMPLES; n++) {
        const float t = (float)n / fs;
        float x = 0.0f;
        for (int k = 0; k < 4; k++) {
            x += signal_tones[idx][k][1]
               * sinf(two_pi * signal_tones[idx][k][0] * t);
        }
        signal_buf[n] = x * norm;
    }
    current_signal = (int)idx;
}

/* ---- Public API -------------------------------------------------------- */

void fft_data_init(void)
{
    if (current_signal < 0) synthesise(0);
}

const float *fft_data_get_signal(uint8_t idx)
{
    if (idx >= NUM_TEST_SIGNALS) idx = 0;
    if ((int)idx != current_signal) synthesise(idx);
    return signal_buf;
}

size_t fft_data_get_length(void) { return SIGNAL_SAMPLES; }

const char *fft_data_get_label(uint8_t idx)
{
    if (idx >= NUM_TEST_SIGNALS) idx = 0;
    return signal_labels[idx];
}
