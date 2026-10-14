#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Geometry ported verbatim from sam70361/aora-bot (emotion-ball).
   All fixed-point tables use x100 coordinates in the upstream viewBox
   (-15..244), whose head centre is eb_head_c. */

#define EB_NO_COLOR 0xFFu
#define EB_ANIM_SLOTS 3u

typedef struct {
    float x;
    float y;
    float sx;
    float sy;
    float eye;
} eb_face_t;

typedef struct {
    const char *id;
    uint16_t ring_points;
    uint16_t body_points;
    const int16_t *ring;
    eb_face_t face;
    float tilt_scale;
} eb_shape_t;

typedef struct {
    uint8_t target;
    uint8_t prop;
    uint8_t type;
    float amp;
    float period;
    float phase;
} eb_anim_t;

typedef struct {
    uint8_t pool[6];
    uint8_t pool_length;
    uint16_t pool_ms_min;
    uint16_t pool_ms_max;
    uint8_t color_index;   /* EB_NO_COLOR keeps the character pack colour */
    uint16_t blink_ms_min; /* 0 disables blinking for this look */
    uint16_t blink_ms_max;
    uint8_t anim_count;
    float body[4];         /* x, y, rotate, scale */
    float eye[7][3];       /* x, y, look_x, look_y, scale_x, scale_y, rotate */
    eb_anim_t anims[EB_ANIM_SLOTS];
} eb_look_t;

extern const float eb_head_c;
extern const float eb_eye_half;
extern const float eb_view_span;

const eb_shape_t *emotion_geometry_shape(uint8_t index);
uint8_t emotion_geometry_shape_count(void);
uint8_t emotion_geometry_shape_index(const char *id);

const int16_t *emotion_geometry_ring(uint8_t expression, uint8_t eye);
uint16_t emotion_geometry_ring_points(void);
uint8_t emotion_geometry_expression_count(void);

const eb_look_t *emotion_geometry_look(uint8_t index);
uint8_t emotion_geometry_look_count(void);
const uint32_t *emotion_geometry_body_colors(void);
const char *emotion_geometry_look_id(uint8_t index);
int8_t emotion_geometry_look_index(const char *id);

#ifdef __cplusplus
}
#endif