#include "emotion_catalog.h"

#include <string.h>

/* Emotion table ported from aora-bot (emotion-ball/js/emotions.js).
   IDs are the stable public contract: 00-09 lifecycle, 10-29 emotions,
   30-49 agent states, 50+ reserved for user-registered emotions. */

#define A(target, prop, type, amp, period, phase) \
    { (target), (prop), (type), (amp), (period), (phase) }

static const emotion_def_t k_emotions[] = {
    /* --- lifecycle 00-09 --- */
    { "00", "sleeping", "睡眠", EMOTION_GROUP_LIFE, 900, false, 0.08f, -1, -1, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_Y, ANIM_SINE, 1.2f, 3600.0f, 0.0f) } },
    { "01", "waking", "唤醒", EMOTION_GROUP_LIFE, 320, true, 1.0f, -1, -1, 0, { {0} } },
    { "02", "idle", "待机", EMOTION_GROUP_LIFE, 600, true, 1.0f, 2500, 5500, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_X, ANIM_GLANCE, 0.55f, 5200.0f, 0.0f) } },
    { "03", "curious", "好奇", EMOTION_GROUP_LIFE, 420, true, 1.0f, 2200, 4200, 2,
      { A(ANIM_TARGET_BODY, ANIM_PROP_ROTATE, ANIM_SINE, 5.0f, 2600.0f, 0.0f),
        A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_PULSE, 0.08f, 1800.0f, 0.0f) } },
    { "04", "spacing_out", "发呆", EMOTION_GROUP_LIFE, 700, true, 0.55f, 3400, 6200, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_X, ANIM_SINE, 0.35f, 7000.0f, 0.0f) } },
    { "05", "booting", "加载", EMOTION_GROUP_LIFE, 380, false, 0.85f, 700, 1400, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_OPEN, ANIM_BLINK, 1.0f, 900.0f, 0.0f) } },
    { "06", "dormant", "休眠", EMOTION_GROUP_LIFE, 800, false, 0.5f, -1, -1, 1,
      { A(ANIM_TARGET_BODY, ANIM_PROP_SCALE, ANIM_SINE, 0.01f, 4200.0f, 0.0f) } },
    { "07", "shake_awake", "抖动唤醒", EMOTION_GROUP_LIFE, 300, true, 0.9f, 900, 1800, 1,
      { A(ANIM_TARGET_BODY, ANIM_PROP_ROTATE, ANIM_JITTER, 4.0f, 1.0f, 0.0f) } },

    /* --- emotions 10-29 --- */
    { "10", "happy", "开心", EMOTION_GROUP_EMOTION, 380, true, 0.9f, 2200, 4200, 2,
      { A(ANIM_TARGET_BODY, ANIM_PROP_Y, ANIM_SINE, 3.0f, 1500.0f, 0.0f),
        A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_Y, ANIM_SINE, 2.5f, 1700.0f, 0.0f) } },
    { "11", "puzzled", "疑惑", EMOTION_GROUP_EMOTION, 400, true, 1.0f, 2400, 4600, 1,
      { A(ANIM_TARGET_BODY, ANIM_PROP_ROTATE, ANIM_SINE, 6.0f, 3000.0f, 0.0f) } },
    { "12", "down", "失落", EMOTION_GROUP_EMOTION, 550, true, 0.6f, 3000, 5200, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_Y, ANIM_SINE, 2.0f, 3800.0f, 0.0f) } },
    { "13", "surprised", "惊讶", EMOTION_GROUP_EMOTION, 220, true, 1.0f, 2600, 4800, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_PULSE, 0.45f, 1400.0f, 0.0f) } },
    { "14", "shy", "害羞", EMOTION_GROUP_EMOTION, 500, true, 0.85f, 2800, 5200, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_X, ANIM_GLANCE, 0.7f, 4200.0f, 0.0f) } },
    { "15", "tired", "疲惫", EMOTION_GROUP_EMOTION, 620, true, 0.55f, 3600, 6400, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_Y, ANIM_SINE, 1.5f, 4600.0f, 0.0f) } },
    { "16", "focused", "专注", EMOTION_GROUP_EMOTION, 360, true, 1.0f, 3200, 6000, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_SINE, 0.03f, 2600.0f, 0.0f) } },
    { "17", "panicked", "慌张", EMOTION_GROUP_EMOTION, 200, true, 1.0f, 1400, 2600, 2,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_X, ANIM_SCAN, 0.9f, 900.0f, 0.0f),
        A(ANIM_TARGET_BODY, ANIM_PROP_ROTATE, ANIM_JITTER, 3.0f, 1.0f, 0.0f) } },
    { "18", "resigned", "无奈", EMOTION_GROUP_EMOTION, 420, true, 0.7f, 2800, 5000, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_Y, ANIM_SINE, 2.2f, 3400.0f, 0.0f) } },
    { "19", "satisfied", "满意", EMOTION_GROUP_EMOTION, 380, true, 0.75f, 2400, 4400, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_Y, ANIM_SINE, 3.2f, 1300.0f, 0.0f) } },
    { "20", "confused", "困惑", EMOTION_GROUP_EMOTION, 420, true, 1.0f, 2600, 4800, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_X, ANIM_SINE, 0.5f, 3200.0f, 0.0f) } },
    { "21", "angry", "生气", EMOTION_GROUP_EMOTION, 240, true, 0.85f, 2200, 4200, 2,
      { A(ANIM_TARGET_BODY, ANIM_PROP_ROTATE, ANIM_JITTER, 3.5f, 1.0f, 0.0f),
        A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_PULSE, -0.15f, 1200.0f, 0.0f) } },

    /* --- agent states 30-49 --- */
    { "30", "thinking", "思考中", EMOTION_GROUP_AGENT, 320, true, 0.9f, 2200, 4000, 2,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_Y, ANIM_SCAN, 0.6f, 2600.0f, 0.0f),
        A(ANIM_TARGET_BODY, ANIM_PROP_ROTATE, ANIM_SINE, 3.0f, 2200.0f, 0.0f) } },
    { "31", "receiving", "接收任务", EMOTION_GROUP_AGENT, 220, true, 1.0f, 2400, 4400, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_PULSE, 0.12f, 900.0f, 0.0f) } },
    { "32", "busy", "处理中", EMOTION_GROUP_AGENT, 300, true, 0.95f, 2200, 4200, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_X, ANIM_SCAN, 0.45f, 1400.0f, 0.0f) } },
    { "33", "done", "任务完成", EMOTION_GROUP_AGENT, 300, true, 0.9f, 2400, 4600, 2,
      { A(ANIM_TARGET_BODY, ANIM_PROP_Y, ANIM_PULSE, 6.0f, 700.0f, 0.0f),
        A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_PULSE, 0.1f, 700.0f, 0.0f) } },
    { "34", "error", "出错", EMOTION_GROUP_AGENT, 180, true, 1.0f, 1600, 3200, 2,
      { A(ANIM_TARGET_BODY, ANIM_PROP_ROTATE, ANIM_JITTER, 4.0f, 1.0f, 0.0f),
        A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_PULSE, 0.3f, 600.0f, 0.0f) } },
    { "35", "listening", "等待输入", EMOTION_GROUP_AGENT, 260, true, 1.0f, 2000, 3800, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_Y, ANIM_SINE, 1.8f, 2400.0f, 0.0f) } },
    { "36", "loading", "联网加载", EMOTION_GROUP_AGENT, 280, false, 0.9f, 700, 1300, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_OPEN, ANIM_BLINK, 1.0f, 800.0f, 0.0f) } },
    { "37", "recalling", "复述回忆", EMOTION_GROUP_AGENT, 340, true, 0.85f, 2400, 4400, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_Y, ANIM_SINE, 2.0f, 3000.0f, 0.0f) } },
    { "38", "refusing", "受限", EMOTION_GROUP_AGENT, 300, true, 0.8f, 2600, 4800, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_X, ANIM_GLANCE, 0.8f, 1600.0f, 0.0f) } },
    { "39", "replying", "输出回复", EMOTION_GROUP_AGENT, 240, true, 0.95f, 2200, 4200, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_PULSE, 0.06f, 260.0f, 0.0f) } },
    { "40", "searching", "检索资料", EMOTION_GROUP_AGENT, 300, true, 0.9f, 2000, 4000, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_LOOK_X, ANIM_SCAN, 1.0f, 1100.0f, 0.0f) } },
    { "41", "powering_off", "停止", EMOTION_GROUP_AGENT, 500, false, 0.45f, -1, -1, 1,
      { A(ANIM_TARGET_EYES, ANIM_PROP_OPEN, ANIM_SINE, 0.05f, 3000.0f, 0.0f) } },
    { "42", "music", "音乐律动", EMOTION_GROUP_AGENT, 220, true, 1.0f, 1800, 3200, 3,
      { A(ANIM_TARGET_BODY, ANIM_PROP_Y, ANIM_PULSE, 5.0f, 500.0f, 0.0f),
        A(ANIM_TARGET_EYES, ANIM_PROP_SCALE, ANIM_PULSE, 0.16f, 500.0f, 0.0f),
        A(ANIM_TARGET_BODY, ANIM_PROP_ROTATE, ANIM_SINE, 3.0f, 1000.0f, 0.0f) } },
};

static const emotion_def_t *k_fallback = &k_emotions[2]; /* 02 idle */

const emotion_def_t *emotion_catalog_by_id(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_emotions) / sizeof(k_emotions[0]); ++i) {
        if (strcmp(k_emotions[i].id, id) == 0) {
            return &k_emotions[i];
        }
    }
    return NULL;
}

const emotion_def_t *emotion_catalog_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_emotions) / sizeof(k_emotions[0]); ++i) {
        if (strcmp(k_emotions[i].name, name) == 0) {
            return &k_emotions[i];
        }
    }
    return NULL;
}

const emotion_def_t *emotion_catalog_fallback(void)
{
    return k_fallback;
}

size_t emotion_catalog_count(void)
{
    return sizeof(k_emotions) / sizeof(k_emotions[0]);
}

const emotion_def_t *emotion_catalog_at(size_t index)
{
    if (index >= emotion_catalog_count()) {
        return NULL;
    }
    return &k_emotions[index];
}
