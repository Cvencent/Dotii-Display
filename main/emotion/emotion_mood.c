#include "emotion_mood.h"

#include <math.h>
#include <string.h>

#define MOOD_EVENT_HOLD_MS 4500u
#define MOOD_DECAY_HALF_LIFE_MS 120000u
#define MOOD_SLEEP_AFTER_MS 900000u

static float clampf(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

static void copy_id(char out[3], const char *id)
{
    if (out == NULL) return;
    if (id == NULL || id[0] == '\0') {
        out[0] = '0';
        out[1] = '0';
        out[2] = '\0';
        return;
    }
    out[0] = id[0];
    out[1] = id[1];
    out[2] = '\0';
}

static bool contains(const char *text, const char *needle)
{
    return text != NULL && needle != NULL && needle[0] != '\0' &&
           strstr(text, needle) != NULL;
}

static bool contains_any(const char *text, const char *const *needles, size_t count)
{
    if (text == NULL) return false;
    for (size_t i = 0; i < count; ++i) {
        if (contains(text, needles[i])) return true;
    }
    return false;
}

static float decayed(float value, float baseline, uint32_t elapsed_ms)
{
    if (elapsed_ms == 0) return value;
    const float factor = powf(0.5f, (float)elapsed_ms / (float)MOOD_DECAY_HALF_LIFE_MS);
    return baseline + (value - baseline) * factor;
}

static void set_event(emotion_mood_t *mood, const char *id, uint32_t now_ms)
{
    copy_id(mood->event_id, id);
    mood->event_until_ms = now_ms + MOOD_EVENT_HOLD_MS;
}

static void learn(emotion_mood_t *mood, bool positive, bool correction)
{
    if (positive) {
        mood->positive_bias = clampf(mood->positive_bias + 0.08f, 0.0f, 0.65f);
    }
    if (correction) {
        mood->correction_bias = clampf(mood->correction_bias + 0.08f, 0.0f, 0.65f);
    }
}

static const char *classify_user(emotion_mood_t *mood, const char *text)
{
    static const char *const praise[] = {"谢谢", "谢谢你", "太棒", "很好", "真棒", "喜欢", "满意", "干得好"};
    static const char *const correction[] = {"不对", "错了", "不是", "重新", "改一下", "弄错", "还是不对", "我说的是"};
    static const char *const sad[] = {"难过", "伤心", "不开心", "失落", "委屈", "想哭"};
    static const char *const angry[] = {"生气", "愤怒", "讨厌", "烦死", "气死"};
    static const char *const tired[] = {"累了", "好累", "困了", "疲惫", "没精神"};
    static const char *const question[] = {"怎么", "为什么", "是什么", "能不能", "可以吗", "如何", "？", "?"};
    static const char *const ambiguous[] = {"这个", "那个", "随便", "都行", "你看着办", "不清楚"};

    if (contains_any(text, correction, sizeof(correction) / sizeof(correction[0]))) {
        mood->valence = clampf(mood->valence - 0.35f, -1.0f, 1.0f);
        mood->arousal = clampf(mood->arousal + 0.20f, 0.0f, 1.0f);
        mood->trust = clampf(mood->trust - 0.08f, 0.0f, 1.0f);
        learn(mood, false, true);
        return "14";
    }
    if (contains_any(text, praise, sizeof(praise) / sizeof(praise[0]))) {
        mood->valence = clampf(mood->valence + 0.42f, -1.0f, 1.0f);
        mood->arousal = clampf(mood->arousal + 0.12f, 0.0f, 1.0f);
        mood->trust = clampf(mood->trust + 0.12f, 0.0f, 1.0f);
        learn(mood, true, false);
        return "10";
    }
    if (contains_any(text, sad, sizeof(sad) / sizeof(sad[0]))) {
        mood->valence = clampf(mood->valence - 0.55f, -1.0f, 1.0f);
        mood->arousal = clampf(mood->arousal - 0.10f, 0.0f, 1.0f);
        return "12";
    }
    if (contains_any(text, angry, sizeof(angry) / sizeof(angry[0]))) {
        mood->valence = clampf(mood->valence - 0.45f, -1.0f, 1.0f);
        mood->arousal = clampf(mood->arousal + 0.45f, 0.0f, 1.0f);
        return "21";
    }
    if (contains_any(text, tired, sizeof(tired) / sizeof(tired[0]))) {
        mood->fatigue = clampf(mood->fatigue + 0.35f, 0.0f, 1.0f);
        mood->arousal = clampf(mood->arousal - 0.25f, 0.0f, 1.0f);
        return "15";
    }
    if (contains_any(text, question, sizeof(question) / sizeof(question[0]))) {
        mood->confusion = clampf(mood->confusion + 0.18f, 0.0f, 1.0f);
        static const char *const direct_question[] = {"怎么", "为什么", "如何", "能不能", "可以吗"};
        if (contains_any(text, direct_question, sizeof(direct_question) / sizeof(direct_question[0]))) {
            return "11";
        }
        return contains_any(text, ambiguous, sizeof(ambiguous) / sizeof(ambiguous[0])) ? "20" : "11";
    }
    return NULL;
}

static const char *classify_assistant(emotion_mood_t *mood, const char *text)
{
    static const char *const failure[] = {"失败", "出错", "错误", "无法", "不能", "抱歉", "做不到", "异常"};
    static const char *const success[] = {"完成", "已完成", "搞定", "成功", "好了", "已经处理"};
    static const char *const refusal[] = {"不能帮你", "无法帮", "拒绝", "不允许", "受限"};
    static const char *const thinking[] = {"正在", "马上", "稍等", "处理中", "稍后"};
    if (contains_any(text, failure, sizeof(failure) / sizeof(failure[0]))) {
        mood->valence = clampf(mood->valence - 0.20f, -1.0f, 1.0f);
        return "34";
    }
    if (contains_any(text, refusal, sizeof(refusal) / sizeof(refusal[0]))) return "38";
    if (contains_any(text, success, sizeof(success) / sizeof(success[0]))) {
        mood->valence = clampf(mood->valence + 0.25f, -1.0f, 1.0f);
        return "33";
    }
    if (contains_any(text, thinking, sizeof(thinking) / sizeof(thinking[0]))) return "30";
    return NULL;
}

static const char *classify_emotion(const char *emotion)
{
    static const char *const positive[] = {"happy", "satisfied", "done", "music"};
    static const char *const negative[] = {"down", "angry", "error", "tired"};
    static const char *const uncertain[] = {"puzzled", "confused", "thinking", "listening"};
    if (contains_any(emotion, positive, sizeof(positive) / sizeof(positive[0]))) return "10";
    if (contains_any(emotion, negative, sizeof(negative) / sizeof(negative[0]))) return "12";
    if (contains_any(emotion, uncertain, sizeof(uncertain) / sizeof(uncertain[0]))) return "11";
    return NULL;
}

void emotion_mood_init(emotion_mood_t *mood, uint32_t now_ms)
{
    if (mood == NULL) return;
    memset(mood, 0, sizeof(*mood));
    mood->trust = 0.55f;
    mood->updated_ms = now_ms;
    mood->event_until_ms = now_ms;
    copy_id(mood->event_id, "02");
}

void emotion_mood_tick(emotion_mood_t *mood, uint32_t now_ms)
{
    if (mood == NULL) return;
    const uint32_t elapsed = now_ms - mood->updated_ms;
    if (elapsed > 0) {
        if (elapsed >= MOOD_SLEEP_AFTER_MS) {
            mood->valence = 0.0f;
            mood->arousal = 0.08f;
            mood->confusion = 0.0f;
            mood->fatigue = 1.0f;
            copy_id(mood->event_id, "00");
            mood->event_until_ms = now_ms;
        } else {
            mood->valence = decayed(mood->valence, 0.0f, elapsed);
            mood->arousal = decayed(mood->arousal, 0.18f, elapsed);
            mood->trust = decayed(mood->trust, 0.55f, elapsed);
            mood->confusion = decayed(mood->confusion, 0.0f, elapsed);
            mood->fatigue = decayed(mood->fatigue, 0.0f, elapsed);
        }
        mood->updated_ms = now_ms;
        mood->revision++;
    }
}

void emotion_mood_ingest_user(emotion_mood_t *mood, const char *text, uint32_t now_ms)
{
    if (mood == NULL) return;
    emotion_mood_tick(mood, now_ms);
    /* A short-lived event must remain visible even when the longer mood is
       still negative. Clearing the event window first gives the new input
       priority without erasing the learned baseline. */
    copy_id(mood->event_id, "00");
    mood->event_until_ms = now_ms;
    const char *event = classify_user(mood, text);
    if (event != NULL) set_event(mood, event, now_ms);
    mood->revision++;
}

void emotion_mood_ingest_assistant(emotion_mood_t *mood, const char *text, uint32_t now_ms)
{
    if (mood == NULL) return;
    emotion_mood_tick(mood, now_ms);
    const char *event = classify_assistant(mood, text);
    if (event != NULL) set_event(mood, event, now_ms);
    mood->revision++;
}

void emotion_mood_ingest_server_emotion(emotion_mood_t *mood, const char *emotion, uint32_t now_ms)
{
    if (mood == NULL) return;
    emotion_mood_tick(mood, now_ms);
    const char *event = classify_emotion(emotion);
    if (event != NULL) set_event(mood, event, now_ms);
    mood->revision++;
}

bool emotion_mood_has_active_event(const emotion_mood_t *mood)
{
    return mood != NULL && mood->event_until_ms > mood->updated_ms &&
           mood->event_id[0] != '\0' && strcmp(mood->event_id, "00") != 0;
}

const char *emotion_mood_current_id(const emotion_mood_t *mood)
{
    if (mood == NULL) return "02";
    if (emotion_mood_has_active_event(mood)) {
        return mood->event_id;
    }
    if (mood->fatigue > 0.82f) return "00";
    if (mood->fatigue > 0.62f) return "15";
    if (mood->confusion > 0.70f) return "20";
    if (mood->valence > 0.28f) return "10";
    if (mood->valence < -0.28f) return "12";
    if (mood->arousal > 0.75f) return "13";
    /* Learned preference colours the resting face, but can never override a
       stronger live emotion above. Praise makes the baseline welcoming;
       repeated corrections make it attentive and slightly apologetic. */
    if (mood->positive_bias > 0.30f &&
        mood->positive_bias > mood->correction_bias) return "19";
    if (mood->correction_bias > 0.30f &&
        mood->correction_bias > mood->positive_bias) return "14";
    return "02";
}

float emotion_mood_valence(const emotion_mood_t *mood)
{
    return mood != NULL ? mood->valence : 0.0f;
}

float emotion_mood_positive_bias(const emotion_mood_t *mood)
{
    return mood != NULL ? mood->positive_bias : 0.0f;
}

float emotion_mood_correction_bias(const emotion_mood_t *mood)
{
    return mood != NULL ? mood->correction_bias : 0.0f;
}

uint32_t emotion_mood_revision(const emotion_mood_t *mood)
{
    return mood != NULL ? mood->revision : 0;
}

void emotion_mood_export(const emotion_mood_t *mood, emotion_mood_snapshot_t *out)
{
    if (mood == NULL || out == NULL) return;
    out->valence = mood->valence;
    out->arousal = mood->arousal;
    out->trust = mood->trust;
    out->confusion = mood->confusion;
    out->fatigue = mood->fatigue;
    out->positive_bias = mood->positive_bias;
    out->correction_bias = mood->correction_bias;
    copy_id(out->event_id, mood->event_id);
    out->event_until_ms = mood->event_until_ms;
    out->updated_ms = mood->updated_ms;
    out->revision = mood->revision;
}

bool emotion_mood_import(emotion_mood_t *mood, const emotion_mood_snapshot_t *snapshot)
{
    if (mood == NULL || snapshot == NULL) return false;
    memset(mood, 0, sizeof(*mood));
    mood->valence = clampf(snapshot->valence, -1.0f, 1.0f);
    mood->arousal = clampf(snapshot->arousal, 0.0f, 1.0f);
    mood->trust = clampf(snapshot->trust, 0.0f, 1.0f);
    mood->confusion = clampf(snapshot->confusion, 0.0f, 1.0f);
    mood->fatigue = clampf(snapshot->fatigue, 0.0f, 1.0f);
    mood->positive_bias = clampf(snapshot->positive_bias, 0.0f, 0.65f);
    mood->correction_bias = clampf(snapshot->correction_bias, 0.0f, 0.65f);
    copy_id(mood->event_id, snapshot->event_id);
    mood->event_until_ms = snapshot->event_until_ms;
    mood->updated_ms = snapshot->updated_ms;
    mood->revision = snapshot->revision;
    return true;
}
