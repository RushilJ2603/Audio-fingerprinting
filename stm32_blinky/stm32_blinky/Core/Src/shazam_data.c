/* ============================================================================
 * shazam_data.c  —  mock audio clips (4 songs) + mock Flash "song database".
 *
 *   idx 0 "Sinewave Anthem"       — tones 100/300/600/1200 Hz  — MATCHES DB song 1
 *   idx 1 "Odd Harmonics"         — tones 150/450/900/1800 Hz  — no match
 *   idx 2 "Crystal Chords"        — tones 125/375/625/1125 Hz  — MATCHES DB song 2
 *   idx 3 "Just Intonation Blues" — tones 220/440/880/1760 Hz  — no match
 *
 * Songs 0 and 2 each produce four constellation peaks whose (anchor, target,
 * delta_time) hashes coincide with pre-computed DB entries, so the offset
 * histogram spikes and shazam_identify() returns match_count > 0. Songs 1
 * and 3 share no bins with the DB, so every hash scan misses and we hit
 * the "no match" path.
 *
 * The blue USER button in main.c cycles which clip gets fed into the
 * pipeline on each press (0 -> 1 -> 2 -> 3 -> 0 -> ...).
 * ============================================================================
 */

#include "shazam.h"
#include <math.h>

#define MOCK_AUDIO_SAMPLES (AUDIO_SAMPLE_RATE * 3)   /* 3 s @ 8 kHz */

/* Single shared buffer — only one song's audio lives in RAM at a time.
 * Synthesized on-demand from `song_tones` on every shazam_data_get_song()
 * call. 3 songs × 96 KB each won't fit in the F407's 192 KB SRAM, so we
 * regenerate instead of caching them all. */
static float mock_audio[MOCK_AUDIO_SAMPLES];
static int   mock_audio_current = -1;   /* which song is currently in the buffer */

/* Per-song tone banks: 4 × {frequency Hz, amplitude}.
 * Each tone is chosen to fall cleanly into one of the 4 log frequency bands
 * (edges at bins 3/25/50/100/256, 7.81 Hz per bin) so each band picks up
 * exactly one peak.  For matching songs (0, 2) those peak bins also line
 * up with anchor/target entries in song_database[] below. */
static const float song_tones[SHAZAM_NUM_SONGS][4][2] = {
    { {100.f,0.8f}, {300.f,0.6f}, {600.f,0.5f}, {1200.f,0.4f} },  /* 0 matches DB */
    { {150.f,0.7f}, {450.f,0.5f}, {900.f,0.5f}, {1800.f,0.3f} },  /* 1 decoy */
    { {125.f,0.8f}, {375.f,0.6f}, {625.f,0.5f}, {1125.f,0.4f} },  /* 2 matches DB */
    { {220.f,0.7f}, {440.f,0.6f}, {880.f,0.5f}, {1760.f,0.4f} }   /* 3 decoy */
};

/* Mock song database.
 *
 *   song_id = 1 → matches audio idx 0 (Sinewave Anthem).
 *     Tones 100/300/600/1200 Hz land on FFT bins {13,38,77,154}.
 *     DB entries plant hashes at absolute_time starting at 1000 ms, so
 *     every live hash match produces offset = 1000, giving a clean spike.
 *
 *   song_id = 2 → matches audio idx 2 (Crystal Chords).
 *     Tones 125/375/625/1125 Hz land on FFT bins {16,48,80,144}. Same
 *     offset-at-1000-ms trick; spike is in offset_histogram[2][1000].
 *
 * 20 + 20 + 10 filler = DB_HASH_COUNT (50). */
