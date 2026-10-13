#pragma once

#include <stdbool.h>

#include "character_pack.h"
#include "emotion_engine.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL face renderer. Owns a small set of layer objects (body, two eyes,
   mouth, two blush patches) and applies an emotion_pose_t each frame. */
typedef struct {
    bool attached;
    lv_obj_t *root;
    lv_obj_t *body;
    lv_obj_t *eye_left;
    lv_obj_t *eye_right;
    lv_obj_t *mouth;
    lv_obj_t *blush_left;
    lv_obj_t *blush_right;
} emotion_render_t;

/* Creates the layer objects under parent. Idempotent. */
bool emotion_render_attach(emotion_render_t *render, lv_obj_t *parent, int canvas);

void emotion_render_update(emotion_render_t *render,
                           const emotion_engine_state_t *state,
                           const emotion_def_t *def,
                           const character_def_t *character);

void emotion_render_detach(emotion_render_t *render);

#ifdef __cplusplus
}
#endif
