/* Dotii v8 cartoon face renderer.
   Port of output/emotion-preview/_html_now.js (v8 overlay).

   Design rules baked into this file:
     - pure 2D, black-round-screen friendly, black/white base with a few
       accent colours for effects;
     - the outer disc NEVER moves; only the features inside the circular clip
       are affected by the head transform;
     - round sclera, big fixed iris, one big highlight -> cute, not uncanny;
     - talking / singing open an elliptical cavity: no tongue, no teeth;
     - no concentric coloured rings anywhere. */

#include "dotii_data.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DOTII_TAU 6.283185307179586f

typedef struct { uint8_t r, g, b; } rgb_t;

#define RGB(r,g,b) ((rgb_t){(uint8_t)(r),(uint8_t)(g),(uint8_t)(b)})

static const rgb_t C_WHITE  = {0xfb, 0xfc, 0xff};
static const rgb_t C_WHITE2 = {0xd7, 0xde, 0xea};
static const rgb_t C_PUPIL  = {0x0a, 0x0b, 0x12};
static const rgb_t C_BLUSH  = {0xff, 0x9f, 0xba};
static const rgb_t C_BROW   = {0xcc, 0xd7, 0xea};
static const rgb_t C_MOUTH  = {0x9c, 0x3d, 0x55};

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float minf2(float a, float b) { return a < b ? a : b; }
static inline float maxf2(float a, float b) { return a > b ? a : b; }
static inline float smoothstepf(float a, float b, float x)
{
    const float d = (b - a) != 0.0f ? (b - a) : 1e-6f;
    const float t = clampf((x - a) / d, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static inline rgb_t mix_rgb(rgb_t a, rgb_t b, float t)
{
    t = clampf(t, 0.0f, 1.0f);
    rgb_t c;
    c.r = (uint8_t)(a.r + (b.r - a.r) * t + 0.5f);
    c.g = (uint8_t)(a.g + (b.g - a.g) * t + 0.5f);
    c.b = (uint8_t)(a.b + (b.b - a.b) * t + 0.5f);
    return c;
}
static rgb_t parse_hex_rgb(const char *hex)
{
    rgb_t c = {0x8f, 0xa6, 0xc4};
    if (hex == NULL || hex[0] != '#') return c;
    long v = strtol(hex + 1, NULL, 16);
    c.r = (uint8_t)((v >> 16) & 255);
    c.g = (uint8_t)((v >> 8) & 255);
    c.b = (uint8_t)(v & 255);
    return c;
}

/* ---------------- affine transform ---------------- */
typedef struct { float a, b, c, d, e, f; } xf_t;  /* x'=a*x+b*y+c ; y'=d*x+e*y+f */

static xf_t xf_translate(float x, float y) { xf_t m = {1, 0, x, 0, 1, y}; return m; }
static xf_t xf_rotate(float r)
{
    const float c = cosf(r), s = sinf(r);
    xf_t m = {c, -s, 0, s, c, 0};
    return m;
}
static xf_t xf_scale(float x, float y) { xf_t m = {x, 0, 0, 0, y, 0}; return m; }

/* m o n: apply n first, then m */
static xf_t xf_mul(xf_t m, xf_t n)
{
    xf_t o;
    o.a = m.a * n.a + m.b * n.d;
    o.b = m.a * n.b + m.b * n.e;
    o.c = m.a * n.c + m.b * n.f + m.c;
    o.d = m.d * n.a + m.e * n.d;
    o.e = m.d * n.b + m.e * n.e;
    o.f = m.d * n.c + m.e * n.f + m.f;
    return o;
}
static void xf_apply(const xf_t *m, float x, float y, float *ox, float *oy)
{
    *ox = m->a * x + m->b * y + m->c;
    *oy = m->d * x + m->e * y + m->f;
}
static bool xf_invert(const xf_t *m, xf_t *out)
{
    const float det = m->a * m->e - m->b * m->d;
    if (fabsf(det) < 1e-9f) return false;
    const float ia = m->e / det;
    const float ib = -m->b / det;
    const float id = -m->d / det;
    const float ie = m->a / det;
    out->a = ia;
    out->b = ib;
    out->c = -(ia * m->c + ib * m->f);
    out->d = id;
    out->e = ie;
    out->f = -(id * m->c + ie * m->f);
    return true;
}

/* ---------------- canvas ---------------- */
typedef struct {
    uint16_t *fb;
    int32_t size;
    float alpha;      /* global alpha multiplier (1 - dim*0.55) */
    dotii_rect_t clip;
} canvas_t;

static void canvas_init(canvas_t *cv, uint16_t *fb, int32_t size, float alpha, dotii_rect_t clip)
{
    cv->fb = fb;
    cv->size = size;
    cv->alpha = alpha;
    cv->clip = clip;
}

static inline void blend_px(canvas_t *cv, int32_t x, int32_t y, rgb_t c, float a)
{
    if (x < cv->clip.x1 || y < cv->clip.y1 || x > cv->clip.x2 || y > cv->clip.y2) return;
    if (x < 0 || y < 0 || x >= cv->size || y >= cv->size) return;
    a *= cv->alpha;
    if (a <= 0.004f) return;
    if (a > 1.0f) a = 1.0f;
    uint16_t *d = &cv->fb[(size_t)y * (size_t)cv->size + (size_t)x];
    const uint32_t A = (uint32_t)(a * 64.0f + 0.5f);
    const uint32_t sr = (uint32_t)c.r >> 3;
    const uint32_t sg = (uint32_t)c.g >> 2;
    const uint32_t sb = (uint32_t)c.b >> 3;
    /* Fully-opaque pixels (the vast majority of every filled feature) only need
       a store. Testing before unpacking the destination avoids three shifts,
       three masks and three multiplies per saturated pixel. */
    if (A >= 64u) {
        *d = (uint16_t)((sr << 11) | (sg << 5) | sb);
        return;
    }
    const uint32_t dr = ((uint32_t)*d >> 11) & 0x1Fu;
    const uint32_t dg = ((uint32_t)*d >> 5) & 0x3Fu;
    const uint32_t db = ((uint32_t)*d) & 0x1Fu;
    const uint32_t r = (dr * (64u - A) + sr * A + 32u) >> 6;
    const uint32_t g = (dg * (64u - A) + sg * A + 32u) >> 6;
    const uint32_t b = (db * (64u - A) + sb * A + 32u) >> 6;
    *d = (uint16_t)((r << 11) | (g << 5) | b);
}

/* additive blend, used by the blush */
static inline void blend_add(canvas_t *cv, int32_t x, int32_t y, rgb_t c, float a)
{
    if (x < cv->clip.x1 || y < cv->clip.y1 || x > cv->clip.x2 || y > cv->clip.y2) return;
    if (x < 0 || y < 0 || x >= cv->size || y >= cv->size) return;
    a *= cv->alpha;
    if (a <= 0.004f) return;
    if (a > 1.0f) a = 1.0f;
    uint16_t *d = &cv->fb[(size_t)y * (size_t)cv->size + (size_t)x];
    const uint32_t A = (uint32_t)(a * 64.0f + 0.5f);
    uint32_t dr = (((uint32_t)*d >> 11) & 0x1Fu) + (((uint32_t)c.r >> 3) * A >> 6);
    uint32_t dg = (((uint32_t)*d >> 5) & 0x3Fu) + (((uint32_t)c.g >> 2) * A >> 6);
    uint32_t db = (((uint32_t)*d) & 0x1Fu) + (((uint32_t)c.b >> 3) * A >> 6);
    if (dr > 31u) dr = 31u;
    if (dg > 63u) dg = 63u;
    if (db > 31u) db = 31u;
    *d = (uint16_t)((dr << 11) | (dg << 5) | db);
}

static void fill_circle(canvas_t *cv, float cx, float cy, float r, rgb_t c, float alpha)
{
    if (r <= 0.0f || alpha <= 0.004f) return;
    const int32_t x0 = (int32_t)floorf(cx - r - 1.0f);
    const int32_t x1 = (int32_t)ceilf(cx + r + 1.0f);
    const int32_t y0 = (int32_t)floorf(cy - r - 1.0f);
    const int32_t y1 = (int32_t)ceilf(cy + r + 1.0f);
    const float r2 = r * r;
    const float inv_2r = 1.0f / (2.0f * r);
    for (int32_t py = y0; py <= y1; ++py) {
        const float fy = (float)py + 0.5f - cy;
        for (int32_t px = x0; px <= x1; ++px) {
            const float fx = (float)px + 0.5f - cx;
            const float d2 = fx * fx + fy * fy;
            if (d2 > r2 + r) continue;
            /* (r^2-d^2)/(2r) == (r-d)*(r+d)/(2r) ~= (r-d), i.e. an exactly
               one-pixel-wide analytic coverage ramp. Dividing by 4r instead
               would stretch it over two pixels and read as a soft edge. */
            const float cov = 0.5f + (r2 - d2) * inv_2r;
            blend_px(cv, px, py, c, alpha * (cov > 1.0f ? 1.0f : (cov < 0.0f ? 0.0f : cov)));
        }
    }
}

static void fill_ellipse_rot(canvas_t *cv, float cx, float cy, float rx, float ry,
                             float rot, rgb_t c, float alpha)
{
    if (rx <= 0.0f || ry <= 0.0f || alpha <= 0.004f) return;
    const float cr = cosf(-rot), sr = sinf(-rot);
    const float ext = maxf2(rx, ry) + 1.0f;
    const int32_t x0 = (int32_t)floorf(cx - ext);
    const int32_t x1 = (int32_t)ceilf(cx + ext);
    const int32_t y0 = (int32_t)floorf(cy - ext);
    const int32_t y1 = (int32_t)ceilf(cy + ext);
    const float minr = minf2(rx, ry);
    const float inv_rx = 1.0f / rx, inv_ry = 1.0f / ry;
    for (int32_t py = y0; py <= y1; ++py) {
        const float fy = (float)py + 0.5f - cy;
        for (int32_t px = x0; px <= x1; ++px) {
            const float fx = (float)px + 0.5f - cx;
            const float u = fx * cr - fy * sr;
            const float v = fx * sr + fy * cr;
            const float ux = u * inv_rx, vy = v * inv_ry;
            const float q2 = ux * ux + vy * vy;
            const float cov = clampf(0.5f + (1.0f - q2) * minr, 0.0f, 1.0f);
            if (cov > 0.0f) blend_px(cv, px, py, c, alpha * cov);
        }
    }
}

static void fill_ellipse(canvas_t *cv, float cx, float cy, float rx, float ry, rgb_t c, float alpha)
{
    fill_ellipse_rot(cv, cx, cy, rx, ry, 0.0f, c, alpha);
}

/* Fill an ellipse that is described in the LOCAL frame of `xf` (local->image),
   using analytic row spans. Interior pixels are stamped opaque and only the
   partially covered pixels at each end of a row pay for a coverage value.

   This exists for the fully-open eye, which is by far the most common state: it
   lets the sclera, iris and highlight each be a plain ellipse fill instead of a
   per-pixel test that had to evaluate the sclera coverage, two lid curves, the
   iris disc and the highlight disc for every single pixel. Growing the eyes to
   fill more of the screen made that loop the frame-time bottleneck. */
static void fill_ellipse_local(canvas_t *cv, const xf_t *xf, const xf_t *inv,
                               float cx, float cy, float rx, float ry,
                               rgb_t col, float alpha)
{
    if (rx <= 0.0f || ry <= 0.0f || alpha <= 0.002f) return;

    float icx, icy;
    xf_apply(xf, cx, cy, &icx, &icy);
    /* Conservative image-space AABB of the transformed ellipse. */
    const float ex = fabsf(xf->a) * rx + fabsf(xf->b) * ry + 1.0f;
    const float ey = fabsf(xf->d) * rx + fabsf(xf->e) * ry + 1.0f;
    int32_t y0 = (int32_t)floorf(icy - ey);
    int32_t y1 = (int32_t)ceilf(icy + ey);
    if (y0 < cv->clip.y1) y0 = cv->clip.y1;
    if (y1 > cv->clip.y2) y1 = cv->clip.y2;

    const float inv_rx2 = 1.0f / (rx * rx);
    const float inv_ry2 = 1.0f / (ry * ry);

    for (int32_t py = y0; py <= y1; ++py) {
        const float fy = (float)py + 0.5f;
        /* U = P*x + Q, V = R*x + S are the local offsets from the ellipse centre. */
        const float P = inv->a;
        const float Q = inv->b * fy + inv->c - cx;
        const float R = inv->d;
        const float S = inv->e * fy + inv->f - cy;
        const float A = P * P * inv_rx2 + R * R * inv_ry2;
        if (A <= 1e-12f) continue;
        const float B = 2.0f * (P * Q * inv_rx2 + R * S * inv_ry2);
        const float C = Q * Q * inv_rx2 + S * S * inv_ry2 - 1.0f;
        const float disc = B * B - 4.0f * A * C;
        if (disc <= 0.0f) continue;
        const float sq = sqrtf(disc);
        const float xl = (-B - sq) / (2.0f * A);
        const float xh = (-B + sq) / (2.0f * A);

        int32_t xa = (int32_t)floorf(xl);
        int32_t xb = (int32_t)ceilf(xh);
        if (xa < cv->clip.x1) xa = cv->clip.x1;
        if (xb > cv->clip.x2) xb = cv->clip.x2;
        /* Interior pixels are fully covered: pack the colour once and store it
           directly, so only the two end pixels of each row need coverage math.
           (Reached only when the caller already guaranteed alpha ~= 1.) */
        const uint16_t packed = (uint16_t)(((uint16_t)(col.r >> 3) << 11) |
                                           ((uint16_t)(col.g >> 2) << 5) |
                                           (uint16_t)(col.b >> 3));
        const int32_t xin0 = (int32_t)ceilf(xl);
        const int32_t xin1 = (int32_t)floorf(xh) - 1;
        uint16_t *rowp = &cv->fb[(size_t)py * (size_t)cv->size];
        for (int32_t x = xa; x <= xin0 - 1 && x < xb; ++x) {
            const float a = maxf2(xl, (float)x);
            const float b = minf2(xh, (float)x + 1.0f);
            if (b <= a) continue;
            float cov = b - a;
            if (cov > 1.0f) cov = 1.0f;
            blend_px(cv, x, py, col, alpha * cov);
        }
        for (int32_t x = xin0; x <= xin1; ++x) rowp[x] = packed;
        for (int32_t x = maxf2((float)(xin1 + 1), (float)xa); x < xb; ++x) {
            const float a = maxf2(xl, (float)x);
            const float b = minf2(xh, (float)x + 1.0f);
            if (b <= a) continue;
            float cov = b - a;
            if (cov > 1.0f) cov = 1.0f;
            blend_px(cv, x, py, col, alpha * cov);
        }
    }
}


/* Squared-distance capsule stamp. Dropping the per-pixel sqrt (and clamping
   the projection with min/max instead of a divide by len2) roughly halves the
   cost of every curve stroke on the ESP32-S3. */
static void stroke_line(canvas_t *cv, float x0, float y0, float x1, float y1,
                        float width, rgb_t c, float alpha)
{
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    const float radius = width * 0.5f;
    const float r2 = radius * radius;
    if (len2 < 1e-6f) { fill_circle(cv, x0, y0, radius, c, alpha); return; }
    const float inv = 1.0f / len2;
    const int32_t bx0 = (int32_t)floorf(minf2(x0, x1) - radius - 1.0f);
    const int32_t bx1 = (int32_t)ceilf(maxf2(x0, x1) + radius + 1.0f);
    const int32_t by0 = (int32_t)floorf(minf2(y0, y1) - radius - 1.0f);
    const int32_t by1 = (int32_t)ceilf(maxf2(y0, y1) + radius + 1.0f);
    for (int32_t py = by0; py <= by1; ++py) {
        const float fy = (float)py + 0.5f;
        for (int32_t px = bx0; px <= bx1; ++px) {
            const float fx = (float)px + 0.5f;
            float t = ((fx - x0) * dx + (fy - y0) * dy) * inv;
            if (t < 0.0f) t = 0.0f;
            else if (t > 1.0f) t = 1.0f;
            const float ddx = fx - (x0 + t * dx);
            const float ddy = fy - (y0 + t * dy);
            const float d2 = ddx * ddx + ddy * ddy;
            if (d2 > r2) continue;
            const float cov = 0.5f + (r2 - d2) / (2.0f * radius);
            blend_px(cv, px, py, c, alpha * (cov > 1.0f ? 1.0f : cov));
        }
    }
}

static void stroke_quad(canvas_t *cv, float x0, float y0, float cx, float cy,
                        float x1, float y1, float width, rgb_t c, float alpha)
{
    const int steps = 22;
    float px = x0, py = y0;
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / (float)steps, mt = 1.0f - t;
        const float nx = mt * mt * x0 + 2.0f * mt * t * cx + t * t * x1;
        const float ny = mt * mt * y0 + 2.0f * mt * t * cy + t * t * y1;
        stroke_line(cv, px, py, nx, ny, width, c, alpha);
        px = nx; py = ny;
    }
}

static void stroke_cubic(canvas_t *cv, float x0, float y0, float c1x, float c1y,
                         float c2x, float c2y, float x1, float y1,
                         float width, rgb_t c, float alpha)
{
    const int steps = 26;
    float px = x0, py = y0;
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / (float)steps, mt = 1.0f - t;
        const float a = mt * mt * mt, b = 3.0f * mt * mt * t;
        const float cc = 3.0f * mt * t * t, d = t * t * t;
        const float nx = a * x0 + b * c1x + cc * c2x + d * x1;
        const float ny = a * y0 + b * c1y + cc * c2y + d * y1;
        stroke_line(cv, px, py, nx, ny, width, c, alpha);
        px = nx; py = ny;
    }
}

