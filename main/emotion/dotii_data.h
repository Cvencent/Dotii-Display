#pragma once

/* Dotii v8 cartoon face: shared data contract.
   Generated data lives in dotii_data.c (tools/gen_dotii_catalog.mjs).
   The numbers mirror output/emotion-preview/emotion-v2-data.json exactly. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Animated scalar channels. Index order is part of the data contract. */
typedef enum {
    DOTII_CH_PUPIL_X = 0,
    DOTII_CH_PUPIL_Y,
    DOTII_CH_EYE_OPEN,
    DOTII_CH_EYE_OPEN_L,   /* left-eye override (blink expression) */
    DOTII_CH_EYE_W,
    DOTII_CH_EYE_H,
    DOTII_CH_LID_TOP,
    DOTII_CH_LID_BOT,
    DOTII_CH_BROW_Y,
    DOTII_CH_BROW_W,
    DOTII_CH_MOUTH_OPEN,
    DOTII_CH_MOUTH_W,
    DOTII_CH_MOUTH_Y,
    DOTII_CH_BLUSH,
    DOTII_CH_DIM,          /* 0 = fully visible, 1 = fully dimmed */
    DOTII_CH_HEAD_X,
    DOTII_CH_HEAD_Y,
    DOTII_CH_HEAD_ROT,     /* degrees */
    DOTII_CH_HEAD_SCALE,   /* additive sway amount, not a multiplier */
    DOTII_CH_COUNT,
} dotii_channel_t;

/* Periodic waveform families used by the multi-track motion system. */
typedef enum {
    DOTII_WAVE_SWAY = 0,
    DOTII_WAVE_BOUNCE,
    DOTII_WAVE_PULSE,
    DOTII_WAVE_DRIFT,
    DOTII_WAVE_SHAKE,
    DOTII_WAVE_NOD,
    DOTII_WAVE_BLINK,
    DOTII_WAVE_POP,
    DOTII_WAVE_TWITCH,
    DOTII_WAVE_SWING,
    DOTII_WAVE_RISE,
    DOTII_WAVE_FLOAT,
    DOTII_WAVE_BREATHE,
    DOTII_WAVE_SWELL,
    DOTII_WAVE_COUNT,
} dotii_wave_t;

/* Forced closed-eye arc drawn instead of the normal eye body. */
typedef enum {
    DOTII_EYE_ARC_NONE = 0,
    DOTII_EYE_ARC_UP,     /* ∩ happy crescent */
    DOTII_EYE_ARC_DOWN,   /* ∪ sleepy */
    DOTII_EYE_ARC_LINE,   /* — */
    DOTII_EYE_ARC_COUNT,
} dotii_eye_arc_t;

/* Eyebrow leaf shapes. DOTII_BROW_NONE hides that side. */
typedef enum {
    DOTII_BROW_NEUTRAL = 0,
    DOTII_BROW_RAISED,
    DOTII_BROW_FOCUSED,
    DOTII_BROW_ANGRY,
    DOTII_BROW_SAD,
    DOTII_BROW_WORRIED,
    DOTII_BROW_DROOP,
    DOTII_BROW_FLAT,
    DOTII_BROW_SOFT,
    DOTII_BROW_FURROW,
    DOTII_BROW_NONE,
    DOTII_BROW_COUNT,
} dotii_brow_t;

/* Mouth shapes: line strokes plus cavity styles. No tongue, no teeth. */
typedef enum {
    DOTII_MOUTH_SMALL_SMILE = 0,
    DOTII_MOUTH_SMILE,
    DOTII_MOUTH_SMALL,
    DOTII_MOUTH_FLAT,
    DOTII_MOUTH_TIGHT,
    DOTII_MOUTH_FROWN,
    DOTII_MOUTH_SMIRK,
    DOTII_MOUTH_WAVY,
    DOTII_MOUTH_GRIT,
    DOTII_MOUTH_O,
    DOTII_MOUTH_TALK,
    DOTII_MOUTH_SING,
    DOTII_MOUTH_BIG_SMILE,
    DOTII_MOUTH_OPEN_SMILE,
    DOTII_MOUTH_COUNT,
} dotii_mouth_t;

