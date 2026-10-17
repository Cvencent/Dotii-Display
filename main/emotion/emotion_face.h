#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Facial channels are deliberately explicit: a smile is not a colour.
   All values are normalised so catalog rows and the renderer can be tested
   without LVGL. */
typedef enum {
    EMOTION_MOUTH_FLAT = 0,
    EMOTION_MOUTH_SMILE,
    EMOTION_MOUTH_FROWN,
    EMOTION_MOUTH_OPEN,
    EMOTION_MOUTH_WAVY,
    EMOTION_MOUTH_SMALL,
} emotion_mouth_t;

typedef enum {
    EMOTION_BROW_NEUTRAL = 0,
    EMOTION_BROW_RAISED,
    EMOTION_BROW_INNER_DOWN,
    EMOTION_BROW_INNER_UP,
    EMOTION_BROW_FLAT,
} emotion_brow_t;

typedef struct {
    emotion_mouth_t mouth;
    float mouth_curve;      /* -1 frown .. +1 smile */
    float mouth_width;      /* multiplier around the character anchor */
    float mouth_open;       /* 0..1 */
    emotion_brow_t brow;
    float brow_left_tilt;   /* degrees; +/- mirrors */
    float brow_right_tilt;
    float brow_height;      /* canvas units */
    float blush;            /* 0..1 */
    float head_tilt_deg;    /* pose tilt */
} emotion_face_t;


const emotion_face_t *emotion_face_lookup(const char *id);
const emotion_face_t *emotion_face_fallback(void);

/* Projects one facial anchor around the panel centre using the same pose
   rotation as the renderer. Exposed for deterministic host tests. */
void emotion_face_rotate_point(float center_x, float center_y,
                               float x, float y, float tilt_deg,
                               float *out_x, float *out_y);
#ifdef __cplusplus
}
#endif