static void stroke_arc(canvas_t *cv, float cx, float cy, float r, float a0, float a1,
                       float width, rgb_t c, float alpha)
{
    const float span = a1 - a0;
    if (span <= 0.0f) return;
    int steps = (int)(span * r / 0.6f) + 2;
    if (steps < 2) steps = 2;
    float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
    for (int i = 1; i <= steps; ++i) {
        const float a = a0 + span * (float)i / (float)steps;
        const float nx = cx + cosf(a) * r, ny = cy + sinf(a) * r;
        stroke_line(cv, px, py, nx, ny, width, c, alpha);
        px = nx; py = ny;
    }
}

/* Scanline polygon fill: 2x vertical supersampling + analytic horizontal
   coverage, with NO large static buffers.

   Memory note (important): an earlier revision of this function kept a
   1024-float row accumulator in .bss. That is 4 KB of internal DRAM, and this
   board only has a few KB of internal RAM spare once Wi-Fi + TLS + the audio
   DMA are up - the firmware then failed to create the xiaozhi audio task
   ("xiaozhi audio task create failed") and the whole system started thrashing.
   The accumulator is therefore gone; coverage is computed on the fly while
   merge-walking the two sub-scanline span lists. Only ~800 bytes of stack are
   used, and nothing static.

   Horizontal-only coverage was not enough: the eyebrow leaf has long, nearly
   horizontal edges whose stair-steps survived, which is what read as jagged
   ("毛刺"). Two sub-rows per scanline fix the vertical quantisation while the
   per-pixel horizontal overlap keeps the ends exact. */
#define DOTII_MAX_POLY_EDGE 128
#define DOTII_MAX_SPAN      16

static void poly_spans(const float *px, const float *py, int n, float fy,
                       float *lo, float *hi, int *out_n)
{
    float xs[DOTII_MAX_POLY_EDGE];
    int cnt = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float yi = py[i], yj = py[j];
        if ((yi > fy) == (yj > fy)) continue;
        const float t = (fy - yi) / (yj - yi);
        xs[cnt++] = px[i] + (px[j] - px[i]) * t;
        if (cnt >= DOTII_MAX_POLY_EDGE) break;
    }
    int pairs = 0;
    if (cnt >= 2) {
        for (int i = 1; i < cnt; ++i) {
            const float v = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; --j; }
            xs[j + 1] = v;
        }
        for (int i = 0; i + 1 < cnt && pairs < DOTII_MAX_SPAN; i += 2) {
            lo[pairs] = xs[i];
            hi[pairs] = xs[i + 1];
            ++pairs;
        }
    }
    *out_n = pairs;
}

/* horizontal overlap of pixel [x,x+1) with a span, given a moving index */
static float poly_cov(const float *lo, const float *hi, int n, int *idx, int32_t x)
{
    int i = *idx;
    while (i < n && hi[i] <= (float)x) ++i;
    *idx = i;
    if (i >= n) return 0.0f;
    const float a = maxf2(lo[i], (float)x);
    const float b = minf2(hi[i], (float)x + 1.0f);
    return b > a ? (b - a) : 0.0f;
}

static void fill_polygon(canvas_t *cv, const float *px, const float *py, int n,
                         rgb_t c, float alpha)
{
    if (n < 3 || alpha <= 0.002f) return;
    if (n > DOTII_MAX_POLY_EDGE) n = DOTII_MAX_POLY_EDGE;

    float minx = px[0], maxx = px[0], miny = py[0], maxy = py[0];
    for (int i = 1; i < n; ++i) {
        minx = minf2(minx, px[i]); maxx = maxf2(maxx, px[i]);
        miny = minf2(miny, py[i]); maxy = maxf2(maxy, py[i]);
    }
    int32_t y0 = (int32_t)floorf(miny);
    int32_t y1 = (int32_t)ceilf(maxy);
    if (y0 < cv->clip.y1) y0 = cv->clip.y1;
    if (y1 > cv->clip.y2) y1 = cv->clip.y2;

    int32_t row0 = (int32_t)floorf(minx);
    int32_t row1 = (int32_t)ceilf(maxx);
    if (row0 < cv->clip.x1) row0 = cv->clip.x1;
    if (row1 > cv->clip.x2) row1 = cv->clip.x2;
    if (row1 < row0) return;

    for (int32_t y = y0; y <= y1; ++y) {
        float loa[DOTII_MAX_SPAN], hia[DOTII_MAX_SPAN];
        float lob[DOTII_MAX_SPAN], hib[DOTII_MAX_SPAN];
        int na = 0, nb = 0;
        poly_spans(px, py, n, (float)y + 0.25f, loa, hia, &na);
        poly_spans(px, py, n, (float)y + 0.75f, lob, hib, &nb);
        if (na == 0 && nb == 0) continue;

        int ia = 0, ib = 0;
        for (int32_t x = row0; x <= row1; ++x) {
            const float ca = poly_cov(loa, hia, na, &ia, x);
            const float cb = poly_cov(lob, hib, nb, &ib, x);
            float cov = (ca + cb) * 0.5f;
            if (cov <= 0.002f) continue;
            if (cov > 1.0f) cov = 1.0f;
            blend_px(cv, x, y, c, alpha * cov);
        }
    }
}
/* ---------------- reusable shape helpers ---------------- */
static void fill_heart(canvas_t *cv, float x, float y, float s, rgb_t c, float alpha)
{
    float px[51], py[51];
    int n = 0;
    /* right lobe then left lobe, sampled from the two cubics */
    for (int i = 0; i <= 24; ++i) {
        const float t = (float)i / 24.0f, mt = 1.0f - t;
        const float a = mt * mt * mt, b = 3 * mt * mt * t, cc = 3 * mt * t * t, d = t * t * t;
        px[n] = a * x + b * (x + s * 0.40f) + cc * (x + s * 1.05f) + d * x;
        py[n] = a * (y - s * 0.34f) + b * (y - s * 0.92f) + cc * (y - s * 0.30f) + d * (y + s * 0.42f);
        ++n;
    }
    for (int i = 0; i <= 24; ++i) {
        const float t = (float)i / 24.0f, mt = 1.0f - t;
        const float a = mt * mt * mt, b = 3 * mt * mt * t, cc = 3 * mt * t * t, d = t * t * t;
        px[n] = a * x + b * (x - s * 0.40f) + cc * (x - s * 1.05f) + d * x;
        py[n] = a * (y + s * 0.42f) + b * (y - s * 0.30f) + cc * (y - s * 0.92f) + d * (y - s * 0.34f);
        ++n;
    }
    fill_polygon(cv, px, py, n, c, alpha);
}


static void spark4(canvas_t *cv, float x, float y, float r, float thin, rgb_t c, float alpha)
{
    float px[8], py[8];
    const float t = thin;
    px[0] = x;        py[0] = y - r;
    px[1] = x + t;    py[1] = y - t;
    px[2] = x + r;    py[2] = y;
    px[3] = x + t;    py[3] = y + t;
    px[4] = x;        py[4] = y + r;
    px[5] = x - t;    py[5] = y + t;
    px[6] = x - r;    py[6] = y;
    px[7] = x - t;    py[7] = y - t;
    fill_polygon(cv, px, py, 8, c, alpha);
}

/* ---------------- layout ---------------- */
typedef struct {
    float size, R, CX, CY;
    float eyeY, eyeSep, eyeW, eyeH;
    float browY, browGap, mouthY;
} lay_t;

static lay_t layout_for(int32_t size)
{
    lay_t L;
    L.size = (float)size;
    L.R = L.size * 0.5f;
    L.CX = L.size * 0.5f;
    L.CY = L.size * 0.5f;
    L.eyeY = L.CY + L.R * DOTII_LAYOUT_EYE_Y;
    L.eyeSep = L.R * DOTII_LAYOUT_EYE_SEP;
    L.eyeW = L.R * DOTII_LAYOUT_EYE_W;
    L.eyeH = L.R * DOTII_LAYOUT_EYE_H;
    L.browY = L.CY + L.R * DOTII_LAYOUT_BROW_Y;
    L.browGap = L.R * DOTII_LAYOUT_BROW_GAP;
    L.mouthY = L.CY + L.R * DOTII_LAYOUT_MOUTH_Y;
    return L;
}

/* ================= eye geometry (v8 single source of truth) ================= */
typedef struct {
    float RX, RY, uC, dC, bulgeU, bulgeD, closeK, openK;
    bool shut, use_arc;
    uint8_t kind;
    float ref_top;
    float nat;
    float body_a, arc_a;
} eye_geom_t;

static eye_geom_t eye_geom(const dotii_pose_t *p, int side /*0=left 1=right*/, float eye_w_px)
{
    eye_geom_t g;
    const float nat = clampf(p->eye_open, 0.0f, 2.4f);
    const float nat_s = (side == 0 && p->eye_open_l >= 0.0f) ? p->eye_open_l : nat;
    const float ew = clampf(p->eye_w, 0.55f, 1.6f);
    const float eh = clampf(p->eye_h, 0.55f, 1.6f);
    const float wide = 1.0f + maxf2(0.0f, nat_s - 1.0f) * 0.16f;
    const float RX = eye_w_px * 0.5f * ew * wide;
    const float RY = RX * clampf(eh, 0.94f, 1.10f);
    const float lt = clampf(p->lid_top, 0.0f, 0.9f);
    const float lb = clampf(p->lid_bot, 0.0f, 0.9f);
    const float oShape = clampf(nat_s, 0.0f, 2.4f);
    const float bk = clampf(p->blink_k, 0.0f, 1.25f);
    const float closeK = clampf((1.0f - minf2(1.0f, bk)) * 1.10f, 0.0f, 1.0f);

    float kU = (1.0f - minf2(1.0f, oShape)) * 0.80f + lt * 0.45f;
    float kD = lb * 0.45f;
    if (oShape <= 0.12f) { kU = 0.5f; kD = 0.5f; }
    const float covU = clampf(kU, 0.0f, 0.94f);
    const float covD = clampf(kD, 0.0f, 0.94f);
    const float uNat = -RY + 2.0f * RY * covU;
    const float dNat =  RY - 2.0f * RY * covD;
    const float hNat = maxf2(0.0f, dNat - uNat);
    const float cNat = (uNat + dNat) * 0.5f;
    const float shrink = 1.0f - closeK;

    g.RX = RX;
    g.RY = RY;
    g.uC = cNat - hNat * 0.5f * shrink * 1.02f;
    g.dC = cNat + hNat * 0.5f * shrink * 0.98f;
    const float dU = clampf((g.uC + RY) / (RY > 0.0f ? RY : 1.0f), 0.0f, 1.0f);
    const float dD = clampf((RY - g.dC) / (RY > 0.0f ? RY : 1.0f), 0.0f, 1.0f);
    g.bulgeU = RY * 0.34f * dU;
    g.bulgeD = RY * 0.24f * dD;
    g.closeK = closeK;
    g.openK = clampf((g.dC - g.uC) / (2.0f * RY > 0.0f ? 2.0f * RY : 1.0f), 0.0f, 1.0f);
    g.shut = hNat < RY * 0.06f;

    const uint8_t arc = p->eye_arc;
    const bool forced = (arc == DOTII_EYE_ARC_UP || arc == DOTII_EYE_ARC_DOWN || arc == DOTII_EYE_ARC_LINE);
    g.use_arc = forced || (g.openK < 0.14f && closeK < 0.55f);
    g.kind = forced ? arc : DOTII_EYE_ARC_NONE;

    if (forced) {
        if (arc == DOTII_EYE_ARC_UP)        g.ref_top = -RX * 0.66f;
        else if (arc == DOTII_EYE_ARC_DOWN) g.ref_top = -RX * 0.40f;
        else if (arc == DOTII_EYE_ARC_LINE) g.ref_top = -RX * 0.18f;
        else                                g.ref_top = -RX * 0.32f;
    } else {
        g.ref_top = -RY;
    }
    g.nat = nat_s;
    g.body_a = 1.0f - smoothstepf(0.62f, 1.0f, closeK);
    g.arc_a  = smoothstepf(0.58f, 1.0f, closeK);
    return g;
}

