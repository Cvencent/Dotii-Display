#include "emotion_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "emotion_geometry.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "emotion_render";

/* ---------------------------------------------------------------------------
 * The round panel itself is the face. Character colour fills the entire
 * canvas and the upstream eye rings are laid out directly in screen space.
 * There is deliberately no body silhouette or sphere shading here: those made
 * the display read as a small 3D head floating inside a black ring.
 * ------------------------------------------------------------------------- */

#define EB_PI 3.14159265f
#define EB_TAU 6.28318531f
#define EB_MAX_POINTS 96

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* -- colour helpers -------------------------------------------------------- */

static inline uint8_t c8(uint32_t rgb, int shift)
{
    return (uint8_t)((rgb >> shift) & 0xFFu);
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static inline uint32_t blend_rgb(uint32_t a, uint32_t b, uint32_t alpha256)
{
    const uint32_t inv = 256u - alpha256;
    const uint32_t ar = (a >> 16) & 0xFFu, ag = (a >> 8) & 0xFFu, ab = a & 0xFFu;
    const uint32_t br = (b >> 16) & 0xFFu, bg = (b >> 8) & 0xFFu, bb = b & 0xFFu;
    const uint32_t r = (ar * inv + br * alpha256 + 128u) >> 8;
    const uint32_t g = (ag * inv + bg * alpha256 + 128u) >> 8;
    const uint32_t bl = (ab * inv + bb * alpha256 + 128u) >> 8;
    return (r << 16) | (g << 8) | bl;
}

static inline int luma8(uint32_t rgb)
{
    const int r = (int)((rgb >> 16) & 0xFFu);
    const int g = (int)((rgb >> 8) & 0xFFu);
    const int b = (int)(rgb & 0xFFu);
    return (r * 54 + g * 183 + b * 19) >> 8;
}

/* Keep each character recognisable: emotion colour is a subtle tint of the
   character body, and the body is pushed away from the eye colour if the
   resulting pair would read as a flat silhouette. */
static uint32_t face_body_color(uint32_t base, uint32_t eye, uint32_t accent,
                                bool has_accent)
{
    uint32_t body = base;
    if (has_accent) {
        body = blend_rgb(body, accent, 96u);
    }
    for (int i = 0; i < 4 && abs(luma8(body) - luma8(eye)) < 72; ++i) {
        body = luma8(eye) > 128 ? blend_rgb(body, 0x000000u, 80u)
                                 : blend_rgb(body, 0xFFFFFFu, 80u);
    }
    return body;
}

/* -- canvas ---------------------------------------------------------------- */

typedef struct {
    uint16_t *px;
    int32_t size;
} eb_surface_t;

static inline void put_px(eb_surface_t *s, int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= s->size || y >= s->size) {
        return;
    }
    s->px[(size_t)y * (size_t)s->size + (size_t)x] = c;
}

/* Blend into the existing RGB565 canvas. Eye polygons cover only a small part
   of the panel, so this integer coverage blend is cheap enough to run at
   animation rate and removes the hard, jagged polygon edges. */
static inline void blend_px(eb_surface_t *s, int x, int y, uint16_t color, uint32_t alpha)
{
    if (x < 0 || y < 0 || x >= s->size || y >= s->size || alpha == 0u) {
        return;
    }
    if (alpha >= 256u) {
        s->px[(size_t)y * (size_t)s->size + (size_t)x] = color;
        return;
    }

    const uint16_t dst = s->px[(size_t)y * (size_t)s->size + (size_t)x];
    const uint32_t inv = 256u - alpha;
    const uint32_t dr = (dst >> 11) & 0x1Fu;
    const uint32_t dg = (dst >> 5) & 0x3Fu;
    const uint32_t db = dst & 0x1Fu;
    const uint32_t sr = (color >> 11) & 0x1Fu;
    const uint32_t sg = (color >> 5) & 0x3Fu;
    const uint32_t sb = color & 0x1Fu;
    const uint32_t r = (dr * inv + sr * alpha + 128u) >> 8;
    const uint32_t g = (dg * inv + sg * alpha + 128u) >> 8;
    const uint32_t b = (db * inv + sb * alpha + 128u) >> 8;
    s->px[(size_t)y * (size_t)s->size + (size_t)x] =
        (uint16_t)((r << 11) | (g << 5) | b);
}

