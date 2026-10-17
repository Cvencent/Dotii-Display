#include "emotion_face.h"
#include <math.h>
#include <string.h>

typedef struct {
    const char *id;
    emotion_face_t face;
} emotion_face_row_t;

#define F(mouth, curve, width, open, brow, left, right, h, blush, tilt) \
    { (mouth), (curve), (width), (open), (brow), (left), (right), (h), (blush), (tilt) }

static const emotion_face_row_t k_faces[] = {
    { "00", F(EMOTION_MOUTH_SMALL,  0.00f, 0.70f, 0.00f, EMOTION_BROW_INNER_UP, -2.0f,  2.0f, -2.0f, 0.00f, 0.0f) },
    { "01", F(EMOTION_MOUTH_FLAT,   0.00f, 0.90f, 0.00f, EMOTION_BROW_RAISED,  -3.0f,  3.0f, 10.0f, 0.00f, 0.0f) },
    { "02", F(EMOTION_MOUTH_FLAT,   0.00f, 1.00f, 0.00f, EMOTION_BROW_NEUTRAL,  0.0f,  0.0f,  0.0f, 0.00f, 0.0f) },
    { "03", F(EMOTION_MOUTH_SMALL,  0.20f, 0.90f, 0.00f, EMOTION_BROW_NEUTRAL, -3.0f,  3.0f,  4.0f, 0.00f, 3.0f) },
    { "04", F(EMOTION_MOUTH_SMALL, -0.10f, 0.80f, 0.00f, EMOTION_BROW_NEUTRAL,  0.0f,  0.0f, -2.0f, 0.00f, 4.0f) },
    { "05", F(EMOTION_MOUTH_OPEN,   0.15f, 0.80f, 0.35f, EMOTION_BROW_RAISED,  -4.0f,  4.0f, 12.0f, 0.00f, 0.0f) },
    { "06", F(EMOTION_MOUTH_SMALL, -0.15f, 0.70f, 0.00f, EMOTION_BROW_INNER_UP, -2.0f,  2.0f, -4.0f, 0.00f, 0.0f) },
    { "07", F(EMOTION_MOUTH_OPEN,   0.00f, 0.80f, 0.22f, EMOTION_BROW_RAISED,  -4.0f,  4.0f, 10.0f, 0.00f, 4.0f) },
    { "10", F(EMOTION_MOUTH_SMILE,  0.85f, 1.05f, 0.05f, EMOTION_BROW_RAISED,  -2.0f,  2.0f,  3.0f, 0.08f, 0.0f) },
    { "11", F(EMOTION_MOUTH_SMALL, -0.15f, 0.85f, 0.00f, EMOTION_BROW_RAISED, -10.0f,  2.0f,  8.0f, 0.00f, 4.0f) },
    { "12", F(EMOTION_MOUTH_FROWN, -0.85f, 1.00f, 0.00f, EMOTION_BROW_INNER_UP, -8.0f,  8.0f, -5.0f, 0.00f, 0.0f) },
    { "13", F(EMOTION_MOUTH_OPEN,   0.00f, 0.85f, 0.85f, EMOTION_BROW_RAISED,  -4.0f,  4.0f, 16.0f, 0.00f, 0.0f) },
    { "14", F(EMOTION_MOUTH_SMILE,  0.55f, 0.80f, 0.00f, EMOTION_BROW_INNER_UP, -3.0f,  3.0f, -2.0f, 0.85f, -4.0f) },
    { "15", F(EMOTION_MOUTH_SMALL, -0.10f, 0.95f, 0.12f, EMOTION_BROW_INNER_UP, -2.0f,  2.0f, -6.0f, 0.00f, 0.0f) },
    { "16", F(EMOTION_MOUTH_FLAT,   0.00f, 1.00f, 0.00f, EMOTION_BROW_FLAT,     0.0f,  0.0f, -3.0f, 0.00f, 0.0f) },
    { "17", F(EMOTION_MOUTH_OPEN,  -0.25f, 0.85f, 0.30f, EMOTION_BROW_INNER_UP, -9.0f,  9.0f,  6.0f, 0.00f, 5.0f) },
    { "18", F(EMOTION_MOUTH_FLAT,  -0.20f, 1.00f, 0.00f, EMOTION_BROW_NEUTRAL, -5.0f,  1.0f,  0.0f, 0.00f, 6.0f) },
    { "19", F(EMOTION_MOUTH_SMILE,  0.70f, 0.95f, 0.00f, EMOTION_BROW_RAISED,  -1.0f,  1.0f,  3.0f, 0.00f, 0.0f) },
    { "20", F(EMOTION_MOUTH_WAVY,  -0.10f, 0.90f, 0.00f, EMOTION_BROW_RAISED, -11.0f,  4.0f,  9.0f, 0.00f, -3.0f) },
    { "21", F(EMOTION_MOUTH_FROWN, -0.90f, 1.05f, 0.12f, EMOTION_BROW_INNER_DOWN, -5.0f,  5.0f, -4.0f, 0.05f, 0.0f) },
    { "30", F(EMOTION_MOUTH_SMALL, -0.10f, 0.90f, 0.00f, EMOTION_BROW_NEUTRAL,  1.0f, -1.0f,  2.0f, 0.00f, 2.0f) },
    { "31", F(EMOTION_MOUTH_SMALL,  0.25f, 0.85f, 0.08f, EMOTION_BROW_RAISED,  -2.0f,  2.0f,  5.0f, 0.00f, 0.0f) },
    { "32", F(EMOTION_MOUTH_FLAT,   0.00f, 1.00f, 0.00f, EMOTION_BROW_FLAT,     0.0f,  0.0f, -1.0f, 0.00f, 0.0f) },
    { "33", F(EMOTION_MOUTH_SMILE,  0.90f, 1.00f, 0.05f, EMOTION_BROW_RAISED,  -1.0f,  1.0f,  4.0f, 0.06f, 0.0f) },
    { "34", F(EMOTION_MOUTH_FROWN, -0.80f, 1.00f, 0.00f, EMOTION_BROW_INNER_UP, -9.0f,  9.0f, -5.0f, 0.00f, 0.0f) },
    { "35", F(EMOTION_MOUTH_SMALL,  0.00f, 0.90f, 0.00f, EMOTION_BROW_NEUTRAL,  0.0f,  0.0f,  0.0f, 0.00f, 0.0f) },
    { "36", F(EMOTION_MOUTH_SMALL,  0.10f, 0.85f, 0.12f, EMOTION_BROW_RAISED,  -3.0f,  3.0f,  8.0f, 0.00f, 2.0f) },
    { "37", F(EMOTION_MOUTH_SMALL, -0.05f, 0.90f, 0.00f, EMOTION_BROW_NEUTRAL, -2.0f,  2.0f,  1.0f, 0.00f, 2.0f) },
    { "38", F(EMOTION_MOUTH_FLAT,  -0.20f, 0.95f, 0.00f, EMOTION_BROW_INNER_UP, -7.0f,  7.0f, -3.0f, 0.00f, 0.0f) },
    { "39", F(EMOTION_MOUTH_OPEN,   0.10f, 0.95f, 0.42f, EMOTION_BROW_NEUTRAL,  0.0f,  0.0f,  1.0f, 0.00f, 0.0f) },
    { "40", F(EMOTION_MOUTH_SMALL,  0.00f, 0.85f, 0.00f, EMOTION_BROW_FLAT,     2.0f, -2.0f,  2.0f, 0.00f, 2.0f) },
    { "41", F(EMOTION_MOUTH_SMALL, -0.10f, 0.75f, 0.00f, EMOTION_BROW_INNER_UP, -4.0f,  4.0f, -4.0f, 0.00f, 0.0f) },
    { "42", F(EMOTION_MOUTH_SMILE,  0.80f, 1.00f, 0.10f, EMOTION_BROW_RAISED,  -1.0f,  1.0f,  4.0f, 0.10f, 2.0f) },
};

const emotion_face_t *emotion_face_lookup(const char *id)
{
    if (id == NULL) return NULL;
    for (size_t i = 0; i < sizeof(k_faces) / sizeof(k_faces[0]); ++i) {
        if (strcmp(k_faces[i].id, id) == 0) return &k_faces[i].face;
    }
    return NULL;
}

const emotion_face_t *emotion_face_fallback(void)
{
    const emotion_face_t *face = emotion_face_lookup("02");
    return face != NULL ? face : &k_faces[2].face;
}

void emotion_face_rotate_point(float center_x, float center_y,
                               float x, float y, float tilt_deg,
                               float *out_x, float *out_y)
{
    const float radians = tilt_deg * 0.017453292519943295f;
    const float cosine = cosf(radians);
    const float sine = sinf(radians);
    const float dx = x - center_x;
    const float dy = y - center_y;
    if (out_x != NULL) *out_x = center_x + dx * cosine - dy * sine;
    if (out_y != NULL) *out_y = center_y + dx * sine + dy * cosine;
}