/* saccadeAt(): deterministic micro-saccades. This is what makes the eyes feel
   alive instead of parked. */
static void saccade_at(uint32_t ms, uint16_t key, float *ox, float *oy)
{
    const float P = 1700.0f;
    const uint32_t c = (uint32_t)((float)ms / P);
    const float tt = (float)ms - (float)c * P;
    /* hash01 as used by the JS path (n * 2654435761 >>> 0) */
    const uint32_t k0 = (uint32_t)key * 131u;
    const uint32_t k1 = (uint32_t)key * 977u;
    uint32_t n0 = k0 + c * 8191u;
    uint32_t n1 = k0 + (c + 1u) * 8191u;
    uint32_t py0 = k1 + c * 3001u;
    uint32_t py1 = k1 + (c + 1u) * 3001u;
    uint32_t h0 = n0 * 2654435761u; h0 = (h0 ^ (h0 >> 15)) * 2246822519u; h0 ^= h0 >> 13;
    uint32_t h1 = n1 * 2654435761u; h1 = (h1 ^ (h1 >> 15)) * 2246822519u; h1 ^= h1 >> 13;
    uint32_t v0 = py0 * 2654435761u; v0 = (v0 ^ (v0 >> 15)) * 2246822519u; v0 ^= v0 >> 13;
    uint32_t v1 = py1 * 2654435761u; v1 = (v1 ^ (v1 >> 15)) * 2246822519u; v1 ^= v1 >> 13;
    const float f0 = (float)h0 / 4294967296.0f;
    const float f1 = (float)h1 / 4294967296.0f;
    const float px = f0 * 2.0f - 1.0f;
    const float py = (((float)v0 / 4294967296.0f) * 2.0f - 1.0f) * 0.55f;
    const float nx = f1 * 2.0f - 1.0f;
    const float ny = (((float)v1 / 4294967296.0f) * 2.0f - 1.0f) * 0.55f;
    const float start = 180.0f + f0 * 1150.0f;
    if (tt < start) { *ox = px; *oy = py; return; }
    const float k = smoothstepf(0.0f, 1.0f, clampf((tt - start) / 85.0f, 0.0f, 1.0f));
    *ox = px + (nx - px) * k;
    *oy = py + (ny - py) * k;
}

/* Eye white + iris, rasterised through the inverse affine so rotation and
   squash are exact. Double clip: sclera circle ∩ eyelid lens. */
static void draw_eye_body(canvas_t *cv, const xf_t *xf, const dotii_pose_t *p,
                          const eye_geom_t *g, float alpha)
{
    const float RX = g->RX, RY = g->RY;
    const float tCut = g->uC, bCut = g->dC;
    if (bCut - tCut < 1.2f) return;
    float bu = g->bulgeU, bd = g->bulgeD;
    const float room = bCut - tCut;
    bu = minf2(bu, room * 0.46f);
    bd = minf2(bd, room * 0.46f);

    /* visible bounds */
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const float ux = (i & 1) ? RX : -RX;
        const float uy = (i & 2) ? RY : -RY;
        float sx, sy;
        xf_apply(xf, ux, uy, &sx, &sy);
        minx = minf2(minx, sx); maxx = maxf2(maxx, sx);
        miny = minf2(miny, sy); maxy = maxf2(maxy, sy);
    }
    xf_t inv;
    if (!xf_invert(xf, &inv)) return;

    /* iris placement */
    const float gx = clampf(p->pupil_x, -1.0f, 1.0f);
    const float gy = clampf(p->pupil_y, -1.0f, 1.0f);
    float sax = 0.0f, say = 0.0f;
    saccade_at(p->ms, p->sac_key, &sax, &say);
    const float pr = RX * 0.66f;
    const float ix = clampf(gx * RX * 0.20f, -RX * 0.26f, RX * 0.26f) + sax * RX * 0.08f;
    const float vTop = tCut - bu * 0.5f;
    const float vBot = bCut + bd * 0.5f;
    const float lo = vTop + pr * 0.24f;
    const float hi = maxf2(lo, vBot - pr * 0.24f);
    const float iy = clampf(gy * RY * 0.15f + say * RY * 0.075f, lo, hi);
    const float hr = pr * 0.32f;
    const float hx = ix - pr * 0.30f;
    const float hy = iy - pr * 0.32f;

    const int32_t x0 = (int32_t)floorf(minx) - 1;
    const int32_t x1 = (int32_t)ceilf(maxx) + 1;
    const int32_t y0 = (int32_t)floorf(miny) - 1;
    const int32_t y1 = (int32_t)ceilf(maxy) + 1;
    const float minr = minf2(RX, RY);
    const float P = 1.5f;
    /* Reciprocals: ESP32-S3 float division is a software routine, and the
       previous inner loop issued three of them per pixel. */
    const float inv_rx = 1.0f / RX;
    const float inv_ry = 1.0f / RY;
    const float inv_2rxp = 1.0f / (2.0f * (RX + P));
    const float inv_pr2 = 1.0f / (2.0f * pr);
    const float inv_hr2 = 1.0f / (2.0f * hr);
    const float pr2 = pr * pr;
    const float hr2 = hr * hr;
    /* When the lids are (almost) fully open the lens degenerates to the sclera
       circle, so the per-pixel lid test can be skipped entirely. This is the
       common idle/blink-open case. */
    const bool lid_active = (bu > 0.02f) || (bd > 0.02f);
    /* The iris/highlight discs and the eyelid cuts only affect a subset of the
       sclera. Precomputing those spans lets the inner loop skip the bulk of the
       per-pixel arithmetic for pixels where the result is identical to a plain
       sclera store. The bounds below are the exact analytic envelopes (with a
       half-pixel pad for the coverage ramp), so no visible pixel changes. */
    const float iu_lo = minf2(ix - pr, hx - hr) - 1.0f;
    const float iu_hi = maxf2(ix + pr, hx + hr) + 1.0f;
    const float iv_lo = minf2(iy - pr, hy - hr) - 1.0f;
    const float iv_hi = maxf2(iy + pr, hy + hr) + 1.0f;
    /* cov_t/cov_b only bite below tCut+0.5 / above bCut-0.5; inside that band
       both clamps evaluate to >= 1 and are pure overhead. */
    const float lid_top_reach = tCut + 0.5f;
    const float lid_bot_reach = bCut - 0.5f;

    /* ---- Fast path: fully open eye ----------------------------------------
       With the lids clear (bu and bd are both zero, which is the case whenever
       the eye is not blinking and has no droop) the visible shape is exactly
       sclera ellipse + iris disc + highlight disc, with no clipping at all.
       Drawing those three as analytic row-span ellipse fills skips the
       per-pixel coverage/eyelid/iris/highlight arithmetic that dominated the
       frame time once the eyes were enlarged. Any other state (blink, droop,
       half-closed) falls through to the precise per-pixel loop below. */
    /* Require the lid cut to be clear of the sclera, not merely a small bulge:
       a slightly-closed eye still reports tiny bulges, and letting the fast path
       run then would shave a sub-pixel sliver off the lid. */
    const bool lid_clear = (bu <= 0.02f) && (bd <= 0.02f) &&
                           (tCut <= -RY + 0.25f) && (bCut >= RY - 0.25f);
    if (lid_clear && alpha >= 0.998f) {
        fill_ellipse_local(cv, xf, &inv, 0.0f, 0.0f, RX, RY, C_WHITE, 1.0f);
        fill_ellipse_local(cv, xf, &inv, ix, iy, pr, pr, C_PUPIL, 1.0f);
        fill_ellipse_local(cv, xf, &inv, hx, hy, hr, hr, C_WHITE, 1.0f);
        return;
    }

    /* Row-limited rasterisation. Instead of testing every pixel of the
       bounding box (pi/4 of which is outside the ellipse) plus two float
       mix_rgb() calls per pixel, walk only the rows the sclera can occupy and
       clamp the x range to the ellipse's analytic half-width for that row. */
    for (int32_t py = y0; py <= y1; ++py) {
        /* Image-space row -> local row is affine; find the x span by solving
           the quadratic (u/RX)^2 + (v/RY)^2 <= 1 with u = a2*x + c2. */
        const float fy = (float)py + 0.5f;
        const float v0 = inv.d * (float)x0 + inv.e * fy + inv.f;
        const float vstep = inv.d;
        const float u0 = inv.a * (float)x0 + inv.b * fy + inv.c;
        const float ustep = inv.a;
        if (fabsf(ustep) < 1e-6f) continue;
        /* Solve for x where the ellipse boundary is crossed. */
        const float A = (ustep * inv_rx) * (ustep * inv_rx) + (vstep * inv_ry) * (vstep * inv_ry);
        /* u(x) = u0 + ustep*(x-x0); sample at px = x0. Use continuous x. */
        const float cu = u0 * inv_rx;
        const float cvv = v0 * inv_ry;
        const float B = 2.0f * cu * (ustep * inv_rx) + 2.0f * cvv * (vstep * inv_ry);
        const float C = cu * cu + cvv * cvv - 1.0f;
        const float disc = B * B - 4.0f * A * C;
        if (disc <= 0.0f) continue;
        const float sq = sqrtf(disc);
        const float t_lo = (-B - sq) / (2.0f * A);
        const float t_hi = (-B + sq) / (2.0f * A);
        int32_t xa = (int32_t)floorf((float)x0 + t_lo);
        int32_t xb = (int32_t)ceilf((float)x0 + t_hi);
        if (xa < x0) xa = x0;
        if (xb > x1) xb = x1;
        for (int32_t px = xa; px <= xb; ++px) {
            const float u = u0 + ustep * ((float)px - (float)x0);
            const float v = v0 + vstep * ((float)px - (float)x0);
            const float qu = u * inv_rx, qv = v * inv_ry;
            const float q2 = qu * qu + qv * qv;
            float cov = clampf(0.5f + (1.0f - q2) * minr, 0.0f, 1.0f);
            if (cov <= 0.002f) continue;
            const bool in_iris_span =
                u >= iu_lo && u <= iu_hi && v >= iv_lo && v <= iv_hi;
            const bool lid_here = lid_active &&
                                  (v < lid_top_reach || v > lid_bot_reach);
            if (lid_here) {
                const float t = clampf(0.5f + u * inv_2rxp, 0.0f, 1.0f);
                const float bulb = 2.0f * t * (1.0f - t);
                const float cov_t = clampf(0.5f + (v - (tCut - bulb * bu)), 0.0f, 1.0f);
                if (cov_t < cov) cov = cov_t;
                const float cov_b = clampf(0.5f + ((bCut + bulb * bd) - v), 0.0f, 1.0f);
                if (cov_b < cov) cov = cov_b;
                if (cov <= 0.002f) continue;
            }
            /* Iris and highlight are drawn as antialiased discs. A hard
               `dp2 < pr2` test left a visibly stepped rim around every pupil,
               which is what read as jagged ("毛刺") on the eyes. Coverage comes
               from the same one-pixel analytic ramp used everywhere else, and the
               iris alpha is folded into the blend so the rim blends into the
               sclera instead of cutting into it. */
            rgb_t col = C_WHITE;
            float a_fg = 0.0f;
            if (in_iris_span) {
            const float dpx = u - ix, dpy = v - iy;
            const float dp2 = dpx * dpx + dpy * dpy;
            const float dhx = u - hx, dhy = v - hy;
            const float dh2 = dhx * dhx + dhy * dhy;
            const float iris_cov = clampf(0.5f + (pr2 - dp2) * inv_pr2, 0.0f, 1.0f);
            if (iris_cov > 0.002f) {
                col = C_PUPIL;
                a_fg = iris_cov;
                const float hi_cov = clampf(0.5f + (hr2 - dh2) * inv_hr2, 0.0f, 1.0f);
                if (hi_cov > 0.002f) {
                    /* mix toward white weighted by the highlight coverage */
                    col = (hi_cov >= 0.995f) ? C_WHITE : mix_rgb(col, C_WHITE, hi_cov);
                    a_fg = a_fg + (1.0f - a_fg) * hi_cov;
                }
            }
            if (a_fg <= 0.002f) {
                blend_px(cv, px, py, C_WHITE, alpha * cov);
            } else if (a_fg >= 0.998f) {
                blend_px(cv, px, py, col, alpha * cov);
            } else {
                /* draw the white sclera, then composite the iris/highlight on top */
                blend_px(cv, px, py, C_WHITE, alpha * cov);
                blend_px(cv, px, py, col, alpha * cov * a_fg);
            }
            } /* in_iris_span */
        }
    }
}