/* -- animation primitives (ported from engine.js ANIM_TYPES) ---------------- */

typedef enum {
    EB_ANIM_SINE = 0,
    EB_ANIM_GLANCE = 1,
    EB_ANIM_PULSE = 2,
    EB_ANIM_JITTER = 3,
    EB_ANIM_SCAN = 4,
    EB_ANIM_BLINK = 5,
} eb_anim_type_t;

typedef enum {
    EB_TARGET_EYES = 0,
    EB_TARGET_BODY = 1,
    EB_TARGET_LEFT = 2,
    EB_TARGET_RIGHT = 3,
} eb_anim_target_t;

typedef enum {
    EB_PROP_LOOK_X = 0,
    EB_PROP_LOOK_Y = 1,
    EB_PROP_X = 2,
    EB_PROP_Y = 3,
    EB_PROP_SCALE = 4,
    EB_PROP_OPEN = 5,
    EB_PROP_ROTATE = 6,
} eb_anim_prop_t;

static float eb_anim_eval(const eb_anim_t *a, float t_ms, float seed)
{
    switch (a->type) {
    case EB_ANIM_SINE: {
        const float period = a->period > 0.0f ? a->period : 2000.0f;
        return a->amp * sinf(EB_TAU * t_ms / period + a->phase);
    }
    case EB_ANIM_PULSE: {
        const float period = a->period > 0.0f ? a->period : 1000.0f;
        return a->amp * 0.5f * (1.0f - cosf(EB_TAU * t_ms / period + a->phase));
    }
    case EB_ANIM_JITTER: {
        const float s = t_ms / 1000.0f * 8.0f;
        return (sinf(s * 3.1f + seed) + sinf(s * 5.7f + seed * 2.3f) +
                sinf(s * 9.3f + seed * 4.1f)) / 3.0f * a->amp;
    }
    case EB_ANIM_SCAN: {
        const float period = a->period > 0.0f ? a->period : 800.0f;
        const float p = fmodf(t_ms, period) / period;
        return a->amp * (p < 0.5f ? p * 4.0f - 1.0f : 3.0f - p * 4.0f);
    }
    case EB_ANIM_GLANCE: {
        const float period = a->period > 0.0f ? a->period : 3600.0f;
        const float ph = EB_TAU * (fmodf(t_ms, period) / period) + a->phase;
        return a->amp * tanhf(2.8f * sinf(ph));
    }
    case EB_ANIM_BLINK: {
        /* Interval-driven blink; the renderer drives this with its own clock. */
        const float interval = a->period > 0.0f ? a->period : 3800.0f;
        const float dur = 200.0f;
        const float p = fmodf(t_ms + seed * 97.0f, interval);
        if (p >= dur) {
            return 0.0f;
        }
        return -a->amp * sinf(EB_PI * (p / dur));
    }
    default:
        return 0.0f;
    }
}
/* ---------------------------------------------------------------------------
 * Pose assembly: the upstream per-look body/eye spec plus the live animation.
 * ------------------------------------------------------------------------- */

typedef struct {
    float body_x, body_y, body_rotate, body_scale;
    float eye_x[3], eye_y[3], eye_look_x[3], eye_look_y[3];
    float eye_scale_x[3], eye_scale_y[3], eye_rotate[3];
    float openness;
    float seed;
} eb_pose_t;

