/* ============================================================================
 * shazam.c - Simplified offline Shazam fingerprinting pipeline for STM32
 *
 * Pipeline:
 *   raw PCM -> Hanning window -> FFT -> magnitude spectrum
 *           -> per-band peak picking (Constellation Map)
 *           -> combinatorial hashing (anchor/target/delta)
 *           -> match vs Flash database
 *           -> offset histogram per song -> winner
 *
 * Why FFT_SIZE = 1024 @ 8 kHz?
 *   - Window length = 1024/8000 = 128 ms. Long enough for decent frequency
 *     resolution (~7.81 Hz per bin), short enough that transients don't
 *     smear across the whole window. A classic Shazam-style trade-off.
 *   - 1024 is a power of 2 supported directly by CMSIS-DSP's
 *     arm_rfft_fast_f32 without zero-padding.
 *
 * Why 50% overlap (hop = 512)?
 *   - Hanning windows attenuate edges; 50% overlap ensures every time
 *     instant is covered by two windows, so peaks on a frame boundary
 *     aren't lost. More overlap costs more CPU with diminishing returns.
 *
 * Why 4 logarithmic bands?
 *   - Music energy is unevenly distributed across frequency (more in bass
 *     than in high-mid). Using log bands picks a peak per band instead of
 *     letting bass energy dominate the global argmax.
 * ============================================================================
 */

#include "shazam.h"
#include "main.h"
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------------
 * External mock data (defined in shazam_data.c)
 * ---------------------------------------------------------------------------
 */
extern const AudioHash song_database[DB_HASH_COUNT];

/* ---------------------------------------------------------------------------
 * Statically allocated SRAM buffers (NO heap use)
 * ---------------------------------------------------------------------------
 */

/* FFT working buffers */
static float32_t fft_input[FFT_SIZE];           /* windowed time-domain input */
static float32_t fft_output[FFT_SIZE];          /* complex interleaved output */
static float32_t magnitude[FFT_BINS];           /* |X[k]| per bin */
static float32_t hanning_window[FFT_SIZE];      /* pre-computed window */

/* Constellation map (time, freq) coordinate pairs */
static ConstellationPoint constellation[MAX_CONSTELLATION_PTS];
static size_t constellation_count;

/* Per-song offset histograms.
 * histogram[song_id][offset_ms] = vote count.
 * We use a small fixed song count to keep SRAM use bounded. */
#define MAX_TRACKED_SONGS 4
static uint16_t offset_histogram[MAX_TRACKED_SONGS][MAX_OFFSET_HISTOGRAM];
static uint32_t total_matches;

/* CMSIS-DSP real FFT instance */
static arm_rfft_fast_instance_f32 rfft;

/* Frequency band boundaries, in FFT bins.
 * With 8 kHz / 1024 = 7.81 Hz per bin:
 *   Band 0: bins 3..25     (~23 Hz  - 195 Hz, bass)
 *   Band 1: bins 25..50    (~195 Hz - 390 Hz, low-mid)
 *   Band 2: bins 50..100   (~390 Hz - 780 Hz, mid)
 *   Band 3: bins 100..256  (~780 Hz - 2000 Hz, upper)
 * Bin 0..2 skipped (DC/very low, dominated by offset). */
static const uint16_t band_edges[NUM_FREQ_BANDS + 1] = { 3, 25, 50, 100, 256 };

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------------
 */

/* Build the Hanning window once at init time. */
static void precompute_hanning(void)
{
    const float32_t N = (float32_t)FFT_SIZE;
    for (size_t n = 0; n < FFT_SIZE; n++) {
        /* w[n] = 0.5 * (1 - cos(2*pi*n / (N-1))) */
        hanning_window[n] =
            0.5f * (1.0f - arm_cos_f32(2.0f * PI * (float32_t)n / (N - 1.0f)));
    }
}

/* For a single FFT frame, pick one peak per logarithmic band and append
 * the (frame, bin) pair to the constellation buffer. */