static void draw_eye_arc(canvas_t *cv, const xf_t *xf, float er, uint8_t kind,
                         rgb_t color, float dy, float alpha)
{
    const float w = er * 0.22f;
    /* Two cubics: left half (start -> apex) then right half (apex -> end).
       apex/end must be independent of the start point - reusing x0/y0 as the
       second endpoint produces a triangular "∧" instead of a crescent "∩". */
    float sx, sy, c1x, c1y, c2x, c2y, ax, ay;
    float c3x, c3y, c4x, c4y, ex, ey;
    if (kind == DOTII_EYE_ARC_UP) {
        sx = -er * 0.72f; sy = er * 0.24f;
        c1x = -er * 0.44f; c1y = -er * 0.24f; c2x = -er * 0.24f; c2y = -er * 0.48f;
        ax = 0.0f; ay = -er * 0.48f;
        c3x = er * 0.24f; c3y = -er * 0.48f; c4x = er * 0.44f; c4y = -er * 0.24f;
        ex = er * 0.72f; ey = er * 0.24f;
    } else if (kind == DOTII_EYE_ARC_LINE) {
        float ax0, ay0, ax1, ay1;
        xf_apply(xf, -er * 0.68f, dy, &ax0, &ay0);
        xf_apply(xf,  er * 0.68f, dy, &ax1, &ay1);
        stroke_line(cv, ax0, ay0, ax1, ay1, w, color, alpha);
        return;
    } else if (kind == DOTII_EYE_ARC_NONE) { /* blink arc: centre sags */
        sx = -er * 0.74f; sy = -er * 0.14f;
        c1x = -er * 0.40f; c1y = er * 0.32f; c2x = -er * 0.22f; c2y = er * 0.42f;
        ax = 0.0f; ay = er * 0.42f;
        c3x = er * 0.22f; c3y = er * 0.42f; c4x = er * 0.40f; c4y = er * 0.32f;
        ex = er * 0.74f; ey = -er * 0.14f;
    } else { /* DOWN: flat U */
        sx = -er * 0.74f; sy = -er * 0.22f;
        c1x = -er * 0.44f; c1y = er * 0.24f; c2x = -er * 0.22f; c2y = er * 0.38f;
        ax = 0.0f; ay = er * 0.38f;
        c3x = er * 0.22f; c3y = er * 0.38f; c4x = er * 0.44f; c4y = er * 0.24f;
        ex = er * 0.74f; ey = -er * 0.22f;
    }
    float a0x, a0y, a1x, a1y, a2x, a2y, a3x, a3y;
    xf_apply(xf, sx, sy + dy, &a0x, &a0y);
    xf_apply(xf, c1x, c1y + dy, &a1x, &a1y);
    xf_apply(xf, c2x, c2y + dy, &a2x, &a2y);
    xf_apply(xf, ax, ay + dy, &a3x, &a3y);
    stroke_cubic(cv, a0x, a0y, a1x, a1y, a2x, a2y, a3x, a3y, w, color, alpha);

    float b0x, b0y, b1x, b1y, b2x, b2y, b3x, b3y;
    xf_apply(xf, ax, ay + dy, &b0x, &b0y);
    xf_apply(xf, c3x, c3y + dy, &b1x, &b1y);
    xf_apply(xf, c4x, c4y + dy, &b2x, &b2y);
    xf_apply(xf, ex, ey + dy, &b3x, &b3y);
    stroke_cubic(cv, b0x, b0y, b1x, b1y, b2x, b2y, b3x, b3y, w, color, alpha);
}

/* one eye: heart override > forced arc > shut > body+blink crossfade */
static void draw_eye(canvas_t *cv, const xf_t *xf, float cx, float cy,
                     const dotii_pose_t *p, int side, float eye_w_px)
{
    const eye_geom_t g = eye_geom(p, side, eye_w_px);
    const uint8_t extra = side == 0 ? p->eye_extra_l : p->eye_extra_r;

    /* centre of this eye in face space; eye-local xf */
    const xf_t local = xf_mul(*xf, xf_translate(cx, cy));
    const xf_t eye = local;

    if (extra == DOTII_EYE_EXTRA_HEART) {
        float sx, sy;
        xf_apply(&eye, 0.0f, 0.0f, &sx, &sy);
        const float s = g.RX * 1.02f;
        fill_heart(cv, sx, sy, s, C_BLUSH, 1.0f);
        fill_circle(cv, sx - s * 0.26f, sy - s * 0.30f, s * 0.14f, C_WHITE, 0.95f);
        return;
    }
    if (g.use_arc && g.kind != DOTII_EYE_ARC_NONE) {
        draw_eye_arc(cv, &eye, g.RX, g.kind, C_WHITE, 0.0f, 1.0f);
        return;
    }
    if (g.shut) {
        draw_eye_arc(cv, &eye, g.RX, DOTII_EYE_ARC_NONE, C_WHITE, 0.0f, 1.0f);
        return;
    }
    if (g.body_a > 0.012f) draw_eye_body(cv, &eye, p, &g, g.body_a);
    if (g.arc_a > 0.012f) draw_eye_arc(cv, &eye, g.RX, DOTII_EYE_ARC_NONE, C_WHITE, 0.0f, g.arc_a);
}


/* ================= eyebrows ================= */
typedef struct { float dy, inner, outer, arch, th; } brow_cfg_t;
static const brow_cfg_t BROW_T[10] = {
    /* neutral */ { 0.000f, -0.010f,  0.014f, 0.058f, 0.034f },
    /* raised  */ {-0.032f, -0.018f,  0.006f, 0.062f, 0.032f },
    /* focused */ {-0.002f,  0.018f,  0.018f, 0.032f, 0.036f },
    /* angry   */ { 0.008f,  0.042f, -0.030f, 0.022f, 0.036f },
    /* sad     */ { 0.016f, -0.012f,  0.044f, 0.050f, 0.033f },
    /* worried */ {-0.020f, -0.042f,  0.018f, 0.060f, 0.032f },
    /* droop   */ { 0.022f, -0.006f,  0.040f, 0.044f, 0.033f },
    /* flat    */ { 0.006f,  0.006f,  0.006f, 0.020f, 0.034f },
    /* soft    */ {-0.008f, -0.014f,  0.012f, 0.056f, 0.032f },
    /* furrow  */ { 0.010f,  0.036f,  0.010f, 0.024f, 0.035f },
};

static void draw_brow(canvas_t *cv, const xf_t *xf, const lay_t *L, float cx,
                      float ref_top, float bw, uint8_t kind,
                      const dotii_pose_t *p, int side)
{
    if (kind >= DOTII_BROW_NONE) return;
    const brow_cfg_t *cfg = &BROW_T[kind];
    const float s = (side == 0) ? -1.0f : 1.0f;
    const float need = cfg->th * L->R * 0.5f;
    const float gap = L->browGap;

    const float base = ref_top - gap - need
                     + p->brow_y * L->R * 0.45f
                     + p->brow_px
                     + cfg->dy * L->R;
    float y_outer = base + cfg->outer * L->R;
    float y_inner = base + cfg->inner * L->R;

    const float lim = ref_top - gap + L->R * 0.020f;
    const float low = maxf2(y_outer, y_inner) + need;
    if (low > lim) {
        const float d = low - lim;
        y_outer -= d;
        y_inner -= d;
    }

    const float x_outer = cx + s * bw * 0.5f;
    const float x_inner = cx - s * bw * 0.5f;
    const float cyc = (y_outer + y_inner) * 0.5f - cfg->arch * L->R;

    enum { N = 22, NPTS = (N + 1) * 2 };
    float px[NPTS], py[NPTS], nx[NPTS], ny[NPTS], hw[NPTS];
    float qx[N + 1], qy[N + 1];
    for (int i = 0; i <= N; ++i) {
        const float t = (float)i / (float)N, mt = 1.0f - t;
        qx[i] = mt * mt * x_outer + 2.0f * mt * t * cx + t * t * x_inner;
        qy[i] = mt * mt * y_outer + 2.0f * mt * t * cyc + t * t * y_inner;
    }
    for (int i = 0; i <= N; ++i) {
        const int i0 = i > 0 ? i - 1 : 0;
        const int i1 = i < N ? i + 1 : N;
        const float dx = qx[i1] - qx[i0];
        const float dy = qy[i1] - qy[i0];
        const float len = sqrtf(dx * dx + dy * dy) + 1e-6f;
        nx[i] = -dy / len;
        ny[i] = dx / len;
        const float t = (float)i / (float)N;
        const float u = powf(t, 0.85f);
        const float sv = sinf((float)M_PI * u);
        const float co = powf(sv > 0.0f ? sv : 0.0f, 0.55f);
        hw[i] = need * (0.22f + 0.78f * co);
    }
    for (int i = 0; i <= N; ++i) {
        float sx, sy;
        xf_apply(xf, qx[i] + nx[i] * hw[i], qy[i] + ny[i] * hw[i], &sx, &sy);
        px[i] = sx; py[i] = sy;
    }
    for (int i = 0; i <= N; ++i) {
        const int j = N - i;
        float sx, sy;
        xf_apply(xf, qx[j] - nx[j] * hw[j], qy[j] - ny[j] * hw[j], &sx, &sy);
        px[N + 1 + i] = sx; py[N + 1 + i] = sy;
    }
    fill_polygon(cv, px, py, NPTS, C_BROW, 1.0f);

    const float sc = sqrtf(fabsf(xf->a * xf->e - xf->b * xf->d));
    float ex, ey;
    xf_apply(xf, qx[0], qy[0], &ex, &ey);
    fill_circle(cv, ex, ey, hw[0] * sc, C_BROW, 1.0f);
    xf_apply(xf, qx[N], qy[N], &ex, &ey);
    fill_circle(cv, ex, ey, hw[N] * sc, C_BROW, 1.0f);
}

/* ================= nose / mouth ================= */
static float xf_scale_avg(const xf_t *m)
{
    return 0.5f * (sqrtf(m->a * m->a + m->d * m->d) + sqrtf(m->b * m->b + m->e * m->e));
}

static void stroke_quad_local(canvas_t *cv, const xf_t *m, float x0, float y0,
                              float cx, float cy, float x1, float y1,
                              float w, rgb_t c, float a)
{
    float ax, ay, bx, by, dx, dy;
    xf_apply(m, x0, y0, &ax, &ay);
    xf_apply(m, cx, cy, &bx, &by);
    xf_apply(m, x1, y1, &dx, &dy);
    stroke_quad(cv, ax, ay, bx, by, dx, dy, w, c, a);
}

static void fill_stroke_path(canvas_t *cv, const xf_t *m, const float *lx, const float *ly,
                             int n, bool closed, rgb_t fill, rgb_t stroke,
                             float sw, float a)
{
    float px[128] = {0.0f}, py[128] = {0.0f};
    if (n > 128) n = 128;
    for (int i = 0; i < n; ++i) xf_apply(m, lx[i], ly[i], &px[i], &py[i]);
    if (fill.r || fill.g || fill.b) fill_polygon(cv, px, py, n, fill, a);
    if (sw > 0.0f) {
        for (int i = 1; i < n; ++i) stroke_line(cv, px[i - 1], py[i - 1], px[i], py[i], sw, stroke, a);
        if (closed) stroke_line(cv, px[n - 1], py[n - 1], px[0], py[0], sw, stroke, a);
        const float r = sw * 0.5f * xf_scale_avg(m);
        fill_circle(cv, px[0], py[0], r, stroke, a);
        fill_circle(cv, px[n - 1], py[n - 1], r, stroke, a);
    }
}

/* sample a quadratic into a closed outline starting at (x0,y0) */
static int sample_quad(float *ox, float *oy, int at, float x0, float y0,
                       float cx, float cy, float x1, float y1, int steps)
{
    for (int i = 0; i <= steps; ++i) {
        const float t = (float)i / (float)steps, mt = 1.0f - t;
        ox[at + i] = mt * mt * x0 + 2.0f * mt * t * cx + t * t * x1;
        oy[at + i] = mt * mt * y0 + 2.0f * mt * t * cy + t * t * y1;
    }
    return at + steps + 1;
}

static int sample_cubic(float *ox, float *oy, int at, float x0, float y0,
                        float c1x, float c1y, float c2x, float c2y,
                        float x1, float y1, int steps)
{
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / (float)steps, mt = 1.0f - t;
        const float a = mt * mt * mt, b = 3.0f * mt * mt * t;
        const float cc = 3.0f * mt * t * t, d = t * t * t;
        ox[at + i - 1] = a * x0 + b * c1x + cc * c2x + d * x1;
        oy[at + i - 1] = a * y0 + b * c1y + cc * c2y + d * y1;
    }
    return at + steps;
}

