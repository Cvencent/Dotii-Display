#include "xiaozhi_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "connectivity.h"
#include "device_identity.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE
#include "bsp/esp-bsp.h"
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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

typedef struct {
    const char *alias;
    const char *id;
} emotion_alias_t;

static const emotion_alias_t k_emotion_aliases[] = {
    { "happy", "10" },
    { "joy", "10" },
    { "excited", "10" },
    { "puzzled", "11" },
    { "sad", "12" },
    { "down", "12" },
    { "surprised", "13" },
    { "surprise", "13" },
    { "shy", "14" },
    { "tired", "15" },
    { "sleepy", "15" },
    { "focused", "16" },
    { "angry", "21" },
    { "thinking", "30" },
    { "done", "33" },
    { "success", "33" },
    { "error", "34" },
    { "listening", "35" },
    { "loading", "36" },
    { "replying", "39" },
    { "speaking", "39" },
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
    nvs_close(handle);
    client->activated = client->websocket_url[0] != '\0';
}

static void persist_activation(xiaozhi_client_t *client)
{
    nvs_handle_t handle;
    if (nvs_open(XIAOZHI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_str(handle, "ws_url", client->websocket_url);
    (void)nvs_set_str(handle, "token", client->token);
    (void)nvs_set_str(handle, "activation_code", client->activation_code);
    (void)nvs_set_str(handle, "activation_msg", client->activation_message);
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

const char *xiaozhi_client_state_emotion(xiaozhi_session_state_t state)
{
    switch (state) {
    case XIAOZHI_STATE_ACTIVATING: return "36";
    case XIAOZHI_STATE_CONNECTING: return "36";
    case XIAOZHI_STATE_LISTENING: return "35";
    case XIAOZHI_STATE_THINKING: return "30";
    case XIAOZHI_STATE_SPEAKING: return "39";
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
}

static bool map_server_state(const char *value, xiaozhi_session_state_t *out)
{
    if (value == NULL) return false;
    if (strcmp(value, "listening") == 0) *out = XIAOZHI_STATE_LISTENING;
    else if (strcmp(value, "thinking") == 0) *out = XIAOZHI_STATE_THINKING;
    else if (strcmp(value, "speaking") == 0) *out = XIAOZHI_STATE_SPEAKING;
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
            if (strcmp(tts_state->valuestring, "start") == 0) client->state = XIAOZHI_STATE_SPEAKING;
            else if (strcmp(tts_state->valuestring, "stop") == 0) client->state = XIAOZHI_STATE_IDLE;
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
static char s_rx_buffer[2048];
static size_t s_rx_length;

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
    if (cJSON_IsObject(activation)) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(activation, "code");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(activation, "message");
        if (cJSON_IsString(code) && code->valuestring != NULL) {
            copy_bounded(client->activation_code, sizeof(client->activation_code), code->valuestring);
            client->state = XIAOZHI_STATE_ACTIVATING;
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
            client->activated = true;
            client->activation_code[0] = '\0';
            persist_activation(client);
            complete = true;
        }
    }
    cJSON_Delete(root);
    return complete;
}

static bool fetch_ota(xiaozhi_client_t *client)
{
    if (starts_with(client->config.server_url, "ws://") ||
        starts_with(client->config.server_url, "wss://")) {
        copy_bounded(client->websocket_url, sizeof(client->websocket_url), client->config.server_url);
        client->activated = true;
        persist_activation(client);
        return true;
    }

    const char *url = client->config.server_url[0] != '\0' ?
        client->config.server_url : XIAOZHI_DEFAULT_OTA_URL;
    char response[4096] = {0};
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
    esp_http_client_set_header(http, "User-Agent", "Dotii/1.1.6");
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
        client->state = XIAOZHI_STATE_ERROR;
        ESP_LOGW(TAG, "websocket disconnected");
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
                xiaozhi_client_handle_text(client, s_rx_buffer);
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

static bool open_websocket(xiaozhi_client_t *client)
{
    if (client->websocket_url[0] == '\0') return false;
    esp_websocket_client_config_t config = {
        .uri = client->websocket_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 12000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_websocket_client_handle_t handle = esp_websocket_client_init(&config);
    if (handle == NULL) return false;
    esp_websocket_client_append_header(handle, "Protocol-Version", "1");
    esp_websocket_client_append_header(handle, "Device-Id", device_identity_get_mac());
    esp_websocket_client_append_header(handle, "Client-Id", device_identity_get_client_id());
    if (client->token[0] != '\0') {
        char authorization[192];
        snprintf(authorization, sizeof(authorization), "Bearer %s", client->token);
        esp_websocket_client_append_header(handle, "Authorization", authorization);
    }
    esp_websocket_register_events(handle, WEBSOCKET_EVENT_ANY, ws_event_handler, client);
    if (esp_websocket_client_start(handle) != ESP_OK) {
        esp_websocket_client_destroy(handle);
        return false;
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
        if (client->websocket_url[0] == '\0' && fetch_ota(client)) {
            ESP_LOGI(TAG, "xiaozhi activation complete");
        }
        if (client->websocket_url[0] != '\0') {
            open_websocket(client);
            break;
        }
        client->state = XIAOZHI_STATE_ACTIVATING;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    client->activation_task = NULL;
    vTaskDelete(NULL);
}

static bool audio_init(xiaozhi_client_t *client)
{
    if (client->speaker != NULL && client->microphone != NULL) return true;
    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    esp_codec_dev_handle_t microphone = bsp_audio_codec_microphone_init();
    if (speaker == NULL || microphone == NULL) return false;
    esp_codec_dev_sample_info_t info = {
        .sample_rate = XIAOZHI_CODEC_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(speaker, &info) != ESP_CODEC_DEV_OK) return false;
    if (esp_codec_dev_open(microphone, &info) != ESP_CODEC_DEV_OK) return false;
    esp_codec_dev_set_out_vol(speaker, 80);
    client->speaker = speaker;
    client->microphone = microphone;
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
    vTaskDelete(NULL);
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
        vTaskDelete(NULL);
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
            esp_ae_rate_cvt_cfg_t config = {
                .src_rate = XIAOZHI_CODEC_RATE,
                .dest_rate = XIAOZHI_INPUT_RATE,
                .channel = 1,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .complexity = 2,
                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };
            if (esp_ae_rate_cvt_open(&config, &client->input_resampler) != ESP_AE_ERR_OK) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
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
    vTaskDelete(NULL);
}

bool xiaozhi_client_send_listen(xiaozhi_client_t *client, bool start)
{
    if (client == NULL || !client->connected || client->transport == NULL) return false;
    cJSON *root = cJSON_CreateObject();
    if (client->session_id[0] != '\0') cJSON_AddStringToObject(root, "session_id", client->session_id);
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", start ? "start" : "stop");
    cJSON_AddStringToObject(root, "mode", "manual");
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

bool xiaozhi_client_start(xiaozhi_client_t *client)
{
    if (client == NULL || client->running) return false;
    client->running = true;
    client->state = client->activated ? XIAOZHI_STATE_CONNECTING : XIAOZHI_STATE_ACTIVATING;
    if (s_audio_queue == NULL) s_audio_queue = xQueueCreate(12, sizeof(audio_packet_t));
    if (client->websocket_url[0] != '\0') {
        if (open_websocket(client)) {
            xTaskCreate(playback_task, "xz_play", 4096, client, 4, NULL);
            xTaskCreate(microphone_task, "xz_mic", 4096, client, 5, NULL);
            return true;
        }
    }
    if (xTaskCreate(activation_task, "xz_act", 6144, client, 4, NULL) != pdPASS) {
        client->running = false;
        client->state = XIAOZHI_STATE_ERROR;
        return false;
    }
    return true;
}

void xiaozhi_client_stop(xiaozhi_client_t *client)
{
    if (client == NULL) return;
    client->running = false;
    client->listening = false;
    if (client->transport != NULL) {
        esp_websocket_client_stop((esp_websocket_client_handle_t)client->transport);
        esp_websocket_client_destroy((esp_websocket_client_handle_t)client->transport);
        client->transport = NULL;
    }
    client->connected = false;
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
