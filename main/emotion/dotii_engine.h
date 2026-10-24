#pragma once

/* Dotii expression engine: evaluates timeline/oscillator/behavior layers from
   the generated catalog into a renderable pose. Mirrors the semantics of
   output/emotion-preview/face-renderer.js. */

#include <stdbool.h>
#include <stdint.h>

#include "dotii_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const dotii_expr_t *current; /* never NULL after init */
    const dotii_expr_t *base;    /* revert target for temporary holds */
    uint32_t entered_at_ms;
    uint32_t hold_ms;  /* 0 = sticky */
    bool temporary;
    char id[3];
    dotii_pose_t pose;
} dotii_engine_state_t;

typedef struct {
    dotii_engine_state_t state;
    uint32_t prng;
    /* blink behavior */
    uint32_t blink_next_ms;
    uint32_t blink_start_ms;
    uint8_t blink_count;
    bool blink_active;
    /* gaze wander */
    uint32_t gaze_next_ms;
    float tgt_gl, tgt_gy, gl, gy;
    /* touch gaze override */
    uint32_t touch_until_ms;
    float touch_nx, touch_ny;
    uint32_t last_now_ms;
} dotii_engine_t;

void dotii_engine_init(dotii_engine_t *engine, uint32_t now_ms);
bool dotii_engine_set(dotii_engine_t *engine, const char *id_or_name, uint32_t now_ms);
bool dotii_engine_set_hold(dotii_engine_t *engine, const char *id_or_name,
                           uint32_t hold_ms, uint32_t now_ms);
void dotii_engine_set_gaze(dotii_engine_t *engine, float nx, float ny, uint32_t now_ms);
/* Advances evaluation; returns true when the active expression changed. */
bool dotii_engine_tick(dotii_engine_t *engine, uint32_t now_ms);
const dotii_engine_state_t *dotii_engine_state(const dotii_engine_t *engine);
const char *dotii_engine_current_id(const dotii_engine_t *engine);

#ifdef __cplusplus
}
#endif
