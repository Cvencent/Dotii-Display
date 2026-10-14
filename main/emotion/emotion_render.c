#include "emotion_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "emotion_geometry.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "emotion_render";

/* ---------------------------------------------------------------------------
 * Upstream geometry is defined in a 259-unit viewBox (-15..244). The canvas is
 * square, so a single scale maps both axes; the head is drawn slightly above
 * the vertical centre to leave room for the status pill underneath.
 * ------------------------------------------------------------------------- */

#define EB_PI 3.14159265f
#define EB_TAU 6.28318531f
#define EB_MAX_POINTS 96

typedef struct {
    float x;
    float y;
    float z;
} eb_pt_t;

typedef struct {
    float xmin;
    float xmax;
    float ymin;
    float ymax;
} eb_bbox_t;

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

/* Upstream shade(color, amount): blend towards white for amount > 0 and
   towards black for amount < 0, matching ball.js. */
static uint16_t shade565(uint32_t rgb, float amount)
{
    const float target = amount < 0.0f ? 0.0f : 255.0f;
    const float a = fabsf(amount);
    const float r = (float)c8(rgb, 16) + (target - (float)c8(rgb, 16)) * a;
    const float g = (float)c8(rgb, 8) + (target - (float)c8(rgb, 8)) * a;
    const float b = (float)c8(rgb, 0) + (target - (float)c8(rgb, 0)) * a;
    return rgb565((uint8_t)(r + 0.5f), (uint8_t)(g + 0.5f), (uint8_t)(b + 0.5f));
}

static inline uint16_t lerp565(uint16_t a, uint16_t b, float t)
{
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int r = ar + (int)((float)(br - ar) * t + 0.5f);
    const int g = ag + (int)((float)(bg - ag) * t + 0.5f);
    const int bl = ab + (int)((float)(bb - ab) * t + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | bl);
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

/* ---------------------------------------------------------------------------
 * Geometry: body outline, silhouette table and the sphere-projected eyes.
 * ------------------------------------------------------------------------- */

/* Transform a source point (x100 fixed point) through the body pose. */
static inline void body_point(const eb_pose_t *pose, const eb_shape_t *shape,
                              float *sx, float *sy, float *dx, float *dy)
{
    const float x = *sx - eb_head_c;
    const float y = *sy - eb_head_c;
    const float c = cosf(pose->body_rotate * EB_PI / 180.0f);
    const float s = sinf(pose->body_rotate * EB_PI / 180.0f);
    const float k = pose->body_scale;
    *dx = eb_head_c + (x * c - y * s) * k + pose->body_x;
    *dy = eb_head_c + (x * s + y * c) * k + pose->body_y;
    (void)shape;
}

/* Scanline polygon fill, used for both the body and the eye rings. */
static void fill_polygon(eb_surface_t *surf, const float *xs, const float *ys, int n,
                         uint16_t color)
{
    float ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; ++i) {
        ymin = ys[i] < ymin ? ys[i] : ymin;
        ymax = ys[i] > ymax ? ys[i] : ymax;
    }
    const int y0 = clampi((int)floorf(ymin), 0, surf->size - 1);
    const int y1 = clampi((int)ceilf(ymax), 0, surf->size - 1);
    float xint[EB_MAX_POINTS];
    for (int y = y0; y <= y1; ++y) {
        const float scan = (float)y + 0.5f;
        int count = 0;
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const float ay = ys[i], by = ys[j];
            if ((ay <= scan && by > scan) || (by <= scan && ay > scan)) {
                if (count < EB_MAX_POINTS) {
                    xint[count++] = xs[i] + (scan - ay) / (by - ay) * (xs[j] - xs[i]);
                }
            }
        }
        if (count < 2) {
            continue;
        }
        for (int a = 1; a < count; ++a) {
            const float key = xint[a];
            int b = a - 1;
            while (b >= 0 && xint[b] > key) {
                xint[b + 1] = xint[b];
                --b;
            }
            xint[b + 1] = key;
        }
        for (int a = 0; a + 1 < count; a += 2) {
            const int xa = clampi((int)ceilf(xint[a] - 0.5f), 0, surf->size - 1);
            const int xb = clampi((int)floorf(xint[a + 1] - 0.5f), 0, surf->size - 1);
            for (int x = xa; x <= xb; ++x) {
                put_px(surf, x, y, color);
            }
        }
    }
}
/* Silhouette table: per 2-unit row, the body's left/right extremes. Mirrors
   ball.js building silRows from the head ring. */