static void extract_peaks(uint32_t frame_index)
{
    /* Find global max magnitude in this frame to normalize threshold. */
    float32_t frame_max = 0.0f;
    uint32_t unused_idx;
    arm_max_f32(magnitude, FFT_BINS, &frame_max, &unused_idx);
    if (frame_max < 1e-6f) return; /* silent frame */

    const float32_t thresh = CONSTELLATION_THRESHOLD * frame_max;

    for (int b = 0; b < NUM_FREQ_BANDS; b++) {
        uint16_t start = band_edges[b];
        uint16_t end   = band_edges[b + 1];

        float32_t peak_mag = 0.0f;
        uint16_t  peak_bin = start;

        for (uint16_t k = start; k < end; k++) {
            if (magnitude[k] > peak_mag) {
                peak_mag = magnitude[k];
                peak_bin = k;
            }
        }

        /* Only keep peaks that are meaningfully above the noise floor. */
        if (peak_mag >= thresh && constellation_count < MAX_CONSTELLATION_PTS) {
            constellation[constellation_count].freq_bin    = peak_bin;
            constellation[constellation_count].frame_index = frame_index;
            constellation_count++;
        }
    }
}

/* Convert an FFT frame index to milliseconds since start of audio. */
static inline uint32_t frame_to_ms(uint32_t frame_index)
{
    /* hop = FFT_SIZE/2 samples, so frame t is at t*hop samples */
    const uint32_t hop_samples = FFT_SIZE / 2;
    return (frame_index * hop_samples * 1000u) / AUDIO_SAMPLE_RATE;
}

/* For each constellation point treated as an anchor, pair it with a
 * "target zone" of a few subsequent points to form hashes, and match
 * those hashes against the Flash database. */