/* Slot 0 = "both" base, slot 1 = left override, slot 2 = right override. */
static void eb_pose_from_look(const eb_look_t *look, float t_ms, float openness,
                              float gaze_x, float gaze_y, eb_pose_t *pose)
{
    memset(pose, 0, sizeof(*pose));
    pose->body_x = look->body[0];
    pose->body_y = look->body[1];
    pose->body_rotate = look->body[2];
    pose->body_scale = look->body[3] > 0.0f ? look->body[3] : 1.0f;
    for (int i = 0; i < 3; ++i) {
        pose->eye_x[i] = look->eye[0][i];
        pose->eye_y[i] = look->eye[1][i];
        pose->eye_look_x[i] = look->eye[2][i];
        pose->eye_look_y[i] = look->eye[3][i];
        pose->eye_scale_x[i] = look->eye[4][i];
        pose->eye_scale_y[i] = look->eye[5][i];
        pose->eye_rotate[i] = look->eye[6][i];
    }
    pose->openness = openness;
    pose->seed = (float)(look - emotion_geometry_look(0)) * 0.37f;

    for (uint8_t i = 0; i < look->anim_count && i < EB_ANIM_SLOTS; ++i) {
        const eb_anim_t *a = &look->anims[i];
        const float v = eb_anim_eval(a, t_ms, pose->seed);
        if (a->target == EB_TARGET_BODY) {
            switch (a->prop) {
            case EB_PROP_X:      pose->body_x += v; break;
            case EB_PROP_Y:      pose->body_y += v; break;
            case EB_PROP_ROTATE: pose->body_rotate += v; break;
            case EB_PROP_SCALE:  pose->body_scale += v; break;
            default: break;
            }
            continue;
        }
        /* eyes / left / right: slot index is 1 or 2, eyes applies to both. */
        const int first = a->target == EB_TARGET_LEFT ? 1 : (a->target == EB_TARGET_RIGHT ? 2 : 0);
        const int last = a->target == EB_TARGET_EYES ? 2 : first;
        for (int k = first; k <= last; ++k) {
            switch (a->prop) {
            case EB_PROP_LOOK_X: pose->eye_look_x[k] += v; break;
            case EB_PROP_LOOK_Y: pose->eye_look_y[k] += v; break;
            case EB_PROP_X:      pose->eye_x[k] += v; break;
            case EB_PROP_Y:      pose->eye_y[k] += v; break;
            case EB_PROP_SCALE:  pose->eye_scale_x[k] += v; pose->eye_scale_y[k] += v; break;
            case EB_PROP_OPEN:   pose->openness += v; break;
            case EB_PROP_ROTATE: pose->eye_rotate[k] += v; break;
            default: break;
            }
        }
    }

    /* Character gaze (touch / viewer tracking) is layered on top of drift. */
    for (int k = 0; k < 3; ++k) {
        pose->eye_look_x[k] += gaze_x;
        pose->eye_look_y[k] += gaze_y;
    }
}

/* Subsampled scanline polygon fill. Four sub-scanlines per output row give
   real vertical coverage instead of the previous hard floor/ceil edge, which
   is what produced the stair-stepped "毛刺" on curved eye rings. */
#define EB_SUBSCAN 8u
#define EB_COV_ROW_PIXELS 512u
static float s_sub_edges[EB_SUBSCAN][EB_MAX_POINTS];
static int s_sub_edge_count[EB_SUBSCAN];
static uint16_t s_cov_row[EB_COV_ROW_PIXELS];

