#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Animation primitives ported from aora-bot emotion-ball/js/emotions.js.
   Each primitive maps (time, amplitude, period, phase) to a scalar. */
typedef enum {
    ANIM_SINE = 0,   /* smooth sine drift / breathing */
    ANIM_GLANCE,     /* smooth square wave, dwells at both ends */
    ANIM_PULSE,      /* 0 -> amp rhythmic scale */
    ANIM_JITTER,     /* bounded pseudo-noise, decays with time */
    ANIM_SCAN,       /* triangular sweep, faster than sine */
    ANIM_BLINK,      /* periodic close, multi-instance de-phased */
    ANIM_TYPE_COUNT,
} anim_type_t;

typedef enum {
    EMOTION_GROUP_LIFE = 0,
    EMOTION_GROUP_EMOTION,
    EMOTION_GROUP_AGENT,
    EMOTION_GROUP_CUSTOM,
    EMOTION_GROUP_COUNT,
} emotion_group_t;

typedef enum {
    ANIM_TARGET_EYES = 0,
    ANIM_TARGET_BODY,
    ANIM_TARGET_LEFT,
    ANIM_TARGET_RIGHT,
    ANIM_TARGET_COUNT,
} anim_target_t;

typedef enum {
    ANIM_PROP_LOOK_X = 0,
    ANIM_PROP_LOOK_Y,
    ANIM_PROP_X,
    ANIM_PROP_Y,
    ANIM_PROP_SCALE,
    ANIM_PROP_OPEN,
    ANIM_PROP_ROTATE,
    ANIM_PROP_COUNT,
} anim_prop_t;

typedef struct {
    anim_target_t target;
    anim_prop_t prop;
    anim_type_t type;
    float amp;
    float period;
    float phase;
} emotion_anim_t;

typedef struct {
    const char *id;      /* two decimal digits, stable forever */
    const char *name;    /* English key used by the AI protocol */
    const char *label;   /* Chinese display label */
    emotion_group_t group;
    uint16_t transition_ms;
    bool gaze;           /* follow the viewer / touch point */
    float openness;      /* persistent eyelid openness, 1.0 = fully open */
    int32_t blink_ms_min; /* < 0 disables blinking */
    int32_t blink_ms_max;
    uint8_t anim_count;
    emotion_anim_t anims[3];
} emotion_def_t;

/* Lookup by two-digit id. Returns NULL when unknown. */
const emotion_def_t *emotion_catalog_by_id(const char *id);

/* Lookup by English name (case sensitive, e.g. "happy"). Returns NULL when unknown. */
const emotion_def_t *emotion_catalog_by_name(const char *name);

/* Idle fallback used for unknown ids and empty input. Never NULL. */
const emotion_def_t *emotion_catalog_fallback(void);

size_t emotion_catalog_count(void);
const emotion_def_t *emotion_catalog_at(size_t index);

/* Evaluates a primitive bounded by |amp| * 1.1. Returns 0 for non-positive period. */
float animation_eval(anim_type_t type, float t_seconds, float amp, float period, float phase);

#ifdef __cplusplus
}
#endif
