/* ============================================================================
 * shazam.c  —  FFT Spectrum Analyzer pipeline for STM32F407
 *
 * Pipeline per button press:
 *   synthesised PCM -> Hanning window -> arm_rfft_fast_f32 -> magnitude
 *   -> normalise to 0-255 uint8 -> printf("FFT_FRAME ...") over SWV/ITM
 *
 * The file is named shazam.c so the existing .cproject build rule picks it
 * up without any IDE project changes.
 * ============================================================================
 */

#include "shazam.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Static working buffers  (no heap)
 * ---------------------------------------------------------------------------
 */
static float32_t fft_input[FFT_SIZE];       /* windowed time-domain frame  */
static float32_t fft_output[FFT_SIZE];      /* complex interleaved output  */
static float32_t magnitude[FFT_BINS];       /* |X[k]| per bin              */
static float32_t hanning_window[FFT_SIZE];  /* pre-computed Hanning window */

static arm_rfft_fast_instance_f32 rfft;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------------
 */

/* Build the Hanning window once at init. */
static void precompute_hanning(void)
{
    const float32_t N = (float32_t)FFT_SIZE;
    for (size_t n = 0; n < FFT_SIZE; n++) {
        hanning_window[n] =
            0.5f * (1.0f - arm_cos_f32(2.0f * PI * (float32_t)n / (N - 1.0f)));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------
 */

void fft_analyzer_init(void)
{
    arm_rfft_fast_init_f32(&rfft, FFT_SIZE);
    precompute_hanning();
}

/* Run the full STFT on signal sig_idx and stream magnitude frames over ITM. */
void fft_analyzer_run(uint8_t sig_idx)
{
    const float *audio      = fft_data_get_signal(sig_idx);
    const size_t num_samples = fft_data_get_length();
    const size_t hop         = FFT_SIZE / 2u;  /* 50 % overlap */
    uint32_t     frame       = 0;

    for (size_t start = 0; start + FFT_SIZE <= num_samples; start += hop) {

        /* 1. Apply Hanning window. */
        arm_mult_f32((float32_t *)&audio[start],
                     hanning_window, fft_input, FFT_SIZE);

        /* 2. Real FFT.
         *    Output layout: [Re0, Re(N/2), Re1, Im1, Re2, Im2, ...] */
        arm_rfft_fast_f32(&rfft, fft_input, fft_output, 0);

        /* 3. Magnitude spectrum.
         *    arm_cmplx_mag_f32 treats input as interleaved (Re, Im) pairs. */
        arm_cmplx_mag_f32(fft_output, magnitude, FFT_BINS);

        /* 4. Normalise first REPORT_BINS to 0-255 and emit one line. */
        float32_t frame_max = 0.0f;
        uint32_t  dummy_idx;
        arm_max_f32(magnitude, REPORT_BINS, &frame_max, &dummy_idx);
        if (frame_max < 1e-6f) frame_max = 1e-6f;

        printf("FFT_FRAME %lu", (unsigned long)frame);
        for (uint32_t b = 0; b < REPORT_BINS; b++) {
            float32_t v = (magnitude[b] / frame_max) * 255.0f;
            if (v > 255.0f) v = 255.0f;
            printf(" %u", (unsigned int)(uint8_t)v);
        }
        printf("\n");

        /* 5. LED progress spinner — one LED per frame, cycling G->O->R->B. */
        HAL_GPIO_WritePin(GPIOD,
            LED_GREEN_Pin | LED_ORANGE_Pin | LED_RED_Pin | LED_BLUE_Pin,
            GPIO_PIN_RESET);
        switch (frame & 0x3u) {
        case 0: HAL_GPIO_WritePin(LED_GREEN_GPIO_Port,  LED_GREEN_Pin,  GPIO_PIN_SET); break;
        case 1: HAL_GPIO_WritePin(LED_ORANGE_GPIO_Port, LED_ORANGE_Pin, GPIO_PIN_SET); break;
        case 2: HAL_GPIO_WritePin(LED_RED_GPIO_Port,    LED_RED_Pin,    GPIO_PIN_SET); break;
        case 3: HAL_GPIO_WritePin(LED_BLUE_GPIO_Port,   LED_BLUE_Pin,   GPIO_PIN_SET); break;
        }

        frame++;
    }

    /* All frames sent — clear LEDs so main.c can take over display. */
    HAL_GPIO_WritePin(GPIOD,
        LED_GREEN_Pin | LED_ORANGE_Pin | LED_RED_Pin | LED_BLUE_Pin,
        GPIO_PIN_RESET);
}