static void draw_mouth(canvas_t *cv, const xf_t *xf, const lay_t *L,
                       const dotii_pose_t *p, float cx, float cy)
{
    const float o = clampf(p->mouth_open, 0.0f, 1.0f);
    const float m = clampf(p->mouth_w, 0.30f, 1.9f);
    const float half = m * L->R * 0.215f;
    const float ck = p->clock;
    const float LW = L->R * 0.026f;
    const float LWF = L->R * 0.009f;
    const float wob = sinf(ck * 3.1f * DOTII_TAU) * 0.5f + sinf(ck * 4.7f * DOTII_TAU + 1.1f) * 0.5f;
    const float shift = (p->mouth == DOTII_MOUTH_TALK || p->mouth == DOTII_MOUTH_SING)
                      ? wob * L->R * 0.010f : 0.0f;
    const xf_t mxf = xf_mul(*xf, xf_translate(cx + shift, cy));
    const rgb_t lip = RGB(0xf3, 0xd9, 0xe0);

    switch (p->mouth) {
    case DOTII_MOUTH_SMALL_SMILE:
        stroke_quad_local(cv, &mxf, -half * 0.58f, 0.0f, 0.0f, half * 0.52f, half * 0.58f, 0.0f, LW, C_WHITE, 1.0f);
        break;
    case DOTII_MOUTH_SMILE:
        stroke_quad_local(cv, &mxf, -half * 0.84f, 0.0f, 0.0f, half * 0.76f, half * 0.84f, 0.0f, LW, C_WHITE, 1.0f);
        break;
    case DOTII_MOUTH_SMALL:
        stroke_quad_local(cv, &mxf, -half * 0.54f, 0.0f, 0.0f, half * 0.48f, half * 0.54f, 0.0f, LW, C_WHITE, 1.0f);
        break;
    case DOTII_MOUTH_FLAT:
        stroke_quad_local(cv, &mxf, -half * 0.54f, 0.0f, 0.0f, L->R * 0.012f, half * 0.54f, 0.0f, LW, C_WHITE, 1.0f);
        break;
    case DOTII_MOUTH_TIGHT:
        stroke_quad_local(cv, &mxf, -half * 0.44f, 0.0f, 0.0f, -half * 0.20f, half * 0.44f, 0.0f, LW, C_WHITE, 1.0f);
        break;
    case DOTII_MOUTH_FROWN:
        stroke_quad_local(cv, &mxf, -half * 0.66f, 0.0f, 0.0f, -half * 0.64f, half * 0.66f, 0.0f, LW, C_WHITE, 1.0f);
        break;
    case DOTII_MOUTH_SMIRK:
        stroke_quad_local(cv, &mxf, -half * 0.66f, half * 0.06f, half * 0.02f, half * 0.34f,
                          half * 0.66f, -half * 0.24f, LW, C_WHITE, 1.0f);
        break;
    case DOTII_MOUTH_WAVY: {
        const float a = half * 0.86f;
        stroke_quad_local(cv, &mxf, -a, 0.0f, -a * 0.52f, -a * 0.30f, -a * 0.10f, 0.0f, LW, C_WHITE, 1.0f);
        stroke_quad_local(cv, &mxf, -a * 0.10f, 0.0f, a * 0.32f, a * 0.30f, a, -a * 0.04f, LW, C_WHITE, 1.0f);
        break;
    }
    case DOTII_MOUTH_GRIT: {
        const float w = half * 0.76f, h = L->R * 0.022f;
        stroke_quad_local(cv, &mxf, -w / 2.0f, -h * 0.55f, -w * 0.22f, h * 0.85f, 0.0f, h * 0.05f, LW * 0.86f, C_WHITE, 1.0f);
        stroke_quad_local(cv, &mxf, 0.0f, h * 0.05f, w * 0.22f, h * 0.85f, w / 2.0f, -h * 0.55f, LW * 0.86f, C_WHITE, 1.0f);
        break;
    }
    case DOTII_MOUTH_O:
    case DOTII_MOUTH_TALK:
    case DOTII_MOUTH_SING: {
        float rx, ry;
        if (p->mouth == DOTII_MOUTH_O)        { rx = half * 0.46f; ry = L->R * (0.052f + 0.075f * o); }
        else if (p->mouth == DOTII_MOUTH_TALK) { rx = half * 0.50f; ry = L->R * (0.030f + 0.130f * o); }
        else                                    { rx = half * 0.56f; ry = L->R * (0.050f + 0.125f * o); }
        float px[40], py[40];
        for (int i = 0; i < 32; ++i) {
            const float a = (float)i / 32.0f * DOTII_TAU;
            px[i] = cosf(a) * rx;
            py[i] = sinf(a) * ry;
        }
        fill_stroke_path(cv, &mxf, px, py, 32, true, C_MOUTH, lip, LWF, 0.55f);
        break;
    }
    case DOTII_MOUTH_BIG_SMILE:
    case DOTII_MOUTH_OPEN_SMILE: {
        const bool big = (p->mouth == DOTII_MOUTH_BIG_SMILE);
        const float h = L->R * ((big ? 0.070f : 0.060f) + 0.115f * o);
        const float a = half * (big ? 0.98f : 0.84f);
        const float topY = big ? -h * 0.14f : -h * 0.18f;
        float px[96], py[96];
        int n = 0;
        n = sample_quad(px, py, n, -a, topY, 0.0f, -h * 0.45f, a, topY, 12);
        n = sample_cubic(px, py, n, a, topY,
                         a * (big ? 0.94f : 0.90f), h * (big ? 1.24f : 1.14f),
                         a * (big ? 0.48f : 0.46f), h * (big ? 1.60f : 1.46f),
                         0.0f, h * (big ? 1.60f : 1.46f), 14);
        n = sample_cubic(px, py, n, 0.0f, h * (big ? 1.60f : 1.46f),
                         -a * (big ? 0.48f : 0.46f), h * (big ? 1.60f : 1.46f),
                         -a * (big ? 0.94f : 0.90f), h * (big ? 1.24f : 1.14f),
                         -a, topY, 14);
        fill_stroke_path(cv, &mxf, px, py, n, true, C_MOUTH, lip, LWF, 0.55f);
        break;
    }
    default:
        stroke_quad_local(cv, &mxf, -half * 0.54f, 0.0f, 0.0f, half * 0.48f, half * 0.54f, 0.0f, LW, C_WHITE, 1.0f);
        break;
    }
}

/* ================= blush ================= */
static void draw_blush(canvas_t *cv, const lay_t *L, const dotii_pose_t *p)
{
    const float base = clampf(0.16f + p->blush * 0.80f, 0.0f, 1.0f);
    if (base <= 0.005f) return;
    const float pulse = 1.0f + 0.16f * sinf(p->clock * 1.9f * DOTII_TAU + 0.6f);
    const float rx = L->R * 0.095f * pulse;
    const float ry = L->R * 0.058f * pulse;
    const float dx = L->R * 0.440f;
    const float dy = L->R * 0.105f;
    for (int side = 0; side < 2; ++side) {
        const float cx = L->CX + (side == 0 ? -dx : dx);
        const float cy = L->CY + dy;
        const int32_t x0 = (int32_t)floorf(cx - rx), x1 = (int32_t)ceilf(cx + rx);
        const int32_t y0 = (int32_t)floorf(cy - ry), y1 = (int32_t)ceilf(cy + ry);
        const float inv_rx = 1.0f / rx, inv_ry = 1.0f / ry;
        for (int32_t py = y0; py <= y1; ++py) {
            const float v = ((float)py + 0.5f - cy) * inv_ry;
            float u = ((float)x0 + 0.5f - cx) * inv_rx;
            const float du = inv_rx;
            for (int32_t px = x0; px <= x1; ++px, u += du) {
                const float q = u * u + v * v;
                if (q >= 1.0f) continue;
                const float f = 1.0f - q;
                blend_add(cv, px, py, C_BLUSH, base * f * f * 0.92f);
            }
        }
    }
}

/* ================= glasses ================= */
static void stroke_ellipse_local(canvas_t *cv, const xf_t *m, float cx, float cy,
                                 float rx, float ry, float w, rgb_t c, float a)
{
    float px[49], py[49];
    for (int i = 0; i < 48; ++i) {
        const float t = (float)i / 48.0f * DOTII_TAU;
        px[i] = cx + cosf(t) * rx;
        py[i] = cy + sinf(t) * ry;
    }
    for (int i = 1; i < 48; ++i) {
        float ax, ay, bx, by;
        xf_apply(m, px[i - 1], py[i - 1], &ax, &ay);
        xf_apply(m, px[i], py[i], &bx, &by);
        stroke_line(cv, ax, ay, bx, by, w, c, a);
    }
    float ax, ay, bx, by;
    xf_apply(m, px[47], py[47], &ax, &ay);
    xf_apply(m, px[0], py[0], &bx, &by);
    stroke_line(cv, ax, ay, bx, by, w, c, a);
}

static void draw_glasses(canvas_t *cv, const xf_t *xf, const lay_t *L, const dotii_pose_t *p)
{
    const float er = L->eyeW * 0.5f;
    const float gx = er * 1.18f, gy = er * 1.16f;
    const float lw = L->R * 0.011f;
    const float sep = L->eyeSep;
    stroke_ellipse_local(cv, xf, L->CX - sep, L->eyeY, gx, gy, lw, C_WHITE, 0.92f);
    stroke_ellipse_local(cv, xf, L->CX + sep, L->eyeY, gx, gy, lw, C_WHITE, 0.92f);
    stroke_quad_local(cv, xf, L->CX - sep + gx * 0.92f, L->eyeY - L->R * 0.012f,
                      L->CX, L->eyeY - L->R * 0.030f,
                      L->CX + sep - gx * 0.92f, L->eyeY - L->R * 0.012f, lw, C_WHITE, 0.92f);
    float ax, ay, bx, by;
    xf_apply(xf, L->CX - sep - gx * 0.98f, L->eyeY - L->R * 0.010f, &ax, &ay);
    xf_apply(xf, L->CX - sep - gx * 1.20f, L->eyeY - L->R * 0.030f, &bx, &by);
    stroke_line(cv, ax, ay, bx, by, lw, C_WHITE, 0.92f);
    xf_apply(xf, L->CX + sep + gx * 0.98f, L->eyeY - L->R * 0.010f, &ax, &ay);
    xf_apply(xf, L->CX + sep + gx * 1.20f, L->eyeY - L->R * 0.030f, &bx, &by);
    stroke_line(cv, ax, ay, bx, by, lw, C_WHITE, 0.92f);
    (void)p;
}

/* ================= effects ================= */
enum { SLOT_A, SLOT_B, SLOT_C, SLOT_D, SLOT_E, SLOT_F, SLOT_G, SLOT_H, SLOT_I, SLOT_J };
static const float k_slots[10][2] = {
    {-0.575f, -0.560f}, { 0.575f, -0.560f}, { 0.000f, -0.795f},
    {-0.735f, -0.045f}, { 0.735f, -0.045f},
    {-0.560f,  0.560f}, { 0.560f,  0.560f}, { 0.000f,  0.790f},
    {-0.700f, -0.395f}, { 0.700f, -0.395f},
};
static void fx_pos(const lay_t *L, int slot, float *x, float *y)
{
    if (slot < 0 || slot > 9) slot = SLOT_A;
    *x = L->CX + k_slots[slot][0] * L->R;
    *y = L->CY + k_slots[slot][1] * L->R;
}

static void gl_line(canvas_t *cv, const xf_t *g, float gs,
                    float x0, float y0, float x1, float y1,
                    float wn, rgb_t c, float a)
{
    float ax, ay, bx, by;
    xf_apply(g, x0, y0, &ax, &ay);
    xf_apply(g, x1, y1, &bx, &by);
    stroke_line(cv, ax, ay, bx, by, wn * gs, c, a);
}
static void gl_poly(canvas_t *cv, const xf_t *g, const float *lx, const float *ly,
                    int n, rgb_t c, float a)
{
    float px[32], py[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; ++i) xf_apply(g, lx[i], ly[i], &px[i], &py[i]);
    fill_polygon(cv, px, py, n, c, a);
}
static void gl_circle(canvas_t *cv, const xf_t *g, float gs, float x, float y,
                      float r, rgb_t c, float a)
{
    float sx, sy;
    xf_apply(g, x, y, &sx, &sy);
    fill_circle(cv, sx, sy, r * gs, c, a);
}
static void gl_rrect(canvas_t *cv, const xf_t *g, float gs, float x, float y,
                     float w, float h, float r, bool stroke, rgb_t c, float a, float sw)
{
    float px[40], py[40];
    int n = 0;
    const float rr = minf2(r, minf2(w, h) * 0.5f);
    const float cx[4] = {x + w - rr, x + w - rr, x + rr, x + rr};
    const float cy[4] = {y + rr, y + h - rr, y + h - rr, y + rr};
    const float a0[4] = {-0.5f * (float)M_PI, 0.0f, 0.5f * (float)M_PI, (float)M_PI};
    for (int q = 0; q < 4; ++q) {
        for (int i = 0; i < 8; ++i) {
            const float t = a0[q] + (float)i / 8.0f * 0.5f * (float)M_PI;
            px[n] = cx[q] + cosf(t) * rr;
            py[n] = cy[q] + sinf(t) * rr;
            ++n;
        }
    }
    if (stroke) {
        for (int i = 1; i < n; ++i) {
            float ax, ay, bx, by;
            xf_apply(g, px[i - 1], py[i - 1], &ax, &ay);
            xf_apply(g, px[i], py[i], &bx, &by);
            stroke_line(cv, ax, ay, bx, by, sw * gs, c, a);
        }
        float ax, ay, bx, by;
        xf_apply(g, px[n - 1], py[n - 1], &ax, &ay);
        xf_apply(g, px[0], py[0], &bx, &by);
        stroke_line(cv, ax, ay, bx, by, sw * gs, c, a);
    } else {
        gl_poly(cv, g, px, py, n, c, a);
    }
}

static void draw_z_glyph(canvas_t *cv, const xf_t *g, float gs, float s, rgb_t c, float a)
{
    gl_line(cv, g, gs, -s, -s, s, -s, 0.20f * s * 4.0f, c, a);
    gl_line(cv, g, gs, s, -s, -s, s, 0.20f * s * 4.0f, c, a);
    gl_line(cv, g, gs, -s, s, s, s, 0.20f * s * 4.0f, c, a);
}

static void fx_glow(canvas_t *cv, float x, float y, float r, rgb_t c, float a)
{
    const int32_t x0 = (int32_t)floorf(x - r), x1 = (int32_t)ceilf(x + r);
    const int32_t y0 = (int32_t)floorf(y - r), y1 = (int32_t)ceilf(y + r);
    for (int32_t py = y0; py <= y1; ++py) {
        for (int32_t px = x0; px <= x1; ++px) {
            const float dx = (float)px + 0.5f - x;
            const float dy = (float)py + 0.5f - y;
            const float q = sqrtf(dx * dx + dy * dy) / r;
            if (q >= 1.0f) continue;
            const float f = 1.0f - q;
            blend_add(cv, px, py, c, a * f * f);
        }
    }
}

