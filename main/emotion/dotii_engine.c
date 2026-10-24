#include "dotii_engine.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DOTII_TAU 6.283185307179586f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* softSat(): tanh saturation. A hard clamp creates a plateau that reads as a
   mechanical, stuttering stop. */
static float soft_sat(float v, float lim)
{
    if (lim <= 0.0f) return 0.0f;
    return lim * tanhf(v / lim);
}

/* Pointer into the pose for one animated channel. */
static float *ch_ptr(dotii_pose_t *p, uint8_t ch)
{
    switch (ch) {
    case DOTII_CH_PUPIL_X: return &p->pupil_x;
    case DOTII_CH_PUPIL_Y: return &p->pupil_y;
    case DOTII_CH_EYE_OPEN: return &p->eye_open;
    case DOTII_CH_EYE_OPEN_L: return &p->eye_open_l;
    case DOTII_CH_EYE_W: return &p->eye_w;
    case DOTII_CH_EYE_H: return &p->eye_h;
    case DOTII_CH_LID_TOP: return &p->lid_top;
    case DOTII_CH_LID_BOT: return &p->lid_bot;
    case DOTII_CH_BROW_Y: return &p->brow_y;
    case DOTII_CH_BROW_W: return &p->brow_w;
    case DOTII_CH_MOUTH_OPEN: return &p->mouth_open;
    case DOTII_CH_MOUTH_W: return &p->mouth_w;
    case DOTII_CH_MOUTH_Y: return &p->mouth_y;
    case DOTII_CH_BLUSH: return &p->blush;
    case DOTII_CH_DIM: return &p->dim;
    case DOTII_CH_HEAD_X: return &p->head_x;
    case DOTII_CH_HEAD_Y: return &p->head_y;
    case DOTII_CH_HEAD_ROT: return &p->head_rot;
    case DOTII_CH_HEAD_SCALE: return &p->head_scale;
    default: return NULL;
    }
}

static const dotii_expr_t *resolve(const char *id_or_name)
{
    if (id_or_name == NULL) return NULL;
    const dotii_expr_t *by_id = dotii_catalog_by_id(id_or_name);
    if (by_id != NULL) return by_id;
    const dotii_expr_t *hit = dotii_catalog_by_name(id_or_name);
    if (hit != NULL) return hit;
    const char *aliased = dotii_alias_resolve(id_or_name);
    if (aliased != NULL) return dotii_catalog_by_name(aliased);
    return NULL;
}

/* Deterministic hash used by gaze/head/blink/saccade. Mirrors the JS
   (n * 2654435761) >>> 0 pipeline with explicit uint32 arithmetic. */
static float hash01(uint32_t n)
{
    uint32_t x = n * 2654435761u;
    x = (x ^ (x >> 15)) * 2246822519u;
    x = x ^ (x >> 13);
    return (float)x / 4294967296.0f;
}

/* Analytic spring step response. */
static float spring_at(float elapsed_s, float response, float damping)
{
    if (elapsed_s <= 0.0f) return 0.0f;
    const float w0 = DOTII_TAU / fmaxf(0.05f, response);
    const float z = damping;
    const float t = elapsed_s;
    if (z < 0.999f) {
        const float wd = w0 * sqrtf(1.0f - z * z);
        const float A = expf(-z * w0 * t);
        return 1.0f - A * (cosf(wd * t) + (z * w0 / wd) * sinf(wd * t));
    }
    return 1.0f - expf(-w0 * t) * (1.0f + w0 * t);
}

/* Two-frequency breathing. Very slow, far below the 0.2 Hz band the Apple
   HIG warns about. */
static float breath_at(uint32_t ms)
{
    const float t = (float)ms;
    return 1.0f + 0.0045f * sinf(t / 11000.0f * DOTII_TAU)
                + 0.0025f * sinf(t / 17000.0f * DOTII_TAU + 1.3f);
}

