#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "emotion_pose.h"

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

int main(void)
{
    test_idle_pose_is_neutral();
    test_sleep_closes_eyes();
    test_null_inputs_are_safe();
    test_gaze_is_bounded();
    printf("emotion_pose tests passed\n");
    return 0;
}
