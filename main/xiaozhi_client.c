#include "xiaozhi_client.h"
#include "dotii_data.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "device_config.h"
#include "qqmusic_client.h"


/* The message/emotion mapping above is plain C and is covered by the host
   tests in test_apps/emotion. Everything that needs the transport, the
   codec or the Wi-Fi stack stays behind this switch. */
#if CONFIG_XIAOZHI_ENABLE
#include "device_config.h"

#include "bsp/esp-bsp.h"
#include "connectivity.h"
#include "device_identity.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
/* MultiNet recognises a command word list injected at runtime, which is how an
   arbitrary user-defined wake word is supported. */
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"
#include "esp_codec_dev.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_websocket_client.h"
#include "wake_reply_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif

#define XIAOZHI_NAMESPACE "xiaozhi"
#define XIAOZHI_DEFAULT_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#define XIAOZHI_FRAME_MS 60
#define XIAOZHI_INPUT_RATE 16000
#define XIAOZHI_CODEC_RATE 24000
#define XIAOZHI_DEFAULT_VOLUME 80
#define XIAOZHI_INPUT_GAIN 2
#define XIAOZHI_MIC_GAIN_DB 37.0f
#define XIAOZHI_MIC_TASK_STACK 24576
#define XIAOZHI_PLAYBACK_TASK_STACK 24576

static const char *TAG = "xiaozhi";

static bool xiaozhi_start_music(xiaozhi_client_t *client);
/* Declared early: configure() may need to tear the recogniser down when the
   wake-word list changes. */
#if CONFIG_XIAOZHI_ENABLE
static void wake_word_stop(xiaozhi_client_t *client);
#else
static void wake_word_stop(xiaozhi_client_t *client)
{
    (void)client;
}
#endif
static bool send_listen_mode(xiaozhi_client_t *client, bool start, const char *mode);
static void send_text(xiaozhi_client_t *client, const char *text);

static bool xiaozhi_mcp_play_mid(xiaozhi_client_t *client, const char *mid,
                                 const char *title);
static bool audio_init(xiaozhi_client_t *client);
static void wake_task(void *argument);
static void ensure_conversation_rate(xiaozhi_client_t *client);

#if !CONFIG_XIAOZHI_ENABLE
/* Host-test build: transport features are stubbed so the JSON mapping code
   stays covered. */
static bool send_listen_mode(xiaozhi_client_t *client, bool start, const char *mode)
{
    (void)client; (void)start; (void)mode;
    return false;
}
static void send_text(xiaozhi_client_t *client, const char *text)
{
    (void)client; (void)text;
}
static bool xiaozhi_mcp_play_mid(xiaozhi_client_t *client, const char *mid,
                                 const char *title)
{
    (void)client; (void)mid; (void)title;
    return false;
}
static void ensure_conversation_rate(xiaozhi_client_t *client)
{
    (void)client;
}
bool xiaozhi_client_play_url(xiaozhi_client_t *client, const char *url, const char *title)
{
    (void)client; (void)url; (void)title;
    return false;
}
bool qqmusic_search(const char *bridge_url, const char *bridge_token,
                    const char *query, qqmusic_song_t *out,
                    int max, int *out_count)
{
    (void)bridge_url; (void)bridge_token; (void)query; (void)out; (void)max;
    if (out_count != NULL) *out_count = 0;
    return false;
}
bool qqmusic_resolve_mid(const char *bridge_url, const char *bridge_token,
                         const char *mid, char *out_url, size_t url_size)
{
    (void)bridge_url; (void)bridge_token; (void)mid; (void)out_url; (void)url_size;
    return false;
}
bool qqmusic_resolve_song(const char *bridge_url, const char *bridge_token,
                          const char *cookie, const char *query,
                          char *out_title, size_t title_size,
                          char *out_url, size_t url_size)
{
    (void)bridge_url; (void)bridge_token; (void)cookie; (void)query;
    (void)out_title; (void)title_size; (void)out_url; (void)url_size;
    return false;
}
bool qqmusic_search_songs(const char *bridge_url, const char *bridge_token,
                          const char *query, int limit,
                          char *out_text, size_t out_size)
{
    (void)bridge_url; (void)bridge_token; (void)query; (void)limit;
    (void)out_text; (void)out_size;
    return false;
}
#endif
static void copy_bounded(char *dst, size_t size, const char *src)
{
    if (dst == NULL || size == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < size && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}
static bool starts_with(const char *value, const char *prefix)
{
    return value != NULL && prefix != NULL && strncmp(value, prefix, strlen(prefix)) == 0;
}

void xiaozhi_pcm_amplify(int16_t *samples, size_t sample_count, uint16_t multiplier)
{
    if (samples == NULL || sample_count == 0) return;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t scaled = (int32_t)samples[i] * (int32_t)multiplier;
        if (scaled > INT16_MAX) scaled = INT16_MAX;
        if (scaled < -INT16_MAX) scaled = -INT16_MAX;
        samples[i] = (int16_t)scaled;
    }
}

void xiaozhi_pcm_fade_out(int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) return;
    const int32_t divisor = sample_count > 1 ? (int32_t)sample_count - 1 : 1;
    for (size_t i = 0; i < sample_count; ++i) {
        const int32_t remaining = (int32_t)sample_count - (int32_t)i - 1;
        samples[i] = (int16_t)(((int32_t)samples[i] * remaining) / divisor);
    }
}

void xiaozhi_pcm_downmix_stereo(int16_t *samples, size_t frame_count)
{
    if (samples == NULL || frame_count == 0) return;
    for (size_t i = 0; i < frame_count; ++i) {
        const int32_t mixed = (int32_t)samples[i * 2] + samples[i * 2 + 1];
        samples[i] = (int16_t)(mixed / 2);
    }
}

void xiaozhi_client_notify_task_completed(xiaozhi_client_t *client, bool failed)
{
    if (client == NULL) return;
    atomic_store_explicit(&client->task_chime_failed, failed, memory_order_release);
    atomic_store_explicit(&client->task_chime_pending, true, memory_order_release);
}

void xiaozhi_client_notify_wake_word(xiaozhi_client_t *client)
{
    if (client != NULL) {
        atomic_store_explicit(&client->wake_event_pending, true, memory_order_release);
        atomic_store_explicit(&client->wake_reply_pending, true, memory_order_release);
    }
}

bool xiaozhi_client_take_wake_event(xiaozhi_client_t *client)
{
    return client != NULL &&
           atomic_exchange_explicit(&client->wake_event_pending, false, memory_order_acq_rel);
}

bool xiaozhi_client_take_wake_reply(xiaozhi_client_t *client)
{
    return client != NULL &&
           atomic_exchange_explicit(&client->wake_reply_pending, false, memory_order_acq_rel);
}

bool xiaozhi_client_take_wake_resume(xiaozhi_client_t *client)
{
    if (client == NULL) return false;
    const bool pending = client->wake_resume_pending;
    client->wake_resume_pending = false;
    return pending;
}

bool xiaozhi_client_wake_detection_allowed(const xiaozhi_client_t *client)
{
    if (client == NULL || !client->running || !client->wake_ready) return false;
    if (client->connected && (client->state == XIAOZHI_STATE_DISABLED ||
                             client->state == XIAOZHI_STATE_ACTIVATING)) return false;
    if (!client->connected && client->state != XIAOZHI_STATE_CONNECTING &&
        client->state != XIAOZHI_STATE_ERROR) return false;
    /* Music remains interruptible. The detector switches to WakeNet while a
       song is active so the expensive MultiNet pass cannot starve the decoder
       and I2S DMA. */
    return !client->listening &&
           client->state != XIAOZHI_STATE_SPEAKING &&
           client->state != XIAOZHI_STATE_THINKING;
}

/* Remember the wake words in NVS. The management snapshot arrives a few
   seconds after boot; without this the first recogniser start would find an
   empty list and fall back to WakeNet until the next session restart. */