/* glyphs are drawn in a [-1,1] normalised box */
static void gl_check(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    gl_line(cv, g, gs, -0.44f, 0.02f, -0.11f, 0.34f, 0.20f, ac, 1.0f);
    gl_line(cv, g, gs, -0.11f, 0.34f, 0.47f, -0.33f, 0.20f, ac, 1.0f);
}
static void gl_cross(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    gl_line(cv, g, gs, -0.34f, -0.34f, 0.34f, 0.34f, 0.19f, ac, 1.0f);
    gl_line(cv, g, gs, 0.34f, -0.34f, -0.34f, 0.34f, 0.19f, ac, 1.0f);
}
static void gl_exclaim(canvas_t *cv, const xf_t *g, float gs, rgb_t ac, bool box)
{
    if (box) {
        float lx[3] = {0.0f, 0.55f, -0.55f}, ly[3] = {-0.52f, 0.40f, 0.40f};
        gl_line(cv, g, gs, lx[0], ly[0], lx[1], ly[1], 0.16f, ac, 1.0f);
        gl_line(cv, g, gs, lx[1], ly[1], lx[2], ly[2], 0.16f, ac, 1.0f);
        gl_line(cv, g, gs, lx[2], ly[2], lx[0], ly[0], 0.16f, ac, 1.0f);
    } else {
        float px[4] = {-0.075f, 0.075f, 0.048f, -0.048f};
        float py[4] = {-0.20f, -0.20f, 0.10f, 0.10f};
        gl_poly(cv, g, px, py, 4, ac, 1.0f);
    }
    gl_line(cv, g, gs, 0.0f, -0.46f, 0.0f, 0.10f, 0.18f, ac, 1.0f);
    gl_circle(cv, g, gs, 0.0f, 0.30f, 0.065f, ac, 1.0f);
}
static void gl_question(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[12], py[12];
    for (int i = 0; i < 10; ++i) {
        const float t = -0.62f + (float)i / 9.0f * 1.35f;
        px[i] = cosf(t) * 0.30f;
        py[i] = -0.30f + sinf(t) * 0.30f;
    }
    for (int i = 1; i < 10; ++i) gl_line(cv, g, gs, px[i - 1], py[i - 1], px[i], py[i], 0.17f, ac, 1.0f);
    gl_line(cv, g, gs, px[9], py[9], 0.0f, 0.02f, 0.17f, ac, 1.0f);
    gl_line(cv, g, gs, 0.0f, 0.02f, 0.0f, 0.12f, 0.17f, ac, 1.0f);
    gl_circle(cv, g, gs, 0.0f, 0.32f, 0.065f, ac, 1.0f);
}
static void gl_gear(canvas_t *cv, const xf_t *g, float gs, rgb_t ac, rgb_t plate)
{
    float px[24], py[24];
    for (int i = 0; i < 20; ++i) {
        const float t = (float)i / 20.0f * DOTII_TAU;
        px[i] = cosf(t) * 0.34f; py[i] = sinf(t) * 0.34f;
    }
    for (int i = 1; i < 20; ++i) gl_line(cv, g, gs, px[i - 1], py[i - 1], px[i], py[i], 0.15f, ac, 1.0f);
    gl_line(cv, g, gs, px[19], py[19], px[0], py[0], 0.15f, ac, 1.0f);
    for (int i = 0; i < 8; ++i) {
        const float t = (float)i / 8.0f * DOTII_TAU;
        gl_line(cv, g, gs, cosf(t) * 0.28f, sinf(t) * 0.28f, cosf(t) * 0.58f, sinf(t) * 0.58f, 0.15f, ac, 1.0f);
    }
    gl_circle(cv, g, gs, 0.0f, 0.0f, 0.145f, plate, 1.0f);
}
static void gl_search(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[16], py[16];
    for (int i = 0; i < 16; ++i) {
        const float t = (float)i / 16.0f * DOTII_TAU;
        px[i] = -0.15f + cosf(t) * 0.31f; py[i] = -0.15f + sinf(t) * 0.31f;
    }
    for (int i = 1; i < 16; ++i) gl_line(cv, g, gs, px[i - 1], py[i - 1], px[i], py[i], 0.16f, ac, 1.0f);
    gl_line(cv, g, gs, px[15], py[15], px[0], py[0], 0.16f, ac, 1.0f);
    gl_line(cv, g, gs, 0.04f, 0.04f, 0.44f, 0.44f, 0.16f, ac, 1.0f);
}
static void gl_code(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    gl_line(cv, g, gs, -0.20f, -0.32f, -0.46f, 0.0f, 0.15f, ac, 1.0f);
    gl_line(cv, g, gs, -0.46f, 0.0f, -0.20f, 0.32f, 0.15f, ac, 1.0f);
    gl_line(cv, g, gs, 0.20f, -0.32f, 0.46f, 0.0f, 0.15f, ac, 1.0f);
    gl_line(cv, g, gs, 0.46f, 0.0f, 0.20f, 0.32f, 0.15f, ac, 1.0f);
    gl_line(cv, g, gs, 0.08f, -0.42f, -0.08f, 0.42f, 0.13f, ac, 1.0f);
}
static void gl_dots(canvas_t *cv, const xf_t *g, float gs, rgb_t ac, float ck)
{
    for (int i = -1; i <= 1; ++i) {
        const float p = 0.40f + 0.60f * fabsf(sinf(ck * 2.15f + (float)i * 1.05f));
        gl_circle(cv, g, gs, (float)i * 0.32f, 0.0f, 0.118f, ac, 0.30f + 0.62f * p);
    }
}
static void gl_refresh(canvas_t *cv, const xf_t *g, float gs, rgb_t ac, float ck)
{
    float px[20], py[20];
    const float a0 = (float)M_PI * 0.18f, a1 = (float)M_PI * 1.58f;
    for (int i = 0; i < 20; ++i) {
        const float t = a0 + (a1 - a0) * (float)i / 19.0f;
        const float r = 0.36f;
        const float x = cosf(t) * r, y = sinf(t) * r;
        px[i] = x * cosf(ck * 1.75f) - y * sinf(ck * 1.75f);
        py[i] = x * sinf(ck * 1.75f) + y * cosf(ck * 1.75f);
    }
    for (int i = 1; i < 20; ++i) gl_line(cv, g, gs, px[i - 1], py[i - 1], px[i], py[i], 0.17f, ac, 1.0f);
    const float ex = px[19], ey = py[19], tip = a1 + 0.30f + ck * 1.75f;
    float tx[3], ty[3];
    tx[0] = ex + cosf(tip) * 0.23f; ty[0] = ey + sinf(tip) * 0.23f;
    tx[1] = ex + cosf(tip + 1.15f) * 0.19f; ty[1] = ey + sinf(tip + 1.15f) * 0.19f;
    tx[2] = ex + cosf(tip - 1.15f) * 0.19f; ty[2] = ey + sinf(tip - 1.15f) * 0.19f;
    gl_poly(cv, g, tx, ty, 3, ac, 1.0f);
}
static void gl_battery(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    gl_rrect(cv, g, gs, -0.54f, -0.30f, 0.88f, 0.60f, 0.15f, true, ac, 1.0f, 0.15f);
    gl_line(cv, g, gs, 0.36f, -0.13f, 0.36f, 0.13f, 0.15f, ac, 1.0f);
    gl_rrect(cv, g, gs, -0.40f, -0.16f, 0.30f, 0.32f, 0.06f, false, ac, 0.55f, 0.0f);
}
static void gl_bolt(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[6] = {0.12f, -0.33f, -0.02f, -0.15f, 0.37f, 0.04f};
    float py[6] = {-0.54f, 0.05f, 0.05f, -0.10f, -0.10f, 0.0f};
    gl_poly(cv, g, px, py, 6, ac, 1.0f);
    /* the JS path is a 7-point bolt; approximate the lower half */
    float qx[4] = {-0.15f, 0.04f, -0.10f, 0.37f};
    float qy[4] = {0.05f, -0.10f, -0.10f, 0.0f};
    (void)qx; (void)qy;
}
static void gl_lock(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[12], py[12];
    for (int i = 0; i < 12; ++i) {
        const float t = (float)M_PI + (float)i / 11.0f * (float)M_PI;
        px[i] = cosf(t) * 0.25f; py[i] = -0.15f + sinf(t) * 0.25f;
    }
    for (int i = 1; i < 12; ++i) gl_line(cv, g, gs, px[i - 1], py[i - 1], px[i], py[i], 0.16f, ac, 1.0f);
    gl_rrect(cv, g, gs, -0.42f, -0.13f, 0.84f, 0.58f, 0.15f, true, ac, 1.0f, 0.15f);
    gl_circle(cv, g, gs, 0.0f, 0.12f, 0.085f, ac, 1.0f);
    gl_line(cv, g, gs, 0.0f, 0.12f, 0.0f, 0.26f, 0.11f, ac, 1.0f);
}
static void gl_mute(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[6] = {-0.52f, -0.24f, 0.06f, 0.06f, -0.24f, -0.52f};
    float py[6] = {-0.20f, -0.20f, -0.50f, 0.50f, 0.20f, 0.20f};
    gl_poly(cv, g, px, py, 6, ac, 1.0f);
    gl_line(cv, g, gs, -0.40f, 0.42f, 0.46f, -0.42f, 0.17f, ac, 1.0f);
}
static void gl_vol(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[6] = {-0.56f, -0.28f, 0.02f, 0.02f, -0.28f, -0.56f};
    float py[6] = {-0.20f, -0.20f, -0.48f, 0.48f, 0.20f, 0.20f};
    gl_poly(cv, g, px, py, 6, ac, 1.0f);
    float ax[10], ay[10];
    for (int i = 0; i < 10; ++i) {
        const float t = -0.88f + (float)i / 9.0f * 1.76f;
        ax[i] = 0.04f + cosf(t) * 0.26f; ay[i] = sinf(t) * 0.26f;
    }
    for (int i = 1; i < 10; ++i) gl_line(cv, g, gs, ax[i - 1], ay[i - 1], ax[i], ay[i], 0.14f, ac, 1.0f);
    for (int i = 0; i < 10; ++i) {
        const float t = -0.80f + (float)i / 9.0f * 1.60f;
        ax[i] = 0.04f + cosf(t) * 0.44f; ay[i] = sinf(t) * 0.44f;
    }
    for (int i = 1; i < 10; ++i) gl_line(cv, g, gs, ax[i - 1], ay[i - 1], ax[i], ay[i], 0.14f, ac, 1.0f);
}
static void gl_moon(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[42], py[42];
    int n = 0;
    for (int i = 0; i <= 20; ++i) {
        const float t = -2.45f + (float)i / 20.0f * 4.90f;
        px[n] = cosf(t) * 0.46f; py[n] = sinf(t) * 0.46f; ++n;
    }
    for (int i = 0; i <= 20; ++i) {
        const float t = 2.45f - (float)i / 20.0f * 4.90f;
        px[n] = 0.21f + cosf(t) * 0.42f; py[n] = -0.13f + sinf(t) * 0.42f; ++n;
    }
    gl_poly(cv, g, px, py, n, ac, 1.0f);
}
static void gl_cloud_slash(canvas_t *cv, const xf_t *g, float gs, rgb_t ac, rgb_t plate)
{
    gl_circle(cv, g, gs, -0.22f, 0.02f, 0.24f, ac, 1.0f);
    gl_circle(cv, g, gs, 0.06f, -0.10f, 0.30f, ac, 1.0f);
    gl_circle(cv, g, gs, 0.34f, 0.04f, 0.22f, ac, 1.0f);
    gl_rrect(cv, g, gs, -0.24f, 0.04f, 0.58f, 0.22f, 0.02f, false, ac, 1.0f, 0.0f);
    gl_line(cv, g, gs, -0.48f, 0.46f, 0.46f, -0.44f, 0.21f, plate, 1.0f);
}
static void gl_scan(canvas_t *cv, const xf_t *g, float gs, rgb_t ac, float ck)
{
    const float L = 0.44f, G = 0.20f;
    for (int s = 0; s < 4; ++s) {
        const float sx = (s == 0 || s == 3) ? -1.0f : 1.0f;
        const float sy = (s < 2) ? -1.0f : 1.0f;
        gl_line(cv, g, gs, sx * L - sx * G, sy * L, sx * L, sy * L, 0.15f, ac, 1.0f);
        gl_line(cv, g, gs, sx * L, sy * L, sx * L, sy * L - sy * G, 0.15f, ac, 1.0f);
    }
    const float sy = sinf(ck * 1.85f) * 0.30f;
    gl_line(cv, g, gs, -0.30f, sy, 0.30f, sy, 0.10f, ac, 0.65f);
}
static void gl_bell(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[24], py[24];
    int n = sample_quad(px, py, 0, -0.40f, 0.24f, 0.0f, -0.58f, 0.40f, 0.24f, 12);
    for (int i = 1; i < n; ++i) gl_line(cv, g, gs, px[i - 1], py[i - 1], px[i], py[i], 0.16f, ac, 1.0f);
    gl_line(cv, g, gs, px[n - 1], py[n - 1], px[0], py[0], 0.16f, ac, 1.0f);
    gl_rrect(cv, g, gs, -0.48f, 0.22f, 0.96f, 0.13f, 0.065f, false, ac, 1.0f, 0.0f);
    gl_circle(cv, g, gs, 0.0f, 0.42f, 0.10f, ac, 1.0f);
    gl_circle(cv, g, gs, 0.0f, -0.38f, 0.095f, ac, 1.0f);
}
static void gl_book(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    float px[40], py[40];
    int n = 0;
    px[n] = 0.0f; py[n] = -0.24f; ++n;
    n = sample_quad(px, py, n, 0.0f, -0.24f, -0.22f, -0.37f, -0.50f, -0.26f, 8);
    px[n] = -0.50f; py[n] = 0.24f; ++n;
    n = sample_quad(px, py, n, -0.50f, 0.24f, -0.22f, 0.13f, 0.0f, 0.24f, 8);
    n = sample_quad(px, py, n, 0.0f, 0.24f, 0.22f, 0.13f, 0.50f, 0.24f, 8);
    px[n] = 0.50f; py[n] = -0.26f; ++n;
    n = sample_quad(px, py, n, 0.50f, -0.26f, 0.22f, -0.37f, 0.0f, -0.24f, 8);
    for (int i = 1; i < n; ++i) gl_line(cv, g, gs, px[i - 1], py[i - 1], px[i], py[i], 0.15f, ac, 1.0f);
    gl_line(cv, g, gs, px[n - 1], py[n - 1], px[0], py[0], 0.15f, ac, 1.0f);
    gl_line(cv, g, gs, 0.0f, -0.24f, 0.0f, 0.24f, 0.15f, ac, 1.0f);
}
static void gl_hand(canvas_t *cv, const xf_t *g, float gs, rgb_t ac)
{
    gl_rrect(cv, g, gs, -0.34f, 0.0f, 0.66f, 0.46f, 0.18f, false, ac, 1.0f, 0.0f);
    const float fx[4] = {-0.29f, -0.11f, 0.07f, 0.25f};
    for (int i = 0; i < 4; ++i) {
        const float h = 0.36f - fabsf((float)i - 1.3f) * 0.07f;
        gl_rrect(cv, g, gs, fx[i], -0.04f - h, 0.13f, h + 0.12f, 0.065f, false, ac, 1.0f, 0.0f);
    }
    gl_rrect(cv, g, gs, -0.42f, 0.12f, 0.14f, 0.28f, 0.07f, false, ac, 1.0f, 0.0f);
}

