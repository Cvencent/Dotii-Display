#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "emotion_pose.h"
#include "emotion_geometry.h"

static void test_idle_pose_is_neutral(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    emotion_engine_tick(&engine, 0);
    const emotion_engine_state_t *state = emotion_engine_state(&engine);
    const emotion_def_t *def = emotion_catalog_by_id("02");
    const character_def_t *pack = character_pack_lookup("dotii");
    emotion_pose_t pose;
    emotion_pose_eval(state, def, pack, &pose);
    assert(pose.eye_openness > 0.9f && pose.eye_openness <= 1.4f);
    assert(pose.eye_w >= 24 && pose.eye_h >= 24);
    assert(pose.body_scale >= 0.5f && pose.body_scale <= 1.5f);
}

static void test_sleep_closes_eyes(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    emotion_engine_set(&engine, "00", 0);
    emotion_engine_tick(&engine, 2000);
    const emotion_engine_state_t *state = emotion_engine_state(&engine);
    emotion_pose_t pose;
    emotion_pose_eval(state, emotion_catalog_by_id("00"), character_pack_lookup("nimbo"), &pose);
    assert(pose.eye_openness < 0.3f);
}

static void test_null_inputs_are_safe(void)
{
    emotion_pose_t pose;
    emotion_pose_eval(NULL, NULL, NULL, &pose);
    assert(pose.eye_openness == 1.0f);
    emotion_pose_eval(NULL, NULL, NULL, NULL); /* must not crash */
}

static void test_gaze_is_bounded(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    emotion_engine_set_gaze(&engine, 9.0f, -9.0f);
    emotion_engine_tick(&engine, 100);
    emotion_pose_t pose;
    emotion_pose_eval(emotion_engine_state(&engine), emotion_catalog_by_id("02"),
                      character_pack_lookup("dotii"), &pose);
    assert(pose.eye_look_x >= -1.0f && pose.eye_look_x <= 1.0f);
    assert(pose.eye_look_y >= -1.0f && pose.eye_look_y <= 1.0f);
}

static void test_every_catalog_emotion_has_geometry(void)
{
    const size_t count = emotion_catalog_count();
    assert(count > 0);
    for (size_t i = 0; i < count; ++i) {
        const emotion_def_t *def = emotion_catalog_at(i);
        assert(def != NULL);
        const int8_t by_id = emotion_geometry_look_index(def->id);
        const int8_t by_name = emotion_geometry_look_index(def->name);
        assert(by_id >= 0);
        assert(by_name == by_id);
        assert(emotion_geometry_look((uint8_t)by_id) != NULL);
    }
}

int main(void)
{
    test_idle_pose_is_neutral();
    test_sleep_closes_eyes();
    test_null_inputs_are_safe();
    test_gaze_is_bounded();
    test_every_catalog_emotion_has_geometry();
    printf("emotion_pose tests passed\n");
    return 0;
}