static void fill_polygon(eb_surface_t *surf, const float *xs, const float *ys, int n,
                         uint16_t color)
{
    if (surf->size > (int32_t)EB_COV_ROW_PIXELS) {
        return;
    }
    float ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; ++i) {
        ymin = ys[i] < ymin ? ys[i] : ymin;
        ymax = ys[i] > ymax ? ys[i] : ymax;
    }
    const int y0 = clampi((int)floorf(ymin), 0, surf->size - 1);
    const int y1 = clampi((int)ceilf(ymax), 0, surf->size - 1);

    for (int y = y0; y <= y1; ++y) {
        int min_x = surf->size;
        int max_x = -1;
        for (uint32_t s = 0; s < EB_SUBSCAN; ++s) {
            const float scan = (float)y + ((float)s + 0.5f) / (float)EB_SUBSCAN;
            int count = 0;
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                const float ay = ys[i], by = ys[j];
                if ((ay <= scan && by > scan) || (by <= scan && ay > scan)) {
                    if (count < EB_MAX_POINTS) {
                        s_sub_edges[s][count++] =
                            xs[i] + (scan - ay) / (by - ay) * (xs[j] - xs[i]);
                    }
                }
            }
            s_sub_edge_count[s] = count;
            for (int a = 1; a < count; ++a) {
                const float key = s_sub_edges[s][a];
                int b = a - 1;
                while (b >= 0 && s_sub_edges[s][b] > key) {
                    s_sub_edges[s][b + 1] = s_sub_edges[s][b];
                    --b;
                }
                s_sub_edges[s][b + 1] = key;
            }
            if (count >= 2) {
                const int xa = clampi((int)floorf(s_sub_edges[s][0]), 0, surf->size - 1);
                const int xb = clampi((int)ceilf(s_sub_edges[s][count - 1]) - 1,
                                      0, surf->size - 1);
                if (xa < min_x) min_x = xa;
                if (xb > max_x) max_x = xb;
            }
        }
        if (min_x > max_x) {
            continue;
        }
        for (int x = min_x; x <= max_x; ++x) {
            s_cov_row[x] = 0;
        }
        for (uint32_t s = 0; s < EB_SUBSCAN; ++s) {
            const int count = s_sub_edge_count[s];
            for (int a = 0; a + 1 < count; a += 2) {
                const float span_left = s_sub_edges[s][a];
                const float span_right = s_sub_edges[s][a + 1];
                const int xa = clampi((int)floorf(span_left), min_x, max_x);
                const int xb = clampi((int)ceilf(span_right) - 1, min_x, max_x);
                for (int x = xa; x <= xb; ++x) {
                    const float overlap_left = fmaxf(span_left, (float)x);
                    const float overlap_right = fminf(span_right, (float)x + 1.0f);
                    const float coverage = clampf(overlap_right - overlap_left, 0.0f, 1.0f);
                    if (coverage > 0.0f) {
                        uint32_t acc = (uint32_t)s_cov_row[x] +
                            (uint32_t)(coverage * (256.0f / (float)EB_SUBSCAN) + 0.5f);
                        s_cov_row[x] = (uint16_t)(acc > 255u ? 255u : acc);
                    }
                }
            }
        }
        for (int x = min_x; x <= max_x; ++x) {
            if (s_cov_row[x] > 0u) {
                blend_px(surf, x, y, color, s_cov_row[x]);
            }
        }
    }
}

static void eb_centroid(const int16_t *ring, float *cx, float *cy)
{
    const uint16_t n = emotion_geometry_ring_points();
    float x = 0.0f, y = 0.0f;
    for (uint16_t i = 0; i < n; ++i) {
        x += (float)ring[i * 2] / 100.0f;
        y += (float)ring[i * 2 + 1] / 100.0f;
    }
    *cx = x / (float)n;
    *cy = y / (float)n;
}

/* Place one expression ring directly on the screen. The ring supplies only
   the eye's shape; all horizontal and vertical layout is done in screen
   space, independent of the removed body silhouette. */