static void fx_badge(canvas_t *cv, const xf_t *xf, const lay_t *L, int slot,
                     rgb_t ac, uint8_t kind, float ck)
{
    float bx, by;
    fx_pos(L, slot, &bx, &by);
    by += sinf(ck * 1.45f + (float)slot * 0.37f) * L->R * 0.0050f;
    const float gs = L->R * 0.070f;
    fx_glow(cv, bx, by, gs * 2.7f, ac, 0.30f);
    const float rot = sinf(ck * 0.85f + (float)slot * 0.41f) * 0.040f;
    const xf_t g = xf_mul(*xf, xf_mul(xf_translate(bx, by), xf_mul(xf_rotate(rot), xf_scale(gs, gs))));
    const rgb_t tint = mix_rgb(ac, C_WHITE, 0.42f);
    const rgb_t dark = mix_rgb(ac, RGB(5, 9, 15), 0.72f);

    switch (kind) {
    case DOTII_FX_CHECK:       gl_check(cv, &g, gs, tint); break;
    case DOTII_FX_CROSS:       gl_cross(cv, &g, gs, tint); break;
    case DOTII_FX_QUESTION:    gl_question(cv, &g, gs, tint); break;
    case DOTII_FX_EXCLAIM:     gl_exclaim(cv, &g, gs, tint, false); break;
    case DOTII_FX_WARN:        gl_exclaim(cv, &g, gs, tint, true); break;
    case DOTII_FX_GEAR:        gl_gear(cv, &g, gs, tint, dark); break;
    case DOTII_FX_SEARCH:      gl_search(cv, &g, gs, tint); break;
    case DOTII_FX_CODE:        gl_code(cv, &g, gs, tint); break;
    case DOTII_FX_DOTS:        gl_dots(cv, &g, gs, tint, ck); break;
    case DOTII_FX_REFRESH:     gl_refresh(cv, &g, gs, tint, ck); break;
    case DOTII_FX_BATTERY_LOW: gl_battery(cv, &g, gs, tint); break;
    case DOTII_FX_BOLT:        gl_bolt(cv, &g, gs, tint); break;
    case DOTII_FX_LOCK:        gl_lock(cv, &g, gs, tint); break;
    case DOTII_FX_MUTE:        gl_mute(cv, &g, gs, tint); break;
    case DOTII_FX_VOL:         gl_vol(cv, &g, gs, tint); break;
    case DOTII_FX_MOON:        gl_moon(cv, &g, gs, tint); break;
    case DOTII_FX_CLOUD_SLASH: gl_cloud_slash(cv, &g, gs, tint, dark); break;
    case DOTII_FX_SCAN:        gl_scan(cv, &g, gs, tint, ck); break;
    case DOTII_FX_BELL:        gl_bell(cv, &g, gs, tint); break;
    case DOTII_FX_BOOK:        gl_book(cv, &g, gs, tint); break;
    case DOTII_FX_HAND:        gl_hand(cv, &g, gs, tint); break;
    default: break;
    }
}

static int badge_slot(uint8_t fx)
{
    switch (fx) {
    case DOTII_FX_QUESTION:
    case DOTII_FX_EXCLAIM: return SLOT_I;
    case DOTII_FX_WARN:
    case DOTII_FX_MOON: return SLOT_C;
    case DOTII_FX_BELL:
    case DOTII_FX_BOOK: return SLOT_B;
    case DOTII_FX_HAND: return SLOT_E;
    case DOTII_FX_SCAN:
    case DOTII_FX_CLOUD_SLASH: return SLOT_D;
    case DOTII_FX_SEARCH:
    case DOTII_FX_GEAR:
    case DOTII_FX_REFRESH:
    case DOTII_FX_BATTERY_LOW:
    case DOTII_FX_LOCK: return SLOT_E;
    case DOTII_FX_CHECK:
    case DOTII_FX_CROSS: return SLOT_F;
    case DOTII_FX_BOLT:
    case DOTII_FX_MUTE:
    case DOTII_FX_VOL: return SLOT_G;
    case DOTII_FX_DOTS: return SLOT_H;
    case DOTII_FX_CODE: return SLOT_J;
    default: return -1;
    }
}

static void draw_effects(canvas_t *cv, const xf_t *xf, const lay_t *L,
                         const dotii_pose_t *p, rgb_t accent)
{
    for (int i = 0; i < p->fx_count && i < 3; ++i) {
        const uint8_t fx = p->fx[i];
        const int slot = badge_slot(fx);
        if (slot >= 0) { fx_badge(cv, xf, L, slot, accent, fx, p->clock); continue; }

        switch (fx) {
        case DOTII_FX_SPARKLE: {
            const int slots[3] = {SLOT_A, SLOT_B, SLOT_C};
            for (int k = 0; k < 3; ++k) {
                float x, y; fx_pos(L, slots[k], &x, &y);
                const float ph = fmodf(p->clock * 0.95f + (float)k * 0.37f, 1.0f);
                const float al = sinf((float)M_PI * ph);
                const float rr = L->R * (0.030f + 0.026f * al);
                spark4(cv, x, y, rr, L->R * 0.0085f, accent, 0.18f + 0.62f * al);
                if (al > 0.72f) fill_circle(cv, x, y, rr * 0.24f, C_WHITE, 0.80f * al);
            }
            break;
        }
        case DOTII_FX_ZZZ: {
            const float bx = L->CX + L->R * 0.22f, by = L->CY - L->R * 0.58f;
            for (int k = 0; k < 3; ++k) {
                const float ph = fmodf(p->clock * 0.30f + (float)k * 0.333f, 1.0f);
                const float al = sinf((float)M_PI * ph);
                const float s = L->R * (0.030f + (float)k * 0.005f) * (0.72f + 0.46f * ph);
                const float x = bx + (float)k * L->R * 0.066f + ph * L->R * 0.050f;
                const float y = by - ph * L->R * 0.24f;
                float ax[3], ay[3];
                xf_apply(xf, x, y, &ax[0], &ay[0]);
                xf_t g = xf_mul(*xf, xf_mul(xf_translate(x, y), xf_scale(s, s)));
                gl_line(cv, &g, s, -1.0f, -1.0f, 1.0f, -1.0f, 0.22f, accent, 0.16f + 0.66f * al);
                gl_line(cv, &g, s, 1.0f, -1.0f, -1.0f, 1.0f, 0.22f, accent, 0.16f + 0.66f * al);
                gl_line(cv, &g, s, -1.0f, 1.0f, 1.0f, 1.0f, 0.22f, accent, 0.16f + 0.66f * al);
                (void)draw_z_glyph;
            }
            break;
        }
        case DOTII_FX_WAVE: {
            const float bx = L->CX + L->R * 0.78f, by = L->CY - L->R * 0.04f;
            for (int k = 0; k < 2; ++k) {
                const float a = 0.40f + 0.38f * fabsf(sinf(p->clock * 2.0f + (float)k * 0.70f));
                stroke_arc(cv, bx, by, L->R * (0.066f + (float)k * 0.030f), -a, a,
                           maxf2(1.6f, L->R * 0.0072f), RGB(0xee, 0xf2, 0xf8), 0.20f + 0.46f * a);
            }
            fill_circle(cv, bx, by, L->R * 0.014f, RGB(0xee, 0xf2, 0xf8), 0.85f);
            break;
        }
        case DOTII_FX_HEARTS: {
            for (int k = 0; k < 3; ++k) {
                const float ph = fmodf(p->clock * 0.42f + (float)k * 0.33f, 1.0f);
                const float al = sinf((float)M_PI * ph);
                const float s = L->R * (0.034f + 0.012f * ph);
                const float x = L->CX + L->R * (0.62f + 0.06f * (float)k) + sinf(p->clock * 1.3f + (float)k) * L->R * 0.02f;
                const float y = L->CY - L->R * (0.30f - ph * 0.55f);
                float sx, sy;
                xf_apply(xf, x, y, &sx, &sy);
                fill_heart(cv, sx, sy, s, C_BLUSH, 0.22f + 0.70f * al);
            }
            break;
        }
        case DOTII_FX_TEAR: {
            const float ph = 0.5f + 0.5f * sinf(p->clock * 1.15f);
            const float x = L->CX - L->R * 0.42f;
            const float y = L->CY + L->R * 0.10f + ph * L->R * 0.045f;
            const float s = L->R * 0.030f * (0.85f + 0.30f * ph);
            float sx, sy;
            xf_apply(xf, x, y, &sx, &sy);
            float px[3] = {sx, sx - s * 0.62f, sx + s * 0.62f};
            float py[3] = {sy - s * 1.10f, sy + s * 0.60f, sy + s * 0.60f};
            fill_polygon(cv, px, py, 3, accent, 0.80f);
            fill_circle(cv, sx, sy + s * 0.48f, s * 0.66f, accent, 0.80f);
            break;
        }
        case DOTII_FX_ANGER: {
            const float x = L->CX, y = L->CY - L->R * 0.56f;
            const float s = L->R * 0.075f;
            for (int k = 0; k < 3; ++k) {
                const float a = -0.6f + (float)k * 0.6f;
                const float ax = x + cosf(a) * s, ay = y + sinf(a) * s;
                float px0, py0;
                xf_apply(xf, ax, ay, &px0, &py0);
                float px1, py1;
                xf_apply(xf, ax + cosf(a + 2.2f) * s * 0.7f, ay + sinf(a + 2.2f) * s * 0.7f, &px1, &py1);
                stroke_line(cv, px0, py0, px1, py1, L->R * 0.010f, accent, 0.85f);
            }
            break;
        }
        case DOTII_FX_NOTE: {
            const float bob = sinf(p->clock * 1.6f) * L->R * 0.012f;
            const float x = L->CX + L->R * 0.62f, y = L->CY - L->R * 0.42f + bob;
            float sx, sy;
            xf_apply(xf, x, y, &sx, &sy);
            const float s = L->R * 0.045f;
            fill_ellipse(cv, sx, sy + s * 0.55f, s * 0.55f, s * 0.42f, accent, 0.9f);
            stroke_line(cv, sx + s * 0.50f, sy + s * 0.55f, sx + s * 0.50f, sy - s * 0.85f, L->R * 0.009f, accent, 0.9f);
            stroke_line(cv, sx + s * 0.50f, sy - s * 0.85f, sx + s * 1.05f, sy - s * 0.55f, L->R * 0.009f, accent, 0.9f);
            break;
        }
        case DOTII_FX_THINK: {
            for (int k = 0; k < 3; ++k) {
                const float ph = fmodf(p->clock * 0.55f + (float)k * 0.30f, 1.0f);
                const float al = sinf((float)M_PI * ph);
                const float x = L->CX + L->R * (0.55f + ph * 0.08f);
                const float y = L->CY - L->R * (0.30f + ph * 0.22f);
                float sx, sy;
                xf_apply(xf, x, y, &sx, &sy);
                fill_circle(cv, sx, sy, L->R * (0.010f + 0.010f * ph), accent, 0.20f + 0.55f * al);
            }
            break;
        }
        default: break;
        }
    }
}

/* ================= background ================= */
/*
 * The face background is literally pure black, edge to edge, and it is written
 * straight into the frame buffer by dotii_draw_render().
 *
 * History of this background, and why it is now nothing at all:
 *   1. A radial gradient (#2c2c35 -> #08080b) quantised into concentric bands
 *      in RGB565 on the real panel.
 *   2. Adding per-channel ordered dithering to hide those bands tinted them
 *      red/green/blue, so the bands became *coloured* rings.
 *   3. Flattening to black but keeping a thin C_EDGE (#2a2e39) outline left a
 *      faint violet-blue halo hugging the panel rim.
 * The panel is a round AMOLED: a black pixel is simply an unlit pixel, which is
 * exactly the clean look wanted here. Every non-zero value near the rim risks
 * another visible halo, so the whole canvas is black and the facial features are
 * the only thing ever painted on top.
 *
 * There used to be a cached 466x466 uint16_t buffer here, purely so the
 * per-frame memset could be skipped. It held 434 KB of contiguous PSRAM, and
 * once MultiNet (2.6 MB) is resident the largest free PSRAM block is only about
 * 256 KB, so that allocation failed. The renderer then bailed out on the NULL
 * cache and left the canvas holding uninitialised PSRAM, which the panel showed
 * as an all-snow screen with one blurry eye. The buffer is owned by the caller
 * and zeroed per frame, so the cache only ever duplicated memory.
 */