/* Saccade-driven gaze: a quick spring to a new fixation every 1.9 s. */
static void gaze_at(uint32_t ms, float *ox, float *oy)
{
    const float T = 1900.0f;
    const uint32_t c = (uint32_t)((float)ms / T);
    const float tt = (float)ms - (float)c * T;
    const float tx = (hash01(c + 1u) - 0.5f) * 2.0f;
    const float ty = (hash01(c + 5u) - 0.5f) * 1.4f;
    const float px = (hash01(c) - 0.5f) * 2.0f;
    const float py = (hash01(c + 4u) - 0.5f) * 1.4f;
    const float k = spring_at(tt / 1000.0f, 0.42f, 0.96f);
    *ox = lerpf(px, tx, k) * 0.62f;
    *oy = lerpf(py, ty, k) * 0.42f;
}

/* Slow chain-lagged head drift, no wrap jumps. */
static void head_at(uint32_t ms, float lag_ms, float *ox, float *oy, float *orot)
{
    const float T = 6000.0f;
    const float t = (float)ms - lag_ms;
    const float tc = t >= 0.0f ? t : 0.0f;
    const uint32_t c = (uint32_t)(tc / T);
    const float tt = tc - (float)c * T;
    const float k = spring_at(tt / 1000.0f, 1.1f, 1.0f);
    const float t3x = hash01(c + 11u) - 0.5f;
    const float t3y = hash01(c + 23u) - 0.5f;
    const float t3r = hash01(c + 37u) - 0.5f;
    const float p3x = hash01(c + 10u) - 0.5f;
    const float p3y = hash01(c + 22u) - 0.5f;
    const float p3r = hash01(c + 36u) - 0.5f;
    *ox = lerpf(p3x, t3x, k);
    *oy = lerpf(p3y, t3y, k);
    *orot = lerpf(p3r, t3r, k);
}

/* Blink envelope: fast close, slow open, jittered start. 1.0 = open. */
static float blink_at(uint32_t ms, const char *id)
{
    const float P = 3400.0f;
    const uint32_t c = (uint32_t)((float)ms / P);
    const float tt = (float)ms - (float)c * P;
    const uint32_t id0 = (uint32_t)(uint8_t)id[0];
    const uint32_t id1 = (uint32_t)(uint8_t)id[1];
    uint32_t h = c * 2654435761u + id0 * 7919u + id1 * 104729u;
    h = (h ^ (h >> 15)) * 2246822519u;
    h = h ^ (h >> 13);
    const float j1 = (float)h / 4294967296.0f;
    if (j1 > 0.86f) return 1.0f;
    const uint32_t h2 = h * 1103515245u + 12345u;
    const float j2 = (float)h2 / 4294967296.0f;
    const float start = 300.0f + j2 * 1500.0f;
    const float span = 330.0f;
    if (tt < start || tt > start + span) return 1.0f;
    const float k = (tt - start) / span;
    if (k < 0.34f)
        return clampf(1.0f - spring_at(k / 0.34f * 0.15f, 0.15f, 0.97f), 0.0f, 1.0f);
    return clampf(spring_at((k - 0.34f) / 0.66f * 0.22f, 0.22f, 0.85f), 0.0f, 1.0f);
}

/* Periodic waveforms. All are C1-continuous across the period boundary. */
static float wave_value(uint8_t kind, float t, float phase)
{
    float p = t + phase;
    p -= floorf(p);
    const float a = p * DOTII_TAU;
    switch (kind) {
    case DOTII_WAVE_SWAY:
        return sinf(a) * 0.86f + sinf(a * 2.0f + 0.6f) * 0.14f;
    case DOTII_WAVE_BOUNCE: {
        const float u = 0.5f - 0.5f * cosf(a);
        return u * u * (3.0f - 2.0f * u) * 0.85f + 0.15f * sinf(a * 2.0f);
    }
    case DOTII_WAVE_PULSE: {
        const float u = 0.5f - 0.5f * cosf(a);
        return u * u;
    }
    case DOTII_WAVE_DRIFT:
        return 0.5f - 0.5f * cosf(a);
    case DOTII_WAVE_SHAKE:
        return sinf(a * 3.0f) * (0.55f + 0.45f * cosf(a));
    case DOTII_WAVE_NOD: {
        const float u = 0.5f - 0.5f * cosf(a);
        return u * u * u;
    }
    case DOTII_WAVE_BLINK: {
        const float u = p / 0.13f;
        return u < 1.0f ? sinf(u * (float)M_PI) : 0.0f;
    }
    case DOTII_WAVE_POP: {
        const float u = 0.5f - 0.5f * cosf(a);
        return u * u * (3.0f - 2.0f * u);
    }
    case DOTII_WAVE_TWITCH: {
        const float s = sinf(a);
        float v = s * s; v *= v; v *= v; v *= v;   /* s^8 */
        return v;
    }
    case DOTII_WAVE_SWING:
        return sinf(a * 2.0f);
    case DOTII_WAVE_RISE:
        return 0.5f - 0.5f * cosf(a);
    case DOTII_WAVE_FLOAT:
        return sinf(a);
    case DOTII_WAVE_BREATHE:
        return sinf(a);
    case DOTII_WAVE_SWELL: {
        const float u = 0.5f - 0.5f * cosf(a);
        return 0.5f + 0.5f * sinf(u * (float)M_PI);
    }
    default:
        return sinf(a);
    }
}

