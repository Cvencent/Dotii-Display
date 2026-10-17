#include "emotion_mood_store.h"

#include "nvs.h"

#define MOOD_NVS_NAMESPACE "dotii_mood"

static uint8_t encode_weight(float value)
{
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static float decode_weight(uint8_t value)
{
    return (float)value / 255.0f;
}

bool emotion_mood_store_load(emotion_mood_t *mood)
{
    if (mood == NULL) return false;
    nvs_handle_t handle;
    if (nvs_open(MOOD_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t positive = 0;
    uint8_t correction = 0;
    const bool have_positive =
        nvs_get_u8(handle, "positive", &positive) == ESP_OK;
    const bool have_correction =
        nvs_get_u8(handle, "correct", &correction) == ESP_OK;
    nvs_close(handle);
    if (!have_positive && !have_correction) return false;

    emotion_mood_snapshot_t snapshot;
    emotion_mood_export(mood, &snapshot);
    if (have_positive) snapshot.positive_bias = decode_weight(positive) * 0.65f;
    if (have_correction) snapshot.correction_bias = decode_weight(correction) * 0.65f;
    return emotion_mood_import(mood, &snapshot);
}

bool emotion_mood_store_save(const emotion_mood_t *mood)
{
    if (mood == NULL) return false;
    nvs_handle_t handle;
    if (nvs_open(MOOD_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    emotion_mood_snapshot_t state;
    emotion_mood_export(mood, &state);
    const uint8_t positive = encode_weight(state.positive_bias / 0.65f);
    const uint8_t correction = encode_weight(state.correction_bias / 0.65f);
    bool ok = nvs_set_u8(handle, "positive", positive) == ESP_OK &&
              nvs_set_u8(handle, "correct", correction) == ESP_OK &&
              nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}