static void eb_eye_points(const eb_pose_t *pose, const eb_shape_t *shape,
                          const int16_t *ring, int slot, float size,
                          float eye_scale, float *sx_out, float *sy_out)
{
    const uint16_t n = emotion_geometry_ring_points();
    float base_x, base_y;
    eb_centroid(ring, &base_x, &base_y);

    const float open = clampf(pose->openness, 0.02f, 2.4f);
    const float eye_shape_scale = clampf(shape->face.eye, 0.68f, 1.08f);
    float local_scale = eye_scale * eye_shape_scale;

    /* Keep the widest expression rings inside the round panel without making
       the common expressions smaller. */
    float min_ring_x = (float)ring[0] / 100.0f;
    float max_ring_x = min_ring_x;
    float min_ring_y = (float)ring[1] / 100.0f;
    float max_ring_y = min_ring_y;
    for (uint16_t i = 1; i < n; ++i) {
        const float px = (float)ring[i * 2] / 100.0f;
        const float py = (float)ring[i * 2 + 1] / 100.0f;
        min_ring_x = px < min_ring_x ? px : min_ring_x;
        max_ring_x = px > max_ring_x ? px : max_ring_x;
        min_ring_y = py < min_ring_y ? py : min_ring_y;
        max_ring_y = py > max_ring_y ? py : max_ring_y;
    }
    const float extent = fmaxf(max_ring_x - min_ring_x, max_ring_y - min_ring_y);
    const float max_extent = size * 0.44f;
    if (extent * local_scale > max_extent) {
        local_scale = max_extent / fmaxf(extent, 0.001f);
    }

    const float body_scale = clampf(pose->body_scale, 0.86f, 1.14f);
    const float base_eye_x = slot == 1 ? size * 0.315f : size * 0.685f;
    const float base_eye_y = size * 0.44f;
    const float body_dx = pose->body_x * eye_scale * 0.55f;
    const float body_dy = pose->body_y * eye_scale * 0.55f;
    const float motion_x = (pose->eye_x[slot] + pose->eye_look_x[slot]) * eye_scale * 0.8f;
    const float motion_y = (pose->eye_y[slot] + pose->eye_look_y[slot]) * eye_scale * 0.8f;

    float ex = size * 0.5f + (base_eye_x - size * 0.5f) * body_scale + body_dx + motion_x;
    float ey = base_eye_y + body_dy + motion_y;

    /* The panel colour does not move, but a little of the body rotation is
       kept as a face tilt so the expressions still have personality. */
    const float body_rot = pose->body_rotate * EB_PI / 180.0f * 0.30f;
    if (fabsf(body_rot) > 0.0001f) {
        const float dx = ex - size * 0.5f;
        const float dy = ey - size * 0.5f;
        const float cr = cosf(body_rot);
        const float sr = sinf(body_rot);
        ex = size * 0.5f + dx * cr - dy * sr;
        ey = size * 0.5f + dx * sr + dy * cr;
    }

    ex = clampf(ex, size * 0.10f, size * 0.90f);
    ey = clampf(ey, size * 0.12f, size * 0.88f);

    const float rot = pose->eye_rotate[slot] * EB_PI / 180.0f;
    const float cr = cosf(rot), sr = sinf(rot);
    const float sx = local_scale * clampf(pose->eye_scale_x[slot], 0.2f, 2.2f);
    const float sy = local_scale * clampf(pose->eye_scale_y[slot], 0.2f, 2.2f) * open;
    for (uint16_t i = 0; i < n; ++i) {
        const float px = (float)ring[i * 2] / 100.0f - base_x;
        const float py = (float)ring[i * 2 + 1] / 100.0f - base_y;
        const float rx = px * sx;
        const float ry = py * sy;
        const float tx = rx * cr - ry * sr;
        const float ty = rx * sr + ry * cr;
        sx_out[i] = ex + tx;
        sy_out[i] = ey + ty;
    }
}

/* The round panel is the face, so the body colour is a flat full-screen fill.
   No silhouette, radial highlight, or edge vignette belongs here. */
static void eb_fill_body(eb_surface_t *surf, uint32_t body_rgb)
{
    const uint16_t color565 = rgb565(c8(body_rgb, 16), c8(body_rgb, 8), c8(body_rgb, 0));
    const size_t total = (size_t)surf->size * (size_t)surf->size;
    const uint32_t color32 = (uint32_t)color565 | ((uint32_t)color565 << 16);
    uint32_t *dst32 = (uint32_t *)surf->px;
    const size_t pairs = total / 2u;
    for (size_t i = 0; i < pairs; ++i) {
        dst32[i] = color32;
    }
    if ((total & 1u) != 0u) {
        surf->px[total - 1u] = color565;
    }
}

static void eb_fill_rect(eb_surface_t *surf, int32_t x1, int32_t y1,
                         int32_t x2, int32_t y2, uint32_t body_rgb)
{
    const uint16_t color565 = rgb565(c8(body_rgb, 16), c8(body_rgb, 8), c8(body_rgb, 0));
    x1 = clampi(x1, 0, surf->size - 1);
    y1 = clampi(y1, 0, surf->size - 1);
    x2 = clampi(x2, 0, surf->size - 1);
    y2 = clampi(y2, 0, surf->size - 1);
    for (int32_t y = y1; y <= y2; ++y) {
        uint16_t *row = surf->px + (size_t)y * (size_t)surf->size;
        for (int32_t x = x1; x <= x2; ++x) {
            row[x] = color565;
        }
    }
}