static void hash_and_match(void)
{
    /* Target zone: look up to TARGET_FAN_OUT points ahead in the
     * constellation list. Keeping this small bounds work per anchor. */
    const size_t TARGET_FAN_OUT = 5;

    for (size_t i = 0; i < constellation_count; i++) {
        const ConstellationPoint *anchor = &constellation[i];
        uint32_t anchor_ms = frame_to_ms(anchor->frame_index);

        size_t j_end = i + 1 + TARGET_FAN_OUT;
        if (j_end > constellation_count) j_end = constellation_count;

        for (size_t j = i + 1; j < j_end; j++) {
            const ConstellationPoint *target = &constellation[j];
            uint32_t target_ms = frame_to_ms(target->frame_index);

            /* Delta time in ms between anchor and target. */
            if (target_ms <= anchor_ms) continue;
            uint32_t dt = target_ms - anchor_ms;
            if (dt > UINT16_MAX) continue;
            uint16_t delta_time = (uint16_t)dt;

            /* Build the live hash for this anchor/target pair. */
            AudioHash live;
            live.anchor_freq   = anchor->freq_bin;
            live.target_freq   = target->freq_bin;
            live.delta_time    = delta_time;
            live.absolute_time = anchor_ms;
            live.song_id       = 0; /* unknown in live input */

            /* Linear scan of the database. For a real system you'd use a
             * hash table keyed on (anchor_freq, target_freq, delta_time),
             * but a linear scan is fine for a 50-entry demo DB and
             * keeps everything const-in-Flash with zero setup cost. */
            for (size_t k = 0; k < DB_HASH_COUNT; k++) {
                const AudioHash *db = &song_database[k];

                if (db->anchor_freq == live.anchor_freq &&
                    db->target_freq == live.target_freq &&
                    db->delta_time  == live.delta_time) {

                    /* Offset = where in the DB song this anchor lines up
                     * with the live sample. A correct match produces the
                     * SAME offset repeatedly -> a spike in the histogram. */
                    int32_t offset = (int32_t)db->absolute_time -
                                     (int32_t)live.absolute_time;
                    if (offset < 0) continue; /* ignore negative offsets */
                    if (offset >= MAX_OFFSET_HISTOGRAM) continue;

                    if (db->song_id < MAX_TRACKED_SONGS) {
                        offset_histogram[db->song_id][offset]++;
                        total_matches++;
                    }
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------
 */

void shazam_init(void)
{
    arm_rfft_fast_init_f32(&rfft, FFT_SIZE);
    precompute_hanning();
    shazam_reset();
}

void shazam_reset(void)
{
    constellation_count = 0;
    total_matches = 0;
    memset(offset_histogram, 0, sizeof(offset_histogram));
}

void shazam_process_audio(const float *audio_samples, size_t num_samples)
{
    const size_t hop = FFT_SIZE / 2; /* 50% overlap */
    uint32_t frame_index = 0;

    /* STFT loop: slide a windowed FFT across the input. */
    for (size_t start = 0; start + FFT_SIZE <= num_samples; start += hop) {

        /* 1) Apply Hanning window: x_w[n] = x[n] * w[n] */
        arm_mult_f32((float32_t *)&audio_samples[start],
                     hanning_window,
                     fft_input,
                     FFT_SIZE);

        /* 2) Real FFT in-place-ish. Output layout:
         *    fft_output = [Re0, Re(N/2), Re1, Im1, Re2, Im2, ...]
         *    CMSIS packs the Nyquist bin into the Im0 slot. */
        arm_rfft_fast_f32(&rfft, fft_input, fft_output, 0);

        /* 3) Magnitude spectrum. arm_cmplx_mag_f32 treats its input as
         *    interleaved (Re, Im) pairs and produces FFT_BINS magnitudes. */
        arm_cmplx_mag_f32(fft_output, magnitude, FFT_BINS);

        /* 4) Constellation: pick one peak per logarithmic band. */
        extract_peaks(frame_index);

        /* "Processing" spinner: light exactly one LED per frame, cycling
         * Green -> Orange -> Red -> Blue so the user can see the math
         * is progressing instead of thinking the board froze. */
        HAL_GPIO_WritePin(GPIOD,
            LED_GREEN_Pin | LED_ORANGE_Pin | LED_RED_Pin | LED_BLUE_Pin,
            GPIO_PIN_RESET);
        switch (frame_index & 0x3) {
        case 0: HAL_GPIO_WritePin(LED_GREEN_GPIO_Port,  LED_GREEN_Pin,  GPIO_PIN_SET); break;
        case 1: HAL_GPIO_WritePin(LED_ORANGE_GPIO_Port, LED_ORANGE_Pin, GPIO_PIN_SET); break;
        case 2: HAL_GPIO_WritePin(LED_RED_GPIO_Port,    LED_RED_Pin,    GPIO_PIN_SET); break;
        case 3: HAL_GPIO_WritePin(LED_BLUE_GPIO_Port,   LED_BLUE_Pin,   GPIO_PIN_SET); break;
        }

        frame_index++;
    }

    /* Done spinning — clear all 4 LEDs so main.c owns the display again. */
    HAL_GPIO_WritePin(GPIOD,
        LED_GREEN_Pin | LED_ORANGE_Pin | LED_RED_Pin | LED_BLUE_Pin,
        GPIO_PIN_RESET);

    /* 5) Once the whole clip has been turned into a constellation, build
     *    combinatorial hashes and match them against the DB. Doing this
     *    after peak extraction (rather than per-frame) lets us hash
     *    anchor/target pairs across frame boundaries. */
    hash_and_match();
}

MatchResult shazam_identify(void)
{
    MatchResult best = { 0 };

    if (total_matches == 0) {
        return best; /* no matches at all */
    }

    /* For each song, find its tallest histogram bin. The song with the
     * single tallest spike wins. That spike's bin index is the offset
     * (in ms) of the live clip within the matched song. */
    uint16_t best_votes = 0;

    for (uint32_t song = 0; song < MAX_TRACKED_SONGS; song++) {
        uint16_t song_peak = 0;
        uint16_t song_peak_offset = 0;

        for (uint16_t off = 0; off < MAX_OFFSET_HISTOGRAM; off++) {
            if (offset_histogram[song][off] > song_peak) {
                song_peak = offset_histogram[song][off];
                song_peak_offset = off;
            }
        }

        if (song_peak > best_votes) {
            best_votes        = song_peak;
            best.song_id      = song;
            best.offset_ms    = song_peak_offset;
            best.match_count  = song_peak;
        }
    }

    best.confidence = (float)best_votes / (float)total_matches;
    return best;
}