typedef struct {
    float min_y;
    float max_y;
    float step;
    int rows;
    float lo[128];
    float hi[128];
} eb_sil_t;

static void eb_sil_build(eb_sil_t *sil, const float *body_x, const float *body_y, int n)
{
    if (n <= 0) {
        memset(sil, 0, sizeof(*sil));
        return;
    }
    float mn = body_y[0], mx = body_y[0];
    for (int i = 1; i < n; ++i) {
        mn = body_y[i] < mn ? body_y[i] : mn;
        mx = body_y[i] > mx ? body_y[i] : mx;
    }
    sil->min_y = mn;
    sil->max_y = mx;
    sil->step = 2.0f;
    int rows = (int)((mx - mn) / sil->step) + 1;
    if (rows > 128) {
        rows = 128;
    }
    sil->rows = rows;
    for (int r = 0; r < rows; ++r) {
        const float y = mn + (float)r * sil->step;
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const float y0 = body_y[i], y1 = body_y[j];
            if ((y0 <= y && y1 >= y) || (y1 <= y && y0 >= y)) {
                const float t = (y1 == y0) ? 0.0f : (y - y0) / (y1 - y0);
                const float x = body_x[i] + (body_x[j] - body_x[i]) * t;
                lo = x < lo ? x : lo;
                hi = x > hi ? x : hi;
            }
        }
        if (lo > hi) {
            lo = eb_head_c - 4.0f;
            hi = eb_head_c + 4.0f;
        }
        sil->lo[r] = lo;
        sil->hi[r] = hi;
    }
}