/* -- public API ------------------------------------------------------------ */

bool emotion_render_attach(emotion_render_t *render, lv_obj_t *parent, int size)
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
    render->shape_index = 0;
    render->ring_index = 0;
    render->ring_valid = false;
    render->dirty_x1 = 0;
    render->dirty_y1 = 0;
    render->dirty_x2 = px - 1;
    render->dirty_y2 = px - 1;
    render->full_redraw = true;
    render->last_body_color = 0;
    render->attached = true;
    return true;
}

void emotion_render_set_shape(emotion_render_t *render, const char *shape_id)
{
    if (render == NULL || !render->attached) {
        return;
    }
    const uint8_t idx = emotion_geometry_shape_index(shape_id);
    if (idx != render->shape_index) {
        render->shape_index = idx;
        render->ring_valid = false;
        render->full_redraw = true;
    }
}

void emotion_render_detach(emotion_render_t *render)
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

/* Maps the active emotion id onto the upstream look table. */
static const eb_look_t *resolve_look(const emotion_def_t *def)
{
    int8_t idx = -1;
    if (def != NULL) {
        idx = emotion_geometry_look_index(def->id);
        if (idx < 0 && def->name != NULL) {
            idx = emotion_geometry_look_index(def->name);
        }
    }
    if (idx < 0) {
        idx = emotion_geometry_look_index("02");
    }
    if (idx < 0) {
        idx = 0;
    }
    return emotion_geometry_look((uint8_t)idx);
}
/* ---------------------------------------------------------------------------
 * Frame compositor: assemble one pose and rasterise it into the canvas.
 * ------------------------------------------------------------------------- */

#define EB_BLINK_DURATION_MS 170.0f

/* The upstream engine walks its expression pool on a random interval between
   poolMs[0] and poolMs[1]. The firmware has no RNG state in the draw path, so
   the pool advances on the mean interval instead. */
static uint8_t eb_pool_expression(const eb_look_t *look, uint32_t now_ms)
{
    if (look->pool_length == 0u) {
        return 0u;
    }
    uint32_t interval = ((uint32_t)look->pool_ms_min + (uint32_t)look->pool_ms_max) / 2u;
    if (interval < 800u) {
        interval = 800u;
    }
    return look->pool[(now_ms / interval) % look->pool_length];
}

/* Blink curve from engine.js: within each interval the lids close for a short
   window, otherwise the eye stays fully open. */
static float eb_blink_scale(const eb_look_t *look, uint32_t now_ms, float seed)
{
    if (look->blink_ms_min == 0u || look->blink_ms_max == 0u) {
        return 1.0f;
    }
    const float interval = 0.5f * (float)((uint32_t)look->blink_ms_min +
                                          (uint32_t)look->blink_ms_max);
    const float phase = fmodf((float)now_ms + seed * 97.0f, interval);
    if (phase >= EB_BLINK_DURATION_MS) {
        return 1.0f;
    }
    return 1.0f - 0.96f * sinf(EB_PI * phase / EB_BLINK_DURATION_MS);
}

