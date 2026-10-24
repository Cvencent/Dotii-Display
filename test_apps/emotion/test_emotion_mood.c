#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "emotion_mood.h"

static void test_feedback_shapes_current_expression(void)
{
    emotion_mood_t mood;
    emotion_mood_init(&mood, 0);

    emotion_mood_ingest_user(&mood, "谢谢，太好了", 100);
    emotion_mood_tick(&mood, 100);
    assert(strcmp(emotion_mood_current_id(&mood), "10") == 0);

    emotion_mood_ingest_user(&mood, "不对，我说的是另一个", 200);
    emotion_mood_tick(&mood, 200);
    assert(strcmp(emotion_mood_current_id(&mood), "14") == 0);
}

static void test_context_questions_and_user_mood(void)
{
    emotion_mood_t mood;
    emotion_mood_init(&mood, 0);
    emotion_mood_ingest_user(&mood, "这个应该怎么选？", 100);
    emotion_mood_tick(&mood, 100);
    assert(strcmp(emotion_mood_current_id(&mood), "18") == 0);

    emotion_mood_ingest_user(&mood, "我今天真的很难过", 200);
    emotion_mood_tick(&mood, 200);
    assert(strcmp(emotion_mood_current_id(&mood), "21") == 0);
}

static void test_assistant_outcomes(void)
{
    emotion_mood_t mood;
    emotion_mood_init(&mood, 0);
    emotion_mood_ingest_assistant(&mood, "操作失败了，很抱歉", 100);
    emotion_mood_tick(&mood, 100);
    assert(strcmp(emotion_mood_current_id(&mood), "34") == 0);

    emotion_mood_ingest_assistant(&mood, "已经完成，搞定了", 200);
    emotion_mood_tick(&mood, 200);
    assert(strcmp(emotion_mood_current_id(&mood), "33") == 0);
}

static void test_learning_persists_and_decays(void)
{
    emotion_mood_t mood;
    emotion_mood_init(&mood, 0);
    for (int i = 0; i < 6; ++i) {
        emotion_mood_ingest_user(&mood, "很好，谢谢", (uint32_t)i * 100);
    }
    emotion_mood_tick(&mood, 600);
    assert(emotion_mood_positive_bias(&mood) > 0);

    emotion_mood_snapshot_t saved;
    emotion_mood_export(&mood, &saved);
    emotion_mood_t restored;
    emotion_mood_init(&restored, 0);
    emotion_mood_import(&restored, &saved);
    assert(emotion_mood_positive_bias(&restored) == emotion_mood_positive_bias(&mood));
    assert(emotion_mood_correction_bias(&restored) == emotion_mood_correction_bias(&mood));

    const float before = emotion_mood_valence(&mood);
    emotion_mood_tick(&mood, 120000);
    assert(emotion_mood_valence(&mood) < before);
}

static void test_active_context_expression_is_exposed(void)
{
    emotion_mood_t mood;
    emotion_mood_init(&mood, 0);
    assert(!emotion_mood_has_active_event(&mood));

    emotion_mood_ingest_user(&mood, "这个应该怎么选？", 100);
    assert(emotion_mood_has_active_event(&mood));
    assert(strcmp(emotion_mood_current_id(&mood), "18") == 0);

    emotion_mood_tick(&mood, 4700);
    assert(!emotion_mood_has_active_event(&mood));
}

int main(void)
{
    test_feedback_shapes_current_expression();
    test_context_questions_and_user_mood();
    test_assistant_outcomes();
    test_learning_persists_and_decays();
    test_active_context_expression_is_exposed();
    printf("emotion_mood tests passed\n");
    return 0;
}