/* ---------- idle life: six coprime slow sines ---------- */
typedef struct {
    float hx, hy, rot, brow, mw, eye_x, eye_y, lid, scl;
} idle_t;

static float mood_energy(const dotii_pose_t *p)
{
    float e = 1.0f;
    switch (p->mouth) {
    case DOTII_MOUTH_TALK:
    case DOTII_MOUTH_SING:
    case DOTII_MOUTH_O:
    case DOTII_MOUTH_BIG_SMILE:
    case DOTII_MOUTH_OPEN_SMILE: e = 1.30f; break;
    case DOTII_MOUTH_SMILE:
    case DOTII_MOUTH_SMALL_SMILE: e = 1.12f; break;
    case DOTII_MOUTH_FROWN:
    case DOTII_MOUTH_GRIT:
    case DOTII_MOUTH_TIGHT:
    case DOTII_MOUTH_WAVY: e = 0.72f; break;
    default: break;
    }
    const float eye = clampf(0.55f + p->eye_open * 0.45f, 0.55f, 1.25f);
    const float bl = 0.78f + 0.22f * clampf(p->blink_k, 0.0f, 1.0f);
    return clampf(e * eye * bl, 0.45f, 1.45f);
}

static idle_t idle_life(const dotii_pose_t *p, float en)
{
    const float ck = p->clock;
    const float a1 = sinf(ck * 0.23f * DOTII_TAU + 0.4f);
    const float a2 = sinf(ck * 0.37f * DOTII_TAU + 1.9f);
    const float a3 = sinf(ck * 0.53f * DOTII_TAU + 3.1f);
    const float a4 = sinf(ck * 0.71f * DOTII_TAU + 5.2f);
    const float a5 = sinf(ck * 0.97f * DOTII_TAU + 2.4f);
    const float a6 = sinf(ck * 1.31f * DOTII_TAU + 0.9f);
    idle_t o;
    o.hx = (a1 * 2.60f + a2 * 1.50f + a5 * 0.70f) * en;
    o.hy = (a2 * 1.60f + a3 * 0.90f + a6 * 0.50f) * en;
    o.rot = (a1 * 0.42f + a4 * 0.24f + a5 * 0.15f) * en;
    o.brow = (a3 * 1.10f + a2 * 0.60f) * en;
    o.mw = (a4 * 0.70f + a1 * 0.45f + a6 * 0.20f) * en;
    o.eye_x = (a3 * 0.85f + a5 * 0.32f) * en;
    o.eye_y = (a2 * 0.70f + a6 * 0.26f) * en;
    o.lid = (a4 * 0.60f + a2 * 0.30f) * en;
    o.scl = (a5 * 0.60f + a2 * 0.35f) * 0.010f * en;
    return o;
}

typedef struct {
    float brow, hx, hy, lid, mw, px, py, rot, scl;
} sig_t;