void emotion_render_update(emotion_render_t *render,
                           const emotion_engine_state_t *state,
                           const emotion_def_t *def,
                           const character_def_t *character)
{
    if (render == NULL || !render->attached || render->buffer == NULL) {
        return;
    }
    const character_def_t *pack = character != NULL ? character : character_pack_fallback();
    const eb_look_t *look = resolve_look(def);
    const eb_shape_t *shape = emotion_geometry_shape(render->shape_index);
    const uint32_t now_ms = lv_tick_get();
    const float size = (float)render->size;

    float gaze_x = 0.0f;
    float gaze_y = 0.0f;
    if (state != NULL && (def == NULL || def->gaze)) {
        gaze_x = clampf(state->gaze_x, -1.0f, 1.0f) * 24.0f;
        gaze_y = clampf(state->gaze_y, -1.0f, 1.0f) * 15.0f;
    }

    const float openness = def != NULL ? def->openness : 1.0f;
    eb_pose_t pose;
    eb_pose_from_look(look, (float)now_ms, openness, gaze_x, gaze_y, &pose);
    pose.openness = clampf(pose.openness, 0.02f, 2.4f) *
                    eb_blink_scale(look, now_ms, pose.seed);

    eb_surface_t surface;
    surface.px = (uint16_t *)render->buffer;
    surface.size = render->size;

    uint32_t accent = 0;
    const bool has_accent = look->color_index != EB_NO_COLOR;
    if (has_accent) {
        accent = emotion_geometry_body_colors()[look->color_index];
    }
    const uint32_t body_rgb = face_body_color(pack->body_color, pack->eye_color,
                                              accent, has_accent);
    const bool full_redraw = render->full_redraw || body_rgb != render->last_body_color;

    const uint8_t expression = eb_pool_expression(look, now_ms);
    const uint16_t eye_color = rgb565(c8(pack->eye_color, 16),
                                      c8(pack->eye_color, 8),
                                      c8(pack->eye_color, 0));
    const uint16_t ring_points = emotion_geometry_ring_points();
    const float eye_scale = size * 0.0043f;
    float point_x[2][EB_MAX_POINTS];
    float point_y[2][EB_MAX_POINTS];
    float min_x = size, min_y = size, max_x = -1.0f, max_y = -1.0f;
    for (int eye = 0; eye < 2; ++eye) {
        const int16_t *ring = emotion_geometry_ring(expression, (uint8_t)eye);
        const int slot = eye == 0 ? 1 : 2;
        eb_eye_points(&pose, shape, ring, slot, size, eye_scale,
                      point_x[eye], point_y[eye]);
        for (uint16_t i = 0; i < ring_points; ++i) {
            if (point_x[eye][i] < min_x) min_x = point_x[eye][i];
            if (point_y[eye][i] < min_y) min_y = point_y[eye][i];
            if (point_x[eye][i] > max_x) max_x = point_x[eye][i];
            if (point_y[eye][i] > max_y) max_y = point_y[eye][i];
        }
    }
    if (max_x < min_x || max_y < min_y) {
        return;
    }
    const int32_t new_x1 = clampi((int32_t)floorf(min_x) - 2, 0, render->size - 1);
    const int32_t new_y1 = clampi((int32_t)floorf(min_y) - 2, 0, render->size - 1);
    const int32_t new_x2 = clampi((int32_t)ceilf(max_x) + 2, 0, render->size - 1);
    const int32_t new_y2 = clampi((int32_t)ceilf(max_y) + 2, 0, render->size - 1);

    int32_t paint_x1 = 0, paint_y1 = 0;
    int32_t paint_x2 = render->size - 1, paint_y2 = render->size - 1;
    if (full_redraw) {
        eb_fill_body(&surface, body_rgb);
    } else {
        paint_x1 = render->dirty_x1 < new_x1 ? render->dirty_x1 : new_x1;
        paint_y1 = render->dirty_y1 < new_y1 ? render->dirty_y1 : new_y1;
        paint_x2 = render->dirty_x2 > new_x2 ? render->dirty_x2 : new_x2;
        paint_y2 = render->dirty_y2 > new_y2 ? render->dirty_y2 : new_y2;
        eb_fill_rect(&surface, paint_x1, paint_y1, paint_x2, paint_y2, body_rgb);
    }

    for (int eye = 0; eye < 2; ++eye) {
        fill_polygon(&surface, point_x[eye], point_y[eye], ring_points, eye_color);
    }

    /* The canvas is 1:1 with the round panel and is anchored at (0,0), so
       the dirty rectangle is already in display coordinates. */
    lv_area_t area = {
        .x1 = full_redraw ? 0 : paint_x1,
        .y1 = full_redraw ? 0 : paint_y1,
        .x2 = full_redraw ? render->size - 1 : paint_x2,
        .y2 = full_redraw ? render->size - 1 : paint_y2,
    };
    lv_obj_invalidate_area(render->canvas, &area);

    render->dirty_x1 = new_x1;
    render->dirty_y1 = new_y1;
    render->dirty_x2 = new_x2;
    render->dirty_y2 = new_y2;
    render->last_body_color = body_rgb;
    render->full_redraw = false;
}

