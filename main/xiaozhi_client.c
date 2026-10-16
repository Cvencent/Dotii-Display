#include "xiaozhi_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

/* The message/emotion mapping above is plain C and is covered by the host
   tests in test_apps/emotion. Everything that needs the transport, the
   codec or the Wi-Fi stack stays behind this switch. */
#if CONFIG_XIAOZHI_ENABLE
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
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"
#include "esp_codec_dev.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif

#define XIAOZHI_NAMESPACE "xiaozhi"
#define XIAOZHI_DEFAULT_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#define XIAOZHI_FRAME_MS 60
#define XIAOZHI_INPUT_RATE 16000
#define XIAOZHI_CODEC_RATE 24000

static const char *TAG = "xiaozhi";

static bool xiaozhi_start_music(xiaozhi_client_t *client);
static bool audio_init(xiaozhi_client_t *client);
static void wake_task(void *argument);

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

/* English/label synonyms an LLM is likely to send, mapped onto the local
   catalog. The catalog itself carries all 32 aora-bot expressions; this table
   only widens what the model may call them. */
static const emotion_alias_t k_emotion_aliases[] = {
    { "sleeping", "00" },
    { "sleep", "00" },
    { "waking", "01" },
    { "idle", "02" },
    { "neutral", "02" },
    { "curious", "03" },
    { "spacing_out", "04" },
    { "booting", "05" },
    { "dormant", "06" },
    { "shake_awake", "07" },
    { "happy", "10" },
    { "joy", "10" },
    { "excited", "10" },
    { "smile", "10" },
    { "puzzled", "11" },
    { "confused", "20" },
    { "sad", "12" },
    { "down", "12" },
    { "unhappy", "12" },
    { "surprised", "13" },
    { "surprise", "13" },
    { "shy", "14" },
    { "embarrassed", "14" },
    { "tired", "15" },
    { "sleepy", "15" },
    { "exhausted", "15" },
    { "focused", "16" },
    { "concentrating", "16" },
    { "panicked", "17" },
    { "panic", "17" },
    { "resigned", "18" },
    { "helpless", "18" },
    { "satisfied", "19" },
    { "content", "19" },
    { "angry", "21" },
    { "mad", "21" },
    { "thinking", "30" },
    { "receiving", "31" },
    { "busy", "32" },
    { "working", "32" },
    { "done", "33" },
    { "success", "33" },
    { "completed", "33" },
    { "error", "34" },
    { "failed", "34" },
    { "listening", "35" },
    { "loading", "36" },
    { "connecting", "36" },
    { "recalling", "37" },
    { "refusing", "38" },
    { "refused", "38" },
    { "replying", "39" },
    { "speaking", "39" },
    { "searching", "40" },
    { "powering_off", "41" },
    { "stopping", "41" },
    { "music", "42" },
    { "playing_music", "42" },
};

static const char *normalize_emotion(const char *name)
{
    if (name == NULL || name[0] == '\0') return NULL;
    for (size_t i = 0; i < sizeof(k_emotion_aliases) / sizeof(k_emotion_aliases[0]); ++i) {
        if (strcmp(k_emotion_aliases[i].alias, name) == 0) return k_emotion_aliases[i].id;
    }
    return name;
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
    client->state = XIAOZHI_STATE_DISABLED;
    client->server_sample_rate = 24000;
    client->server_frame_duration = XIAOZHI_FRAME_MS;
    load_persisted(client);
}

void xiaozhi_client_configure(xiaozhi_client_t *client, const xiaozhi_config_t *config)
{
    if (client == NULL || config == NULL) return;
    const bool server_changed = client->config.server_url[0] != '\0' &&
                                strcmp(client->config.server_url, config->server_url) != 0;
    client->config = *config;
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
    case XIAOZHI_STATE_ACTIVATING: return "36";
    case XIAOZHI_STATE_CONNECTING: return "36";
    case XIAOZHI_STATE_LISTENING: return "35";
    case XIAOZHI_STATE_THINKING: return "30";
    case XIAOZHI_STATE_SPEAKING: return "39";
    case XIAOZHI_STATE_MUSIC: return "42";
    case XIAOZHI_STATE_ERROR: return "34";
    default: return NULL;
    }
}

