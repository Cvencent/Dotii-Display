#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "emotion_engine.h"

static void test_init_and_set(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 1000);
    assert(strcmp(emotion_engine_current_id(&engine), "02") == 0);

    assert(emotion_engine_set(&engine, "10", 1100));
    assert(strcmp(emotion_engine_current_id(&engine), "10") == 0);
    emotion_engine_tick(&engine, 1200);
    const emotion_engine_state_t *state = emotion_engine_state(&engine);
    assert(state != NULL);
    assert(strcmp(state->id, "10") == 0);
    assert(state->active);
}

static void test_unknown_falls_back(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    assert(emotion_engine_set(&engine, "10", 0));
    assert(!emotion_engine_set(&engine, "99", 10));
    assert(!emotion_engine_set(&engine, "not_a_name", 10));
    /* Failed set must not disturb the current emotion. */
    assert(strcmp(emotion_engine_current_id(&engine), "10") == 0);

    assert(!emotion_engine_set(NULL, "10", 0));
}

static void test_set_by_name(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    assert(emotion_engine_set(&engine, "happy", 0));
    assert(strcmp(emotion_engine_current_id(&engine), "10") == 0);
    assert(emotion_engine_set(&engine, "thinking", 0));
    assert(strcmp(emotion_engine_current_id(&engine), "30") == 0);
}

static void test_hold_expires_to_base(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    assert(emotion_engine_set_hold(&engine, "13", 2000, 1000));
    assert(strcmp(emotion_engine_current_id(&engine), "13") == 0);

    assert(!emotion_engine_tick(&engine, 2500));
    assert(strcmp(emotion_engine_current_id(&engine), "13") == 0);

    assert(emotion_engine_tick(&engine, 3100));
    assert(strcmp(emotion_engine_current_id(&engine), "02") == 0);
}

static void test_hold_reverts_to_custom_base(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    assert(emotion_engine_set(&engine, "10", 0));
    assert(emotion_engine_set_hold(&engine, "13", 1000, 2000));
    assert(strcmp(emotion_engine_current_id(&engine), "13") == 0);
    assert(emotion_engine_tick(&engine, 3100));
    assert(strcmp(emotion_engine_current_id(&engine), "10") == 0);
}

static void test_gaze_clamped(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    emotion_engine_set_gaze(&engine, 5.0f, -9.0f);
    emotion_engine_tick(&engine, 1);
    const emotion_engine_state_t *state = emotion_engine_state(&engine);
    assert(state->gaze_x == 1.0f);
    assert(state->gaze_y == -1.0f);
    emotion_engine_set_gaze(&engine, 0.25f, 0.5f);
    emotion_engine_tick(&engine, 2);
    assert(state->gaze_x == 0.25f);
    assert(state->gaze_y == 0.5f);
}

int main(void)
{
    test_init_and_set();
    test_unknown_falls_back();
    test_set_by_name();
    test_hold_expires_to_base();
    test_hold_reverts_to_custom_base();
    test_gaze_clamped();
    printf("emotion_engine tests passed\n");
    return 0;
}