static void eb_sil_at(const eb_sil_t *sil, float y, float *lo, float *hi)
{
    const float clamped = clampf(y, sil->min_y, sil->max_y);
    int r = (int)((clamped - sil->min_y) / sil->step + 0.5f);
    r = clampi(r, 0, sil->rows - 1);
    *lo = sil->lo[r];
    *hi = sil->hi[r];
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

/* Project one eye ring using the exact ball.js setEye maths, returning the
   transformed outline in screen space. Returns false when the eye rotates
   past the sphere's terminator and must be hidden. */
static bool eb_eye_points(const eb_pose_t *pose, const eb_shape_t *shape,
                          const int16_t *ring, int slot, const eb_sil_t *sil,
                          float scale, float offset_x, float offset_y,
                          float *sx_out, float *sy_out)
{
    const uint16_t n = emotion_geometry_ring_points();
    float base_x, base_y;
    eb_centroid(ring, &base_x, &base_y);

    const float open = clampf(pose->openness, 0.02f, 2.4f);
    const float sy = clampf(pose->eye_scale_y[slot] * open * shape->face.eye, 0.02f, 2.4f);
    const float sx_base = pose->eye_scale_x[slot] * shape->face.eye;

    const float half_h = eb_eye_half * sy + 2.0f;
    float ey0 = eb_head_c + shape->face.y + (base_y - eb_head_c) * shape->face.sy +
                pose->eye_y[slot] + pose->eye_look_y[slot];
    ey0 = clampf(ey0, sil->min_y + half_h, sil->max_y - half_h);

    float sil_lo, sil_hi;
    eb_sil_at(sil, ey0, &sil_lo, &sil_hi);
    const float cx0 = (sil_lo + sil_hi) * 0.5f;
    const float hw = fmaxf((sil_hi - sil_lo) * 0.5f, 12.0f);

    const float ox = shape->face.x + (base_x - eb_head_c) * shape->face.sx +
                     pose->eye_x[slot] + pose->eye_look_x[slot];
    const float theta = clampf(ox / hw, -1.15f, 1.15f);
    const float cn = cosf(theta);
    if (cn <= 0.02f) {
        return false;
    }
    const float ex = cx0 + hw * sinf(theta) * 0.985f;
    const float dyn = (ey0 - eb_head_c) / 130.0f;
    const float fy = sqrtf(1.0f - dyn * dyn * 0.22f);

    const float rot = pose->eye_rotate[slot] * EB_PI / 180.0f;
    const float cr = cosf(rot), sr = sinf(rot);
    const float sx = sx_base * cn;
    const float syy = sy * fy;
    for (uint16_t i = 0; i < n; ++i) {
        const float px = (float)ring[i * 2] / 100.0f - base_x;
        const float py = (float)ring[i * 2 + 1] / 100.0f - base_y;
        const float rx = px * sx;
        const float ry = py * syy;
        const float tx = rx * cr - ry * sr;
        const float ty = rx * sr + ry * cr;
        sx_out[i] = (ex + tx + offset_x) * scale;
        sy_out[i] = (ey0 + ty + offset_y) * scale;
    }
    return true;
}
/* Body radial gradient matching the upstream SVG (cx 38%, cy 32%, r 75%):
   shade(+0.22) -> base at 62% -> shade(-0.12) at 100%. */
static void eb_fill_body(eb_surface_t *surf, const float *bx, const float *by, int n,
                         const eb_shape_t *shape, uint32_t body_rgb, float scale,
                         float offset_x, float offset_y)
{
    const uint16_t c0 = shade565(body_rgb, 0.22f);
    const uint16_t c1 = rgb565(c8(body_rgb, 16), c8(body_rgb, 8), c8(body_rgb, 0));
    const uint16_t c2 = shade565(body_rgb, -0.12f);

    const float gx = (eb_head_c + (0.38f - 0.5f) * eb_view_span + offset_x) * scale;
    const float gy = (eb_head_c + (0.32f - 0.5f) * eb_view_span + offset_y) * scale;
    const float gr = 0.75f * eb_view_span * scale;

    float ymin = by[0], ymax = by[0];
    for (int i = 1; i < n; ++i) {
        ymin = by[i] < ymin ? by[i] : ymin;
        ymax = by[i] > ymax ? by[i] : ymax;
    }
    const int y0 = clampi((int)floorf(ymin), 0, surf->size - 1);
    const int y1 = clampi((int)ceilf(ymax), 0, surf->size - 1);
    float xint[EB_MAX_POINTS];
    for (int y = y0; y <= y1; ++y) {
        const float scan = (float)y + 0.5f;
        int count = 0;
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const float ay = by[i], b_y = by[j];
            if ((ay <= scan && b_y > scan) || (b_y <= scan && ay > scan)) {
                if (count < EB_MAX_POINTS) {
                    xint[count++] = bx[i] + (scan - ay) / (b_y - ay) * (bx[j] - bx[i]);
                }
            }
        }
        if (count < 2) {
            continue;
        }
        for (int a = 1; a < count; ++a) {
            const float key = xint[a];
            int b = a - 1;
            while (b >= 0 && xint[b] > key) {
                xint[b + 1] = xint[b];
                --b;
            }
            xint[b + 1] = key;
        }
        for (int a = 0; a + 1 < count; a += 2) {
            const int xa = clampi((int)ceilf(xint[a] - 0.5f), 0, surf->size - 1);
            const int xb = clampi((int)floorf(xint[a + 1] - 0.5f), 0, surf->size - 1);
            for (int x = xa; x <= xb; ++x) {
                const float dx = (float)x + 0.5f - gx;
                const float dy = (float)y + 0.5f - gy;
                const float d = sqrtf(dx * dx + dy * dy);
                uint16_t c;
                if (d >= gr) {
                    c = c2;
                } else if (d >= gr * 0.62f) {
                    c = lerp565(c1, c2, (d - gr * 0.62f) / (gr * 0.38f));
                } else {
                    c = lerp565(c0, c1, d / (gr * 0.62f));
                }
                put_px(surf, x, y, c);
            }
        }
    }
    (void)shape;
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

#define EB_FACE_BBOX_SPAN 228.54f
#define EB_BODY_DIAMETER_RATIO (400.0f / 466.0f)
#define EB_BODY_CENTRE_OFFSET (-6.0f)
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

    /* Map the 259-unit upstream viewBox onto the canvas. The head ring spans
       228.54 units; rendering it at 400 px on a 466 px panel leaves a small
       rim for the status pill without shrinking the face. */
    const float size = (float)render->size;
    const float diameter = size * EB_BODY_DIAMETER_RATIO;
    const float scale = diameter / EB_FACE_BBOX_SPAN;
    const float stage = 259.0f * scale;
    const float left = (size - stage) * 0.5f;
    const float top = left + EB_BODY_CENTRE_OFFSET;
    const float offset_x = 15.0f + left / scale;
    const float offset_y = 15.0f + top / scale;

    /* Viewer gaze is expressed in normalised units; upstream clamps the look
       offset to +/-24 horizontal and +/-15 vertical viewBox units. */
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
    memset(surface.px, 0, (size_t)render->size * (size_t)render->size * sizeof(uint16_t));

    /* Body: the silhouette lookup stays in raw viewBox space (it drives the
       eye clamp), while the drawn outline carries the pose transform. */
    const uint16_t body_points = shape->body_points;
    float body_vx[EB_MAX_POINTS] = {0};
    float body_vy[EB_MAX_POINTS] = {0};
    float body_px[EB_MAX_POINTS] = {0};
    float body_py[EB_MAX_POINTS] = {0};
    for (uint16_t i = 0; i < body_points; ++i) {
        body_vx[i] = (float)shape->ring[i * 2u] / 100.0f;
        body_vy[i] = (float)shape->ring[i * 2u + 1u] / 100.0f;
        float vx = body_vx[i];
        float vy = body_vy[i];
        float dx = 0.0f;
        float dy = 0.0f;
        body_point(&pose, shape, &vx, &vy, &dx, &dy);
        body_px[i] = (dx + offset_x) * scale;
        body_py[i] = (dy + offset_y) * scale;
    }

    eb_sil_t silhouette;
    eb_sil_build(&silhouette, body_vx, body_vy, body_points);

    uint32_t body_rgb = pack->body_color;
    if (look->color_index != EB_NO_COLOR) {
        body_rgb = emotion_geometry_body_colors()[look->color_index];
    }
    eb_fill_body(&surface, body_px, body_py, body_points, shape, body_rgb,
                 scale, offset_x, offset_y);

    /* Eyes: the pool rotates the expression ring; each eye owns slot 1 or 2
       after the compositor folded the shared "both" spec into it. */
    const uint8_t expression = eb_pool_expression(look, now_ms);
    const uint16_t eye_color = rgb565(c8(pack->eye_color, 16),
                                      c8(pack->eye_color, 8),
                                      c8(pack->eye_color, 0));
    const uint16_t ring_points = emotion_geometry_ring_points();
    float eye_x[EB_MAX_POINTS];
    float eye_y[EB_MAX_POINTS];
    for (int eye = 0; eye < 2; ++eye) {
        const int16_t *ring = emotion_geometry_ring(expression, (uint8_t)eye);
        const int slot = eye == 0 ? 1 : 2;
        if (eb_eye_points(&pose, shape, ring, slot, &silhouette, scale,
                          offset_x, offset_y, eye_x, eye_y)) {
            fill_polygon(&surface, eye_x, eye_y, ring_points, eye_color);
        }
    }

    lv_obj_invalidate(render->canvas);
}
