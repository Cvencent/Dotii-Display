#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Small local affect model. It is intentionally deterministic and pure C so
   the same behaviour can be unit tested on a host and run on the device. */
typedef struct {
    float valence;          /* -1 negative .. +1 positive */
    float arousal;          /* 0 calm .. 1 excited */
    float trust;            /* 0 guarded .. 1 comfortable */
    float confusion;        /* 0 clear .. 1 uncertain */
    float fatigue;          /* 0 rested .. 1 tired */
    float positive_bias;    /* learned preference from praise */
    float correction_bias;  /* learned preference from corrections */
    char event_id[3];       /* current context event, "00" means none */
    uint32_t event_until_ms;
    uint32_t updated_ms;
    uint32_t revision;
} emotion_mood_t;

typedef struct {
    float valence;
    float arousal;
    float trust;
    float confusion;
    float fatigue;
    float positive_bias;
    float correction_bias;
    char event_id[3];
    uint32_t event_until_ms;
    uint32_t updated_ms;
    uint32_t revision;
} emotion_mood_snapshot_t;

void emotion_mood_init(emotion_mood_t *mood, uint32_t now_ms);
void emotion_mood_tick(emotion_mood_t *mood, uint32_t now_ms);

/* Feed separated text streams. The parser only uses local keywords; no
   audio, account data, or cloud classification is involved. */
void emotion_mood_ingest_user(emotion_mood_t *mood, const char *text, uint32_t now_ms);
void emotion_mood_ingest_assistant(emotion_mood_t *mood, const char *text, uint32_t now_ms);
void emotion_mood_ingest_server_emotion(emotion_mood_t *mood, const char *emotion,
                                        uint32_t now_ms);

const char *emotion_mood_current_id(const emotion_mood_t *mood);
bool emotion_mood_has_active_event(const emotion_mood_t *mood);
float emotion_mood_valence(const emotion_mood_t *mood);
float emotion_mood_positive_bias(const emotion_mood_t *mood);
float emotion_mood_correction_bias(const emotion_mood_t *mood);
uint32_t emotion_mood_revision(const emotion_mood_t *mood);

void emotion_mood_export(const emotion_mood_t *mood, emotion_mood_snapshot_t *out);
bool emotion_mood_import(emotion_mood_t *mood, const emotion_mood_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