static sig_t signature(const dotii_expr_t *e, float ck)
{
    sig_t o;
    memset(&o, 0, sizeof(o));
    if (e->sig == NULL) return o;
    const float ph0 = hash01((uint32_t)e->id[0] * 131u + (uint32_t)e->id[1] * 17u) * DOTII_TAU;
    for (int i = 0; i < e->sig_count; ++i) {
        const dotii_sig_layer_t *L = &e->sig[i];
        const float freq = L->f != 0.0f ? L->f : 0.3f;
        const float ph = ck * freq * DOTII_TAU + L->phase +
                         (L->wave == DOTII_SIG_WAVE_SINE ? ph0 : 0.0f);
        float v;
        if (L->wave == DOTII_SIG_WAVE_BOUNCE) {
            v = 0.5f - 0.5f * cosf(ph);
        } else if (L->wave == DOTII_SIG_WAVE_PULSE) {
            const float b = 0.5f - 0.5f * cosf(ph);
            v = b * b;
        } else {
            v = sinf(ph);
        }
        o.brow += L->a[DOTII_SIG_BROW] * v;
        o.hx   += L->a[DOTII_SIG_HX] * v;
        o.hy   += L->a[DOTII_SIG_HY] * v;
        o.lid  += L->a[DOTII_SIG_LID] * v;
        o.mw   += L->a[DOTII_SIG_MW] * v;
        o.px   += L->a[DOTII_SIG_PX] * v;
        o.py   += L->a[DOTII_SIG_PY] * v;
        o.rot  += L->a[DOTII_SIG_ROT] * v;
        o.scl  += L->a[DOTII_SIG_SCL] * v;
    }
    return o;
}

static void apply_track(dotii_pose_t *p, const dotii_track_t *tr, uint32_t ms)
{
    float *slot = ch_ptr(p, tr->channel);
    if (slot == NULL) return;
    const float period = tr->period_ms > 1.0f ? tr->period_ms : 3000.0f;
    const float ph = fmodf((float)ms, period) / period;
    *slot += wave_value(tr->wave, ph, tr->phase) * tr->amp;
}

static void load_base(const dotii_expr_t *e, dotii_pose_t *p)
{
    memset(p, 0, sizeof(*p));
    p->pupil_x = e->base.value[DOTII_CH_PUPIL_X];
    p->pupil_y = e->base.value[DOTII_CH_PUPIL_Y];
    p->eye_open = e->base.value[DOTII_CH_EYE_OPEN];
    p->eye_open_l = e->base.value[DOTII_CH_EYE_OPEN_L];
    p->eye_w = e->base.value[DOTII_CH_EYE_W];
    p->eye_h = e->base.value[DOTII_CH_EYE_H];
    p->lid_top = e->base.value[DOTII_CH_LID_TOP];
    p->lid_bot = e->base.value[DOTII_CH_LID_BOT];
    p->brow_y = e->base.value[DOTII_CH_BROW_Y];
    p->brow_w = e->base.value[DOTII_CH_BROW_W];
    p->mouth_open = e->base.value[DOTII_CH_MOUTH_OPEN];
    p->mouth_w = e->base.value[DOTII_CH_MOUTH_W];
    p->mouth_y = e->base.value[DOTII_CH_MOUTH_Y];
    p->blush = e->base.value[DOTII_CH_BLUSH];
    p->dim = e->base.value[DOTII_CH_DIM];
    p->head_x = e->base.value[DOTII_CH_HEAD_X];
    p->head_y = e->base.value[DOTII_CH_HEAD_Y];
    p->head_rot = e->base.value[DOTII_CH_HEAD_ROT];
    p->head_scale = e->base.value[DOTII_CH_HEAD_SCALE];
    p->eye_arc = e->base.eye_arc;
    p->brow_l = e->base.brow_l;
    p->brow_r = e->base.brow_r;
    if (p->brow_l == 0xFF) p->brow_l = e->base.brow;
    if (p->brow_r == 0xFF) p->brow_r = e->base.brow;
    p->mouth = e->base.mouth;
    p->eye_extra_l = e->base.eye_extra_l;
    p->eye_extra_r = e->base.eye_extra_r;
    p->glasses = e->base.glasses;
    p->fx_count = e->base.fx_count;
    memcpy(p->fx, e->base.fx, sizeof(p->fx));
}

void dotii_engine_init(dotii_engine_t *engine, uint32_t now_ms)
{
    memset(engine, 0, sizeof(*engine));
    engine->state.current = dotii_catalog_idle();
    engine->state.base = engine->state.current;
    engine->state.entered_at_ms = now_ms;
    engine->state.id[0] = engine->state.current->id[0];
    engine->state.id[1] = engine->state.current->id[1];
    engine->state.id[2] = '\0';
    engine->touch_until_ms = 0;
    engine->last_now_ms = now_ms;
    dotii_engine_tick(engine, now_ms);
}

