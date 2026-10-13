#include "xiaozhi_client.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static void copy_bounded(char *dst, size_t size, const char *src)
{
    if (dst == NULL || size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < size && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

typedef struct {
    const char *server_state; /* xiaozhi chat "state" value */
    xiaozhi_session_state_t local;
    const char *emotion_id;
} state_map_t;

/* Server chat states (documented in xiaozhi websocket.md) -> local state. */
static const state_map_t k_state_map[] = {
    { "listening", XIAOZHI_STATE_LISTENING, "35" },
    { "thinking", XIAOZHI_STATE_THINKING, "30" },
    { "speaking", XIAOZHI_STATE_SPEAKING, "39" },
    { "idle", XIAOZHI_STATE_IDLE, NULL },
    { "connecting", XIAOZHI_STATE_CONNECTING, "36" },
    { "error", XIAOZHI_STATE_ERROR, "34" },
};

void xiaozhi_client_init(xiaozhi_client_t *client)
{
    if (client == NULL) {
        return;
    }
    memset(client, 0, sizeof(*client));
    client->state = XIAOZHI_STATE_DISABLED;
}

void xiaozhi_client_configure(xiaozhi_client_t *client, const xiaozhi_config_t *config)
{
    if (client == NULL || config == NULL) {
        return;
    }
    client->running = false;
    client->connected = false;
    client->state = XIAOZHI_STATE_IDLE;
    client->last_emotion[0] = '\0';
    client->last_text[0] = '\0';
    client->session_id[0] = '\0';
}

xiaozhi_session_state_t xiaozhi_client_state(const xiaozhi_client_t *client)
{
    if (client == NULL) {
        return XIAOZHI_STATE_DISABLED;
    }
    return client->state;
}

const char *xiaozhi_client_last_emotion(const xiaozhi_client_t *client)
{
    if (client == NULL || client->last_emotion[0] == '\0') {
        return NULL;
    }
    return client->last_emotion;
}

const char *xiaozhi_client_state_emotion(xiaozhi_session_state_t state)
{
    switch (state) {
    case XIAOZHI_STATE_CONNECTING: return "36";
    case XIAOZHI_STATE_LISTENING:  return "35";
    case XIAOZHI_STATE_THINKING:   return "30";
    case XIAOZHI_STATE_SPEAKING:   return "39";
    case XIAOZHI_STATE_ERROR:      return "34";
    default:                       return NULL;
    }
}

static bool map_server_state(const char *value, xiaozhi_session_state_t *out)
{
    if (value == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_state_map) / sizeof(k_state_map[0]); ++i) {
        if (strcmp(k_state_map[i].server_state, value) == 0) {
            *out = k_state_map[i].local;
            return true;
        }
    }
    return false;
}

static void set_emotion(xiaozhi_client_t *client, const char *name)
{
    if (name == NULL) {
        return;
    }
    copy_bounded(client->last_emotion, sizeof(client->last_emotion), name);
}

bool xiaozhi_client_handle_text(xiaozhi_client_t *client, const char *json)
{
    if (client == NULL || json == NULL) {
        return false;
    }
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }

    bool changed = false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");

    /* Documented server emotion event: {"type":"llm","emotion":"happy",...}. */
    const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    if (cJSON_IsString(emotion) && emotion->valuestring != NULL) {
        set_emotion(client, emotion->valuestring);
        changed = true;
    }

    /* Chat state changes (listening/thinking/speaking). */
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    xiaozhi_session_state_t mapped;
    if (cJSON_IsString(state) && map_server_state(state->valuestring, &mapped)) {
        if (client->state != mapped) {
            client->state = mapped;
            changed = true;
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "hello") == 0) {
        client->connected = true;
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        if (cJSON_IsString(session) && session->valuestring != NULL) {
            copy_bounded(client->session_id, sizeof(client->session_id), session->valuestring);
        }
        changed = true;
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "stt") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            copy_bounded(client->last_text, sizeof(client->last_text), text->valuestring);
        }
    }

    cJSON_Delete(root);
    return changed;
}

/* ---- Transport ---------------------------------------------------------
   The socket loop is only compiled when xiaozhi support is enabled, so the
   default build has no WebSocket dependency. */

#if CONFIG_XIAOZHI_ENABLE
#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "xiaozhi";
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void ws_event_handler(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    xiaozhi_client_t *client = (xiaozhi_client_t *)handler_arg;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        client->connected = true;
        client->state = XIAOZHI_STATE_IDLE;
        ESP_LOGI(TAG, "websocket connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        client->connected = false;
        client->state = XIAOZHI_STATE_ERROR;
        ESP_LOGW(TAG, "websocket disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data != NULL && data->op_code == 0x01 /* text frame */) {
            char buffer[512];
            const int length = data->data_len < (int)sizeof(buffer) - 1 ? data->data_len : (int)sizeof(buffer) - 1;
            memcpy(buffer, data->data_ptr, length);
            buffer[length] = '\0';
            xiaozhi_client_handle_text(client, buffer);
        }
        break;
    default:
        break;
    }
}

bool xiaozhi_client_start(xiaozhi_client_t *client)
{
    if (client == NULL || client->running) {
        return false;
    }
    client->state = XIAOZHI_STATE_CONNECTING;
    /* The concrete esp_websocket_client start is wired here once the xiaozhi
       server URL and token are provisioned. Kept minimal on purpose. */
    client->running = true;
    return true;
}

void xiaozhi_client_stop(xiaozhi_client_t *client)
{
    if (client == NULL) {
        return;
    }
    client->running = false;
    client->connected = false;
    client->state = XIAOZHI_STATE_IDLE;
}

bool xiaozhi_client_available(void)
{
    return true;
}

#else

bool xiaozhi_client_start(xiaozhi_client_t *client)
{
    if (client != NULL) {
        client->state = XIAOZHI_STATE_DISABLED;
    }
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
