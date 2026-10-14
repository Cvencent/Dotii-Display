#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "character_pack.h"
#include "emotion_engine.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native full-resolution face renderer.

   The upstream aora-bot ball is a 259-unit SVG rendered through DOM
   transforms: a body ring plus 48-point eye rings projected onto the sphere.
   This renderer reproduces that geometry exactly, rasterised into an LVGL
   canvas at the panel's native 466x466 resolution so nothing is upscaled.

   One renderer instance owns one canvas object; attach() allocates the canvas
   and its pixel buffer in PSRAM. */
typedef struct {
    bool attached;
    lv_obj_t *root;        /* host object passed by the caller */
    lv_obj_t *canvas;
    void *buffer;          /* RGB565 canvas pixels in PSRAM */
    int32_t size;
    uint8_t shape_index;
    uint8_t ring_index;    /* last expression ring pair drawn */
    bool ring_valid;
    /* Dirty rectangle of the animated eye region in canvas-local pixel
       coordinates. Body colour changes mark the whole canvas dirty. */
    int32_t dirty_x1;
    int32_t dirty_y1;
    int32_t dirty_x2;
    int32_t dirty_y2;
    bool full_redraw;
    uint32_t last_body_color;
} emotion_render_t;

/* Creates the canvas under parent at the given pixel size. Idempotent. */
bool emotion_render_attach(emotion_render_t *render, lv_obj_t *parent, int canvas);

/* Rasterises one pose. The character pack supplies the body ring and palette
   when the active look does not override them. */
void emotion_render_update(emotion_render_t *render,
                           const emotion_engine_state_t *state,
                           const emotion_def_t *def,
                           const character_def_t *character);

/* Selects the upstream body shape (blob/wedge/gem) for this renderer. */
void emotion_render_set_shape(emotion_render_t *render, const char *shape_id);

void emotion_render_detach(emotion_render_t *render);

#ifdef __cplusplus
}
#endif