/* Optional eye replacements. */
typedef enum {
    DOTII_EYE_EXTRA_NONE = 0,
    DOTII_EYE_EXTRA_HEART,
    DOTII_EYE_EXTRA_COUNT,
} dotii_eye_extra_t;

/* Floating effects: organic symbols and system badges. */
typedef enum {
    DOTII_FX_SPARKLE = 0,
    DOTII_FX_ZZZ,
    DOTII_FX_WAVE,
    DOTII_FX_THINK,
    DOTII_FX_HEARTS,
    DOTII_FX_QUESTION,
    DOTII_FX_TEAR,
    DOTII_FX_ANGER,
    DOTII_FX_HAND,
    DOTII_FX_SCAN,
    DOTII_FX_SEARCH,
    DOTII_FX_BOOK,
    DOTII_FX_CODE,
    DOTII_FX_GEAR,
    DOTII_FX_DOTS,
    DOTII_FX_CHECK,
    DOTII_FX_CROSS,
    DOTII_FX_EXCLAIM,
    DOTII_FX_NOTE,
    DOTII_FX_BELL,
    DOTII_FX_CLOUD_SLASH,
    DOTII_FX_REFRESH,
    DOTII_FX_BATTERY_LOW,
    DOTII_FX_BOLT,
    DOTII_FX_MUTE,
    DOTII_FX_VOL,
    DOTII_FX_MOON,
    DOTII_FX_WARN,
    DOTII_FX_LOCK,
    DOTII_FX_COUNT,
} dotii_effect_t;

/* One periodic motion layer: value = base + wave(phase) * amp. */
typedef struct {
    uint8_t channel;  /* dotii_channel_t */
    uint8_t wave;     /* dotii_wave_t */
    float amp;
    float period_ms;
    float phase;      /* 0..1 */
} dotii_track_t;

/* Per-expression signature layer (hits the same channels as JS `signature()`). */
typedef enum {
    DOTII_SIG_WAVE_SINE = 0,
    DOTII_SIG_WAVE_BOUNCE,
    DOTII_SIG_WAVE_PULSE,
} dotii_sig_wave_t;

typedef enum {
    DOTII_SIG_BROW = 0,
    DOTII_SIG_HX,
    DOTII_SIG_HY,
    DOTII_SIG_LID,
    DOTII_SIG_MW,
    DOTII_SIG_PX,
    DOTII_SIG_PY,
    DOTII_SIG_ROT,
    DOTII_SIG_SCL,
    DOTII_SIG_COUNT,
} dotii_sig_channel_t;

typedef struct {
    float f;              /* frequency in Hz */
    float phase;          /* radians or 0..1 depending on wave */
    uint8_t wave;         /* dotii_sig_wave_t */
    float a[DOTII_SIG_COUNT];
} dotii_sig_layer_t;

typedef struct {
    uint32_t mask;              /* bit per dotii_channel_t */
    float value[DOTII_CH_COUNT];
    uint8_t eye_arc;            /* dotii_eye_arc_t */
    uint8_t brow;               /* dotii_brow_t (both sides) */
    uint8_t brow_l;             /* 0xFF = inherit brow */
    uint8_t brow_r;             /* 0xFF = inherit brow */
    uint8_t mouth;              /* dotii_mouth_t */
    uint8_t eye_extra_l;        /* dotii_eye_extra_t */
    uint8_t eye_extra_r;        /* dotii_eye_extra_t */
    bool glasses;
    uint8_t fx_count;
    uint8_t fx[3];              /* dotii_effect_t */
} dotii_base_t;

typedef struct {
    const char *id;    /* "00".."47", stable forever */
    const char *name;  /* e.g. "IDLE"; lowercase form drives behavior layer */
    const char *label; /* e.g. "待机" */
    const char *accent;/* "#rrggbb" */
    dotii_base_t base;
    const dotii_track_t *tracks;
    uint16_t track_count;
    const dotii_sig_layer_t *sig;
    uint8_t sig_count;
} dotii_expr_t;

