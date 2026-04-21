/* ============================================================================
 * fft_analyzer.h  —  FFT Spectrum Analyzer for STM32F407
 *
 * Replaces the Shazam fingerprinting header. The board now:
 *   1. Synthesizes a test signal (4 choices, cycled by the blue button).
 *   2. Runs a windowed STFT using CMSIS-DSP arm_rfft_fast_f32.
 *   3. Streams REPORT_BINS magnitude values per frame over SWV/ITM so the
 *      host-side Python UI can draw a live frequency spectrum.
 *
 * Protocol lines emitted over printf -> ITM port 0:
 *   BOOT                                    on startup
 *   FFT_START sig=<n>                       about to analyse signal n
 *   FFT_FRAME <frame> <b0> <b1> ... <b63>  one frame: 64 uint8 magnitudes
 *   FFT_DONE sig=<n>                        all frames sent
 * ============================================================================
 */

#ifndef SHAZAM_H          /* keep old guard so .cproject doesn't need changes */
#define SHAZAM_H

#include <stdint.h>
#include <stddef.h>
#include "arm_math.h"

/* ---- DSP parameters --------------------------------------------------- */
#define AUDIO_SAMPLE_RATE   8000u        /* synthesised signal sample rate   */
#define FFT_SIZE            1024u        /* window length (128 ms @ 8 kHz)   */
#define FFT_BINS            (FFT_SIZE / 2u)
#define REPORT_BINS         64u          /* bins sent per frame (0-500 Hz)   */
#define NUM_TEST_SIGNALS    4u           /* number of test signal presets     */

/* ---- Public API -------------------------------------------------------- */
void fft_analyzer_init(void);
void fft_analyzer_run(uint8_t sig_idx);

/* ---- Test signal data (implemented in shazam_data.c) ------------------ */
void         fft_data_init(void);
const float *fft_data_get_signal(uint8_t idx);
size_t       fft_data_get_length(void);
const char  *fft_data_get_label(uint8_t idx);

#endif /* SHAZAM_H */