static void set_emotion(xiaozhi_client_t *client, const char *name)
{
    const char *normalized = normalize_emotion(name);
    if (normalized == NULL) return;
    copy_bounded(client->last_emotion, sizeof(client->last_emotion), normalized);
    client->emotion_until_ms = now_ms() + 5000;
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

bool xiaozhi_client_handle_text(xiaozhi_client_t *client, const char *json)
{
    if (client == NULL || json == NULL) return false;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return false;

    bool changed = false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type_value = cJSON_IsString(type) ? type->valuestring : NULL;

    const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    if (cJSON_IsString(emotion) && emotion->valuestring != NULL) {
        set_emotion(client, emotion->valuestring);
        changed = true;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    xiaozhi_session_state_t mapped;
    if (cJSON_IsString(state) && map_server_state(state->valuestring, &mapped)) {
        client->listening = mapped == XIAOZHI_STATE_LISTENING;
        if (client->state != mapped) {
            client->state = mapped;
            changed = true;
        }
    }

    if (type_value != NULL && strcmp(type_value, "hello") == 0) {
        client->connected = true;
        client->state = XIAOZHI_STATE_IDLE;
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
            copy_bounded(client->last_text, sizeof(client->last_text), text->valuestring);
            changed = true;
        }
    } else if (type_value != NULL && strcmp(type_value, "llm") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            copy_bounded(client->last_text, sizeof(client->last_text), text->valuestring);
        }
        changed = true;
    } else if (type_value != NULL && strcmp(type_value, "tts") == 0) {
        const cJSON *tts_state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(tts_state) && tts_state->valuestring != NULL) {
            if (strcmp(tts_state->valuestring, "start") == 0) {
                client->listening = false;
                client->state = XIAOZHI_STATE_SPEAKING;
            }
            else if (strcmp(tts_state->valuestring, "stop") == 0) client->state = XIAOZHI_STATE_IDLE;
            changed = true;
        }
    } else if (type_value != NULL && strcmp(type_value, "notify") == 0) {
        const cJSON *audio_url = cJSON_GetObjectItemCaseSensitive(root, "audio_url");
        if (cJSON_IsString(audio_url) && audio_url->valuestring != NULL &&
            audio_url->valuestring[0] != '\0') {
            copy_bounded(client->pending_audio_url, sizeof(client->pending_audio_url),
                         audio_url->valuestring);
            client->listening = false;
            client->state = XIAOZHI_STATE_MUSIC;
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
       handshake on this call needs the room). */
    static char response[4096];
    response[0] = '\0';
    ota_body_t body = {.body = response, .capacity = sizeof(response)};
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
    if (http == NULL) return false;
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
        return false;
    }
    ESP_LOGI(TAG, "OTA reply %d bytes", (int)body.length);
    return parse_ota_response(client, response);
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
    cJSON_AddBoolToObject(features, "mcp", client->config.mcp_enabled);
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
        client->state = XIAOZHI_STATE_IDLE;
        send_hello(client);
        ESP_LOGI(TAG, "websocket connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        client->connected = false;
        client->listening = false;
        client->state = XIAOZHI_STATE_ERROR;
        ESP_LOGW(TAG, "websocket disconnected");
        break;
    case WEBSOCKET_EVENT_CLOSED:
        /* The service ends idle sessions with a clean close handshake. With
           enable_close_reconnect the client reconnects and re-sends hello on
           its own, so report "connecting" instead of a hard error; the wake
           task below rebuilds the socket if that reconnect never lands. */
        client->connected = false;
        client->listening = false;
        client->state = XIAOZHI_STATE_CONNECTING;
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
                if (xiaozhi_client_handle_text(client, s_rx_buffer) &&
                    client->state == XIAOZHI_STATE_MUSIC) {
                    xiaozhi_start_music(client);
                }
                s_rx_length = 0;
            }
        } else if (data->op_code == 0x02 && data->data_len > 0 && s_audio_queue != NULL) {
            audio_packet_t packet = {0};
            packet.data = malloc((size_t)data->data_len);
            if (packet.data != NULL) {
                packet.length = (size_t)data->data_len;
                memcpy(packet.data, data->data_ptr, packet.length);
                if (xQueueSend(s_audio_queue, &packet, 0) != pdTRUE) free(packet.data);
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
    TaskHandle_t handle = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(music_task, "xz_music", 6144, client, 4,
                                         &handle, tskNO_AFFINITY,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "music task create failed");
        client->state = XIAOZHI_STATE_IDLE;
        client->pending_audio_url[0] = '\0';
        return false;
    }
    client->music_task = handle;
    return true;
}

