#include "dotii_render.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "dotii_render";

bool dotii_render_attach(dotii_render_t *render, lv_obj_t *parent, int size)
{
    if (render == NULL || parent == NULL) {
        return false;
    }
    if (render->attached) {
        return true;
    }
    memset(render, 0, sizeof(*render));

    const int32_t px = size > 0 ? size : 466;
    const size_t bytes = (size_t)px * (size_t)px * sizeof(uint16_t);
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "canvas allocation failed (%u bytes)", (unsigned)bytes);
        return false;
    }

    lv_obj_t *canvas = lv_canvas_create(parent);
    if (canvas == NULL) {
        heap_caps_free(buf);
        return false;
    }
    lv_canvas_set_buffer(canvas, buf, px, px, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_style_all(canvas);
    lv_obj_set_size(canvas, px, px);
    lv_obj_center(canvas);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);

    render->root = parent;
    render->canvas = canvas;
    render->buffer = buf;
    render->size = px;
    render->attached = true;
    ESP_LOGI(TAG, "dotii face attached: %dx%d rgb565 canvas", (int)px, (int)px);
    return true;
}

void dotii_render_update(dotii_render_t *render, const dotii_expr_t *expr,
                         const dotii_pose_t *pose)
{
    if (render == NULL || !render->attached || render->buffer == NULL ||
        expr == NULL || pose == NULL) {
        return;
    }
    const uint32_t now_ms = lv_tick_get();
    /* The face animates at LVGL timer rate (~30 Hz); skip duplicate frames
       when two timers land inside the same millisecond window. */
    if (render->last_frame_ms == now_ms) {
        return;
    }
    render->last_frame_ms = now_ms;

    /* Partial invalidation against the previous frame's rectangle. A NULL
       clip (first frame or expression switch) makes the renderer repaint the
       whole canvas, which also refreshes the cached background after an
       accent change. */
    dotii_rect_t clip;
    const dotii_rect_t *clip_arg = NULL;
    const bool full = render->force_full || !render->has_dirty || render->last_expr != expr;
    render->force_full = false;
    if (full) {
        render->has_dirty = false;
    } else {
        clip = render->last_dirty;
        clip_arg = &clip;
    }
    const int64_t raster_start = esp_timer_get_time();
    dotii_rect_t dirty;
    dotii_rect_t content;
    dotii_draw_render((uint16_t *)render->buffer, render->size, expr, pose, now_ms,
                      clip_arg, &dirty, &content);
    render->last_dirty = content;
    render->last_expr = expr;
    render->has_dirty = true;

    /* 1 Hz perf summary: how often the face is rasterised and how long it takes. */
    static uint32_t perf_frames;
    static int64_t perf_us;
    static int64_t perf_window_us;
    perf_frames++;
    perf_us += esp_timer_get_time() - raster_start;
    const int64_t perf_now_us = esp_timer_get_time();
    if (perf_window_us == 0) perf_window_us = perf_now_us;
    if (perf_now_us - perf_window_us >= 1000000) {
        ESP_LOGI(TAG, "face raster: %lu fps, avg %lld us",
                 (unsigned long)perf_frames,
                 (long long)(perf_us / (perf_frames ? perf_frames : 1)));
        perf_frames = 0;
        perf_us = 0;
        perf_window_us = perf_now_us;
    }
    lv_area_t area;
    area.x1 = (int32_t)dirty.x1;
    area.y1 = (int32_t)dirty.y1;
    area.x2 = (int32_t)dirty.x2;
    area.y2 = (int32_t)dirty.y2;
    lv_obj_invalidate_area(render->canvas, &area);
}

void dotii_render_invalidate_all(dotii_render_t *render)
{
    if (render == NULL) return;
    render->force_full = true;
    /* The next update's dirty rectangle covers the whole canvas because the
       renderer unions this rectangle in before clipping. */
    render->last_dirty.x1 = 0;
    render->last_dirty.y1 = 0;
    render->last_dirty.x2 = (int32_t)render->size - 1;
    render->last_dirty.y2 = (int32_t)render->size - 1;
    render->has_dirty = true;
}

void dotii_render_detach(dotii_render_t *render)
{
    if (render == NULL || !render->attached) {
        return;
    }
    if (render->canvas != NULL) {
        lv_obj_delete(render->canvas);
    }
    if (render->buffer != NULL) {
        heap_caps_free(render->buffer);
    }
    memset(render, 0, sizeof(*render));
}
