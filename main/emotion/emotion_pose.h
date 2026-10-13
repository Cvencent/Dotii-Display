#pragma once

#include <stdint.h>

#include "character_pack.h"
#include "emotion_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A renderer-agnostic description of how the face should look this frame.
   Offsets are in canvas units (240x240 design space, centre 120). */
typedef struct {
    float body_x;
    float body_y;
    float body_rotate_deg;
    float body_scale;
    float eye_openness;      /* 0..1.4 */
    float eye_scale;         /* 1.0 = neutral */
    float eye_look_x;        /* normalised -1..1 */
    float eye_look_y;
    float mouth_open;        /* 0..1 */
    float blush;             /* 0..1 */
    int eye_w;
    int eye_h;
} emotion_pose_t;

/* Projects the engine state and character anchors into a concrete pose.
   Pure function: no LVGL, no globals, safe to unit test on the host. */
void emotion_pose_eval(const emotion_engine_state_t *state,
                       const emotion_def_t *def,
                       const character_def_t *character,
                       emotion_pose_t *out);

#ifdef __cplusplus
}
#endif
