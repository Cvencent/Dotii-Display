#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "emotion_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMOTION_GAZE_HISTORY 4

typedef struct {
    const char *id;
    emotion_group_t group;
    float openness;
    bool gaze;
} emotion_engine_visual_t;

typedef struct {
    char id[3];
    bool active;
    bool temporary;
    uint32_t hold_ms;
    uint32_t entered_at_ms;
    float gaze_x;
    float gaze_y;
    float elapsed_seconds;
    emotion_engine_visual_t visual;
} emotion_engine_state_t;

typedef struct {
    const emotion_def_t *current;
    const emotion_def_t *base;
    uint32_t entered_at_ms;
    uint32_t now_ms;
    uint32_t hold_ms;
    bool temporary;
    float gaze_x;
    float gaze_y;
    float openness;
    emotion_engine_state_t snapshot;
} emotion_engine_t;

/* Initialises the engine to the idle fallback (02). */
void emotion_engine_init(emotion_engine_t *engine, uint32_t now_ms);

/* Sets a persistent emotion by id or English name. Returns true on success. */
bool emotion_engine_set(emotion_engine_t *engine, const char *id_or_name, uint32_t now_ms);

/* Sets a temporary emotion that reverts to the current base when hold_ms elapses. */
bool emotion_engine_set_hold(emotion_engine_t *engine, const char *id_or_name,
                             uint32_t hold_ms, uint32_t now_ms);

/* Normalised gaze in [-1, 1]; out-of-range input is clamped. */
void emotion_engine_set_gaze(emotion_engine_t *engine, float nx, float ny);

/* Advances time. Returns true when the active emotion changed this tick. */
bool emotion_engine_tick(emotion_engine_t *engine, uint32_t now_ms);

/* Read-only view of the current state. Never NULL after init. */
const emotion_engine_state_t *emotion_engine_state(const emotion_engine_t *engine);

const char *emotion_engine_current_id(const emotion_engine_t *engine);

#ifdef __cplusplus
}
#endif