/* ================= content bounds ================= */
static const float k_fx_bbox[DOTII_FX_COUNT][4] = {
    /* SPARKLE   */ {-0.66f, -0.88f,  0.66f, -0.45f},
    /* ZZZ       */ { 0.16f, -0.86f,  0.58f, -0.34f},
    /* WAVE      */ { 0.68f, -0.20f,  0.92f,  0.10f},
    /* THINK     */ { 0.45f, -0.62f,  0.75f, -0.26f},
    /* HEARTS    */ { 0.55f, -0.90f,  0.82f, -0.20f},
    /* QUESTION  */ {-0.78f, -0.50f, -0.62f, -0.28f},
    /* TEAR      */ {-0.48f,  0.00f, -0.34f,  0.22f},
    /* ANGER     */ {-0.20f, -0.66f,  0.20f, -0.46f},
    /* HAND      */ { 0.66f, -0.12f,  0.82f,  0.08f},
    /* SCAN      */ {-0.80f, -0.12f, -0.66f,  0.06f},
    /* SEARCH    */ { 0.66f, -0.12f,  0.82f,  0.06f},
    /* BOOK      */ { 0.51f, -0.64f,  0.65f, -0.47f},
    /* CODE      */ { 0.66f, -0.47f,  0.80f, -0.32f},
    /* GEAR      */ { 0.66f, -0.12f,  0.82f,  0.06f},
    /* DOTS      */ {-0.12f,  0.72f,  0.12f,  0.87f},
    /* CHECK     */ {-0.64f,  0.48f, -0.48f,  0.65f},
    /* CROSS     */ {-0.64f,  0.48f, -0.48f,  0.65f},
    /* EXCLAIM   */ {-0.76f, -0.50f, -0.64f, -0.26f},
    /* NOTE      */ { 0.56f, -0.52f,  0.72f, -0.32f},
    /* BELL      */ { 0.51f, -0.64f,  0.65f, -0.47f},
    /* CLOUDSLASH*/ {-0.80f, -0.12f, -0.66f,  0.06f},
    /* REFRESH   */ { 0.66f, -0.12f,  0.82f,  0.06f},
    /* BATTERYLOW*/ { 0.66f, -0.12f,  0.82f,  0.06f},
    /* BOLT      */ { 0.51f,  0.48f,  0.65f,  0.65f},
    /* MUTE      */ { 0.51f,  0.48f,  0.65f,  0.65f},
    /* VOL       */ { 0.51f,  0.48f,  0.65f,  0.65f},
    /* MOON      */ {-0.10f, -0.88f,  0.10f, -0.72f},
    /* WARN      */ {-0.10f, -0.88f,  0.10f, -0.72f},
    /* LOCK      */ { 0.66f, -0.12f,  0.82f,  0.06f},
};

static void rect_reset(dotii_rect_t *r)
{
    r->x1 = 0x7FFFFFFF; r->y1 = 0x7FFFFFFF;
    r->x2 = -0x7FFFFFFF; r->y2 = -0x7FFFFFFF;
}
static void rect_add(dotii_rect_t *r, float x, float y, float pad)
{
    const int32_t x1 = (int32_t)floorf(x - pad), x2 = (int32_t)ceilf(x + pad);
    const int32_t y1 = (int32_t)floorf(y - pad), y2 = (int32_t)ceilf(y + pad);
    if (x1 < r->x1) r->x1 = x1;
    if (y1 < r->y1) r->y1 = y1;
    if (x2 > r->x2) r->x2 = x2;
    if (y2 > r->y2) r->y2 = y2;
}
static void rect_add_box(const xf_t *m, float cx, float cy, float rx, float ry, dotii_rect_t *r)
{
    for (int i = 0; i < 4; ++i) {
        const float ux = (i & 1) ? rx : -rx;
        const float uy = (i & 2) ? ry : -ry;
        float sx, sy;
        xf_apply(m, cx + ux, cy + uy, &sx, &sy);
        rect_add(r, sx, sy, 2.0f);
    }
}
static dotii_rect_t compute_content(const lay_t *L, const dotii_pose_t *p, const xf_t *xf_eye,
                                    const xf_t *xf_mouth, const xf_t *xf_brow)
{
    dotii_rect_t r;
    rect_reset(&r);
    for (int side = 0; side < 2; ++side) {
        const eye_geom_t g = eye_geom(p, side, L->eyeW);
        const float cx = (side == 0) ? (L->CX - L->eyeSep) : (L->CX + L->eyeSep);
        rect_add_box(xf_eye, cx, L->eyeY, g.RX + 4.0f, g.RY + 4.0f, &r);
        const float bw = L->eyeW * 0.66f * clampf(p->brow_w, 0.4f, 2.0f);
        const uint8_t kind = (side == 0) ? p->brow_l : p->brow_r;
        if (kind < DOTII_BROW_NONE) {
            const float ref = L->eyeY + g.ref_top;
            rect_add_box(xf_brow, cx, ref - L->R * 0.10f, bw * 0.62f, L->R * 0.10f, &r);
        }
        if (p->glasses) {
            const float gx = L->eyeW * 0.5f * 1.30f;
            rect_add_box(xf_eye, cx, L->eyeY, gx, gx, &r);
        }
    }
    {
        const float half = clampf(p->mouth_w, 0.30f, 1.9f) * L->R * 0.215f;
        const float cy = L->mouthY + p->mouth_y * L->R * 0.5f;
        rect_add_box(xf_mouth, L->CX, cy, half * 1.35f + 3.0f, L->R * 0.34f, &r);
        /* nose */
        rect_add_box(xf_mouth, L->CX, L->CY + L->R * 0.17f, L->R * 0.02f, L->R * 0.015f, &r);
    }
    if (p->blush > 0.002f) {
        const float dx = L->R * 0.55f, dy = L->R * 0.18f;
        rect_add(&r, L->CX - dx, L->CY + dy, 2.0f);
        rect_add(&r, L->CX + dx, L->CY + dy, 2.0f);
        rect_add(&r, L->CX - dx, L->CY - dy * 0.2f, 2.0f);
        rect_add(&r, L->CX + dx, L->CY - dy * 0.2f, 2.0f);
    }
    for (int i = 0; i < p->fx_count && i < 3; ++i) {
        const uint8_t fx = p->fx[i];
        if (fx >= DOTII_FX_COUNT) continue;
        const float bx1 = L->CX + k_fx_bbox[fx][0] * L->R;
        const float by1 = L->CY + k_fx_bbox[fx][1] * L->R;
        const float bx2 = L->CX + k_fx_bbox[fx][2] * L->R;
        const float by2 = L->CY + k_fx_bbox[fx][3] * L->R;
        rect_add_box(xf_brow /* effects ride the head transform */, (bx1 + bx2) * 0.5f,
                     (by1 + by2) * 0.5f, (bx2 - bx1) * 0.5f + 3.0f, (by2 - by1) * 0.5f + 3.0f, &r);
    }
    return r;
}

static void rect_clip_disc(dotii_rect_t *r, int32_t size, float R)
{
    const int32_t lo = (int32_t)floorf(R - R + 4.0f);      /* 4 */
    const int32_t hi = (int32_t)ceilf((float)size - 4.0f);
    if (r->x1 < lo) r->x1 = lo;
    if (r->y1 < lo) r->y1 = lo;
    if (r->x2 > hi) r->x2 = hi;
    if (r->y2 > hi) r->y2 = hi;
    if (r->x1 < 0) r->x1 = 0;
    if (r->y1 < 0) r->y1 = 0;
    if (r->x2 >= size) r->x2 = size - 1;
    if (r->y2 >= size) r->y2 = size - 1;
}

/* ================= main entry ================= */
void dotii_draw_render(uint16_t *fb, int32_t size, const dotii_expr_t *expr,
                       const dotii_pose_t *pose, uint32_t now_ms,
                       const dotii_rect_t *clip_in, dotii_rect_t *dirty_out,
                       dotii_rect_t *content_out)
{
    if (fb == NULL || size <= 0 || size > 1024) return;

    const lay_t L = layout_for(size);
    dotii_pose_t local;
    const dotii_pose_t *p = pose;
    if (p == NULL && expr != NULL) {
        memset(&local, 0, sizeof(local));
        local.eye_open = expr->base.value[DOTII_CH_EYE_OPEN];
        local.eye_open_l = expr->base.value[DOTII_CH_EYE_OPEN_L];
        local.eye_w = expr->base.value[DOTII_CH_EYE_W];
        local.eye_h = expr->base.value[DOTII_CH_EYE_H];
        local.lid_top = expr->base.value[DOTII_CH_LID_TOP];
        local.lid_bot = expr->base.value[DOTII_CH_LID_BOT];
        local.brow_y = expr->base.value[DOTII_CH_BROW_Y];
        local.brow_w = expr->base.value[DOTII_CH_BROW_W];
        local.mouth_open = expr->base.value[DOTII_CH_MOUTH_OPEN];
        local.mouth_w = expr->base.value[DOTII_CH_MOUTH_W];
        local.mouth_y = expr->base.value[DOTII_CH_MOUTH_Y];
        local.blush = expr->base.value[DOTII_CH_BLUSH];
        local.dim = expr->base.value[DOTII_CH_DIM];
        local.eye_arc = expr->base.eye_arc;
        local.brow_l = expr->base.brow_l == 0xFF ? expr->base.brow : expr->base.brow_l;
        local.brow_r = expr->base.brow_r == 0xFF ? expr->base.brow : expr->base.brow_r;
        local.mouth = expr->base.mouth;
        local.eye_extra_l = expr->base.eye_extra_l;
        local.eye_extra_r = expr->base.eye_extra_r;
        local.glasses = expr->base.glasses;
        local.fx_count = expr->base.fx_count;
        memcpy(local.fx, expr->base.fx, sizeof(local.fx));
        local.blink_k = 1.0f;
        local.ms = now_ms;
        local.clock = (float)now_ms / 1000.0f;
        local.sac_key = (uint16_t)((uint32_t)expr->id[0] * 131u + (uint32_t)expr->id[1] * 17u);
        p = &local;
    }
    if (p == NULL) return;

    const rgb_t accent = parse_hex_rgb(expr != NULL ? expr->accent : "#8fa6c4");
    const float dim = clampf(p->dim, 0.0f, 1.0f);
    const float alpha = 1.0f - dim * 0.55f;
    /* head transform */
    const float hx = p->head_x, hy = p->head_y;
    const float rot = p->head_rot;
    const float scl = clampf(p->head_scale, 0.6f, 1.6f);
    const float sq = clampf(-hy / (L.R * 0.045f), -1.0f, 1.0f);
    const float sx = scl * (1.0f - sq * 0.026f);
    const float sy = scl * (1.0f + sq * 0.032f);
    const xf_t head = xf_mul(xf_translate(L.CX + hx, L.CY + hy),
                             xf_mul(xf_rotate(rot),
                                    xf_mul(xf_scale(sx, sy), xf_translate(-L.CX, -L.CY))));
    const xf_t xf_eye = xf_mul(head, xf_translate(-hx * 0.08f, -hy * 0.06f));
    const xf_t xf_mouth = xf_mul(head, xf_translate(-hx * 0.02f, -hy * 0.01f));
    const xf_t xf_brow = xf_mul(head, xf_translate(-hx * 0.16f, -hy * 0.12f));

    dotii_rect_t content = compute_content(&L, p, &xf_eye, &xf_mouth, &xf_brow);
    rect_clip_disc(&content, size, L.R);
    if (content.x1 > content.x2 || content.y1 > content.y2) {
        content.x1 = 0; content.y1 = 0; content.x2 = size - 1; content.y2 = size - 1;
    }

    dotii_rect_t clip;
    if (clip_in == NULL) {
        clip.x1 = 0; clip.y1 = 0; clip.x2 = size - 1; clip.y2 = size - 1;
    } else {
        clip = content;
        if (clip_in->x1 < clip.x1) clip.x1 = clip_in->x1;
        if (clip_in->y1 < clip.y1) clip.y1 = clip_in->y1;
        if (clip_in->x2 > clip.x2) clip.x2 = clip_in->x2;
        if (clip_in->y2 > clip.y2) clip.y2 = clip_in->y2;
        clip.x1 -= 2; clip.y1 -= 2; clip.x2 += 2; clip.y2 += 2;
        rect_clip_disc(&clip, size, L.R);
    }

    /* Reset the dirty rectangle before re-drawing the features.
       The cached background is uniformly black now, so this is a memset of the
       destination instead of a memcpy from the PSRAM cache: no source read at
       all, and the row loop is replaced by one call. On this board the copy used
       to cost several milliseconds per frame for the large dirty rectangles the
       head/eye motion produces. */
    const size_t row_bytes = (size_t)(clip.x2 - clip.x1 + 1) * sizeof(uint16_t);
    for (int32_t y = clip.y1; y <= clip.y2; ++y) {
        memset(&fb[(size_t)y * (size_t)size + (size_t)clip.x1], 0, row_bytes);
    }

    canvas_t cv;
    canvas_init(&cv, fb, size, alpha, clip);


    /* 1. blush */
    draw_blush(&cv, &L, p);

    /* 2. eyes + glasses */
    draw_eye(&cv, &xf_eye, L.CX - L.eyeSep, L.eyeY, p, 0, L.eyeW);
    draw_eye(&cv, &xf_eye, L.CX + L.eyeSep, L.eyeY, p, 1, L.eyeW);
    if (p->glasses) draw_glasses(&cv, &xf_eye, &L, p);

    /* 3. nose + mouth */
    {
        float nx, ny;
        xf_apply(&xf_mouth, L.CX, L.CY + L.R * 0.170f, &nx, &ny);
        const float sc = xf_scale_avg(&xf_mouth);
        fill_ellipse(&cv, nx, ny, L.R * 0.014f * sc, L.R * 0.009f * sc, C_WHITE2, 0.22f);
        draw_mouth(&cv, &xf_mouth, &L, p, L.CX, L.mouthY + p->mouth_y * L.R * 0.5f);
    }

    /* 4. brows (last feature layer -> parallax) */
    {
        const float bw = L.eyeW * 0.66f * clampf(p->brow_w, 0.4f, 2.0f);
        const eye_geom_t gl = eye_geom(p, 0, L.eyeW);
        const eye_geom_t gr = eye_geom(p, 1, L.eyeW);
        if (p->brow_l < DOTII_BROW_NONE)
            draw_brow(&cv, &xf_brow, &L, L.CX - L.eyeSep, L.eyeY + gl.ref_top, bw, p->brow_l, p, 0);
        if (p->brow_r < DOTII_BROW_NONE)
            draw_brow(&cv, &xf_brow, &L, L.CX + L.eyeSep, L.eyeY + gr.ref_top, bw, p->brow_r, p, 1);
    }

    /* 5. effects */
    draw_effects(&cv, &xf_brow, &L, p, accent);

    if (dirty_out) *dirty_out = clip;
    if (content_out) *content_out = content;
}




