bool dotii_engine_set(dotii_engine_t *engine, const char *id_or_name, uint32_t now_ms)
{
    const dotii_expr_t *expr = resolve(id_or_name);
    if (expr == NULL) return false;
    if (expr != engine->state.current) {
        engine->state.current = expr;
        engine->state.entered_at_ms = now_ms;
        engine->state.id[0] = expr->id[0];
        engine->state.id[1] = expr->id[1];
        engine->state.id[2] = '\0';
    }
    engine->state.temporary = false;
    engine->state.hold_ms = 0;
    engine->state.base = expr;
    return true;
}

bool dotii_engine_set_hold(dotii_engine_t *engine, const char *id_or_name,
                           uint32_t hold_ms, uint32_t now_ms)
{
    const dotii_expr_t *prev = engine->state.current;
    if (!dotii_engine_set(engine, id_or_name, now_ms)) return false;
    engine->state.base = prev;
    engine->state.temporary = hold_ms > 0;
    engine->state.hold_ms = hold_ms;
    return true;
}

void dotii_engine_set_gaze(dotii_engine_t *engine, float nx, float ny, uint32_t now_ms)
{
    engine->touch_nx = clampf(nx, -1.0f, 1.0f);
    engine->touch_ny = clampf(ny, -1.0f, 1.0f);
    engine->touch_until_ms = now_ms + 3000u;
}

