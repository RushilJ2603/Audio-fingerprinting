#ifndef SHAZAM_H
#define SHAZAM_H

#include <stdint.h>
#include <stddef.h>
#include "arm_math.h"

/* ============================================================================
 * SHAZAM AUDIO FINGERPRINTING - STM32 Implementation
 * Simplified offline Shazam algorithm for embedded systems
 * ============================================================================
 */

/* Configuration Constants */
#define AUDIO_SAMPLE_RATE      8000         /* 8 kHz mono audio */
#define FFT_SIZE               1024         /* FFT window = 128ms at 8kHz */
#define FFT_BINS               (FFT_SIZE/2) /* Real FFT: N/2 output bins */
#define NUM_FREQ_BANDS         4            /* Logarithmic frequency bands */
#define MAX_CONSTELLATION_PTS  256          /* Max constellation points cached */
#define DB_HASH_COUNT          50           /* Songs in mock database */
#define MAX_OFFSET_HISTOGRAM   500          /* Track offsets 0-500ms */
#define CONSTELLATION_THRESHOLD 0.1f        /* Min normalized magnitude */

/* ============================================================================
 * Data Structures
 * ============================================================================
 */

typedef struct {
    uint16_t anchor_freq;    /* Frequency bin of anchor point */
    uint16_t target_freq;    /* Frequency bin of target point */
    uint16_t delta_time;     /* Time delta (ms) between anchor and target */
    uint32_t absolute_time;  /* Absolute time in song (samples) */
    uint32_t song_id;        /* Which song this hash belongs to */
} AudioHash;

typedef struct {
    uint16_t freq_bin;       /* FFT bin index */
    uint32_t frame_index;    /* Which FFT frame this point came from */
} ConstellationPoint;

typedef struct {
    uint32_t song_id;        /* Matched song ID */
    uint16_t offset_ms;      /* Time offset (ms) */
    uint16_t match_count;    /* Number of hash matches at this offset */
    float confidence;        /* Match confidence (0.0 to 1.0) */
} MatchResult;

/* ============================================================================
 * Public API
 * ============================================================================
 */

void shazam_init(void);
void shazam_process_audio(const float *audio_samples, size_t num_samples);
MatchResult shazam_identify(void);
void shazam_reset(void);

/* Mock data accessors (implemented in shazam_data.c) */
#define SHAZAM_NUM_SONGS 4
void         shazam_data_init(void);
const float *shazam_data_get_audio(void);
size_t       shazam_data_get_audio_len(void);
const float *shazam_data_get_song(uint8_t idx);

#endif // SHAZAM_H