static void persist_wake_words(const char *list)
{
    if (list == NULL) return;
    nvs_handle_t handle;
    if (nvs_open(XIAOZHI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_str(handle, "wake_words", list);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static void load_wake_words(char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) return;
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(XIAOZHI_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = capacity;
    (void)nvs_get_str(handle, "wake_words", out, &length);
    nvs_close(handle);
}

static void persist_volume(const xiaozhi_client_t *client)
{
    if (client == NULL) return;
    nvs_handle_t handle;
    if (nvs_open(XIAOZHI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_u8(handle, "volume", client->output_volume);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

/* The management center stores whatever address the user paired against, and
   a websocket address is a common choice. The official activation flow always
   needs the OTA endpoint, so a ws(s):// address is mapped back onto its OTA
   counterpart instead of being treated as an already-paired session. */
static bool derive_ota_url(const char *server, char *out, size_t capacity)
{
    if (server == NULL || out == NULL || capacity == 0) return false;
    if (starts_with(server, "http://") || starts_with(server, "https://")) {
        copy_bounded(out, capacity, server);
        return out[0] != '\0';
    }

    const char *rest;
    const char *scheme;
    if (starts_with(server, "wss://")) {
        rest = server + 6;
        scheme = "https://";
    } else if (starts_with(server, "ws://")) {
        rest = server + 5;
        scheme = "http://";
    } else {
        return false;
    }
    if (rest[0] == '\0') return false;

    char base[192];
    copy_bounded(base, sizeof(base), rest);
    size_t length = strlen(base);
    while (length > 0 && base[length - 1] == '/') base[--length] = '\0';
    char *suffix = strrchr(base, '/');
    if (suffix != NULL && strcmp(suffix, "/v1") == 0) {
        *suffix = '\0';
    }
    if (base[0] == '\0') return false;

    /* A bare host gets the official path; anything else keeps its own prefix. */
    if (strchr(base, '/') == NULL) {
        return snprintf(out, capacity, "%s%s/xiaozhi/ota/", scheme, base) > 0;
    }
    return snprintf(out, capacity, "%s%s/ota/", scheme, base) > 0;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

typedef struct {
    const char *alias;
    const char *id;
} emotion_alias_t;

/* English/label synonyms an LLM is likely to send, mapped onto the Dotii
   48-expression vocabulary (dotii_data.c). Values are stable two-digit ids. */
static const emotion_alias_t k_emotion_aliases[] = {
    { "sleeping", "02" },
    { "sleep", "02" },
    { "waking", "01" },
    { "idle", "00" },
    { "neutral", "00" },
    { "calm", "08" },
    { "curious", "09" },
    { "spacing_out", "08" },
    { "booting", "45" },
    { "dormant", "02" },
    { "shake_awake", "01" },
    { "happy", "10" },
    { "joy", "11" },
    { "excited", "12" },
    { "smile", "10" },
    { "laugh", "11" },
    { "puzzled", "18" },
    { "confused", "19" },
    { "sad", "21" },
    { "down", "21" },
    { "unhappy", "21" },
    { "surprised", "20" },
    { "surprise", "20" },
    { "shy", "14" },
    { "embarrassed", "14" },
    { "tired", "24" },
    { "sleepy", "24" },
    { "exhausted", "24" },
    { "bored", "25" },
    { "proud", "15" },
    { "satisfied", "16" },
    { "content", "16" },
    { "relieved", "17" },
    { "love", "13" },
    { "angry", "23" },
    { "mad", "23" },
    { "thinking", "04" },
    { "receiving", "32" },
    { "busy", "30" },
    { "working", "30" },
    { "coding", "30" },
    { "done", "33" },
    { "success", "33" },
    { "completed", "33" },
    { "error", "34" },
    { "failed", "34" },
    { "listening", "03" },
    { "loading", "39" },
    { "connecting", "39" },
    { "reconnecting", "39" },
    { "recalling", "29" },
    { "reading", "29" },
    { "refusing", "47" },
    { "refused", "47" },
    { "replying", "05" },
    { "speaking", "05" },
    { "searching", "28" },
    { "tool_use", "31" },
    { "powering_off", "02" },
    { "stopping", "02" },
    { "music", "36" },
    { "playing_music", "36" },
    { "notification", "37" },
    { "offline", "38" },
    { "low_battery", "40" },
    { "charging", "41" },
    { "muted", "42" },
    { "volume", "43" },
    { "dnd", "44" },
    { "update", "45" },
    { "locked", "46" },
    { "warning", "47" },
    { "greeting", "26" },
    { "recognizing", "27" },
    { "waiting", "32" },
    { "interrupted", "35" },
};

static const char *normalize_emotion(const char *name)
{
    if (name == NULL || name[0] == '\0') return NULL;
    for (size_t i = 0; i < sizeof(k_emotion_aliases) / sizeof(k_emotion_aliases[0]); ++i) {
        if (strcmp(k_emotion_aliases[i].alias, name) == 0) return k_emotion_aliases[i].id;
    }
    return name;
}

static bool catalog_has_id(const char *id)
{
    return id != NULL && dotii_catalog_by_id(id) != NULL;
}

static bool contains_any(const char *text, const char *const *needles, size_t count)
{
    if (text == NULL) return false;
    for (size_t i = 0; i < count; ++i) {
        if (needles[i] != NULL && needles[i][0] != '\0' &&
            strstr(text, needles[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static const char *emotion_from_text(const char *text)
{
    static const char *const error_words[] = {
        "失败", "出错", "错误", "无法", "不能", "异常"
    };
    static const char *const refuse_words[] = {
        "不能帮你", "无法帮", "拒绝", "不允许", "受限"
    };
    static const char *const done_words[] = {
        "完成", "已完成", "好了", "成功", "搞定"
    };
    static const char *const search_words[] = {
        "检索", "搜索", "查一下", "查找", "联网"
    };
    static const char *const happy_words[] = {
        "开心", "高兴", "太好了", "哈哈", "喜欢", "棒", "谢谢"
    };
    static const char *const down_words[] = {
        "难过", "伤心", "抱歉", "遗憾", "糟糕", "不开心"
    };
    static const char *const tired_words[] = {
        "累", "疲劳", "困", "休息", "疲惫"
    };
    static const char *const confused_words[] = {
        "不明白", "听不懂", "困惑", "不懂", "为什么", "疑惑"
    };
    static const char *const surprised_words[] = {
        "惊讶", "吃惊", "真的吗", "居然", "没想到"
    };
    static const char *const angry_words[] = {
        "生气", "愤怒", "讨厌", "烦人"
    };

    if (contains_any(text, error_words, sizeof(error_words) / sizeof(error_words[0]))) return "34";
    if (contains_any(text, refuse_words, sizeof(refuse_words) / sizeof(refuse_words[0]))) return "47";
    if (contains_any(text, done_words, sizeof(done_words) / sizeof(done_words[0]))) return "33";
    if (contains_any(text, search_words, sizeof(search_words) / sizeof(search_words[0]))) return "28";
    if (contains_any(text, happy_words, sizeof(happy_words) / sizeof(happy_words[0]))) return "10";
    if (contains_any(text, down_words, sizeof(down_words) / sizeof(down_words[0]))) return "21";
    if (contains_any(text, tired_words, sizeof(tired_words) / sizeof(tired_words[0]))) return "24";
    if (contains_any(text, confused_words, sizeof(confused_words) / sizeof(confused_words[0]))) return "19";
    if (contains_any(text, surprised_words, sizeof(surprised_words) / sizeof(surprised_words[0]))) return "20";
    if (contains_any(text, angry_words, sizeof(angry_words) / sizeof(angry_words[0]))) return "23";
    return NULL;
}

const char *xiaozhi_client_emotion_policy(xiaozhi_session_state_t state,
                                          const char *server_emotion,
                                          const char *text)
{
    switch (state) {
    case XIAOZHI_STATE_ACTIVATING: return "07";
    case XIAOZHI_STATE_CONNECTING: return "39";
    case XIAOZHI_STATE_LISTENING: return "03";
    case XIAOZHI_STATE_MUSIC: return "36";
    case XIAOZHI_STATE_ERROR: return "34";
    case XIAOZHI_STATE_THINKING: {
        const char *searching = contains_any(text, (const char *const[]){ "检索", "搜索", "查一下", "查找", "联网" }, 5)
            ? "28" : NULL;
        return searching != NULL ? searching : "04";
    }
    case XIAOZHI_STATE_SPEAKING: {
        const char *normalized = normalize_emotion(server_emotion);
        if (catalog_has_id(normalized)) return normalized;
        const char *emotion = emotion_from_text(text);
        return emotion != NULL ? emotion : "05";
    }
    default: {
        const char *normalized = normalize_emotion(server_emotion);
        return catalog_has_id(normalized) ? normalized : "00";
    }
    }
}
xiaozhi_music_format_t xiaozhi_music_format_from_bytes(const uint8_t *data, size_t size)
{
    if (data == NULL || size < 4) return XIAOZHI_MUSIC_FORMAT_UNKNOWN;
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WAVE", 4) == 0) {
        return XIAOZHI_MUSIC_FORMAT_WAV;
    }
    if (memcmp(data, "OggS", 4) == 0) return XIAOZHI_MUSIC_FORMAT_OGG;
    if (size >= 8 && memcmp(data + 4, "ftyp", 4) == 0) return XIAOZHI_MUSIC_FORMAT_M4A;
    if (memcmp(data, "ID3", 3) == 0) return XIAOZHI_MUSIC_FORMAT_MP3;
    if (data[0] == 0xFFu && (data[1] & 0xE0u) == 0xE0u) return XIAOZHI_MUSIC_FORMAT_MP3;
    return XIAOZHI_MUSIC_FORMAT_UNKNOWN;
}

xiaozhi_music_format_t xiaozhi_music_format_from_url(const char *url)
{
    if (url == NULL) return XIAOZHI_MUSIC_FORMAT_UNKNOWN;
    const char *end = strchr(url, '?');
    if (end == NULL) end = url + strlen(url);
    const char *dot = NULL;
    for (const char *cursor = url; cursor < end; ++cursor) {
        if (*cursor == '.') dot = cursor;
    }
    if (dot == NULL) return XIAOZHI_MUSIC_FORMAT_UNKNOWN;
    if (strncasecmp(dot, ".mp3", 4) == 0) return XIAOZHI_MUSIC_FORMAT_MP3;
    if (strncasecmp(dot, ".wav", 4) == 0) return XIAOZHI_MUSIC_FORMAT_WAV;
    if (strncasecmp(dot, ".m4a", 4) == 0) return XIAOZHI_MUSIC_FORMAT_M4A;
    if (strncasecmp(dot, ".ogg", 4) == 0 || strncasecmp(dot, ".opus", 5) == 0) {
        return XIAOZHI_MUSIC_FORMAT_OGG;
    }
    return XIAOZHI_MUSIC_FORMAT_UNKNOWN;
}
static void load_persisted(xiaozhi_client_t *client)
{
    nvs_handle_t handle;
    if (nvs_open(XIAOZHI_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = sizeof(client->websocket_url);
    (void)nvs_get_str(handle, "ws_url", client->websocket_url, &length);
    length = sizeof(client->token);
    (void)nvs_get_str(handle, "token", client->token, &length);
    length = sizeof(client->activation_code);
    (void)nvs_get_str(handle, "activation_code", client->activation_code, &length);
    length = sizeof(client->activation_message);
    (void)nvs_get_str(handle, "activation_msg", client->activation_message, &length);
    uint8_t paired = 0;
    if (nvs_get_u8(handle, "paired", &paired) != ESP_OK) paired = 0;
    uint8_t volume = XIAOZHI_DEFAULT_VOLUME;
    if (nvs_get_u8(handle, "volume", &volume) == ESP_OK && volume <= 100) {
        client->output_volume = volume;
    }
    nvs_close(handle);
    /* A websocket address alone does not mean the device is paired: older
       firmware stored the endpoint before the user ever bound a code. Only an
       explicit flag written after a bound OTA reply counts. Without it the
       activation task asks the server again and puts the code back on screen. */
    client->activated = paired != 0 && client->websocket_url[0] != '\0';
}

static void persist_activation(xiaozhi_client_t *client)
{
    nvs_handle_t handle;
    if (nvs_open(XIAOZHI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_str(handle, "ws_url", client->websocket_url);
    (void)nvs_set_str(handle, "token", client->token);
    (void)nvs_set_str(handle, "activation_code", client->activation_code);
    (void)nvs_set_str(handle, "activation_msg", client->activation_message);
    (void)nvs_set_u8(handle, "paired", client->activated ? 1 : 0);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

void xiaozhi_client_init(xiaozhi_client_t *client)
{
    if (client == NULL) return;
    memset(client, 0, sizeof(*client));
    atomic_init(&client->wake_event_pending, false);
    atomic_init(&client->wake_reply_pending, false);
    atomic_init(&client->wake_reply_playing, false);
    atomic_init(&client->music_task_started, false);
    atomic_init(&client->music_fade_requested, false);
    atomic_init(&client->task_chime_pending, false);
    atomic_init(&client->task_chime_failed, false);
    client->state = XIAOZHI_STATE_DISABLED;
    client->server_sample_rate = 24000;
    client->server_frame_duration = XIAOZHI_FRAME_MS;
    client->output_volume = XIAOZHI_DEFAULT_VOLUME;
    load_persisted(client);
}

void xiaozhi_client_configure(xiaozhi_client_t *client, const xiaozhi_config_t *config)
{
    if (client == NULL || config == NULL) return;
    const bool server_changed = client->config.server_url[0] != '\0' &&
                                strcmp(client->config.server_url, config->server_url) != 0;
    /* The wake-word list is injected into the recogniser at start, so a change
       has to tear the model down rather than just update the config. */
    const bool wake_list_changed =
        strcmp(client->config.wake_words, config->wake_words) != 0;
    client->config = *config;
    if (wake_list_changed) {
        if (config->wake_words[0] != '\0') {
            persist_wake_words(config->wake_words);
        }
        wake_word_stop(client);
    }
    if (server_changed) {
        client->websocket_url[0] = '\0';
        client->token[0] = '\0';
        client->activation_code[0] = '\0';
        client->activation_message[0] = '\0';
        client->activated = false;
    }
    client->state = XIAOZHI_STATE_IDLE;
}

xiaozhi_session_state_t xiaozhi_client_state(const xiaozhi_client_t *client)
{
    return client == NULL ? XIAOZHI_STATE_DISABLED : client->state;
}

const char *xiaozhi_client_state_name(xiaozhi_session_state_t state)
{
    switch (state) {
    case XIAOZHI_STATE_IDLE: return "待机";
    case XIAOZHI_STATE_ACTIVATING: return "等待配对";
    case XIAOZHI_STATE_CONNECTING: return "连接中";
    case XIAOZHI_STATE_LISTENING: return "聆听中";
    case XIAOZHI_STATE_THINKING: return "思考中";
    case XIAOZHI_STATE_SPEAKING: return "说话中";
    case XIAOZHI_STATE_MUSIC: return "播放音乐";
    case XIAOZHI_STATE_ERROR: return "连接异常";
    default: return "未启用";
    }
}

bool xiaozhi_client_activation_required(const xiaozhi_client_t *client)
{
    return client != NULL && !client->activated;
}

const char *xiaozhi_client_activation_code(const xiaozhi_client_t *client)
{
    return client == NULL || client->activation_code[0] == '\0' ? NULL : client->activation_code;
}

const char *xiaozhi_client_activation_message(const xiaozhi_client_t *client)
{
    return client == NULL || client->activation_message[0] == '\0' ? NULL : client->activation_message;
}

const char *xiaozhi_client_websocket_url(const xiaozhi_client_t *client)
{
    return client == NULL || client->websocket_url[0] == '\0' ? NULL : client->websocket_url;
}

const char *xiaozhi_client_last_emotion(const xiaozhi_client_t *client)
{
    if (client == NULL || client->last_emotion[0] == '\0') return NULL;
    if (client->emotion_until_ms != 0 && (int32_t)(now_ms() - client->emotion_until_ms) >= 0) return NULL;
    return client->last_emotion;
}

const char *xiaozhi_client_pending_audio_url(const xiaozhi_client_t *client)
{
    return client == NULL || client->pending_audio_url[0] == '\0' ?
        NULL : client->pending_audio_url;
}

const char *xiaozhi_client_state_emotion(xiaozhi_session_state_t state)
{
    switch (state) {
    case XIAOZHI_STATE_ACTIVATING: return "07";
    case XIAOZHI_STATE_CONNECTING: return "39";
    case XIAOZHI_STATE_LISTENING: return "03";
    case XIAOZHI_STATE_THINKING: return "04";
    case XIAOZHI_STATE_SPEAKING: return "05";
    case XIAOZHI_STATE_MUSIC: return "36";
    case XIAOZHI_STATE_ERROR: return "34";
    default: return NULL;
    }
}

uint8_t xiaozhi_client_get_volume(const xiaozhi_client_t *client)
{
    return client == NULL ? XIAOZHI_DEFAULT_VOLUME : client->output_volume;
}

uint8_t xiaozhi_client_set_volume(xiaozhi_client_t *client, int volume, bool persist)
{
    if (client == NULL) return XIAOZHI_DEFAULT_VOLUME;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    client->output_volume = (uint8_t)volume;
#if CONFIG_XIAOZHI_ENABLE
    if ((client->codec_ready || client->speaker_ready) && client->speaker != NULL) {
        (void)esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)client->speaker,
                                        client->output_volume);
    }
#endif
    if (persist) persist_volume(client);
    return client->output_volume;
}

static void set_emotion(xiaozhi_client_t *client, const char *name)
{
    const char *normalized = normalize_emotion(name);
    if (normalized == NULL) return;
    copy_bounded(client->last_emotion, sizeof(client->last_emotion), normalized);
    client->emotion_until_ms = now_ms() + 5000;
    client->server_emotion_revision++;
    ESP_LOGI(TAG, "emotion -> %s (raw %s)", normalized, name);
}

static bool map_server_state(const char *value, xiaozhi_session_state_t *out)
{
    if (value == NULL) return false;
    if (strcmp(value, "listening") == 0) *out = XIAOZHI_STATE_LISTENING;
    else if (strcmp(value, "thinking") == 0) *out = XIAOZHI_STATE_THINKING;
    else if (strcmp(value, "speaking") == 0) *out = XIAOZHI_STATE_SPEAKING;
    else if (strcmp(value, "music") == 0) *out = XIAOZHI_STATE_MUSIC;
    else if (strcmp(value, "idle") == 0) *out = XIAOZHI_STATE_IDLE;
    else if (strcmp(value, "connecting") == 0) *out = XIAOZHI_STATE_CONNECTING;
    else if (strcmp(value, "error") == 0) *out = XIAOZHI_STATE_ERROR;
    else return false;
    return true;
}

/* ---- MCP ---------------------------------------------------------------
   The Xiaozhi service speaks MCP (JSON-RPC 2.0) over the same websocket.
   We advertise a single play_music tool so the model decides *when* the user
   asked for music; the device then resolves the song with the user's own QQ
   Music session and starts the local music pipeline. */
/* Replies go back inside the same envelope the service used, so the payload
   stays valid JSON-RPC while the transport keeps its type/session routing. */
static bool s_mcp_reply_enveloped;

static void mcp_reply(xiaozhi_client_t *client, cJSON *root, cJSON *result)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) return;
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id != NULL) {
        cJSON *copy = cJSON_Duplicate(id, 1);
        if (copy != NULL) cJSON_AddItemToObject(payload, "id", copy);
    }
    if (result != NULL) cJSON_AddItemToObject(payload, "result", result);

    /* Mirror the request shape: the service expects the payload nested when it
       sent an envelope, but a bare JSON-RPC peer expects a flat reply. */
    cJSON *out = payload;
    if (s_mcp_reply_enveloped) {
        cJSON *envelope = cJSON_CreateObject();
        if (envelope == NULL) {
            cJSON_Delete(payload);
            return;
        }
        if (client->session_id[0] != '\0') {
            cJSON_AddStringToObject(envelope, "session_id", client->session_id);
        }
        cJSON_AddStringToObject(envelope, "type", "mcp");
        cJSON_AddItemToObject(envelope, "payload", payload);
        out = envelope;
    }

    char *text = cJSON_PrintUnformatted(out);
    if (text != NULL) {
        ESP_LOGI(TAG, "mcp reply -> %.200s", text);
        send_text(client, text);
        cJSON_free(text);
    }
    cJSON_Delete(out);
}

#if CONFIG_XIAOZHI_ENABLE
static cJSON *make_mcp_tool(const char *name, const char *description,
                            cJSON *properties, const char *required_name)
{
    cJSON *tool = cJSON_CreateObject();
    if (tool == NULL) return NULL;
    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "description", description);
    cJSON *schema = cJSON_AddObjectToObject(tool, "inputSchema");
    cJSON_AddStringToObject(schema, "type", "object");
    if (properties != NULL) cJSON_AddItemToObject(schema, "properties", properties);
    cJSON *required = cJSON_AddArrayToObject(schema, "required");
    if (required_name != NULL) cJSON_AddItemToArray(required, cJSON_CreateString(required_name));
    return tool;
}

static cJSON *string_property(const char *name, const char *description)
{
    cJSON *props = cJSON_CreateObject();
    cJSON *item = cJSON_AddObjectToObject(props, name);
    cJSON_AddStringToObject(item, "type", "string");
    cJSON_AddStringToObject(item, "description", description);
    return props;
}

static cJSON *string_and_integer_properties(const char *sname, const char *sdesc,
                                            const char *iname, const char *idesc)
{
    cJSON *props = cJSON_CreateObject();
    cJSON *sitem = cJSON_AddObjectToObject(props, sname);
    cJSON_AddStringToObject(sitem, "type", "string");
    cJSON_AddStringToObject(sitem, "description", sdesc);
    cJSON *iitem = cJSON_AddObjectToObject(props, iname);
    cJSON_AddStringToObject(iitem, "type", "integer");
    cJSON_AddStringToObject(iitem, "description", idesc);
    return props;
}

static void mcp_tools_list(xiaozhi_client_t *client, cJSON *root)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(result, "tools");
    cJSON *search = make_mcp_tool("search_music",
        "Search QQ Music for a song the user asked for. Pass the title and the "
        "artist together when the user named them (for example "
        "\"陈奕迅 单车\"). If the title and artist both match the best hit the "
        "device starts playing immediately and you must NOT ask the user to "
        "choose. Only when the result text lists numbered candidates (an "
        "ambiguous request) should you read them out and ask which one. "
        "Never invent a song or claim playback before this tool returns.",
        string_property("query", "Song title and, when known, the artist"), "query");
    cJSON *play = make_mcp_tool("play_music",
        "Play one candidate from the most recent search_music result. "
        "song_index is the 1-based number the user confirmed. "
        "This tool will refuse to play without a valid song_index, so never "
        "call it before search_music and never guess a song.",
        string_and_integer_properties("query", "The same query used for search_music",
                                      "song_index", "1-based candidate number the user chose"),
        "song_index");
    cJSON *music = make_mcp_tool("music",
        "Control local music playback. action=stop stops the current song. "
        "action=play requires song_index from a confirmed search_music result.",
        string_and_integer_properties("action", "play or stop",
                                      "song_index", "1-based confirmed candidate number"),
        "action");
    if (tools != NULL) {
        if (search != NULL) cJSON_AddItemToArray(tools, search);
        if (play != NULL) cJSON_AddItemToArray(tools, play);
        if (music != NULL) cJSON_AddItemToArray(tools, music);
    }
    mcp_reply(client, root, result);
}

static void mcp_reply_tool_text(xiaozhi_client_t *client, cJSON *root,
                                const char *text, bool is_error)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    if (content != NULL && item != NULL) cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", is_error);
    mcp_reply(client, root, result);
}

/* The confirmed-search cache is device-side on purpose: the cloud model cannot
   play anything unless it first obtained a real candidate list from this
   device and the user confirmed one of those numbered entries. */
#define MCP_MUSIC_CACHE_MS 300000u
typedef struct {
    bool valid;
    uint32_t stamp_ms;
    char query[160];
    qqmusic_song_t songs[QQMUSIC_MAX_RESULTS];
    int count;
} mcp_music_cache_t;

EXT_RAM_BSS_ATTR static mcp_music_cache_t s_music_cache;

typedef struct {
    xiaozhi_client_t *client;
    char query[192];
    char id_json[64];
} mcp_search_job_t;

/* Sends a tools/call reply using a stored raw JSON-RPC id, from the worker
   task rather than the websocket callback. */
static void mcp_reply_by_id(xiaozhi_client_t *client, const char *id_json,
                            const char *text, bool is_error)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) return;
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    if (id_json != NULL && id_json[0] != '\0') {
        cJSON *id = cJSON_Parse(id_json);
        if (id != NULL) cJSON_AddItemToObject(payload, "id", id);
    }
    cJSON *result = cJSON_AddObjectToObject(payload, "result");
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text != NULL ? text : "");
    if (content != NULL && item != NULL) cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", is_error);

    cJSON *envelope = cJSON_CreateObject();
    if (envelope == NULL) {
        cJSON_Delete(payload);
        return;
    }
    if (client->session_id[0] != '\0') {
        cJSON_AddStringToObject(envelope, "session_id", client->session_id);
    }
    cJSON_AddStringToObject(envelope, "type", "mcp");
    cJSON_AddItemToObject(envelope, "payload", payload);
    char *text_out = cJSON_PrintUnformatted(envelope);
    if (text_out != NULL) {
        ESP_LOGI(TAG, "mcp async reply -> %.200s", text_out);
        send_text(client, text_out);
        cJSON_free(text_out);
    }
    cJSON_Delete(envelope);
}

static void mcp_search_task(void *argument);
static void music_search_dispatch(xiaozhi_client_t *client, const char *query)
{
    xiaozhi_client_cancel_pending_music(client);
    ESP_LOGI(TAG, "music search dispatch: %s", query);
    mcp_search_job_t *job = heap_caps_calloc(1, sizeof(*job),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job == NULL) return;
    job->client = client;
    copy_bounded(job->query, sizeof(job->query), query);
    job->id_json[0] = '\0';
    if (xTaskCreatePinnedToCoreWithCaps(mcp_search_task, "mcp_search", 12288, job, 3,
                                        NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        heap_caps_free(job);
    }
}

static void mcp_search_task(void *argument)
{
    mcp_search_job_t *job = (mcp_search_job_t *)argument;
    const device_config_values_t *cfg = device_config_get();
    qqmusic_song_t songs[QQMUSIC_MAX_RESULTS] = {0};
    int count = 0;
    /* The bridge owns the QQ Music session; the device just asks it. */
    if (!qqmusic_search(cfg->bridge_url, cfg->bridge_token, job->query,
                        songs, 3, &count)) {
        ESP_LOGW(TAG, "mcp search_music: no results from bridge");
        /* Drop the previous candidate list: it belongs to an earlier query and
           a later play_music must not be able to select from it. */
        s_music_cache.valid = false;
        mcp_reply_by_id(job->client, job->id_json,
            "No QQ Music results. Check the login in the management centre.", true);
        heap_caps_free(job);
        vTaskDelete(NULL);
        return;
    }

    s_music_cache.valid = true;
    s_music_cache.stamp_ms = now_ms();
    copy_bounded(s_music_cache.query, sizeof(s_music_cache.query), job->query);
    memcpy(s_music_cache.songs, songs, sizeof(songs));
    s_music_cache.count = count;

    /* If the user already named both the song and the artist, and the top hit
       matches both, the request is unambiguous: start it instead of making the
       assistant read a list back. A bare title ("单车") stays ambiguous because
       many covers share it. */
    const bool title_named = songs[0].title[0] != '\0' &&
                             strstr(job->query, songs[0].title) != NULL;
    const bool artist_named = songs[0].artist[0] != '\0' &&
                              strstr(job->query, songs[0].artist) != NULL;
    if (title_named && artist_named) {
        char url[512] = {0};
        if (qqmusic_resolve_mid(cfg->bridge_url, cfg->bridge_token,
                                songs[0].mid, url, sizeof(url))) {
            char label[320];
            snprintf(label, sizeof(label), "%s - %s", songs[0].title, songs[0].artist);
            ESP_LOGI(TAG, "mcp search_music: confident match, queuing %s", label);
            /* Queue rather than start: the assistant is about to speak its
               confirmation, and starting now would be cut off by that TTS. */
            xiaozhi_client_queue_music(job->client, url, label);
            s_music_cache.valid = false;
            char reply[256];
            snprintf(reply, sizeof(reply),
                     "已找到《%s》，马上为你播放。请用一句话确认，不要说列表。",
                     songs[0].title);
            mcp_reply_by_id(job->client, job->id_json, reply, false);
            heap_caps_free(job);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "confident match but stream resolve failed");
    }

    char text[1024] = {0};
    size_t used = 0;
    for (int i = 0; i < count && used + 1 < sizeof(text); ++i) {
        char line[320];
        int written;
        if (songs[i].album[0] != '\0') {
            written = snprintf(line, sizeof(line), "%d. %s - %s（%s）\n",
                               i + 1, songs[i].title,
                               songs[i].artist[0] ? songs[i].artist : "未知歌手",
                               songs[i].album);
        } else {
            written = snprintf(line, sizeof(line), "%d. %s - %s\n",
                               i + 1, songs[i].title,
                               songs[i].artist[0] ? songs[i].artist : "未知歌手");
        }
        if (written <= 0) continue;
        size_t length = (size_t)written;
        const size_t room = sizeof(text) - used - 1;
        if (length > room) length = room;
        memcpy(text + used, line, length);
        used += length;
        text[used] = '\0';
    }
    ESP_LOGI(TAG, "mcp search_music results: %.400s", text);
    mcp_reply_by_id(job->client, job->id_json, text, false);
    heap_caps_free(job);
    vTaskDelete(NULL);
}

static void mcp_tool_call(xiaozhi_client_t *client, cJSON *root)
{
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    const cJSON *query = cJSON_GetObjectItemCaseSensitive(args, "query");
    const cJSON *song_index = cJSON_GetObjectItemCaseSensitive(args, "song_index");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(args, "action");

    if (!cJSON_IsString(name) || name->valuestring == NULL) {
        mcp_reply_tool_text(client, root, "missing tool name", true);
        return;
    }
    ESP_LOGI(TAG, "mcp tool call: %s", name->valuestring);

    const device_config_values_t *cfg = device_config_get();
    const bool logged_in = cfg->qqmusic_cookie[0] != '\0';

    if (strcmp(name->valuestring, "search_music") == 0) {
        if (!cJSON_IsString(query) || query->valuestring[0] == '\0') {
            mcp_reply_tool_text(client, root, "missing song query", true);
            return;
        }
        /* A new request supersedes anything still parked from a previous one:
           leaving the old entry queued is what made an older song start after a
           newer request had already failed. */
        xiaozhi_client_cancel_pending_music(client);
        /* A QQ Music search performs an HTTPS round trip. This handler runs on
           the websocket task, whose stack is far too small for a TLS handshake,
           so the work is handed to a worker with its own stack. */
        ESP_LOGI(TAG, "mcp search_music query=%s (async)", query->valuestring);
        mcp_search_job_t *job = heap_caps_calloc(1, sizeof(*job),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (job == NULL) {
            mcp_reply_tool_text(client, root, "out of memory", true);
            return;
        }
        job->client = client;
        copy_bounded(job->query, sizeof(job->query), query->valuestring);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (id != NULL) {
            char *id_text = cJSON_PrintUnformatted((cJSON *)id);
            if (id_text != NULL) {
                copy_bounded(job->id_json, sizeof(job->id_json), id_text);
                cJSON_free(id_text);
            }
        }
        if (xTaskCreatePinnedToCoreWithCaps(mcp_search_task, "mcp_search", 12288, job, 3,
                                            NULL, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            heap_caps_free(job);
            mcp_reply_tool_text(client, root, "search task failed", true);
        }
        return;
    }

    if (strcmp(name->valuestring, "play_music") == 0 ||
        strcmp(name->valuestring, "music") == 0) {
        const char *verb = cJSON_IsString(action) ? action->valuestring : "play";
        if (strcmp(verb, "stop") == 0) {
            /* Music stop must win over any queued TTS: clearing the flag first
               lets the playback task unwind before speech resumes. A queued
               song is dropped too, or it would start right after stop. */
            client->music_stop_requested = true;
            client->pending_music_ready = false;
            client->state = XIAOZHI_STATE_IDLE;
            ESP_LOGI(TAG, "mcp music: stopped");
            mcp_reply_tool_text(client, root, "playback stopped", false);
            return;
        }
        if (strcmp(verb, "play") != 0) {
            mcp_reply_tool_text(client, root, "action must be play or stop", true);
            return;
        }
        if (!cJSON_IsNumber(song_index)) {
            ESP_LOGW(TAG, "mcp play refused: missing song_index");
            mcp_reply_tool_text(client, root,
                "Refused: call search_music first, ask the user to pick a numbered "
                "candidate, then pass that number as song_index.", true);
            return;
        }
        const int index = song_index->valueint;
        if (!s_music_cache.valid ||
            (uint32_t)(now_ms() - s_music_cache.stamp_ms) > MCP_MUSIC_CACHE_MS) {
            ESP_LOGW(TAG, "mcp play refused: no fresh search result");
            mcp_reply_tool_text(client, root,
                "Refused: no recent search result. Call search_music again.", true);
            return;
        }
        if (index < 1 || index > s_music_cache.count) {
            ESP_LOGW(TAG, "mcp play refused: index %d out of range 1..%d",
                     index, s_music_cache.count);
            mcp_reply_tool_text(client, root, "Refused: invalid song_index.", true);
            return;
        }
        const qqmusic_song_t *song = &s_music_cache.songs[index - 1];
        char label[256];
        snprintf(label, sizeof(label), "%s%s%s", song->title,
                 song->artist[0] ? " - " : "", song->artist);
        const bool ok = xiaozhi_mcp_play_mid(client, song->mid, label);
        ESP_LOGI(TAG, "mcp play_music index=%d title=%s mid=%s queued=%d",
                 index, label, song->mid, ok);
        s_music_cache.valid = false;
        mcp_reply_tool_text(client, root,
            ok ? "playback started" : "song unavailable or account expired", !ok);
        return;
    }

    mcp_reply_tool_text(client, root, "unknown tool", true);
}

#else
static void mcp_tools_list(xiaozhi_client_t *client, cJSON *root)
{
    (void)client; (void)root;
}
static void mcp_tool_call(xiaozhi_client_t *client, cJSON *root)
{
    (void)client; (void)root;
}
#endif

static bool xiaozhi_handle_mcp(xiaozhi_client_t *client, cJSON *root)
{
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (!cJSON_IsString(method) || method->valuestring == NULL) return false;
    ESP_LOGI(TAG, "mcp method: %s", method->valuestring);
    if (strcmp(method->valuestring, "initialize") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
        cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
        cJSON_AddObjectToObject(caps, "tools");
        cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
        cJSON_AddStringToObject(info, "name", "dotii-face");
        cJSON_AddStringToObject(info, "version", "1.1.9");
        mcp_reply(client, root, result);
        return true;
    }
    if (strcmp(method->valuestring, "tools/list") == 0) {
        mcp_tools_list(client, root);
        return true;
    }
    if (strcmp(method->valuestring, "tools/call") == 0) {
        mcp_tool_call(client, root);
        return true;
    }
    if (strcmp(method->valuestring, "notifications/initialized") == 0) {
        return true;
    }
    return false;
}

bool xiaozhi_client_handle_text(xiaozhi_client_t *client, const char *json)
{
    if (client == NULL || json == NULL) return false;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return false;

    bool changed = false;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type_value = cJSON_IsString(type) ? type->valuestring : NULL;

    /* Xiaozhi wraps JSON-RPC MCP messages in an envelope:
         {"session_id":"..","type":"mcp","payload":{jsonrpc,id,method,params}}
       The payload must be unwrapped before the method/params lookup. Earlier
       versions only looked at the top level, so every MCP call was silently
       ignored and tools never ran. */
    if (type_value != NULL && strcmp(type_value, "mcp") == 0) {
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        ESP_LOGI(TAG, "mcp envelope received (payload=%s)",
                 cJSON_IsObject(payload) ? "object" : "missing");
        s_mcp_reply_enveloped = true;
        if (cJSON_IsObject(payload) && xiaozhi_handle_mcp(client, (cJSON *)payload)) {
            cJSON_Delete(root);
            return true;
        }
        cJSON_Delete(root);
        return true;
    }

    /* Backwards compatibility: accept a bare JSON-RPC message too. */
    if (cJSON_GetObjectItemCaseSensitive(root, "method") != NULL) {
        s_mcp_reply_enveloped = false;
    }
    if (cJSON_GetObjectItemCaseSensitive(root, "method") != NULL &&
        xiaozhi_handle_mcp(client, root)) {
        cJSON_Delete(root);
        return true;
    }

    const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    if (cJSON_IsString(emotion) && emotion->valuestring != NULL) {
        set_emotion(client, emotion->valuestring);
        changed = true;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    xiaozhi_session_state_t mapped;
    if (cJSON_IsString(state) && map_server_state(state->valuestring, &mapped)) {
        /* Music owns the session until the song ends or the user wakes the
           assistant. Server-side session bookkeeping used to flip the device
           back to LISTENING mid-song, which re-enabled continuous dialogue
           and made Xiaozhi say goodbye while the song was still playing. */
        if ((client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL) &&
            mapped != XIAOZHI_STATE_MUSIC &&
            !client->music_stop_requested) {
            ESP_LOGI(TAG, "ignore server state %s during music playback",
                     state->valuestring);
            /* The message was consumed (deliberately ignored), so report it as
               handled; callers must not treat it as malformed input. */
            changed = true;
        } else {
            client->listening = mapped == XIAOZHI_STATE_LISTENING;
            if (client->state != mapped) {
                client->state = mapped;
                changed = true;
            }
        }
    }

    if (type_value != NULL && strcmp(type_value, "hello") == 0) {
        client->connected = true;
        /* The service closes idle sockets and the client auto-reconnects. A
           reconnect must NOT reset the face while a song is still playing:
           music owns the session, so only refresh the connection state. */
        if (!((client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL) &&
              !client->music_stop_requested)) {
            client->state = XIAOZHI_STATE_IDLE;
        }
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        if (cJSON_IsString(session) && session->valuestring != NULL) {
            copy_bounded(client->session_id, sizeof(client->session_id), session->valuestring);
        }
        const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        if (cJSON_IsObject(audio)) {
            const cJSON *rate = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
            const cJSON *duration = cJSON_GetObjectItemCaseSensitive(audio, "frame_duration");
            if (cJSON_IsNumber(rate) && rate->valueint >= 8000 && rate->valueint <= 48000) {
                client->server_sample_rate = rate->valueint;
            }
            if (cJSON_IsNumber(duration) && duration->valueint >= 5 && duration->valueint <= 120) {
                client->server_frame_duration = duration->valueint;
            }
        }
        changed = true;
    } else if (type_value != NULL && strcmp(type_value, "stt") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            copy_bounded(client->last_user_text, sizeof(client->last_user_text), text->valuestring);
            client->user_text_revision++;
            copy_bounded(client->last_text, sizeof(client->last_text), text->valuestring);
            changed = true;
            /* The cloud LLM sometimes chats without calling search_music, even
               though the MCP tool description tells it to. Music commands are
               stable enough to dispatch locally, so playback does not depend on
               the model remembering the tool. */
#if CONFIG_XIAOZHI_ENABLE
            const char *song_text = text->valuestring;
            const char *play_marker = strstr(song_text, "播放");
            if (play_marker != NULL) {
                song_text = play_marker + strlen("播放");
                while (*song_text == ' ') ++song_text;
                if (*song_text != '\0') {
                    client->listening = false;
                    client->state = XIAOZHI_STATE_THINKING;
                    music_search_dispatch(client, song_text);
                }
            }
#endif
        }
    } else if (type_value != NULL && strcmp(type_value, "llm") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            copy_bounded(client->last_assistant_text, sizeof(client->last_assistant_text), text->valuestring);
            client->assistant_text_revision++;
            copy_bounded(client->last_text, sizeof(client->last_text), text->valuestring);
        }
        changed = true;
    } else if (type_value != NULL && strcmp(type_value, "tts") == 0) {
        const cJSON *tts_state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(tts_state) && tts_state->valuestring != NULL) {
            if (strcmp(tts_state->valuestring, "start") == 0) {
                /* A song owns the session: server-side chatter (farewells,
                   reminders, stray confirmations) must not cut it off. Only
                   the user's wake word or a play_music( stop ) call may stop
                   playback. */
                const bool music_owns_session =
                    (client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL) &&
                    !client->music_stop_requested;
                if (music_owns_session) {
                    ESP_LOGI(TAG, "tts start ignored during music playback");
                } else {
                    ensure_conversation_rate(client);
                    client->listening = false;
                    client->state = XIAOZHI_STATE_SPEAKING;
                }
            }
            else if (strcmp(tts_state->valuestring, "stop") == 0) {
                /* Speech has finished: release a song that was waiting for the
                   confirmation line to end. */
                if (client->pending_music_ready) {
                    client->state = XIAOZHI_STATE_IDLE;
                    xiaozhi_client_tick_pending_music(client);
                }
                /* Read the ownership flag AFTER the queue release above: that
                   release may have just started the song (music_task created).
                   Testing before it returned a stale "no music yet" answer, so
                   the code re-opened the microphone and uploaded the song back
                   to the cloud - the mic/speaker fight the user reported. */
                const bool music_owns_session =
                    (client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL) &&
                    !client->music_stop_requested;
                if (music_owns_session) {
                    /* Music owns the speaker: never restart listening. */
                    client->listening = false;
                    client->state = XIAOZHI_STATE_MUSIC;
                } else {
                    client->state = XIAOZHI_STATE_LISTENING;
                    client->listening = true;
                    if (!send_listen_mode(client, true, "auto")) {
                        client->state = XIAOZHI_STATE_IDLE;
                        client->listening = false;
                    } else {
                        ESP_LOGI(TAG, "tts stop: auto listening restarted");
                    }
                }
            }
            changed = true;
        }
    } else if (type_value != NULL && strcmp(type_value, "notify") == 0) {
        const cJSON *audio_url = cJSON_GetObjectItemCaseSensitive(root, "audio_url");
        if (cJSON_IsString(audio_url) && audio_url->valuestring != NULL &&
            audio_url->valuestring[0] != '\0') {
            /* SERVER-PUSHED MUSIC. The xiaozhi service resolves this with its
               OWN catalogue, so it cannot honour the user's QQ Music account,
               membership, or the confirmed-search gate - it was the reason an
               unconfirmed request still produced the wrong recording.
               Playback is refused; music must come through the device MCP
               tools (search_music -> confirmed song_index -> play_music). */
            ESP_LOGW(TAG,
                     "REFUSED server-pushed audio (use MCP search_music instead): url=%s",
                     audio_url->valuestring);
            changed = true;
        }
    }
    cJSON_Delete(root);
    return changed;
}

#if CONFIG_XIAOZHI_ENABLE
typedef struct {
    uint8_t *data;
    size_t length;
} audio_packet_t;
typedef struct {
    char *body;
    size_t capacity;
    size_t length;
} ota_body_t;

static QueueHandle_t s_audio_queue;
static uint32_t s_audio_init_tick;
static uint32_t s_speaker_init_tick;
static char s_rx_buffer[2048];
static size_t s_rx_length;
static bool s_audio_decoders_ready;

static esp_err_t ota_http_event(esp_http_client_event_t *event)
{
    ota_body_t *body = (ota_body_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && body != NULL && event->data_len > 0) {
        size_t required = body->length + (size_t)event->data_len + 1;
        if (required > body->capacity) return ESP_ERR_NO_MEM;
        memcpy(body->body + body->length, event->data, (size_t)event->data_len);
        body->length += (size_t)event->data_len;
        body->body[body->length] = '\0';
    }
    return ESP_OK;
}

static bool parse_ota_response(xiaozhi_client_t *client, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return false;
    bool complete = false;

    const cJSON *activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    bool activation_pending = false;
    if (cJSON_IsObject(activation)) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(activation, "code");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(activation, "message");
        if (cJSON_IsString(code) && code->valuestring != NULL) {
            copy_bounded(client->activation_code, sizeof(client->activation_code), code->valuestring);
            client->state = XIAOZHI_STATE_ACTIVATING;
            activation_pending = true;
            ESP_LOGI(TAG, "activation code %s", client->activation_code);
        }
        if (cJSON_IsString(message) && message->valuestring != NULL) {
            copy_bounded(client->activation_message, sizeof(client->activation_message), message->valuestring);
        }
    }

    const cJSON *websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(websocket, "url");
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(websocket, "token");
        if (cJSON_IsString(url) && url->valuestring != NULL && url->valuestring[0] != '\0') {
            copy_bounded(client->websocket_url, sizeof(client->websocket_url), url->valuestring);
            if (cJSON_IsString(token) && token->valuestring != NULL) {
                copy_bounded(client->token, sizeof(client->token), token->valuestring);
            }
            /* The official server returns the websocket endpoint and the
               pairing code together. Until the user binds the code at
               xiaozhi.me the device is not paired yet, so the code stays on
               screen and the activation task keeps polling for confirmation. */
            if (activation_pending) {
                persist_activation(client);
                complete = false;
            } else {
                client->activated = true;
                client->activation_code[0] = '\0';
                client->activation_message[0] = '\0';
                persist_activation(client);
                complete = true;
            }
        }
    }
    cJSON_Delete(root);
    return complete;
}

/* A self-hosted xiaozhi-server is spoken to directly over its websocket, while
   the official service requires the OTA activation handshake first. */
static bool is_official_server(const char *server)
{
    return server != NULL && strstr(server, "tenclass.net") != NULL;
}

static bool fetch_ota(xiaozhi_client_t *client)
{
    const char *configured = client->config.server_url[0] != '\0' ?
        client->config.server_url : XIAOZHI_DEFAULT_OTA_URL;

    if (!is_official_server(configured) &&
        (starts_with(configured, "ws://") || starts_with(configured, "wss://"))) {
        /* Self-hosted server: the websocket endpoint is used as-is. */
        copy_bounded(client->websocket_url, sizeof(client->websocket_url), configured);
        client->activation_code[0] = '\0';
        client->activation_message[0] = '\0';
        client->activated = true;
        persist_activation(client);
        ESP_LOGI(TAG, "self-hosted websocket %s", client->websocket_url);
        return true;
    }

    char url[192];
    if (!derive_ota_url(configured, url, sizeof(url))) {
        copy_bounded(url, sizeof(url), XIAOZHI_DEFAULT_OTA_URL);
    }
    ESP_LOGI(TAG, "requesting activation from %s", url);
    /* One activation task runs at a time, so this scratch area is shared
       instead of eating 4 KB of an already tight task stack (the TLS
       handshake on this call needs the room). It is only ever touched during
       activation, so it lives in PSRAM: keeping it in .bss permanently
       reserved 4 KB of internal RAM that the panel DMA strips need far more. */
    const size_t response_capacity = 4096;
    char *response = heap_caps_malloc(response_capacity,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) {
        ESP_LOGW(TAG, "OTA scratch buffer unavailable");
        return false;
    }
    response[0] = '\0';
    ota_body_t body = {.body = response, .capacity = response_capacity};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = ota_http_event,
        .user_data = &body,
        .timeout_ms = 12000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (http == NULL) {
        heap_caps_free(response);
        return false;
    }
    esp_http_client_set_header(http, "Activation-Version", "1");
    esp_http_client_set_header(http, "Device-Id", device_identity_get_mac());
    esp_http_client_set_header(http, "Client-Id", device_identity_get_client_id());
    char user_agent[64];
    snprintf(user_agent, sizeof(user_agent), "Dotii/%s", esp_app_get_description()->version);
    esp_http_client_set_header(http, "User-Agent", user_agent);
    esp_http_client_set_header(http, "Accept-Language", "zh-CN");
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_post_field(http, "{}", 2);
    esp_err_t error = esp_http_client_perform(http);
    int status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    if (error != ESP_OK || status != 200 || body.length == 0) {
        ESP_LOGW(TAG, "OTA request failed: %s HTTP %d", esp_err_to_name(error), status);
        heap_caps_free(response);
        return false;
    }
    ESP_LOGI(TAG, "OTA reply %d bytes", (int)body.length);
    const bool parsed = parse_ota_response(client, response);
    heap_caps_free(response);
    return parsed;
}

static void send_text(xiaozhi_client_t *client, const char *json)
{
    if (client == NULL || client->transport == NULL || json == NULL) return;
    esp_websocket_client_send_text((esp_websocket_client_handle_t)client->transport,
                                   json, (int)strlen(json), pdMS_TO_TICKS(3000));
}

static void send_hello(xiaozhi_client_t *client)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *features = cJSON_CreateObject();
    /* Always advertise the tool server. The confirmed-search gate and every
       music tool live behind this flag, so it must not silently fall back to
       false when the management snapshot has not arrived yet. */
    (void)client->config.mcp_enabled;
    cJSON_AddBoolToObject(features, "mcp", true);
    /* Tells a compatible server that this device can render the full local
       emotion palette, so the model may pick the expression itself. */
    cJSON_AddBoolToObject(features, "emotion", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "opus");
    cJSON_AddNumberToObject(audio, "sample_rate", XIAOZHI_INPUT_RATE);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_duration", XIAOZHI_FRAME_MS);
    cJSON_AddItemToObject(root, "audio_params", audio);
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        ESP_LOGI(TAG, "hello -> %s", json);
        send_text(client, json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}


static void ws_event_handler(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    xiaozhi_client_t *client = (xiaozhi_client_t *)handler_arg;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        client->connected = true;
        /* A reconnect during playback keeps the singing face; only a normal
           (non-music) session falls back to idle. */
        if (!((client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL) &&
              !client->music_stop_requested)) {
            client->state = XIAOZHI_STATE_IDLE;
        }
        send_hello(client);
        ESP_LOGI(TAG, "websocket connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        client->connected = false;
        client->listening = false;
        /* Local playback is independent of the socket: losing Wi-Fi mid-song
           must not blank the singing face or abort the track. */
        if (!((client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL) &&
              !client->music_stop_requested)) {
            client->state = XIAOZHI_STATE_ERROR;
        }
        ESP_LOGW(TAG, "websocket disconnected");
        break;
    case WEBSOCKET_EVENT_CLOSED:
        /* The service ends idle sessions with a clean close handshake. With
           enable_close_reconnect the client reconnects and re-sends hello on
           its own, so report "connecting" instead of a hard error; the wake
           task below rebuilds the socket if that reconnect never lands.
           A song keeps playing locally, so the face must stay on MUSIC. */
        client->connected = false;
        client->listening = false;
        if (!((client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL) &&
              !client->music_stop_requested)) {
            client->state = XIAOZHI_STATE_CONNECTING;
        }
        ESP_LOGW(TAG, "websocket closed by server");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data == NULL) break;
        if (data->op_code == 0x01 && data->data_len > 0) {
            if (data->payload_offset == 0) s_rx_length = 0;
            size_t copy = (size_t)data->data_len;
            if (s_rx_length + copy >= sizeof(s_rx_buffer)) copy = sizeof(s_rx_buffer) - s_rx_length - 1;
            memcpy(s_rx_buffer + s_rx_length, data->data_ptr, copy);
            s_rx_length += copy;
            if (data->payload_offset + data->data_len >= data->payload_len) {
                s_rx_buffer[s_rx_length] = '\0';
                ESP_LOGI(TAG, "rx text (%u bytes): %.240s",
                         (unsigned)s_rx_length, s_rx_buffer);
                if (xiaozhi_client_handle_text(client, s_rx_buffer) &&
                    client->state == XIAOZHI_STATE_MUSIC) {
                    xiaozhi_start_music(client);
                }
                bool resume_wake = xiaozhi_client_take_wake_resume(client);
                if (resume_wake && client->connected &&
                    send_listen_mode(client, true, "auto")) {
                    ESP_LOGI(TAG, "offline wake resumed after reconnect");
                } else if (resume_wake) {
                    client->wake_resume_pending = true;
                }
                s_rx_length = 0;
            }
        } else if (data->op_code == 0x02 && data->data_len > 0 && s_audio_queue != NULL) {
            /* Music owns the speaker while a song plays. The state guards above
               only stop the assistant from *starting* a turn, but its TTS
               speech arrives as binary opus packets on this same socket. If
               those were queued, playback_task would write them to the codec
               alongside the song and the two would fight for the speaker. */
            const bool music_owns_speaker =
                (client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL) &&
                !client->music_stop_requested;
            if (music_owns_speaker) {
                static uint32_t s_last_drop_log;
                const uint32_t drop_now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if ((uint32_t)(drop_now - s_last_drop_log) >= 3000u) {
                    s_last_drop_log = drop_now;
                    ESP_LOGI(TAG, "dropped TTS audio while music is playing");
                }
            } else {
                audio_packet_t packet = {0};
                packet.data = malloc((size_t)data->data_len);
                if (packet.data != NULL) {
                    packet.length = (size_t)data->data_len;
                    memcpy(packet.data, data->data_ptr, packet.length);
                    if (xQueueSend(s_audio_queue, &packet, 0) != pdTRUE) free(packet.data);
                }
            }
        }
        break;
    default:
        break;
    }
}

static bool send_listen_mode(xiaozhi_client_t *client, bool start, const char *mode);
static void music_task(void *argument);

static bool xiaozhi_start_music(xiaozhi_client_t *client)
{
    if (client == NULL || client->music_task != NULL ||
        client->pending_audio_url[0] == '\0') {
        return false;
    }
    client->music_stop_requested = false;
    atomic_store_explicit(&client->music_fade_requested, false, memory_order_release);
    atomic_store_explicit(&client->music_task_started, false, memory_order_release);
    TaskHandle_t handle = NULL;
    /* Pin to core 0. The LVGL worker renders the face on core 1 and each
       frame takes ~42 ms; when this task floated onto core 1 the decoder was
       preempted for up to 240 ms and the speaker ring ran dry mid-song. */
    if (xTaskCreatePinnedToCoreWithCaps(music_task, "xz_music", 8192, client, 6,
                                         &handle, 0,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "music task create failed");
        client->state = XIAOZHI_STATE_IDLE;
        client->pending_audio_url[0] = '\0';
        return false;
    }
    client->music_task = handle;
    atomic_store_explicit(&client->music_task_started, true, memory_order_release);
    return true;
}

static esp_audio_simple_dec_type_t music_decoder_type(xiaozhi_music_format_t format)
{
    switch (format) {
    case XIAOZHI_MUSIC_FORMAT_MP3: return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    case XIAOZHI_MUSIC_FORMAT_WAV: return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    case XIAOZHI_MUSIC_FORMAT_M4A: return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    case XIAOZHI_MUSIC_FORMAT_OGG:
    default: return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    }
}

static bool music_decoders_ensure(void)
{
    if (s_audio_decoders_ready) return true;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    s_audio_decoders_ready = true;
    return true;
}

/* Current codec output rate during music playback; 0 means "conversation rate". */
static uint32_t s_active_music_rate;

/* The conversation/TTS path always expects XIAOZHI_CODEC_RATE. Music may have
   left the codec at 44.1 kHz, so restore it before any spoken audio. */
static void ensure_conversation_rate(xiaozhi_client_t *client)
{
    if (client == NULL || client->speaker == NULL) return;
    if (s_active_music_rate == 0) return;
    esp_codec_dev_sample_info_t info = {
        .sample_rate = XIAOZHI_CODEC_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    (void)esp_codec_dev_close((esp_codec_dev_handle_t)client->speaker);
    if (esp_codec_dev_open((esp_codec_dev_handle_t)client->speaker, &info) == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)client->speaker, client->output_volume);
    }
    s_active_music_rate = 0;
    ESP_LOGI(TAG, "codec restored to conversation rate");
}

/* The decoder emits small blocks (one MP3 frame is ~26 ms of audio). Writing
   each one straight to the codec called into the I2S DMA hundreds of times per
   second and left audible gaps whenever a call was late. Accumulate a few blocks
   and hand the codec one larger write instead. */
#define MUSIC_WRITE_TARGET_SAMPLES 1024
static int16_t *s_write_buf;
static size_t s_write_capacity;
static size_t s_write_length;

static bool music_flush_codec(xiaozhi_client_t *client)
{
    if (client == NULL || client->speaker == NULL || s_write_length == 0) return true;
    const bool ok = esp_codec_dev_write((esp_codec_dev_handle_t)client->speaker,
                                        s_write_buf,
                                        (int)(s_write_length * sizeof(int16_t)))
                    == ESP_CODEC_DEV_OK;
    s_write_length = 0;
    return ok;
}

static bool music_emit(xiaozhi_client_t *client, const int16_t *samples, size_t count)
{
    if (samples == NULL || count == 0) return true;
    if (s_write_capacity < MUSIC_WRITE_TARGET_SAMPLES) {
        int16_t *grown = heap_caps_realloc(s_write_buf,
                                           MUSIC_WRITE_TARGET_SAMPLES * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) return false;
        s_write_buf = grown;
        s_write_capacity = MUSIC_WRITE_TARGET_SAMPLES;
    }
    for (size_t i = 0; i < count; ++i) {
        s_write_buf[s_write_length++] = samples[i];
        if (s_write_length >= MUSIC_WRITE_TARGET_SAMPLES) {
            if (!music_flush_codec(client)) return false;
        }
    }
    return true;
}

static bool music_fade_out_and_flush(xiaozhi_client_t *client)
{
    if (s_write_length > 0) {
        xiaozhi_pcm_fade_out(s_write_buf, s_write_length);
    }
    return music_flush_codec(client);
}

static bool music_write_pcm(xiaozhi_client_t *client,
                            esp_ae_rate_cvt_handle_t *resampler,
                            int *resampler_rate,
                            uint8_t *data, uint32_t size,
                            const esp_audio_simple_dec_info_t *info)
{
    if (client == NULL || data == NULL || info == NULL ||
        info->bits_per_sample != ESP_AUDIO_BIT16 ||
        info->channel == 0 || info->channel > 2) {
        return false;
    }

    const int channels = info->channel;
    const int frames = (int)(size / ((uint32_t)channels * sizeof(int16_t)));
    if (frames <= 0) return true;

    /* Scratch buffers are grown once and reused. Allocating and freeing them on
       every decoded block both fragmented internal RAM and cost enough time to
       make the I2S DMA underrun. */
    static int16_t *s_mono;
    static size_t s_mono_capacity;
    static int16_t *s_resampled;
    static size_t s_resampled_capacity;

    int16_t *source = (int16_t *)data;
    int16_t *mono = NULL;
    if (channels == 2) {
        if (s_mono_capacity < (size_t)frames) {
            int16_t *grown = heap_caps_realloc(s_mono, (size_t)frames * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (grown == NULL) return false;
            s_mono = grown;
            s_mono_capacity = (size_t)frames;
        }
        mono = s_mono;
        for (int index = 0; index < frames; ++index) {
            const int mixed = (int)source[index * 2] + (int)source[index * 2 + 1];
            mono[index] = (int16_t)(mixed / 2);
        }
    }
    int16_t *pcm = mono != NULL ? mono : source;

    static uint32_t s_last_logged_rate;
    static uint8_t s_last_logged_channels;
    if (info->sample_rate != s_last_logged_rate || info->channel != s_last_logged_channels) {
        s_last_logged_rate = info->sample_rate;
        s_last_logged_channels = info->channel;
        ESP_LOGI(TAG, "music pcm: rate=%u ch=%u bit=%u (codec=%d)",
                 (unsigned)info->sample_rate, (unsigned)info->channel,
                 (unsigned)info->bits_per_sample, XIAOZHI_CODEC_RATE);
    }
    bool ok = true;
    if (info->sample_rate == XIAOZHI_CODEC_RATE) {
        ok = music_emit(client, pcm, (size_t)frames);
    } else {
        if (*resampler == NULL || *resampler_rate != (int)info->sample_rate) {
            if (*resampler != NULL) {
                esp_ae_rate_cvt_close(*resampler);
                *resampler = NULL;
            }
            esp_ae_rate_cvt_cfg_t cfg = {
                .src_rate = info->sample_rate,
                .dest_rate = XIAOZHI_CODEC_RATE,
                .channel = 1,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .complexity = 2,
                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };
            if (esp_ae_rate_cvt_open(&cfg, resampler) != ESP_AE_ERR_OK) {
                *resampler = NULL;
                ok = false;
            } else {
                *resampler_rate = (int)info->sample_rate;
            }
        }
        if (ok && *resampler != NULL) {
            uint32_t max_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(*resampler, (uint32_t)frames,
                                                   &max_samples);
            if (s_resampled_capacity < (size_t)max_samples) {
                int16_t *grown = heap_caps_realloc(s_resampled,
                                                   (size_t)max_samples * sizeof(int16_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (grown == NULL) {
                    ok = false;
                    goto resample_done;
                }
                s_resampled = grown;
                s_resampled_capacity = (size_t)max_samples;
            }
            int16_t *resampled = s_resampled;
            {
                uint32_t produced = max_samples;
                if (esp_ae_rate_cvt_process(*resampler, (esp_ae_sample_t)pcm,
                                            (uint32_t)frames, (esp_ae_sample_t)resampled,
                                            &produced) != ESP_AE_ERR_OK) {
                    ok = false;
                } else if (produced > 0) {
                    ok = music_emit(client, resampled, (size_t)produced);
                }
            }
resample_done:;
        }
    }
    return ok;
}

static void music_task(void *argument)
{
    xiaozhi_client_t *client = (xiaozhi_client_t *)argument;
    while (!atomic_load_explicit(&client->music_task_started, memory_order_acquire)) {
        vTaskDelay(1);
    }
    char url[sizeof(client->pending_audio_url)];
    copy_bounded(url, sizeof(url), client->pending_audio_url);

    esp_http_client_handle_t http = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_ae_rate_cvt_handle_t resampler = NULL;
    int resampler_rate = 0;
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    /* Keep several seconds of compressed input buffered. The 80 ms internal
       DMA ring is the hard real-time margin; this larger PSRAM staging area
       absorbs Wi-Fi and decoder jitter between codec writes. */
    size_t input_capacity = 65536;
    size_t input_length = 0;
    size_t input_offset = 0;
    size_t output_capacity = 65536;
    bool eos = false;
    bool success = false;
    /* Bytes of PCM actually handed to the speaker; used to decide whether a
       late stream failure should count as "done" instead of "error". */
    uint32_t played_pcm_bytes = 0;

    if (!audio_init(client) || !music_decoders_ensure()) {
        ESP_LOGE(TAG, "music audio setup failed");
        goto cleanup;
    }

    input = heap_caps_malloc(input_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    output = heap_caps_malloc(output_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || output == NULL) goto cleanup;
    ESP_LOGI(TAG, "music buffers: input=%u output=%u psram_free=%u",
             (unsigned)input_capacity, (unsigned)output_capacity,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const device_config_values_t *mcfg = device_config_get();
    /* Keep this read buffer small: esp_http_client allocates it from internal
       DRAM, and the TLS websocket needs that same pool. The large staging area
       for audio lives in PSRAM (input_capacity below). */
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 1500,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    http = esp_http_client_init(&http_config);
    if (http == NULL) goto cleanup;
    client->music_http = http;
    /* The bridge proxies QQ Music for the device; its stream endpoint is
       token-gated. */
    if (mcfg->bridge_token[0] != '\0') {
        esp_http_client_set_header(http, "X-Bridge-Token", mcfg->bridge_token);
    }
    esp_http_client_set_header(http, "Accept", "audio/ogg, application/ogg, audio/mpeg, audio/wav, audio/*");
    esp_http_client_set_header(http, "Accept-Encoding", "identity");
    if (esp_http_client_open(http, 0) != ESP_OK) {
        ESP_LOGE(TAG, "music stream open failed");
        goto cleanup;
    }
    if (esp_http_client_fetch_headers(http) < 0 ||
        esp_http_client_get_status_code(http) < 200 ||
        esp_http_client_get_status_code(http) >= 300) {
        ESP_LOGE(TAG, "music stream HTTP status failed");
        goto cleanup;
    }

    while (input_length < 12 && !eos) {
        int read = esp_http_client_read(http, (char *)input + input_length,
                                        (int)(input_capacity - input_length));
        if (read > 0) input_length += (size_t)read;
        else if (read == 0) eos = true;
        else {
            ESP_LOGE(TAG, "music stream read failed");
            goto cleanup;
        }
    }
    xiaozhi_music_format_t format = xiaozhi_music_format_from_bytes(input, input_length);
    if (format == XIAOZHI_MUSIC_FORMAT_UNKNOWN) format = xiaozhi_music_format_from_url(url);
    if (format == XIAOZHI_MUSIC_FORMAT_UNKNOWN) {
        ESP_LOGE(TAG, "music format unknown");
        goto cleanup;
    }
    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = music_decoder_type(format),
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&decoder_config, &decoder) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "music decoder open failed");
        goto cleanup;
    }

    /* Prebuffer before the first write. Without this the very first decode
       ran while the network was still ramping up, the I2S DMA drained, and the
       result was the slow, clicking playback. ~24 KB is roughly a second of
       audio at typical stream bitrates. */
    int prebuffer_errors = 0;
    while (!eos && input_length < input_capacity * 3 / 4 &&
           client->running && !client->music_stop_requested) {
        int read = esp_http_client_read(http, (char *)input + input_length,
                                        (int)(input_capacity - input_length));
        if (read > 0) {
            input_length += (size_t)read;
            prebuffer_errors = 0;
        } else if (read == 0) {
            eos = true;
        } else if (++prebuffer_errors > 10) {
            ESP_LOGE(TAG, "music prebuffer failed after %d retries", prebuffer_errors);
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ESP_LOGI(TAG, "music prebuffer: %u bytes", (unsigned)input_length);

    int flush_attempts = 0;
    int read_errors = 0;
    uint32_t stat_bytes_in = 0, stat_pcm_out = 0, stat_since = now_ms();
    while (client->running && !client->music_stop_requested) {
        if (atomic_exchange_explicit(&client->music_fade_requested, false,
                                     memory_order_acq_rel)) {
            ESP_LOGI(TAG, "music fade-out requested");
            (void)music_fade_out_and_flush(client);
            client->music_stop_requested = true;
            break;
        }
        /* Read only when the buffer is half empty. Refilling on every pass
           interleaved a blocking network read with every decode step. */
        if (input_length < input_capacity / 2 && !eos) {
            /* Compact before reading so the free tail is always large enough;
               otherwise a shrinking tail can make read() return 0 and be
               mistaken for end-of-stream. */
            const size_t room = input_capacity - input_offset - input_length;
            if (room < 4096u && input_offset > 0) {
                if (input_length > 0) {
                    memmove(input, input + input_offset, input_length);
                }
                input_offset = 0;
            }
            int read = esp_http_client_read(http,
                                            (char *)input + input_offset + input_length,
                                            (int)(input_capacity - input_offset - input_length));
            if (read > 0) {
                input_length += (size_t)read;
                stat_bytes_in += (uint32_t)read;
                read_errors = 0;
            } else if (read == 0) {
                eos = true;
            } else {
                /* A transient Wi-Fi hiccup used to abort playback outright.
                   Retry a few times before giving up so the song survives a
                   momentary drop and, crucially, does not flip the face to
                   "error" mid-song. */
                if (++read_errors > 20) {
                    ESP_LOGE(TAG, "music stream read failed %d times, giving up", read_errors);
                    break;
                }
                ESP_LOGW(TAG, "music stream read failed (%d), retrying", read_errors);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        if (input_length == 0 && eos) {
            success = true;
            break;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input + input_offset,
            .len = (uint32_t)input_length,
            .eos = eos,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = output,
            .len = (uint32_t)output_capacity,
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_err_t result = esp_audio_simple_dec_process(decoder, &raw, &out);
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            size_t needed = out.needed_size > output_capacity ?
                out.needed_size + 4096 : output_capacity * 2;
            uint8_t *larger = heap_caps_realloc(output, needed,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (larger == NULL) break;
            output = larger;
            output_capacity = needed;
            continue;
        }
        if (result != ESP_AUDIO_ERR_OK && result != ESP_AUDIO_ERR_CONTINUE &&
            result != ESP_AUDIO_ERR_DATA_LACK) {
            ESP_LOGE(TAG, "music decode failed: %d", (int)result);
            break;
        }

        if (raw.consumed > 0 && raw.consumed <= input_length) {
            input_offset += raw.consumed;
            input_length -= raw.consumed;
            flush_attempts = 0;
            /* Slide only when the dead prefix grows large. Moving the whole
               buffer on every decoded frame was O(n^2) across a second of
               audio and measurably starved the codec. */
            if (input_offset >= 8192u || input_length == 0) {
                if (input_length > 0) {
                    memmove(input, input + input_offset, input_length);
                }
                input_offset = 0;
            }
        }
        if (out.decoded_size > 0) {
            esp_audio_simple_dec_info_t info = {0};
            if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                !music_write_pcm(client, &resampler, &resampler_rate, output,
                                 out.decoded_size, &info)) {
                ESP_LOGE(TAG, "music PCM output failed");
                break;
            }
            flush_attempts = 0;
            stat_pcm_out += out.decoded_size;
            played_pcm_bytes += out.decoded_size;
        }

        /* Throughput sample: if the bytes decoded per second fall below real
           time, the stream is starving the codec and playback will stutter. */
        {
            const uint32_t stat_now = now_ms();
            if ((uint32_t)(stat_now - stat_since) >= 3000u) {
                ESP_LOGI(TAG, "music stats: in=%u pcm=%u over %.1fs | int_free=%u dma_free=%u",
                         (unsigned)stat_bytes_in, (unsigned)stat_pcm_out,
                         (double)(stat_now - stat_since) / 1000.0,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
                stat_bytes_in = 0;
                stat_pcm_out = 0;
                stat_since = stat_now;
            }
        }

        if (raw.consumed == 0 && out.decoded_size == 0) {
            if (input_offset + input_length >= input_capacity && !eos) {
                size_t larger_capacity = input_capacity * 2;
                uint8_t *larger = heap_caps_realloc(input, larger_capacity,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (larger == NULL) break;
                input = larger;
                input_capacity = larger_capacity;
            } else if (eos) {
                if (++flush_attempts >= 2) {
                    success = true;
                    break;
                }
            }
        }
    }

cleanup:
    /* A song that already played a meaningful amount of audio must not flip
       the face to "error" just because the tail of the stream was lost; from
       the user's point of view the song simply ended. */
    if (!success && !client->music_stop_requested && played_pcm_bytes > 240000u) {
        ESP_LOGW(TAG, "music ended early after %u PCM bytes; treating as complete",
                 (unsigned)played_pcm_bytes);
        success = true;
    }
    if (client->music_http == http) client->music_http = NULL;
    if (http != NULL) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    /* Push the tail of the last partial batch so the song does not end early. */
    (void)music_flush_codec(client);
    if (decoder != NULL) esp_audio_simple_dec_close(decoder);
    if (resampler != NULL) esp_ae_rate_cvt_close(resampler);
    heap_caps_free(input);
    heap_caps_free(output);

    if (strcmp(client->pending_audio_url, url) == 0) {
        client->pending_audio_url[0] = '\0';
        if (client->running && !client->music_stop_requested) {
            client->state = success ? XIAOZHI_STATE_IDLE : XIAOZHI_STATE_ERROR;
        }
    }
    if (success) {
        ESP_LOGI(TAG, "music playback complete");
    } else if (client->running && !client->music_stop_requested) {
        ESP_LOGW(TAG, "music playback stopped after an error");
    }
    atomic_store_explicit(&client->music_task_started, false, memory_order_release);
    if (client->music_task == (void *)xTaskGetCurrentTaskHandle()) {
        client->music_task = NULL;
    }
    vTaskDeleteWithCaps(NULL);
}

/* Split the configured semi-colon separated list into command entries. Empty
   entries are skipped so a trailing separator is harmless. Returns the number
   of words accepted, or 0 when the caller should fall back to WakeNet. */
static int wake_word_split(const char *list, char out[][XIAOZHI_WAKE_WORD_EACH_BYTES])
{
    if (list == NULL || list[0] == '\0') return 0;
    int count = 0;
    const char *cursor = list;
    while (*cursor != '\0' && count < XIAOZHI_WAKE_WORD_MAX) {
        const char *separator = strchr(cursor, ';');
        const size_t length = separator != NULL ? (size_t)(separator - cursor) : strlen(cursor);
        if (length > 0 && length < XIAOZHI_WAKE_WORD_EACH_BYTES) {
            memcpy(out[count], cursor, length);
            out[count][length] = '\0';
            /* Trim leading and trailing spaces so "a; b" behaves like "a;b". */
            char *start = out[count];
            while (*start == ' ') start++;
            char *end = start + strlen(start);
            while (end > start && end[-1] == ' ') --end;
            *end = '\0';
            if (*start != '\0') {
                if (start != out[count]) memmove(out[count], start, strlen(start) + 1U);
                count++;
            }
        }
        if (separator == NULL) break;
        cursor = separator + 1;
    }
    return count;
}

/* Start the MultiNet command-word recogniser and inject the user's list. The
   model binary already lives in the model partition; only the vocabulary is
   configured here, which is why a custom wake word costs no extra flash. */
static bool wake_word_start_multinet(xiaozhi_client_t *client, srmodel_list_t *models,
                                      const char *wake_words)
{
    char words[XIAOZHI_WAKE_WORD_MAX][XIAOZHI_WAKE_WORD_EACH_BYTES];
    const int word_count = wake_word_split(wake_words, words);
    if (word_count == 0) return false;

    char *model_name = esp_srmodel_filter(models, ESP_MN_PREFIX, "cn");
    if (model_name == NULL) model_name = esp_srmodel_filter(models, ESP_MN_PREFIX, NULL);
    if (model_name == NULL) {
        ESP_LOGW(TAG, "no MultiNet model in the model partition");
        return false;
    }
    const esp_mn_iface_t *iface = esp_mn_handle_from_name(model_name);
    if (iface == NULL) {
        ESP_LOGW(TAG, "no MultiNet interface for %s", model_name);
        return false;
    }
    /* 3000 ms is the phrase window the official implementation uses. */
    model_iface_data_t *data = iface->create(model_name, 3000);
    if (data == NULL || iface->get_samp_rate(data) != XIAOZHI_INPUT_RATE) {
        ESP_LOGW(TAG, "MultiNet model initialization failed");
        if (data != NULL) iface->destroy(data);
        return false;
    }
    /* Bind the shared command list to this model. Without it every
       esp_mn_commands_add() below fails with ESP_ERR_INVALID_STATE and the
       recogniser keeps an empty vocabulary, which is exactly the "nothing ever
       wakes it" symptom. */
    /* The threshold must be set before the command list is committed; the
       official implementation does it in this order. */
    /* MultiNet is a general command-word engine, so it produces lower
       confidence for short wake phrases than a purpose-built WakeNet model.
       0.10 balances recall against false triggers; 0.20 was too aggressive and
       required several attempts before "你好小智" was accepted. */
    (void)iface->set_det_threshold(data, 0.10f);
    if (esp_mn_commands_alloc(iface, data) != ESP_OK) {
        ESP_LOGE(TAG, "MultiNet command list allocation failed");
        iface->destroy(data);
        return false;
    }
    int accepted = 0;
    for (int index = 0; index < word_count; ++index) {
        if (esp_mn_commands_add(index + 1, words[index]) != ESP_OK) {
            ESP_LOGW(TAG, "wake word %d rejected: %s", index + 1, words[index]);
        } else {
            accepted++;
        }
    }
    if (accepted == 0) {
        ESP_LOGE(TAG, "no usable wake word; MultiNet disabled");
        esp_mn_commands_free();
        iface->destroy(data);
        return false;
    }
    esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors != NULL && errors->num > 0) {
        ESP_LOGW(TAG, "MultiNet dropped %d unusable wake word(s)", errors->num);
    }
    esp_mn_commands_print();
    ESP_LOGI(TAG, "MultiNet ready: rate=%d chunk=%d threshold=0.10",
             iface->get_samp_rate(data), iface->get_samp_chunksize(data));
    if (iface->print_active_speech_commands != NULL) {
        iface->print_active_speech_commands(data);
    }
    client->wake_iface = (void *)iface;
    client->wake_data = data;
    client->wake_multinet = true;
    ESP_LOGI(TAG, "MultiNet wake word ready: model=%s words=%d", model_name, word_count);
    for (int index = 0; index < word_count; ++index) {
        ESP_LOGI(TAG, "  wake word %d: '%s'", index + 1, words[index]);
    }
    return true;
}

static bool wake_word_start_wakenet(xiaozhi_client_t *client, srmodel_list_t *models)
{
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (model_name == NULL) model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    const esp_wn_iface_t *iface = model_name != NULL ?
        esp_wn_handle_from_name(model_name) : NULL;
    model_iface_data_t *data = iface != NULL ?
        iface->create(model_name, DET_MODE_90) : NULL;
    if (data == NULL || iface->get_samp_rate(data) != XIAOZHI_INPUT_RATE) {
        ESP_LOGW(TAG, "wake model initialization failed");
        if (data != NULL) iface->destroy(data);
        return false;
    }
    ESP_LOGI(TAG, "wake model format: rate=%d chunk=%d channels=%d",
             iface->get_samp_rate(data), iface->get_samp_chunksize(data),
             iface->get_channel_num(data));
    if (!iface->set_det_threshold(data, 0.50f, 1)) {
        ESP_LOGW(TAG, "wake threshold override failed");
    }
    client->wake_wn_iface = (void *)iface;
    client->wake_wn_data = data;
    client->wake_wn_ready = true;
    ESP_LOGI(TAG, "WakeNet ready: %s", model_name != NULL ? model_name : "unknown");
    return true;
}

static bool wake_word_start(xiaozhi_client_t *client)
{
    if (client == NULL || client->wake_ready) return client != NULL && client->wake_ready;
    /* No configuration yet (first boot, or the management centre is down):
       reuse the words remembered from last time so the device is still
       wakeable with the user's own phrase. Keep the restored copy local: the
       supervisor compares client->config against the wanted snapshot, so
       mutating the config here would look like a permanent difference and
       would reload the 3 MB MultiNet model every few seconds. */
    char wake_words[XIAOZHI_WAKE_WORD_LIST_BYTES];
    strlcpy(wake_words, client->config.wake_words, sizeof(wake_words));
    if (wake_words[0] == '\0') {
        load_wake_words(wake_words, sizeof(wake_words));
        if (wake_words[0] != '\0') {
            ESP_LOGI(TAG, "wake words restored from NVS: %s", wake_words);
        }
    }
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL || models->num <= 0) {
        ESP_LOGW(TAG, "wake model partition unavailable");
        if (models != NULL) esp_srmodel_deinit(models);
        return false;
    }
    /* Load one recogniser engine at a time. MultiNet is the normal idle engine
       because it supports arbitrary user phrases; the microphone task swaps to
       WakeNet only while music plays to keep inference cost low. */
    client->wake_models = models;
    const bool mn_ok = wake_word_start_multinet(client, models, wake_words);
    bool wn_ok = false;
    if (!mn_ok) {
        ESP_LOGW(TAG, "MultiNet unavailable; WakeNet only");
        wn_ok = wake_word_start_wakenet(client, models);
        if (!wn_ok) ESP_LOGW(TAG, "WakeNet unavailable");
    }
    if (!mn_ok && !wn_ok) {
        esp_srmodel_deinit(models);
        client->wake_models = NULL;
        return false;
    }
    client->wake_ready = true;
    ESP_LOGI(TAG, "wake engines: multinet=%d wakenet=%d", (int)mn_ok, (int)wn_ok);
    return true;
}

static void wake_engine_drop_multinet(xiaozhi_client_t *client)
{
    if (client->wake_data != NULL && client->wake_iface != NULL) {
        const esp_mn_iface_t *iface = (const esp_mn_iface_t *)client->wake_iface;
        /* Clear the global command table first. It keeps pointers into the
           model; destroying the model first leaves a dangling root and the
           next esp_mn_commands_alloc() walks freed memory. */
        (void)esp_mn_commands_free();
        iface->destroy((model_iface_data_t *)client->wake_data);
    }
    client->wake_data = NULL;
    client->wake_iface = NULL;
    client->wake_multinet = false;
}

static void wake_engine_drop_wakenet(xiaozhi_client_t *client)
{
    if (client->wake_wn_data != NULL && client->wake_wn_iface != NULL) {
        const esp_wn_iface_t *iface = (const esp_wn_iface_t *)client->wake_wn_iface;
        iface->destroy((model_iface_data_t *)client->wake_wn_data);
    }
    client->wake_wn_data = NULL;
    client->wake_wn_iface = NULL;
    client->wake_wn_ready = false;
}

static void wake_word_stop(xiaozhi_client_t *client)
{
    if (client == NULL) return;
    wake_engine_drop_multinet(client);
    wake_engine_drop_wakenet(client);
    if (client->wake_models != NULL) esp_srmodel_deinit((srmodel_list_t *)client->wake_models);
    client->wake_models = NULL;
    client->wake_ready = false;
}

static void wake_words_current(xiaozhi_client_t *client, char *out, size_t capacity)
{
    strlcpy(out, client->config.wake_words, capacity);
    if (out[0] == '\0') load_wake_words(out, capacity);
}

static bool wake_engine_use_wakenet(xiaozhi_client_t *client)
{
    if (client->wake_wn_ready) return true;
    wake_engine_drop_multinet(client);
    client->wake_ready = false;

    srmodel_list_t *models = (srmodel_list_t *)client->wake_models;
    if (models != NULL && wake_word_start_wakenet(client, models)) {
        client->wake_ready = true;
        client->wake_music_fallback = false;
        ESP_LOGI(TAG, "wake engine switched to WakeNet for music");
        return true;
    }

    /* If the low-cost model cannot start, restore MultiNet so voice wake
       remains functional even though it uses more CPU. */
    char wake_words[XIAOZHI_WAKE_WORD_LIST_BYTES];
    wake_words_current(client, wake_words, sizeof(wake_words));
    const bool mn_ok = models != NULL &&
        wake_word_start_multinet(client, models, wake_words);
    client->wake_ready = mn_ok;
    client->wake_music_fallback = true;
    ESP_LOGW(TAG, "WakeNet switch failed; music keeps MultiNet wake fallback");
    return mn_ok;
}

static bool wake_engine_use_multinet(xiaozhi_client_t *client)
{
    if (client->wake_multinet) {
        client->wake_music_fallback = false;
        return true;
    }
    wake_engine_drop_wakenet(client);
    client->wake_ready = false;

    char wake_words[XIAOZHI_WAKE_WORD_LIST_BYTES];
    wake_words_current(client, wake_words, sizeof(wake_words));
    srmodel_list_t *models = (srmodel_list_t *)client->wake_models;
    if (models != NULL && wake_word_start_multinet(client, models, wake_words)) {
        client->wake_ready = true;
        client->wake_music_fallback = false;
        ESP_LOGI(TAG, "wake engine restored to MultiNet");
        return true;
    }

    const bool wn_ok = models != NULL && wake_word_start_wakenet(client, models);
    client->wake_ready = wn_ok;
    ESP_LOGW(TAG, "MultiNet restore failed; keeping WakeNet");
    return wn_ok;
}
/* Short two-tone completion chime, synthesised at the codec output rate so
   no extra audio asset is needed. */
static bool play_task_chime(xiaozhi_client_t *client, bool failed)
{
    if (client == NULL || client->speaker == NULL) return false;
    const int sample_rate = XIAOZHI_CODEC_RATE;
    const int tone1_ms = 120, tone2_ms = 180;
    const int fade_samples = sample_rate / 200; /* 5 ms */
    int16_t chunk[960];
    const int total = sample_rate * (tone1_ms + tone2_ms) / 1000;
    const float f1 = failed ? 520.0f : 880.0f;
    const float f2 = failed ? 390.0f : 1320.0f;
    const int boundary = sample_rate * tone1_ms / 1000;
    for (int offset = 0; offset < total; offset += (int)(sizeof(chunk) / sizeof(chunk[0]))) {
        int count = total - offset;
        if (count > (int)(sizeof(chunk) / sizeof(chunk[0]))) count = (int)(sizeof(chunk) / sizeof(chunk[0]));
        for (int i = 0; i < count; ++i) {
            const int n = offset + i;
            const float freq = n < boundary ? f1 : f2;
            float envelope = 0.35f;
            if (n < fade_samples) envelope *= (float)n / fade_samples;
            const int tail = total - 1 - n;
            if (tail < fade_samples) envelope *= (float)tail / fade_samples;
            chunk[i] = (int16_t)(sinf(6.2831853f * freq * n / sample_rate) * envelope * 32767.0f);
        }
        const int written = esp_codec_dev_write((esp_codec_dev_handle_t)client->speaker,
                                                chunk, count * (int)sizeof(int16_t));
        if (written != ESP_CODEC_DEV_OK) return false;
    }
    return true;
}

static void playback_task(void *argument);
static void microphone_task(void *argument);
static bool audio_init(xiaozhi_client_t *client);
static void xiaozhi_start_wake_task(xiaozhi_client_t *client);
static void xiaozhi_detect_wake_word(xiaozhi_client_t *client);

/* Worker startup is serialized here on purpose: the playback and microphone
   tasks used to initialize the shared codec/Opus handles lazily from their
   own task, so a first reply and a first push-to-talk could open the codec
   twice. Handles are stored so a stop can wait for the workers to leave
   before the websocket client is destroyed underneath them. */
static bool xiaozhi_start_workers(xiaozhi_client_t *client)
{
    (void)audio_init(client);
    if (client->playback_task == NULL) {
        TaskHandle_t handle = NULL;
        /* Audio must never underrun: it runs on core 0 (radio core) above the
           LVGL worker so a heavy face frame cannot starve the I2S DMA. */
        if (xTaskCreatePinnedToCoreWithCaps(playback_task, "xz_play", XIAOZHI_PLAYBACK_TASK_STACK, client, 7,
                                             &handle, 0,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
            client->playback_task = handle;
        }
    }
    if (client->microphone_task == NULL) {
        TaskHandle_t handle = NULL;
        /* Pinned to core 0: LVGL rasterises the face on core 1. Leaving this
           task free to float meant a priority-5 Opus-capture loop preempted
           the renderer, halving the frame rate whenever the device listened. */
        if (xTaskCreatePinnedToCoreWithCaps(microphone_task, "xz_mic",
                                             XIAOZHI_MIC_TASK_STACK, client, 5,
                                             &handle, 0,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
            client->microphone_task = handle;
        }
    }
    if (client->playback_task == NULL || client->microphone_task == NULL) {
        ESP_LOGE(TAG, "xiaozhi audio task create failed");
        return false;
    }
    return true;
}

static bool open_websocket(xiaozhi_client_t *client)
{
    if (client->websocket_url[0] == '\0') return false;
    esp_websocket_client_config_t config = {
        .uri = client->websocket_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 12000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Four kilobytes is what the proven stable build used. Doubling it
           to eight kilobytes took exactly four kilobytes out of the internal
           pool, and with the display and audio already holding most of it the
           device ran with under 2 kB free and panicked inside esp_timer and
           esp_event. Keep the smaller stack. */
        .task_stack = 4096,
        /* The service closes idle sessions cleanly; without this the client
           stops for good after the first server-side close. */
        .enable_close_reconnect = true,
    };
    /* Release the previous transport before asking the library for a new
       one. Building the new client first kept two sets of websocket buffers
       alive at the same time, which is more internal RAM than this board can
       spare next to the display and the audio codec. */
    if (client->transport != NULL) {
        esp_websocket_client_stop((esp_websocket_client_handle_t)client->transport);
        esp_websocket_client_destroy((esp_websocket_client_handle_t)client->transport);
        client->transport = NULL;
    }
    ESP_LOGI(TAG, "open websocket: internal free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    esp_websocket_client_handle_t handle = esp_websocket_client_init(&config);
    if (handle == NULL) {
        ESP_LOGE(TAG, "websocket init failed: internal free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    esp_websocket_client_append_header(handle, "Protocol-Version", "1");
    esp_websocket_client_append_header(handle, "Device-Id", device_identity_get_mac());
    esp_websocket_client_append_header(handle, "Client-Id", device_identity_get_client_id());
    if (client->token[0] != '\0') {
        char authorization[192];
        snprintf(authorization, sizeof(authorization), "Bearer %s", client->token);
        esp_websocket_client_append_header(handle, "Authorization", authorization);
    }
    esp_websocket_register_events(handle, WEBSOCKET_EVENT_ANY, ws_event_handler, client);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (esp_websocket_client_start(handle) == ESP_OK) break;
        if (attempt == 2) {
            esp_websocket_client_destroy(handle);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    client->transport = handle;
    client->state = XIAOZHI_STATE_CONNECTING;
    return true;
}

static void activation_task(void *argument)
{
    xiaozhi_client_t *client = (xiaozhi_client_t *)argument;
    while (client->running) {
        if (!connectivity_is_wifi_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!client->activated && fetch_ota(client)) {
            ESP_LOGI(TAG, "xiaozhi activation complete");
        }
        /* A stop during the OTA request must not leave a live session behind. */
        if (!client->running) break;
        if (client->activated) {
            if (open_websocket(client) && xiaozhi_start_workers(client) &&
                wake_word_start(client)) {
                xiaozhi_start_wake_task(client);
            } else {
                ESP_LOGW(TAG, "activated session started without wake word");
            }
            break;
        }
        /* Still waiting for the user to bind the code at xiaozhi.me. Keep the
           code visible and ask again shortly. Sliced so a stop request is not
           held up for the whole poll interval. */
        client->state = XIAOZHI_STATE_ACTIVATING;
        for (int slice = 0; slice < 50 && client->running; ++slice) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (client->activation_task == (void *)xTaskGetCurrentTaskHandle()) {
        client->activation_task = NULL;
    }
    vTaskDelete(NULL);
}

/* The completion chime only needs the speaker, so keep it independent from
   the microphone: a failing mic init must not swallow the notification. */
static bool speaker_init(xiaozhi_client_t *client)
{
    if (client == NULL) return false;
    if (client->speaker_ready) return true;
    /* The playback worker calls this on every pass, so keep the retries
       sparse instead of hammering the codec and the log. */
    const uint32_t now = xTaskGetTickCount();
    if ((uint32_t)(now - s_speaker_init_tick) < pdMS_TO_TICKS(1000)) return false;
    s_speaker_init_tick = now;

    if (client->speaker == NULL) client->speaker = bsp_audio_codec_speaker_init();
    if (client->speaker == NULL) return false;
    esp_codec_dev_sample_info_t speaker_info = {
        .sample_rate = XIAOZHI_CODEC_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open((esp_codec_dev_handle_t)client->speaker, &speaker_info) != ESP_CODEC_DEV_OK) {
        return false;
    }
    esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)client->speaker,
                              client->output_volume);
    client->speaker_ready = true;
    ESP_LOGI(TAG, "speaker ready for task chime");
    return true;
}

static bool audio_init(xiaozhi_client_t *client)
{
    if (client->codec_ready) return true;
    /* The playback and microphone workers both call this on every pass, so
       keep the retries sparse instead of hammering the codec and the log. */
    const uint32_t now = xTaskGetTickCount();
    if ((uint32_t)(now - s_audio_init_tick) < pdMS_TO_TICKS(1000)) return false;
    s_audio_init_tick = now;

    /* The duplex I2S path needs a few KB of internal DMA memory for its
       descriptors. Wi-Fi, BLE and the AMOLED panel can claim more than the
       board has left, so a failure here is a soft error: the BSP returns
       NULL instead of aborting, and the next pass tries again. */
    if (!speaker_init(client)) return false;
    if (client->microphone == NULL) client->microphone = bsp_audio_codec_microphone_init();
    if (client->microphone == NULL) return false;

    esp_codec_dev_sample_info_t microphone_info = {
        .sample_rate = XIAOZHI_CODEC_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open((esp_codec_dev_handle_t)client->microphone,
                           &microphone_info) != ESP_CODEC_DEV_OK) {
        return false;
    }
    if (client->input_resampler == NULL) {
        esp_ae_rate_cvt_cfg_t input_config = {
            .src_rate = XIAOZHI_CODEC_RATE,
            .dest_rate = XIAOZHI_INPUT_RATE,
            .channel = 1,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        if (esp_ae_rate_cvt_open(&input_config, &client->input_resampler) != ESP_AE_ERR_OK) {
            client->input_resampler = NULL;
            return false;
        }
    }
    esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)client->speaker,
                              client->output_volume);
    const int mic_gain_result = esp_codec_dev_set_in_gain(
        (esp_codec_dev_handle_t)client->microphone, XIAOZHI_MIC_GAIN_DB);
    if (mic_gain_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "microphone gain setup failed: %d", mic_gain_result);
    }
    client->codec_ready = true;
    ESP_LOGI(TAG, "codec ready: dma free=%u largest=%u internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

static bool ensure_encoder(xiaozhi_client_t *client)
{
    if (client->audio_encode != NULL) return true;
    esp_opus_enc_config_t config = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };
    void *encoder = NULL;
    if (esp_opus_enc_open(&config, sizeof(config), &encoder) != ESP_AUDIO_ERR_OK) return false;
    client->audio_encode = encoder;
    return true;
}

static bool ensure_decoder(xiaozhi_client_t *client, int sample_rate)
{
    if (client->audio_decode != NULL && client->server_sample_rate == sample_rate) return true;
    if (client->audio_decode != NULL) {
        esp_opus_dec_close(client->audio_decode);
        client->audio_decode = NULL;
    }
    if (client->output_resampler != NULL) {
        esp_ae_rate_cvt_close(client->output_resampler);
        client->output_resampler = NULL;
    }
    esp_opus_dec_cfg_t config = {
        .sample_rate = (uint32_t)sample_rate,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    void *decoder = NULL;
    if (esp_opus_dec_open(&config, sizeof(config), &decoder) != ESP_AUDIO_ERR_OK) return false;
    client->audio_decode = decoder;
    client->server_sample_rate = sample_rate;
    if (sample_rate != XIAOZHI_CODEC_RATE) {
        esp_ae_rate_cvt_cfg_t resampler_config = {
            .src_rate = (uint32_t)sample_rate,
            .dest_rate = XIAOZHI_CODEC_RATE,
            .channel = 1,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        if (esp_ae_rate_cvt_open(&resampler_config, &client->output_resampler) != ESP_AE_ERR_OK) {
            client->output_resampler = NULL;
        }
    }
    return true;
}

static bool play_wake_reply(xiaozhi_client_t *client)
{
    if (client == NULL || client->speaker == NULL) return false;
    atomic_store_explicit(&client->wake_reply_playing, true, memory_order_release);
    ESP_LOGI(TAG, "wake reply: playing \"在\"");
    bool ok = true;
    size_t offset = 0;
    const size_t chunk_samples = 960;
    while (offset < k_wake_reply_pcm_samples && client->running) {
        size_t count = k_wake_reply_pcm_samples - offset;
        if (count > chunk_samples) count = chunk_samples;
        const int written = esp_codec_dev_write(
            (esp_codec_dev_handle_t)client->speaker,
            (void *)&k_wake_reply_pcm[offset],
            (int)(count * sizeof(int16_t)));
        if (written != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "wake reply write failed: %d", written);
            ok = false;
            break;
        }
        offset += count;
    }
    atomic_store_explicit(&client->wake_reply_playing, false, memory_order_release);
    if (ok && offset == k_wake_reply_pcm_samples) {
        ESP_LOGI(TAG, "wake reply: done");
    }
    return ok;
}
static int16_t *playback_pcm_reserve(int16_t **buffer, size_t *capacity,
                                   size_t samples)
{
    if (samples == 0) return NULL;
    if (*capacity < samples) {
        int16_t *grown = heap_caps_realloc(*buffer, samples * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) return NULL;
        *buffer = grown;
        *capacity = samples;
    }
    return *buffer;
}

static void playback_task(void *argument)
{
    xiaozhi_client_t *client = (xiaozhi_client_t *)argument;
    while (client->running) {
        const bool chime_pending =
            atomic_load_explicit(&client->task_chime_pending, memory_order_acquire);
        if (chime_pending && speaker_init(client)) {
            atomic_store_explicit(&client->task_chime_pending, false, memory_order_release);
            const bool failed =
                atomic_load_explicit(&client->task_chime_failed, memory_order_acquire);
            ESP_LOGI(TAG, "task chime %s", failed ? "failed" : "completed");
            (void)play_task_chime(client, failed);
        }

        const bool wake_reply_pending =
            atomic_load_explicit(&client->wake_reply_pending, memory_order_acquire);
        if (wake_reply_pending && client->music_task == NULL && audio_init(client) &&
            xiaozhi_client_take_wake_reply(client)) {
            (void)play_wake_reply(client);
        }

        audio_packet_t packet = {0};
        /* Keep this short so a wake reply deferred by music starts promptly
           after the music worker retires. */
        const TickType_t queue_wait = pdMS_TO_TICKS(20);
        if (xQueueReceive(s_audio_queue, &packet, queue_wait) != pdTRUE) continue;
        if (packet.data == NULL) continue;
        if (audio_init(client) && ensure_decoder(client, client->server_sample_rate)) {
            static int16_t *s_play_pcm;
            static size_t s_play_pcm_capacity;
            static int16_t *s_play_resampled;
            static size_t s_play_resampled_capacity;
            int frame_samples = client->server_sample_rate * XIAOZHI_FRAME_MS / 1000;
            int16_t *pcm = playback_pcm_reserve(&s_play_pcm, &s_play_pcm_capacity,
                                                (size_t)frame_samples);
            if (pcm != NULL) {
                esp_audio_dec_in_raw_t raw = {
                    .buffer = packet.data,
                    .len = (uint32_t)packet.length,
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out = {
                    .buffer = (uint8_t *)pcm,
                    .len = (uint32_t)(frame_samples * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t info = {0};
                if (esp_opus_dec_decode(client->audio_decode, &raw, &out, &info) == ESP_AUDIO_ERR_OK) {
                    int decoded_samples = (int)(out.decoded_size / sizeof(int16_t));
                    if (client->output_resampler != NULL && decoded_samples > 0) {
                        uint32_t max_samples = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(client->output_resampler,
                                                               (uint32_t)decoded_samples, &max_samples);
                        int16_t *resampled = playback_pcm_reserve(
                            &s_play_resampled, &s_play_resampled_capacity,
                            (size_t)max_samples);
                        if (resampled != NULL) {
                            uint32_t produced = max_samples;
                            if (esp_ae_rate_cvt_process(client->output_resampler, (esp_ae_sample_t)pcm,
                                                        (uint32_t)decoded_samples, (esp_ae_sample_t)resampled,
                                                        &produced) == ESP_AE_ERR_OK) {
                                esp_codec_dev_write((esp_codec_dev_handle_t)client->speaker, resampled,
                                                    (int)(produced * sizeof(int16_t)));
                            }
                        }
                    } else {
                        esp_codec_dev_write((esp_codec_dev_handle_t)client->speaker, pcm, out.decoded_size);
                    }
                }
            }
        }
        free(packet.data);
    }
    if (client->playback_task == (void *)xTaskGetCurrentTaskHandle()) {
        client->playback_task = NULL;
    }
    vTaskDeleteWithCaps(NULL);
}

static void microphone_task(void *argument)
{
    xiaozhi_client_t *client = (xiaozhi_client_t *)argument;
    const int source_samples = XIAOZHI_CODEC_RATE * XIAOZHI_FRAME_MS / 1000;
    int16_t *source = malloc((size_t)source_samples * 2 * sizeof(int16_t));
    int16_t *resampled = malloc((size_t)XIAOZHI_INPUT_RATE * sizeof(int16_t));
    uint8_t *encoded = malloc(2048);
    if (source == NULL || resampled == NULL || encoded == NULL) {
        free(source); free(resampled); free(encoded);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    while (client->running) {
        /* Release a queued song as soon as the assistant stops talking. This
           task runs continuously, so it is the right place to drive it. */
        xiaozhi_client_tick_pending_music(client);
        if (!audio_init(client)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* The worker starts before the supervisor finishes loading the model
           list. Do not touch recogniser state until wake_word_start() has
           published wake_models. */
        if (client->wake_ready && client->wake_models != NULL) {
            const bool music_active = client->state == XIAOZHI_STATE_MUSIC ||
                                      client->music_task != NULL;
            if (music_active && !client->wake_wn_ready &&
                !client->wake_music_fallback) {
                (void)wake_engine_use_wakenet(client);
            } else if (!music_active && !client->listening &&
                       client->state != XIAOZHI_STATE_SPEAKING &&
                       client->state != XIAOZHI_STATE_THINKING &&
                       !client->wake_multinet) {
                (void)wake_engine_use_multinet(client);
            }
        }
        /* Do not upload the device's own wake acknowledgement as speech. */
        if (atomic_load_explicit(&client->wake_reply_pending, memory_order_acquire) ||
            atomic_load_explicit(&client->wake_reply_playing, memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        /* ES7210, the input resampler and WakeNet form one continuous capture
           pipeline. The official audio service owns that pipeline from a
           single input task; allowing the wake and upload paths to read it
           concurrently corrupts the codec/resampler state when a wake word
           switches the session to listening. */
        if (!client->listening) {
            if (xiaozhi_client_wake_detection_allowed(client)) {
                xiaozhi_detect_wake_word(client);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }
        if (!client->connected || !ensure_encoder(client)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int read = esp_codec_dev_read((esp_codec_dev_handle_t)client->microphone, source,
                                      source_samples * 2 * (int)sizeof(int16_t));
        if (read != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        xiaozhi_pcm_downmix_stereo(source, (size_t)source_samples);
        xiaozhi_pcm_amplify(source, (size_t)source_samples, XIAOZHI_INPUT_GAIN);
        if (client->input_resampler == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t output_samples = XIAOZHI_INPUT_RATE;
        if (esp_ae_rate_cvt_process(client->input_resampler, (esp_ae_sample_t)source,
                                    (uint32_t)source_samples, (esp_ae_sample_t)resampled,
                                    &output_samples) != ESP_AE_ERR_OK) continue;
        int frame_samples = XIAOZHI_INPUT_RATE * XIAOZHI_FRAME_MS / 1000;
        if (output_samples < (uint32_t)frame_samples) {
            memset(resampled + output_samples, 0, (size_t)(frame_samples - (int)output_samples) * sizeof(int16_t));
        }
        esp_audio_enc_in_frame_t in = {
            .buffer = (uint8_t *)resampled,
            .len = (uint32_t)(frame_samples * sizeof(int16_t)),
        };
        esp_audio_enc_out_frame_t out = {
            .buffer = encoded,
            .len = 2048,
            .encoded_bytes = 0,
        };
        if (esp_opus_enc_process(client->audio_encode, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
            esp_websocket_client_send_bin((esp_websocket_client_handle_t)client->transport,
                                          (const char *)encoded, (int)out.encoded_bytes, pdMS_TO_TICKS(2000));
        }
    }
    free(source); free(resampled); free(encoded);
    if (client->microphone_task == (void *)xTaskGetCurrentTaskHandle()) {
        client->microphone_task = NULL;
    }
    vTaskDeleteWithCaps(NULL);
}

static bool send_listen_mode(xiaozhi_client_t *client, bool start, const char *mode)
{
    if (client == NULL || !client->connected || client->transport == NULL) return false;
    cJSON *root = cJSON_CreateObject();
    if (client->session_id[0] != '\0') cJSON_AddStringToObject(root, "session_id", client->session_id);
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", start ? "start" : "stop");
    if (start && mode != NULL && mode[0] != '\0') {
        cJSON_AddStringToObject(root, "mode", mode);
    }
    char *json = cJSON_PrintUnformatted(root);
    bool ok = false;
    if (json != NULL) {
        send_text(client, json);
        cJSON_free(json);
        ok = true;
    }
    cJSON_Delete(root);
    client->listening = start;
    client->state = start ? XIAOZHI_STATE_LISTENING : XIAOZHI_STATE_IDLE;
    return ok;
}

typedef struct {
    xiaozhi_client_t *client;
    char mid[48];
    char title[128];
} mcp_music_job_t;

static void mcp_music_resolve_task(void *argument)
{
    mcp_music_job_t *job = (mcp_music_job_t *)argument;
    const device_config_values_t *cfg = device_config_get();
    char url[512] = {0};
    if (qqmusic_resolve_mid(cfg->bridge_url, cfg->bridge_token, job->mid, url, sizeof(url))) {
        ESP_LOGI(TAG, "mcp play_music: %s (%s)", job->title, job->mid);
        xiaozhi_client_play_url(job->client, url, job->title);
    } else {
        ESP_LOGW(TAG, "mcp play_music: unavailable mid=%s", job->mid);
    }
    heap_caps_free(job);
    vTaskDelete(NULL);
}

static bool xiaozhi_mcp_play_mid(xiaozhi_client_t *client, const char *mid,
                                 const char *title)
{
    if (client == NULL || mid == NULL || mid[0] == '\0') return false;
    mcp_music_job_t *job = heap_caps_calloc(1, sizeof(*job),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job == NULL) return false;
    job->client = client;
    copy_bounded(job->mid, sizeof(job->mid), mid);
    copy_bounded(job->title, sizeof(job->title), title != NULL ? title : "");
    if (xTaskCreatePinnedToCoreWithCaps(mcp_music_resolve_task, "mcp_music", 8192, job, 3,
                                        NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        heap_caps_free(job);
        return false;
    }
    return true;
}

/* Starts playback immediately. Callers that may overlap assistant speech use
   xiaozhi_client_queue_music() instead so the two never fight for the codec. */
bool xiaozhi_client_play_url(xiaozhi_client_t *client, const char *url, const char *title)
{
    if (client == NULL || url == NULL || url[0] == '\0') return false;
    ESP_LOGI(TAG, "play_url: title=%s", title != NULL ? title : "(none)");
    copy_bounded(client->pending_audio_url, sizeof(client->pending_audio_url), url);
    if (title != NULL) {
        copy_bounded(client->last_assistant_text, sizeof(client->last_assistant_text), title);
        client->assistant_text_revision++;
    }
    /* Stop uploading microphone audio the moment a song starts. Leaving the
       mic live meant the assistant heard the music, decided the request was
       over, and said goodbye - which also ended playback. Wake-word detection
       resumes on its own because it only requires listening == false. */
    client->listening = false;
    /* Tell the cloud to stop listening too, but do NOT let send_listen_mode()
       overwrite the music face/state: it assigns IDLE on its own. */
    {
        cJSON *listen = cJSON_CreateObject();
        if (listen != NULL) {
            if (client->session_id[0] != '\0') {
                cJSON_AddStringToObject(listen, "session_id", client->session_id);
            }
            cJSON_AddStringToObject(listen, "type", "listen");
            cJSON_AddStringToObject(listen, "state", "stop");
            char *json = cJSON_PrintUnformatted(listen);
            if (json != NULL) {
                send_text(client, json);
                cJSON_free(json);
            }
            cJSON_Delete(listen);
        }
    }
    client->listening = false;
    client->state = XIAOZHI_STATE_MUSIC;
    return xiaozhi_start_music(client);
}

bool xiaozhi_client_send_listen(xiaozhi_client_t *client, bool start)
{
    return send_listen_mode(client, start, "manual");
}
bool xiaozhi_client_send_abort(xiaozhi_client_t *client)
{
    if (client == NULL || !client->connected || client->transport == NULL) return false;
    cJSON *root = cJSON_CreateObject();
    if (client->session_id[0] != '\0') cJSON_AddStringToObject(root, "session_id", client->session_id);
    cJSON_AddStringToObject(root, "type", "abort");
    cJSON_AddStringToObject(root, "reason", "touch_stop");
    char *json = cJSON_PrintUnformatted(root);
    bool ok = false;
    if (json != NULL) {
        send_text(client, json);
        cJSON_free(json);
        ok = true;
    }
    cJSON_Delete(root);
    client->listening = false;
    return ok;
}

static int16_t *s_wake_chunk;
static size_t s_wake_chunk_capacity;
static int16_t *s_wake_source;
static size_t s_wake_source_capacity;

static bool wake_buffers_reserve(size_t source_samples, size_t chunk_samples)
{
    if (s_wake_source_capacity < source_samples) {
        int16_t *grown = heap_caps_realloc(s_wake_source,
                                           source_samples * 2 * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) return false;
        s_wake_source = grown;
        s_wake_source_capacity = source_samples;
    }
    if (s_wake_chunk_capacity < chunk_samples) {
        int16_t *grown = heap_caps_realloc(s_wake_chunk,
                                           chunk_samples * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) return false;
        s_wake_chunk = grown;
        s_wake_chunk_capacity = chunk_samples;
    }
    return true;
}

static void xiaozhi_detect_wake_word(xiaozhi_client_t *client)
{
    if (!xiaozhi_client_wake_detection_allowed(client)) return;
    if (!audio_init(client)) return;

    const esp_wn_iface_t *wn_iface = NULL;
    const esp_mn_iface_t *mn_iface = NULL;
    model_iface_data_t *mn_data = (model_iface_data_t *)client->wake_data;
    model_iface_data_t *wn_data = (model_iface_data_t *)client->wake_wn_data;
    if (client->wake_multinet && mn_data != NULL) {
        mn_iface = (const esp_mn_iface_t *)client->wake_iface;
    }
    if (client->wake_wn_ready && wn_data != NULL) {
        wn_iface = (const esp_wn_iface_t *)client->wake_wn_iface;
    }


    if (mn_iface == NULL && wn_iface == NULL) return;
    int target_samples = 0;
    if (mn_iface != NULL) {
        target_samples = mn_iface->get_samp_chunksize(mn_data);
    } else {
        target_samples = wn_iface->get_samp_chunksize(wn_data);
    }
    if (target_samples <= 0 || target_samples > 4096) return;

    const int source_samples =
        (target_samples * XIAOZHI_CODEC_RATE + XIAOZHI_INPUT_RATE - 1) /
        XIAOZHI_INPUT_RATE;
    uint32_t max_output_samples = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(client->input_resampler,
                                               (uint32_t)source_samples,
                                               &max_output_samples) != ESP_AE_ERR_OK ||
        max_output_samples < (uint32_t)target_samples) {
        return;
    }
    if (!wake_buffers_reserve((size_t)source_samples,
                              (size_t)max_output_samples)) {
        ESP_LOGW(TAG, "wake buffers unavailable");
        return;
    }

    const int read = esp_codec_dev_read((esp_codec_dev_handle_t)client->microphone,
                                        s_wake_source,
                                        source_samples * 2 * (int)sizeof(int16_t));
    if (read == ESP_CODEC_DEV_OK) {
        xiaozhi_pcm_downmix_stereo(s_wake_source, (size_t)source_samples);
        xiaozhi_pcm_amplify(s_wake_source, (size_t)source_samples, XIAOZHI_INPUT_GAIN);
        uint32_t output_samples = max_output_samples;
        if (esp_ae_rate_cvt_process(client->input_resampler,
                                    (esp_ae_sample_t)s_wake_source,
                                    (uint32_t)source_samples,
                                    (esp_ae_sample_t)s_wake_chunk,
                                    &output_samples) == ESP_AE_ERR_OK &&
            output_samples >= (uint32_t)target_samples) {
            bool detected = false;
            if (mn_iface != NULL) {
                const esp_mn_state_t state = mn_iface->detect(mn_data, s_wake_chunk);
                if (state == ESP_MN_STATE_DETECTED) {
                    esp_mn_results_t *results = mn_iface->get_results(mn_data);
                    if (results != NULL && results->num > 0) {
                        detected = true;
                        ESP_LOGI(TAG, "wake word detected: command_id=%d string=%s prob=%.3f",
                                 results->command_id[0], results->string,
                                 (double)results->prob[0]);
                    }
                    mn_iface->clean(mn_data);
                } else if (state == ESP_MN_STATE_TIMEOUT) {
                    mn_iface->clean(mn_data);
                }
            }
            if (wn_iface != NULL) {
                const wakenet_state_t wn_state = wn_iface->detect(wn_data, s_wake_chunk);
                if (wn_state > 0) {
                    detected = true;
                    ESP_LOGI(TAG, "wake word detected: 你好小智");
                }
            }
            if (detected) {
                const bool interrupting_music =
                    client->state == XIAOZHI_STATE_MUSIC || client->music_task != NULL;
                if (interrupting_music) {
                    client->pending_music_ready = false;
                    atomic_store_explicit(&client->music_fade_requested, true,
                                          memory_order_release);
                    esp_http_client_handle_t music_http =
                        (esp_http_client_handle_t)client->music_http;
                    if (music_http != NULL) esp_http_client_close(music_http);
                    ESP_LOGI(TAG, "wake during music: fading out playback");
                }
                xiaozhi_client_notify_wake_word(client);
                if (interrupting_music) {
                    const uint32_t deadline = now_ms() + 250u;
                    while (client->music_task != NULL &&
                           (int32_t)(deadline - now_ms()) > 0) {
                        vTaskDelay(pdMS_TO_TICKS(5));
                    }
                    if (client->music_task != NULL) {
                        client->music_stop_requested = true;
                        ESP_LOGW(TAG, "music worker did not retire before listen");
                    } else {
                        ESP_LOGI(TAG, "music stopped before conversation");
                    }
                }
                if (client->connected && client->session_id[0] != '\0' &&
                    send_listen_mode(client, true, "auto")) {
                    ESP_LOGI(TAG, "auto listening started");
                } else {
                    client->wake_resume_pending = true;
                    ESP_LOGI(TAG, "wake queued until reconnect");
                }
            }
        }
    }
}

static void xiaozhi_start_wake_task(xiaozhi_client_t *client)
{
    if (client == NULL || client->wake_task != NULL || !client->running) return;
    TaskHandle_t wake = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "xz_wake", 6144, client, 4,
                                         &wake, 0,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        client->wake_task = wake;
    } else {
        ESP_LOGW(TAG, "wake task create failed");
    }
}

static void wake_task(void *argument)
{
    xiaozhi_client_t *client = (xiaozhi_client_t *)argument;
    if (!wake_word_start(client)) {
        if (client->wake_task == (void *)xTaskGetCurrentTaskHandle()) {
            client->wake_task = NULL;
        }
        vTaskDeleteWithCaps(NULL);
        return;
    }
    /* Last line of defence for voice control: the websocket library reconnects
       transport errors by itself, and clean server closes once
       enable_close_reconnect is set. If the socket is still down half a minute
       after Wi-Fi came back, rebuild the session instead of staying silent
       until the next reboot. */
    uint32_t offline_since_ms = 0;
    while (client->running) {
        if (client->connected) {
            offline_since_ms = 0;
        } else if (offline_since_ms == 0) {
            offline_since_ms = now_ms();
        } else if ((uint32_t)(now_ms() - offline_since_ms) >= 30000 &&
                   connectivity_is_wifi_connected()) {
            offline_since_ms = now_ms();
            ESP_LOGW(TAG, "xiaozhi offline for 30s, rebuilding session");
            if (!open_websocket(client)) {
                ESP_LOGW(TAG, "xiaozhi session rebuild failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (client->wake_task == (void *)xTaskGetCurrentTaskHandle()) {
        client->wake_task = NULL;
    }
    vTaskDeleteWithCaps(NULL);
}

bool xiaozhi_client_start(xiaozhi_client_t *client)
{
    if (client == NULL || client->running) return false;
    if (client->activation_task != NULL || client->playback_task != NULL ||
        client->microphone_task != NULL || client->music_task != NULL ||
        client->wake_task != NULL) {
        /* A previous stop could not retire its workers (a stalled network call,
           for example). Reusing the client now would orphan them together with
           the socket they still hold, so refuse until they are really gone. */
        ESP_LOGE(TAG, "xiaozhi start refused: previous workers still alive");
        client->state = XIAOZHI_STATE_ERROR;
        return false;
    }
    client->running = true;
    client->state = client->activated ? XIAOZHI_STATE_CONNECTING : XIAOZHI_STATE_ACTIVATING;
    if (s_audio_queue == NULL) s_audio_queue = xQueueCreate(12, sizeof(audio_packet_t));
    if (client->activated && client->websocket_url[0] != '\0') {
        if (open_websocket(client)) {
            if (xiaozhi_start_workers(client) &&
                wake_word_start(client)) {
                TaskHandle_t wake = NULL;
                /* WakeNet inference is a continuous CPU burner; keep it on
                   core 0 so the face renderer on core 1 is never starved. */
                if (xTaskCreatePinnedToCoreWithCaps(wake_task, "xz_wake", 6144, client, 4,
                                                     &wake, 0,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
                    client->wake_task = wake;
                } else {
                    ESP_LOGW(TAG, "wake task create failed");
                }
                return true;
            }
            esp_websocket_client_stop((esp_websocket_client_handle_t)client->transport);
            esp_websocket_client_destroy((esp_websocket_client_handle_t)client->transport);
            client->transport = NULL;
            client->running = false;
            client->state = XIAOZHI_STATE_ERROR;
            return false;
        }
    }
    /* 8 KB covers the HTTP client and the TLS handshake; the response body
       lives in a file-scope buffer so it costs only internal DRAM, not stack.
       The stack stays internal: the activation flow touches the model loader,
       which freezes the caches and requires a cache-safe task stack. */
    TaskHandle_t activation = NULL;
    if (xTaskCreate(activation_task, "xz_act", 8192, client, 4, &activation) != pdPASS) {
        ESP_LOGE(TAG, "activation task create failed");
        client->running = false;
        client->state = XIAOZHI_STATE_ERROR;
        return false;
    }
    client->activation_task = activation;
    ESP_LOGI(TAG, "activation task started");
    return true;
}

void xiaozhi_client_stop(xiaozhi_client_t *client)
{
    if (client == NULL) return;
    client->running = false;
    client->listening = false;
    client->wake_resume_pending = false;
    client->music_stop_requested = true;
    atomic_store_explicit(&client->wake_reply_pending, false, memory_order_release);
    atomic_store_explicit(&client->wake_reply_playing, false, memory_order_release);
    esp_http_client_handle_t music_http = (esp_http_client_handle_t)client->music_http;
    if (music_http != NULL) {
        esp_http_client_close(music_http);
    }

    /* Workers poll client->running and talk through client->transport, so a
       restart must not create a second microphone/playback pair while the
       old one is still running, and the socket must not be destroyed while
       a worker can still send on it. */
    const TickType_t wait_started = xTaskGetTickCount();
    const TickType_t wait_limit = pdMS_TO_TICKS(3000);
    bool workers_gone = true;
    while (client->activation_task != NULL || client->playback_task != NULL ||
           client->microphone_task != NULL || client->music_task != NULL ||
           client->wake_task != NULL) {
        if ((TickType_t)(xTaskGetTickCount() - wait_started) >= wait_limit) {
            /* Destroying the socket now would pull it out from under a worker
               that is still using it. Leave both alive; the next start is
               refused until the workers really retire, which is safe. */
            ESP_LOGW(TAG, "xiaozhi stop timed out waiting for workers");
            workers_gone = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (workers_gone && client->transport != NULL) {
        esp_websocket_client_stop((esp_websocket_client_handle_t)client->transport);
        esp_websocket_client_destroy((esp_websocket_client_handle_t)client->transport);
        client->transport = NULL;
    }

    if (s_audio_queue != NULL) {
        audio_packet_t packet = {0};
        while (xQueueReceive(s_audio_queue, &packet, 0) == pdTRUE) {
            free(packet.data);
        }
    }

    wake_word_stop(client);
    client->connected = false;
    client->pending_audio_url[0] = '\0';
    client->state = XIAOZHI_STATE_IDLE;
}
bool xiaozhi_client_available(void)
{
    return true;
}

#else

bool xiaozhi_client_send_listen(xiaozhi_client_t *client, bool start)
{
    (void)client; (void)start; return false;
}

bool xiaozhi_client_send_abort(xiaozhi_client_t *client)
{
    (void)client; return false;
}

bool xiaozhi_client_start(xiaozhi_client_t *client)
{
    if (client != NULL) client->state = XIAOZHI_STATE_DISABLED;
    return false;
}

void xiaozhi_client_stop(xiaozhi_client_t *client)
{
    if (client != NULL) {
        client->running = false;
        client->state = XIAOZHI_STATE_DISABLED;
    }
}

bool xiaozhi_client_available(void)
{
    return false;
}

#endif/* Park a resolved song until speech ends. Playing immediately raced the
   assistant's own confirmation line: the TTS preemption then killed the song. */
void xiaozhi_client_queue_music(xiaozhi_client_t *client, const char *url, const char *title)
{
    if (client == NULL || url == NULL || url[0] == '\0') return;
    copy_bounded(client->pending_music_url, sizeof(client->pending_music_url), url);
    copy_bounded(client->pending_music_title, sizeof(client->pending_music_title),
                 title != NULL ? title : "");
    client->pending_music_ms = now_ms();
    client->pending_music_ready = true;
    ESP_LOGI(TAG, "music queued until speech ends: %s",
             client->pending_music_title[0] ? client->pending_music_title : "(untitled)");
}

/* Called often from the client's own task: start the queued song once the
   assistant is quiet, or after a short grace period if it never speaks. */
/* A queued song is only valid for a short window. Without an expiry, a request
   that never got released (the assistant kept talking, or the session stalled)
   stayed parked forever and then fired later - the user asked for one song,
   was told it failed, and a much older request suddenly started playing. */
#define XIAOZHI_PENDING_MUSIC_TTL_MS 12000u

void xiaozhi_client_cancel_pending_music(xiaozhi_client_t *client)
{
    if (client == NULL) return;
    if (client->pending_music_ready) {
        ESP_LOGI(TAG, "music queue cancelled: %s",
                 client->pending_music_title[0] ? client->pending_music_title : "(untitled)");
    }
    client->pending_music_ready = false;
    client->pending_music_url[0] = '\0';
    client->pending_music_title[0] = '\0';
}

void xiaozhi_client_tick_pending_music(xiaozhi_client_t *client)
{
    if (client == NULL || !client->pending_music_ready) return;
    /* Never start a song while the assistant is speaking: that is exactly the
       overlap that made the song cut out. Wait for the spoken reply to end. */
    if (client->state == XIAOZHI_STATE_SPEAKING) {
        if ((uint32_t)(now_ms() - client->pending_music_ms) >= XIAOZHI_PENDING_MUSIC_TTL_MS) {
            xiaozhi_client_cancel_pending_music(client);
        }
        return;
    }
    /* Give the assistant a moment to begin its confirmation line; otherwise the
       song can still start a few milliseconds before the speech does. */
    const uint32_t age = (uint32_t)(now_ms() - client->pending_music_ms);
    if (age < 900u) return;
    if (age >= XIAOZHI_PENDING_MUSIC_TTL_MS) {
        ESP_LOGW(TAG, "music queue expired after %u ms; dropping", (unsigned)age);
        xiaozhi_client_cancel_pending_music(client);
        return;
    }

    client->pending_music_ready = false;
    const char *url = client->pending_music_url;
    const char *title = client->pending_music_title;
    ESP_LOGI(TAG, "music queue released -> %s", title[0] ? title : "(untitled)");
    (void)xiaozhi_client_play_url(client, url, title);
}