bool dotii_engine_tick(dotii_engine_t *engine, uint32_t now_ms)
{
    bool changed = false;

    /* hold expiry reverts to the base expression */
    if (engine->state.temporary && engine->state.hold_ms > 0 &&
        (uint32_t)(now_ms - engine->state.entered_at_ms) >= engine->state.hold_ms) {
        const dotii_expr_t *base = engine->state.base;
        if (base != NULL) {
            engine->state.current = base;
            engine->state.entered_at_ms = now_ms;
            engine->state.temporary = false;
            engine->state.hold_ms = 0;
            engine->state.id[0] = base->id[0];
            engine->state.id[1] = base->id[1];
            engine->state.id[2] = '\0';
            changed = true;
        }
    }

    const dotii_expr_t *expr = engine->state.current;
    dotii_pose_t *p = &engine->state.pose;
    load_base(expr, p);

    /* global gaze + head drift */
    float gx = 0.0f, gy = 0.0f;
    if ((int32_t)(engine->touch_until_ms - now_ms) > 0) {
        gx = engine->touch_nx;
        gy = engine->touch_ny;
    } else {
        gaze_at(now_ms, &gx, &gy);
    }
    p->pupil_x += gx * 0.55f;
    p->pupil_y += gy * 0.55f;

    /* motion tracks */
    for (int i = 0; i < expr->track_count; ++i)
        apply_track(p, &expr->tracks[i], now_ms);

    float hx, hy, hr;
    head_at(now_ms, 70.0f, &hx, &hy, &hr);
    const float br = breath_at(now_ms);
    p->head_x += hx * 1.5f + (br - 1.0f) * 90.0f;
    p->head_y += hy * 1.4f + (br - 1.0f) * 260.0f;
    p->head_rot += hr * 1.6f;
    p->eye_h *= (1.0f + (br - 1.0f) * 2.2f);

    /* mouth envelope */
    const float base_mo = expr->base.value[DOTII_CH_MOUTH_OPEN];
    const float delta = p->mouth_open - base_mo;
    float rate = 0.72f, depth = 0.32f, period = 190.0f;
    switch (p->mouth) {
    case DOTII_MOUTH_TALK:       rate = 0.18f; depth = 1.05f; period = 85.0f;  break;
    case DOTII_MOUTH_OPEN_SMILE: rate = 0.46f; depth = 0.56f; period = 95.0f;  break;
    case DOTII_MOUTH_O:          rate = 0.62f; depth = 0.30f; period = 170.0f; break;
    case DOTII_MOUTH_SING:       rate = 0.42f; depth = 0.78f; period = 120.0f; break;
    default: break;
    }
    if (base_mo > 0.001f || fabsf(delta) > 0.001f) {
        const float u1 = fmodf((float)now_ms, period) / period;
        const float atk = 0.5f - 0.5f * cosf((float)M_PI * fminf(1.0f, u1 / 0.38f));
        const float rel = 0.5f + 0.5f * cosf((float)M_PI * fminf(1.0f, fmaxf(0.0f, (u1 - 0.38f) / 0.62f)));
        const float env1 = u1 < 0.38f ? atk : rel;
        const float env2 = 0.5f + 0.5f * sinf((float)now_ms / (period * 0.37f) + 1.1f);
        const float talk = clampf(env1 * 0.70f + env2 * 0.30f, 0.0f, 1.0f);
        p->mouth_open = clampf(base_mo * (rate + depth * talk) + delta, 0.0f, 1.0f);
    }

    /* blink rules: these expressions keep their eyes closed / never blink */
    const char *id = expr->id;
    const bool no_blink =
        (id[0] == '0' && (id[1] == '2' || id[1] == '6')) ||
        (id[0] == '1' && (id[1] == '0' || id[1] == '1' || id[1] == '6')) ||
        (id[0] == '2' && id[1] == '6') ||
        (id[0] == '3' && id[1] == '3') ||
        (id[0] == '4' && (id[1] == '1' || id[1] == '4' || id[1] == '6'));
    p->blink_k = no_blink ? 1.0f : blink_at(now_ms, id);
    p->clock = (float)now_ms / 1000.0f;
    p->ms = now_ms;
    p->sac_key = (uint16_t)((uint32_t)id[0] * 131u + (uint32_t)id[1] * 17u);

    /* ---- idle life + signature: fold into the pose so the renderer stays
       time-free apart from effect phases ---- */
    const float en = mood_energy(p);
    const idle_t il = idle_life(p, en);
    const sig_t sg = signature(expr, p->clock);

    const float talk = clampf(p->mouth_open, 0.0f, 1.0f);
    const float nod = talk * sinf(p->clock * 1.9f * DOTII_TAU + 0.7f);

    p->head_x = soft_sat(p->head_x, 2.6f) * 6.0f + il.hx + sg.hx;
    p->head_y = soft_sat(p->head_y, 3.2f) * 4.0f + il.hy + sg.hy
              + (br - 1.0f) * 120.0f + nod * 2.0f;
    p->head_rot = (soft_sat(p->head_rot, 3.0f) * 1.6f + il.rot + sg.rot) * (float)M_PI / 180.0f;
    p->head_scale = 1.0f + sg.scl + il.scl
                  + soft_sat(p->head_scale, 2.3f) * 0.016f + (br - 1.0f) * 1.6f;
    p->nod = nod;

    float smile_k = 0.0f;
    switch (p->mouth) {
    case DOTII_MOUTH_SMILE:
    case DOTII_MOUTH_SMALL_SMILE: smile_k = 0.85f; break;
    case DOTII_MOUTH_BIG_SMILE:   smile_k = 0.55f + 0.45f * talk; break;
    case DOTII_MOUTH_OPEN_SMILE:  smile_k = 0.50f + 0.45f * talk; break;
    default: break;
    }

    p->pupil_x = clampf(p->pupil_x + il.eye_x * 0.085f + sg.px * 0.90f, -1.0f, 1.0f);
    p->pupil_y = clampf(p->pupil_y + il.eye_y * 0.085f + sg.py * 0.90f, -1.0f, 1.0f);
    p->lid_top = clampf(p->lid_top + il.lid * 0.05f + sg.lid * 0.55f, 0.0f, 0.9f);
    p->lid_bot = clampf(p->lid_bot + il.lid * 0.05f + smile_k * 0.16f, 0.0f, 0.9f);
    p->eye_h *= (1.0f + il.lid * 0.022f);
    p->mouth_w *= (1.0f + il.mw * 0.012f) * (1.0f + sg.mw * 2.0f);
    p->brow_y += il.brow * 0.010f;
    p->brow_px = -(sg.brow + il.brow * 0.42f);
    p->dim = clampf(p->dim, 0.0f, 1.0f);

    engine->last_now_ms = now_ms;
    return changed;
}

const dotii_engine_state_t *dotii_engine_state(const dotii_engine_t *engine)
{
    return &engine->state;
}

const char *dotii_engine_current_id(const dotii_engine_t *engine)
{
    return engine->state.id;
}