static esp_audio_simple_dec_type_t music_decoder_type(const char *url)
{
    const char *end = strchr(url, '?');
    if (end == NULL) end = url + strlen(url);
    const char *dot = NULL;
    for (const char *cursor = url; cursor < end; ++cursor) {
        if (*cursor == '.') dot = cursor;
    }
    if (dot != NULL) {
        if (strncasecmp(dot, ".mp3", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        if (strncasecmp(dot, ".wav", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        if (strncasecmp(dot, ".m4a", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
        if (strncasecmp(dot, ".ogg", 4) == 0 ||
            strncasecmp(dot, ".opus", 5) == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
        }
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
}

static bool music_decoders_ensure(void)
{
    if (s_audio_decoders_ready) return true;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    s_audio_decoders_ready = true;
    return true;
}

static bool music_write_pcm(xiaozhi_client_t *client,
                            esp_ae_rate_cvt_handle_t *resampler,
                            int *resampler_rate,
                            const uint8_t *data, uint32_t size,
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

    const int16_t *source = (const int16_t *)data;
    int16_t *mono = NULL;
    if (channels == 2) {
        mono = malloc((size_t)frames * sizeof(int16_t));
        if (mono == NULL) return false;
        for (int index = 0; index < frames; ++index) {
            const int mixed = (int)source[index * 2] + (int)source[index * 2 + 1];
            mono[index] = (int16_t)(mixed / 2);
        }
    }
    const int16_t *pcm = mono != NULL ? mono : source;

    bool ok = true;
    if (info->sample_rate == XIAOZHI_CODEC_RATE) {
        ok = esp_codec_dev_write((esp_codec_dev_handle_t)client->speaker, pcm,
                                 frames * (int)sizeof(int16_t)) == ESP_CODEC_DEV_OK;
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
            int16_t *resampled = malloc((size_t)max_samples * sizeof(int16_t));
            if (resampled == NULL) {
                ok = false;
            } else {
                uint32_t produced = max_samples;
                if (esp_ae_rate_cvt_process(*resampler, (esp_ae_sample_t)pcm,
                                            (uint32_t)frames, (esp_ae_sample_t)resampled,
                                            &produced) != ESP_AE_ERR_OK) {
                    ok = false;
                } else if (produced > 0) {
                    ok = esp_codec_dev_write((esp_codec_dev_handle_t)client->speaker,
                                             resampled,
                                             (int)(produced * sizeof(int16_t))) == ESP_CODEC_DEV_OK;
                }
                free(resampled);
            }
        }
    }
    free(mono);
    return ok;
}

static void music_task(void *argument)
{
    xiaozhi_client_t *client = (xiaozhi_client_t *)argument;
    char url[sizeof(client->pending_audio_url)];
    copy_bounded(url, sizeof(url), client->pending_audio_url);

    esp_http_client_handle_t http = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_ae_rate_cvt_handle_t resampler = NULL;
    int resampler_rate = 0;
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    size_t input_capacity = 4096;
    size_t input_length = 0;
    size_t output_capacity = 32768;
    bool eos = false;
    bool success = false;

    if (!audio_init(client) || !music_decoders_ensure()) {
        ESP_LOGE(TAG, "music audio setup failed");
        goto cleanup;
    }

    input = heap_caps_malloc(input_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    output = heap_caps_malloc(output_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || output == NULL) goto cleanup;

    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = music_decoder_type(url),
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&decoder_config, &decoder) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "music decoder open failed");
        goto cleanup;
    }

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 5000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    http = esp_http_client_init(&http_config);
    if (http == NULL) goto cleanup;
    client->music_http = http;
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

    int flush_attempts = 0;
    while (client->running && !client->music_stop_requested) {
        if (input_length < input_capacity && !eos) {
            int read = esp_http_client_read(http, (char *)input + input_length,
                                            (int)(input_capacity - input_length));
            if (read > 0) {
                input_length += (size_t)read;
            } else if (read == 0) {
                eos = true;
            } else {
                ESP_LOGE(TAG, "music stream read failed");
                break;
            }
        }

        if (input_length == 0 && eos) {
            success = true;
            break;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input,
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
            memmove(input, input + raw.consumed, input_length - raw.consumed);
            input_length -= raw.consumed;
            flush_attempts = 0;
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
        }

        if (raw.consumed == 0 && out.decoded_size == 0) {
            if (input_length == input_capacity && !eos) {
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
    if (client->music_http == http) client->music_http = NULL;
    if (http != NULL) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
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
    if (client->music_task == (void *)xTaskGetCurrentTaskHandle()) {
        client->music_task = NULL;
    }
    vTaskDeleteWithCaps(NULL);
}

static bool wake_word_start(xiaozhi_client_t *client)
{
    if (client == NULL || client->wake_ready) return client != NULL && client->wake_ready;
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL || models->num <= 0) {
        ESP_LOGW(TAG, "wake model partition unavailable");
        if (models != NULL) esp_srmodel_deinit(models);
        return false;
    }
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (model_name == NULL) model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    const esp_wn_iface_t *iface = model_name != NULL ?
        esp_wn_handle_from_name(model_name) : NULL;
    model_iface_data_t *data = iface != NULL ?
        iface->create(model_name, DET_MODE_95) : NULL;
    if (data == NULL || iface->get_samp_rate(data) != XIAOZHI_INPUT_RATE) {
        ESP_LOGW(TAG, "wake model initialization failed");
        if (data != NULL) iface->destroy(data);
        esp_srmodel_deinit(models);
        return false;
    }
    client->wake_models = models;
    client->wake_iface = (void *)iface;
    client->wake_data = data;
    client->wake_ready = true;
    ESP_LOGI(TAG, "wake word ready: %s", model_name != NULL ? model_name : "unknown");
    return true;
}

static void wake_word_stop(xiaozhi_client_t *client)
{
    if (client == NULL) return;
    if (client->wake_data != NULL && client->wake_iface != NULL) {
        const esp_wn_iface_t *iface = (const esp_wn_iface_t *)client->wake_iface;
        iface->destroy((model_iface_data_t *)client->wake_data);
    }
    if (client->wake_models != NULL) esp_srmodel_deinit((srmodel_list_t *)client->wake_models);
    client->wake_data = NULL;
    client->wake_iface = NULL;
    client->wake_models = NULL;
    client->wake_ready = false;
}
static void playback_task(void *argument);
static void microphone_task(void *argument);
static bool audio_init(xiaozhi_client_t *client);
static void xiaozhi_start_wake_task(xiaozhi_client_t *client);

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
        if (xTaskCreatePinnedToCoreWithCaps(playback_task, "xz_play", 4096, client, 4,
                                             &handle, tskNO_AFFINITY,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
            client->playback_task = handle;
        }
    }
    if (client->microphone_task == NULL) {
        TaskHandle_t handle = NULL;
        if (xTaskCreatePinnedToCoreWithCaps(microphone_task, "xz_mic", 4096, client, 5,
                                             &handle, tskNO_AFFINITY,
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
        .task_stack = 4096,
        /* The service closes idle sessions cleanly; without this the client
           stops for good after the first server-side close. */
        .enable_close_reconnect = true,
    };
    esp_websocket_client_handle_t handle = esp_websocket_client_init(&config);
    if (handle == NULL) return false;
    /* The library allocates its task on internal RAM. If the radio and
       panel have momentarily starved the pool, wait and rebuild once
       instead of leaving the client permanently unconnected. */
    if (client->transport != NULL) {
        esp_websocket_client_stop((esp_websocket_client_handle_t)client->transport);
        esp_websocket_client_destroy((esp_websocket_client_handle_t)client->transport);
        client->transport = NULL;
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
    if (client->speaker == NULL) client->speaker = bsp_audio_codec_speaker_init();
    if (client->speaker == NULL) return false;
    if (client->microphone == NULL) client->microphone = bsp_audio_codec_microphone_init();
    if (client->microphone == NULL) return false;

    esp_codec_dev_sample_info_t info = {
        .sample_rate = XIAOZHI_CODEC_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open((esp_codec_dev_handle_t)client->speaker, &info) != ESP_CODEC_DEV_OK) {
        return false;
    }
    if (esp_codec_dev_open((esp_codec_dev_handle_t)client->microphone, &info) != ESP_CODEC_DEV_OK) {
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
    esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)client->speaker, 80);
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

static void playback_task(void *argument)
{
    xiaozhi_client_t *client = (xiaozhi_client_t *)argument;
    while (client->running) {
        audio_packet_t packet = {0};
        if (xQueueReceive(s_audio_queue, &packet, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (packet.data == NULL) continue;
        if (audio_init(client) && ensure_decoder(client, client->server_sample_rate)) {
            int frame_samples = client->server_sample_rate * XIAOZHI_FRAME_MS / 1000;
            int16_t *pcm = malloc((size_t)frame_samples * sizeof(int16_t));
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
                        int16_t *resampled = malloc((size_t)max_samples * sizeof(int16_t));
                        if (resampled != NULL) {
                            uint32_t produced = max_samples;
                            if (esp_ae_rate_cvt_process(client->output_resampler, (esp_ae_sample_t)pcm,
                                                        (uint32_t)decoded_samples, (esp_ae_sample_t)resampled,
                                                        &produced) == ESP_AE_ERR_OK) {
                                esp_codec_dev_write((esp_codec_dev_handle_t)client->speaker, resampled,
                                                    (int)(produced * sizeof(int16_t)));
                            }
                            free(resampled);
                        }
                    } else {
                        esp_codec_dev_write((esp_codec_dev_handle_t)client->speaker, pcm, out.decoded_size);
                    }
                }
                free(pcm);
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
    int16_t *source = malloc((size_t)source_samples * sizeof(int16_t));
    int16_t *resampled = malloc((size_t)XIAOZHI_INPUT_RATE * sizeof(int16_t));
    uint8_t *encoded = malloc(2048);
    if (source == NULL || resampled == NULL || encoded == NULL) {
        free(source); free(resampled); free(encoded);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    while (client->running) {
        if (!client->listening || !client->connected || !audio_init(client) || !ensure_encoder(client)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int read = esp_codec_dev_read((esp_codec_dev_handle_t)client->microphone, source,
                                      source_samples * (int)sizeof(int16_t));
        if (read != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
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

static void xiaozhi_detect_wake_word(xiaozhi_client_t *client)
{
    if (client == NULL || !client->wake_ready || !client->running ||
        !client->connected || client->listening || client->music_task != NULL ||
        client->state == XIAOZHI_STATE_SPEAKING ||
        client->state == XIAOZHI_STATE_THINKING ||
        client->state == XIAOZHI_STATE_MUSIC) {
        return;
    }
    if (!audio_init(client)) return;

    const esp_wn_iface_t *iface = (const esp_wn_iface_t *)client->wake_iface;
    model_iface_data_t *data = (model_iface_data_t *)client->wake_data;
    const int target_samples = iface->get_samp_chunksize(data);
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
    int16_t *chunk = malloc((size_t)max_output_samples * sizeof(int16_t));
    int16_t *source = malloc((size_t)source_samples * sizeof(int16_t));
    if (chunk == NULL || source == NULL) {
        free(chunk);
        free(source);
        return;
    }

    const int read = esp_codec_dev_read((esp_codec_dev_handle_t)client->microphone,
                                        source, source_samples * (int)sizeof(int16_t));
    if (read == ESP_CODEC_DEV_OK) {
        uint32_t output_samples = max_output_samples;
        if (esp_ae_rate_cvt_process(client->input_resampler,
                                    (esp_ae_sample_t)source, (uint32_t)source_samples,
                                    (esp_ae_sample_t)chunk,
                                    &output_samples) == ESP_AE_ERR_OK &&
            output_samples >= (uint32_t)target_samples) {
            const wakenet_state_t detected = iface->detect(data, chunk);
            if (detected > 0) {
                ESP_LOGI(TAG, "wake word detected: 你好小智");
                if (send_listen_mode(client, true, "auto")) {
                    ESP_LOGI(TAG, "auto listening started");
                }
            }
        }
    }
    free(chunk);
    free(source);
}

static void xiaozhi_start_wake_task(xiaozhi_client_t *client)
{
    if (client == NULL || client->wake_task != NULL || !client->running) return;
    TaskHandle_t wake = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "xz_wake", 6144, client, 4,
                                         &wake, tskNO_AFFINITY,
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
        xiaozhi_detect_wake_word(client);
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
                if (xTaskCreatePinnedToCoreWithCaps(wake_task, "xz_wake", 6144, client, 4,
                                                     &wake, tskNO_AFFINITY,
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
       lives in a file-scope buffer so it costs only internal DRAM, not stack. */
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
    client->music_stop_requested = true;
    esp_http_client_handle_t music_http = (esp_http_client_handle_t)client->music_http;
    if (music_http != NULL) {
        esp_http_client_close(music_http);
    }

    /* Workers poll client->running and talk through client->transport, so a
       restart must not create a second microphone/playback pair while the
       old one is still running, and the socket must not be destroyed while
       a worker can still send on it. */
    while (client->activation_task != NULL || client->playback_task != NULL ||
           client->microphone_task != NULL || client->music_task != NULL ||
           client->wake_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (client->transport != NULL) {
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

#endif