/* Dirty rectangle passed between the renderer and LVGL (inclusive pixels). */
typedef struct {
    int32_t x1, y1, x2, y2;
} dotii_rect_t;

/* Fully evaluated face state handed to the renderer. All values are already
   composed (tracks + breath + blink + idle life + signature), so draw code
   never needs to look at time-derived curves again. */
typedef struct {
    float pupil_x, pupil_y;
    float eye_open, eye_open_l;
    float eye_w, eye_h;
    float lid_top, lid_bot;
    float brow_y, brow_w, brow_px;
    float mouth_open, mouth_w, mouth_y;
    float blush, dim;
    float head_x, head_y, head_rot, head_scale;

    uint8_t eye_arc;
    uint8_t brow_l, brow_r;
    uint8_t mouth;
    uint8_t eye_extra_l, eye_extra_r;
    bool glasses;
    uint8_t fx_count;
    uint8_t fx[3];

    float blink_k;
    float clock;      /* seconds since expression start (drives wobbles) */
    float nod;        /* precomputed talk nod offset */
    uint32_t ms;
    uint16_t sac_key; /* deterministic saccade key for this expression */
} dotii_pose_t;

/* Layout ratios relative to the round-screen radius R. These are part of the
   visual contract and are asserted by the host tests.

   These were widened on user feedback ("too compact, the face looks small on a
   large screen"). The original v8 numbers (eyeSep 0.375R, eyeW 0.470R,
   mouthY 0.290R) left the whole expression clustered in the middle of the disc
   with only ~13 px between the eye bottoms and the mouth, and ~87 px of unused
   black at the sides. The set below spreads the features outwards and slightly
   enlarges the eyes so the character fills the round screen:
     - the eyes move up and noticeably further apart (0.375R -> 0.450R),
     - the mouth moves down, roughly tripling the eye-to-mouth gap,
     - the eye SIZE is deliberately left at the original 0.470R: the request was
       for more breathing room between the features, not bigger eyes, and
       enlarging them cost ~13% more raster time and pushed frames past the
       33 ms budget.
   Keep the "too close = fierce, too far = blank" balance in mind: these are
   deliberately still below the point where the gaze reads as vacant. */
#define DOTII_LAYOUT_EYE_Y    (-0.135f)
#define DOTII_LAYOUT_EYE_SEP  ( 0.450f)
#define DOTII_LAYOUT_EYE_W    ( 0.470f)
#define DOTII_LAYOUT_EYE_H    ( 0.470f)
#define DOTII_LAYOUT_BROW_Y   (-0.350f)
#define DOTII_LAYOUT_BROW_GAP ( 0.026f)
#define DOTII_LAYOUT_MOUTH_Y  ( 0.365f)

#define DOTII_EXPR_COUNT 48

/* Catalog access. Unknown input returns NULL; the caller falls back to idle. */
const dotii_expr_t *dotii_catalog_by_id(const char *id);
const dotii_expr_t *dotii_catalog_by_name(const char *name);
const dotii_expr_t *dotii_catalog_at(size_t index);
size_t dotii_catalog_count(void);
const dotii_expr_t *dotii_catalog_idle(void);

/* Legacy aora-bot name (e.g. "panicked") -> new vocabulary name.
   Returns NULL when the input is not a known legacy alias. */
const char *dotii_alias_resolve(const char *legacy_name);

/* Renders one face frame into an RGB565 framebuffer (size x size,
   little-endian, stride = size pixels). now_ms drives effect phases only;
   pose carries everything else. Implemented in dotii_draw.c (LVGL-free). */
void dotii_draw_render(uint16_t *fb, int32_t size, const dotii_expr_t *expr,
                       const dotii_pose_t *pose, uint32_t now_ms,
                       const dotii_rect_t *clip_in, dotii_rect_t *dirty_out,
                       dotii_rect_t *content_out);

#ifdef __cplusplus
}
#endif