const AudioHash song_database[DB_HASH_COUNT] = {
    /* -- song_id = 1 (audio idx 0, bins 13/38/77/154) -------------------- */
    { .anchor_freq=13,  .target_freq=38,  .delta_time=64,  .absolute_time=1000, .song_id=1 },
    { .anchor_freq=13,  .target_freq=77,  .delta_time=64,  .absolute_time=1000, .song_id=1 },
    { .anchor_freq=13,  .target_freq=154, .delta_time=64,  .absolute_time=1000, .song_id=1 },
    { .anchor_freq=38,  .target_freq=13,  .delta_time=64,  .absolute_time=1064, .song_id=1 },
    { .anchor_freq=38,  .target_freq=77,  .delta_time=64,  .absolute_time=1064, .song_id=1 },
    { .anchor_freq=38,  .target_freq=154, .delta_time=64,  .absolute_time=1064, .song_id=1 },
    { .anchor_freq=77,  .target_freq=13,  .delta_time=64,  .absolute_time=1128, .song_id=1 },
    { .anchor_freq=77,  .target_freq=38,  .delta_time=64,  .absolute_time=1128, .song_id=1 },
    { .anchor_freq=77,  .target_freq=154, .delta_time=64,  .absolute_time=1128, .song_id=1 },
    { .anchor_freq=154, .target_freq=13,  .delta_time=64,  .absolute_time=1192, .song_id=1 },
    { .anchor_freq=154, .target_freq=38,  .delta_time=64,  .absolute_time=1192, .song_id=1 },
    { .anchor_freq=154, .target_freq=77,  .delta_time=64,  .absolute_time=1192, .song_id=1 },
    { .anchor_freq=13,  .target_freq=38,  .delta_time=128, .absolute_time=1000, .song_id=1 },
    { .anchor_freq=13,  .target_freq=77,  .delta_time=128, .absolute_time=1000, .song_id=1 },
    { .anchor_freq=38,  .target_freq=154, .delta_time=128, .absolute_time=1064, .song_id=1 },
    { .anchor_freq=77,  .target_freq=13,  .delta_time=128, .absolute_time=1128, .song_id=1 },
    { .anchor_freq=77,  .target_freq=154, .delta_time=128, .absolute_time=1128, .song_id=1 },
    { .anchor_freq=154, .target_freq=38,  .delta_time=128, .absolute_time=1192, .song_id=1 },
    { .anchor_freq=13,  .target_freq=154, .delta_time=192, .absolute_time=1000, .song_id=1 },
    { .anchor_freq=38,  .target_freq=77,  .delta_time=192, .absolute_time=1064, .song_id=1 },

    /* -- song_id = 2 (audio idx 2, bins 16/48/80/144) -------------------- */
    { .anchor_freq=16,  .target_freq=48,  .delta_time=64,  .absolute_time=1000, .song_id=2 },
    { .anchor_freq=16,  .target_freq=80,  .delta_time=64,  .absolute_time=1000, .song_id=2 },
    { .anchor_freq=16,  .target_freq=144, .delta_time=64,  .absolute_time=1000, .song_id=2 },
    { .anchor_freq=48,  .target_freq=16,  .delta_time=64,  .absolute_time=1064, .song_id=2 },
    { .anchor_freq=48,  .target_freq=80,  .delta_time=64,  .absolute_time=1064, .song_id=2 },
    { .anchor_freq=48,  .target_freq=144, .delta_time=64,  .absolute_time=1064, .song_id=2 },
    { .anchor_freq=80,  .target_freq=16,  .delta_time=64,  .absolute_time=1128, .song_id=2 },
    { .anchor_freq=80,  .target_freq=48,  .delta_time=64,  .absolute_time=1128, .song_id=2 },
    { .anchor_freq=80,  .target_freq=144, .delta_time=64,  .absolute_time=1128, .song_id=2 },
    { .anchor_freq=144, .target_freq=16,  .delta_time=64,  .absolute_time=1192, .song_id=2 },
    { .anchor_freq=144, .target_freq=48,  .delta_time=64,  .absolute_time=1192, .song_id=2 },
    { .anchor_freq=144, .target_freq=80,  .delta_time=64,  .absolute_time=1192, .song_id=2 },
    { .anchor_freq=16,  .target_freq=48,  .delta_time=128, .absolute_time=1000, .song_id=2 },
    { .anchor_freq=16,  .target_freq=80,  .delta_time=128, .absolute_time=1000, .song_id=2 },
    { .anchor_freq=48,  .target_freq=144, .delta_time=128, .absolute_time=1064, .song_id=2 },
    { .anchor_freq=80,  .target_freq=16,  .delta_time=128, .absolute_time=1128, .song_id=2 },
    { .anchor_freq=80,  .target_freq=144, .delta_time=128, .absolute_time=1128, .song_id=2 },
    { .anchor_freq=144, .target_freq=48,  .delta_time=128, .absolute_time=1192, .song_id=2 },
    { .anchor_freq=16,  .target_freq=144, .delta_time=192, .absolute_time=1000, .song_id=2 },
    { .anchor_freq=48,  .target_freq=80,  .delta_time=192, .absolute_time=1064, .song_id=2 },

    /* -- filler, intentionally unreachable ------------------------------ */
    { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }
};

static void synthesize_song(uint8_t idx)
{
    const float fs     = (float)AUDIO_SAMPLE_RATE;
    const float two_pi = 2.0f * 3.14159265358979f;

    float amp_sum = 0.0f;
    for (int k = 0; k < 4; k++) amp_sum += song_tones[idx][k][1];
    const float norm = 1.0f / amp_sum;

    for (size_t n = 0; n < MOCK_AUDIO_SAMPLES; n++) {
        const float t = (float)n / fs;
        float x = 0.0f;
        for (int k = 0; k < 4; k++) {
            x += song_tones[idx][k][1] *
                 sinf(two_pi * song_tones[idx][k][0] * t);
        }
        mock_audio[n] = x * norm;
    }
    mock_audio_current = (int)idx;
}

void shazam_data_init(void)
{
    if (mock_audio_current < 0) synthesize_song(0);
}

const float *shazam_data_get_audio(void)     { return mock_audio; }
size_t       shazam_data_get_audio_len(void) { return MOCK_AUDIO_SAMPLES; }

const float *shazam_data_get_song(uint8_t idx)
{
    if (idx >= SHAZAM_NUM_SONGS) idx = 0;
    if ((int)idx != mock_audio_current) synthesize_song(idx);
    return mock_audio;
}
