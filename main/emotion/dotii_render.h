#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dotii_data.h"
#include "dotii_engine.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native full-resolution Dotii face renderer.

   One renderer instance owns one 466x466 RGB565 canvas allocated in PSRAM.
   update() rasterises the current engine pose through dotii_draw_render and
   invalidates the LVGL canvas. */
typedef struct {
    bool attached;
    lv_obj_t *root;
    lv_obj_t *canvas;
    void *buffer; /* RGB565 canvas pixels (PSRAM when available) */
    int32_t size;
    uint32_t last_frame_ms; /* skips redundant redraws within one tick window */
    dotii_rect_t last_dirty; /* previous frame's repaint rectangle */
    bool has_dirty;
    const dotii_expr_t *last_expr; /* expression switch forces a full repaint */
    /* Set when the canvas must be fully repainted on the next update. The
       canvas lives in PSRAM and is only ever written inside the previous
       frame's dirty rectangle, so the area outside it holds stale or
       uninitialised pixels; showing the canvas again (e.g. re-entering the
       Xiaozhi page) without a full repaint displays that garbage. */
    bool force_full;
} dotii_render_t;

/* Creates the canvas under parent at the given pixel size. Idempotent. */
bool dotii_render_attach(dotii_render_t *render, lv_obj_t *parent, int size);

/* Rasterises the engine pose for expr and refreshes the canvas. */
void dotii_render_update(dotii_render_t *render, const dotii_expr_t *expr,
                         const dotii_pose_t *pose);

/* Requests one complete repaint on the next update(). Call when the canvas
   becomes visible again so no stale pixels are shown. */
void dotii_render_invalidate_all(dotii_render_t *render);

void dotii_render_detach(dotii_render_t *render);

#ifdef __cplusplus
}
#endif
