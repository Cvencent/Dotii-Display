#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "emotion_catalog.h"
#include "emotion_face.h"

static void test_core_faces_are_distinct(void)
{
    const emotion_face_t *happy = emotion_face_lookup("10");
    const emotion_face_t *down = emotion_face_lookup("12");
    const emotion_face_t *surprised = emotion_face_lookup("13");
    const emotion_face_t *angry = emotion_face_lookup("21");
    assert(happy != NULL && down != NULL && surprised != NULL && angry != NULL);
    assert(happy->mouth_curve > 0.4f);
    assert(down->mouth_curve < -0.3f);
    assert(surprised->mouth_open > 0.5f);
    assert(angry->brow == EMOTION_BROW_INNER_DOWN);
}

static void test_every_catalog_row_has_face_channels(void)
{
    for (size_t i = 0; i < emotion_catalog_count(); ++i) {
        const emotion_def_t *def = emotion_catalog_at(i);
        const emotion_face_t *face = emotion_face_lookup(def->id);
        assert(face != NULL);
        assert(face->mouth_width > 0.0f);
        assert(face->brow_height >= -12.0f && face->brow_height <= 18.0f);
    }
}

static void test_face_rotation_moves_features_with_head_tilt(void)
{
    float x = 0.0f;
    float y = 0.0f;
    emotion_face_rotate_point(100.0f, 100.0f, 60.0f, 100.0f, 10.0f, &x, &y);
    assert(fabsf(x - 60.61f) < 0.01f);
    assert(fabsf(y - 93.05f) < 0.01f);
}

int main(void)
{
    test_core_faces_are_distinct();
    test_every_catalog_row_has_face_channels();
    test_face_rotation_moves_features_with_head_tilt();
    printf("emotion_face tests passed\n");
    return 0;
}
