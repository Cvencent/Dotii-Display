#include "connectivity.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "cJSON.h"
#include "codex_snapshot_merge.h"
#include "device_config.h"
#include "esp_crt_bundle.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "management_snapshot.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define REFRESH_REQUESTED_BIT BIT1
#define HTTP_BODY_INITIAL_CAPACITY 49152
#define CUSTOM_IMAGE_WIDTH 466
#define CUSTOM_IMAGE_HEIGHT 466
#define CUSTOM_IMAGE_BYTES (CUSTOM_IMAGE_WIDTH * CUSTOM_IMAGE_HEIGHT * 2)
#define BAMBU_CAMERA_WIDTH 320
#define BAMBU_CAMERA_HEIGHT 180
#define BAMBU_CAMERA_BYTES (BAMBU_CAMERA_WIDTH * BAMBU_CAMERA_HEIGHT * 2)

static const char *TAG = "connectivity";
static EventGroupHandle_t s_events;
static bool s_wifi_connected;
static bool s_bridge_online;
static bool s_codex_was_online;
static char s_ip[24] = "--";
static char s_bridge_note[48] = "正在启动";
/* Snapshot payloads are large and do not participate in DMA. Keep them in
   PSRAM so the Wi-Fi driver retains enough internal RAM for its RX buffers. */
EXT_RAM_BSS_ATTR static codex_snapshot_t s_last_snapshot;
EXT_RAM_BSS_ATTR static codex_snapshot_t s_work_snapshot;
EXT_RAM_BSS_ATTR static codex_snapshot_t s_management_snapshot;
static bool s_have_snapshot;
static uint8_t *s_custom_image_buffers[2];
static uint32_t s_custom_image_revisions[2];
static int s_custom_image_active = -1;
static uint8_t *s_bambu_camera_buffers[2];
static uint32_t s_bambu_camera_revisions[2];
static int s_bambu_camera_active = -1;
static QueueHandle_t s_bambu_command_queue;
static codex_task_detail_t *s_work_tasks;
static size_t s_work_task_count;
static SemaphoreHandle_t s_state_lock;

typedef enum {
    BAMBU_COMMAND_PAUSE = 1,
    BAMBU_COMMAND_RESUME,
    BAMBU_COMMAND_STOP,
} bambu_command_t;

typedef struct {
    char *body;
    size_t capacity;
    size_t length;
    bool overflow;
} http_body_t;

static void blacken_custom_frame_near_black_edge(uint8_t *data)
{
    const int edge = 12;
    for (int y = 0; y < CUSTOM_IMAGE_HEIGHT; ++y) {
        for (int x = 0; x < CUSTOM_IMAGE_WIDTH; ++x) {
            if (x >= edge && x < CUSTOM_IMAGE_WIDTH - edge &&
                y >= edge && y < CUSTOM_IMAGE_HEIGHT - edge) {
                continue;
            }
            size_t offset = ((size_t)y * CUSTOM_IMAGE_WIDTH + x) * 2;
            uint16_t pixel = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
            uint16_t red = (pixel >> 11) & 0x1F;
            uint16_t green = (pixel >> 5) & 0x3F;
            uint16_t blue = pixel & 0x1F;
            if (red <= 1 && green <= 2 && blue <= 1) {
                data[offset] = 0;
                data[offset + 1] = 0;
            }
        }
    }
}

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t length;
    bool overflow;
} image_body_t;

static size_t utf8_char_length(const char *text, size_t available)
{
    unsigned char lead = (unsigned char)text[0];
    size_t length = lead < 0x80 ? 1 :
                    (lead & 0xE0) == 0xC0 ? 2 :
                    (lead & 0xF0) == 0xE0 ? 3 :
                    (lead & 0xF8) == 0xF0 ? 4 : 1;
    if (length > available) return 0;
    for (size_t index = 1; index < length; ++index) {
        if (((unsigned char)text[index] & 0xC0) != 0x80) return 1;
    }
    return length;
}

static bool plain_append(char *destination, size_t destination_size, size_t *out,
                         const char *text, size_t length)
{
    for (size_t index = 0; index < length;) {
        size_t character_length = utf8_char_length(text + index, length - index);
        if (character_length == 0 || *out + character_length >= destination_size) return false;
        memcpy(destination + *out, text + index, character_length);
        *out += character_length;
        index += character_length;
    }
    return true;
}

static bool is_table_rule(const char *line, size_t length)
{
    size_t dashes = 0;
    for (size_t index = 0; index < length; ++index) {
        char ch = line[index];
        if (ch == '-') dashes++;
        else if (ch != '|' && ch != ':' && ch != ' ' && ch != '\t') return false;
    }
    return dashes >= 3;
}

/* LVGL labels intentionally stay plain-text.  Convert common Markdown
   structures into a compact reading view and never split a UTF-8 character
   when the small round-screen buffers fill. */
static void markdown_to_plain(const char *source, char *destination, size_t destination_size)
{
    if (destination_size == 0) return;
    destination[0] = '\0';
    if (source == NULL) return;

    size_t out = 0;
    bool in_code_fence = false;
    const char *line = source;
    while (*line != '\0' && out + 1 < destination_size) {
        const char *newline = strchr(line, '\n');
        size_t length = newline != NULL ? (size_t)(newline - line) : strlen(line);
        if (length > 0 && line[length - 1] == '\r') length--;

        size_t start = 0;
        while (start < length && (line[start] == ' ' || line[start] == '\t')) start++;
        size_t end = length;
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;

        bool fence = end - start >= 3 &&
                     ((line[start] == '`' && line[start + 1] == '`' && line[start + 2] == '`') ||
                      (line[start] == '~' && line[start + 1] == '~' && line[start + 2] == '~'));
        if (fence) {
            if (!in_code_fence) {
                if (out > 0 && destination[out - 1] != '\n') plain_append(destination, destination_size, &out, "\n", 1);
                plain_append(destination, destination_size, &out, "代码片段", strlen("代码片段"));
            }
            in_code_fence = !in_code_fence;
            goto next_line;
        }
        if (in_code_fence || start == end || is_table_rule(line + start, end - start)) goto next_line;

        if (out > 0 && destination[out - 1] != '\n' &&
            !plain_append(destination, destination_size, &out, "\n", 1)) break;

        while (start < end && line[start] == '#') start++;
        if (start < end && line[start] == ' ') start++;
        if (start < end && line[start] == '|') start++;
        if (end > start && line[end - 1] == '|') end--;

        if (start + 1 < end && (line[start] == '-' || line[start] == '*' || line[start] == '+') &&
            line[start + 1] == ' ') {
            start += 2;
            plain_append(destination, destination_size, &out, "• ", strlen("• "));
        } else if (start + 1 < end && line[start] == '>' && line[start + 1] == ' ') {
            start += 2;
            plain_append(destination, destination_size, &out, "引用：", strlen("引用："));
        }
        if (start + 2 < end && line[start] == '[' && line[start + 2] == ']' &&
            (line[start + 1] == ' ' || line[start + 1] == 'x' || line[start + 1] == 'X')) {
            plain_append(destination, destination_size, &out,
                         line[start + 1] == ' ' ? "待办 " : "完成 ",
                         strlen(line[start + 1] == ' ' ? "待办 " : "完成 "));
            start += 3;
            if (start < end && line[start] == ' ') start++;
        }

        for (size_t index = start; index < end && out + 1 < destination_size;) {
            char ch = line[index];
            if (ch == '\\' && index + 1 < end) {
                index++;
                size_t character_length = utf8_char_length(line + index, end - index);
                if (character_length == 0 || !plain_append(destination, destination_size, &out,
                                                            line + index, character_length)) break;
                index += character_length;
                continue;
            }
            if (ch == '!' && index + 1 < end && line[index + 1] == '[') {
                index++;
                plain_append(destination, destination_size, &out, "图片：", strlen("图片："));
                ch = '[';
            }
            if (ch == '[') {
                const char *close = find_bounded(line + index + 1, end - index - 1, ']');
                if (close != NULL && close + 1 < line + end && close[1] == '(') {
                    const char *url_end = find_bounded(close + 2, (size_t)(line + end - close - 2), ')');
                    if (url_end != NULL) {
                        size_t label_length = (size_t)(close - line - index - 1);
                        if (!plain_append(destination, destination_size, &out, line + index + 1, label_length)) break;
                        index = (size_t)(url_end - line) + 1;
                        continue;
                    }
                }
            }
            if (ch == '<') {
                const char *close = find_bounded(line + index + 1, end - index - 1, '>');
                if (close != NULL) {
                    if (strncmp(line + index + 1, "http://", 7) == 0 ||
                        strncmp(line + index + 1, "https://", 8) == 0) {
                        plain_append(destination, destination_size, &out, "链接", strlen("链接"));
                    }
                    index = (size_t)(close - line) + 1;
                    continue;
                }
            }
            if (ch == '`' || ch == '*' || ch == '~' ||
                (ch == '_' && index + 1 < end && line[index + 1] == '_')) {
                index += ch == '_' ? 2 : 1;
                continue;
            }
            if (ch == '|') {
                if (!plain_append(destination, destination_size, &out, " · ", strlen(" · "))) break;
                index++;
                continue;
            }
            if (ch == '\t') ch = ' ';
            if (ch == ' ' && out > 0 && destination[out - 1] == ' ') {
                index++;
                continue;
            }
            size_t character_length = utf8_char_length(line + index, end - index);
            if (character_length == 0 || !plain_append(destination, destination_size, &out,
                                                        line + index, character_length)) break;
            index += character_length;
        }

next_line:
        if (newline == NULL) break;
        line = newline + 1;
    }
    while (out > 0 && (destination[out - 1] == ' ' || destination[out - 1] == '\n')) out--;
    destination[out] = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_body_t *response = (http_body_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && response != NULL &&
        response->body != NULL && event->data_len > 0) {
        size_t required = response->length + (size_t)event->data_len + 1;
        if (required > response->capacity) {
            size_t capacity = response->capacity;
            while (capacity < required && capacity <= SIZE_MAX / 2) capacity *= 2;
            if (capacity < required) capacity = required;
            char *expanded = heap_caps_realloc(response->body, capacity,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (expanded == NULL) {
                response->overflow = true;
                return ESP_OK;
            }
            response->body = expanded;
            response->capacity = capacity;
        }
        memcpy(response->body + response->length, event->data, (size_t)event->data_len);
        response->length += (size_t)event->data_len;
        response->body[response->length] = '\0';
    }
    return ESP_OK;
}

static esp_err_t image_http_event_handler(esp_http_client_event_t *event)
{
    image_body_t *response = (image_body_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && response != NULL && event->data_len > 0) {
        size_t available = response->capacity - response->length;
        size_t count = (size_t)event->data_len;
        if (count > available) {
            count = available;
            response->overflow = true;
        }
        if (count > 0) {
            memcpy(response->data + response->length, event->data, count);
            response->length += count;
        }
    }
    return ESP_OK;
}

static bool fetch_custom_image(uint32_t revision)
{
    const device_config_values_t *device_config = device_config_get();
    for (size_t index = 0; index < 2; index++) {
        if (s_custom_image_buffers[index] != NULL && s_custom_image_revisions[index] == revision) {
            s_custom_image_active = (int)index;
            return true;
        }
    }
    char url[256];
    strlcpy(url, device_config->bridge_url, sizeof(url));
    char *last_slash = strrchr(url, '/');
    if (last_slash == NULL) return false;
    strlcpy(last_slash + 1, "custom-screen.rgb565", sizeof(url) - (size_t)(last_slash + 1 - url));

    int slot = s_custom_image_active == 0 ? 1 : 0;
    if (s_custom_image_buffers[slot] == NULL) {
        s_custom_image_buffers[slot] = heap_caps_malloc(CUSTOM_IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_custom_image_buffers[slot] == NULL) {
            ESP_LOGE(TAG, "Unable to allocate custom image buffer");
            return false;
        }
    }
    image_body_t response = {
        .data = s_custom_image_buffers[slot],
        .capacity = CUSTOM_IMAGE_BYTES,
    };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = image_http_event_handler,
        .user_data = &response,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return false;
    if (strlen(device_config->bridge_token) > 0) {
        esp_http_client_set_header(client, "X-Bridge-Token", device_config->bridge_token);
    }
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK || status != 200 || response.overflow || response.length != CUSTOM_IMAGE_BYTES) {
        ESP_LOGW(TAG, "Custom image request failed: %s, HTTP %d, bytes %u",
                 esp_err_to_name(error), status, (unsigned)response.length);
        return false;
    }
    blacken_custom_frame_near_black_edge(s_custom_image_buffers[slot]);
    s_custom_image_revisions[slot] = revision;
    s_custom_image_active = slot;
    ESP_LOGI(TAG, "Custom image received: revision %lu", (unsigned long)revision);
    return true;
}

static bool fetch_bambu_camera(uint32_t revision)
{
    const device_config_values_t *device_config = device_config_get();
    for (size_t index = 0; index < 2; index++) {
        if (s_bambu_camera_buffers[index] != NULL && s_bambu_camera_revisions[index] == revision) {
            s_bambu_camera_active = (int)index;
            return true;
        }
    }
    char url[256];
    strlcpy(url, device_config->bridge_url, sizeof(url));
    char *last_slash = strrchr(url, '/');
    if (last_slash == NULL) return false;
    strlcpy(last_slash + 1, "bambu-camera.rgb565", sizeof(url) - (size_t)(last_slash + 1 - url));
    int slot = s_bambu_camera_active == 0 ? 1 : 0;
    if (s_bambu_camera_buffers[slot] == NULL) {
        s_bambu_camera_buffers[slot] = heap_caps_malloc(BAMBU_CAMERA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_bambu_camera_buffers[slot] == NULL) return false;
    }
    image_body_t response = {.data = s_bambu_camera_buffers[slot], .capacity = BAMBU_CAMERA_BYTES};
    esp_http_client_config_t config = {
        .url = url, .event_handler = image_http_event_handler, .user_data = &response,
        .timeout_ms = 12000, .buffer_size = 4096, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return false;
    if (strlen(device_config->bridge_token) > 0) {
        esp_http_client_set_header(client, "X-Bridge-Token", device_config->bridge_token);
    }
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK || status != 200 || response.overflow || response.length != BAMBU_CAMERA_BYTES) {
        ESP_LOGW(TAG, "Bambu camera request failed: %s, HTTP %d, bytes %u",
                 esp_err_to_name(error), status, (unsigned)response.length);
        return false;
    }
    s_bambu_camera_revisions[slot] = revision;
    s_bambu_camera_active = slot;
    return true;
}

static bool fetch_snapshot(codex_snapshot_t *management)
{
    const device_config_values_t *device_config = device_config_get();
    if (strlen(device_config->bridge_url) == 0) return false;

    char *body = heap_caps_calloc(1, HTTP_BODY_INITIAL_CAPACITY,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) return false;
    http_body_t response = {.body = body, .capacity = HTTP_BODY_INITIAL_CAPACITY};
    esp_http_client_config_t config = {
        .url = device_config->bridge_url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        heap_caps_free(response.body);
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    if (strlen(device_config->bridge_token) > 0) {
        esp_http_client_set_header(client, "X-Bridge-Token", device_config->bridge_token);
    }
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Bridge request failed: %s, HTTP %d", esp_err_to_name(error), status);
        heap_caps_free(response.body);
        return false;
    }
    if (response.overflow) {
        ESP_LOGW(TAG, "Bridge snapshot could not fit in available PSRAM");
        heap_caps_free(response.body);
        return false;
    }
    bool parsed = management_snapshot_parse(response.body, management);
    heap_caps_free(response.body);
    if (parsed && management->custom_image_available &&
        !fetch_custom_image(management->custom_image_revision)) {
        management->custom_image_available = false;
    }
    if (parsed && management->bambu_camera_available &&
        !fetch_bambu_camera(management->bambu_camera_revision)) {
        management->bambu_camera_available = false;
    }
    return parsed;
}

const uint8_t *connectivity_custom_image_data(uint32_t revision)
{
    for (size_t index = 0; index < 2; index++) {
        if (s_custom_image_buffers[index] != NULL && s_custom_image_revisions[index] == revision) {
            return s_custom_image_buffers[index];
        }
    }
    return NULL;
}

const uint8_t *connectivity_bambu_camera_data(uint32_t revision)
{
    for (size_t index = 0; index < 2; index++) {
        if (s_bambu_camera_buffers[index] != NULL && s_bambu_camera_revisions[index] == revision) {
            return s_bambu_camera_buffers[index];
        }
    }
    return NULL;
}

static bool send_bambu_command(bambu_command_t command)
{
    const device_config_values_t *device_config = device_config_get();
    const char *action = command == BAMBU_COMMAND_PAUSE ? "pause" :
                         command == BAMBU_COMMAND_RESUME ? "resume" : "stop";
    char url[256];
    strlcpy(url, device_config->bridge_url, sizeof(url));
    char *last_slash = strrchr(url, '/');
    if (last_slash == NULL) return false;
    strlcpy(last_slash + 1, "bambu/command", sizeof(url) - (size_t)(last_slash + 1 - url));
    char payload[40];
    snprintf(payload, sizeof(payload), "{\"action\":\"%s\"}", action);
    char response_body[256] = {0};
    http_body_t response = {.body = response_body, .capacity = sizeof(response_body)};
    esp_http_client_config_t config = {
        .url = url, .event_handler = http_event_handler, .user_data = &response,
        .timeout_ms = 8000, .buffer_size = 512, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return false;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (strlen(device_config->bridge_token) > 0) {
        esp_http_client_set_header(client, "X-Bridge-Token", device_config->bridge_token);
    }
    esp_http_client_set_post_field(client, payload, strlen(payload));
    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Bambu %s command: %s, HTTP %d", action, esp_err_to_name(error), status);
    return error == ESP_OK && status == 202;
}

static void publish_current_state(void)
{
    if (s_state_lock != NULL) xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (!s_have_snapshot && CONFIG_STATE_DISPLAY_DEMO_MODE) {
        app_state_make_preview(&s_work_snapshot);
        s_last_snapshot = s_work_snapshot;
        s_have_snapshot = true;
    }
    app_state_publish(&s_work_snapshot);
    if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
}

void connectivity_apply_codex_bridge_message(
    const codex_bridge_message_t *message
)
{
    if (message == NULL || message->ignored) return;
    if (s_state_lock != NULL) xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (strcmp(message->type, "read_state") == 0) {
        codex_snapshot_apply_read_state(
            &s_work_snapshot,
            s_work_tasks,
            s_work_task_count,
            message->data
        );
        s_last_snapshot = s_work_snapshot;
        app_state_tasks_publish(s_work_tasks, s_work_task_count);
        app_state_publish(&s_work_snapshot);
        if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
        return;
    }
    if (strcmp(message->type, "message") != 0) {
        if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
        return;
    }

    if (strcmp(message->kind, "status") == 0) {
        codex_snapshot_merge_status(&s_work_snapshot, message->data);
    } else if (strcmp(message->kind, "task") == 0) {
        codex_snapshot_merge_tasks(
            &s_work_snapshot,
            message->data,
            s_work_tasks,
            &s_work_task_count,
            CODEX_TASK_DETAIL_MAX
        );
    } else if (strcmp(message->kind, "result") == 0) {
        codex_snapshot_merge_result(&s_work_snapshot, message->data);
        codex_snapshot_attach_result(
            s_work_tasks,
            s_work_task_count,
            message->thread_id,
            message->message_id
        );
    } else {
        if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
        return;
    }

    s_have_snapshot = true;
    s_last_snapshot = s_work_snapshot;
    app_state_tasks_publish(s_work_tasks, s_work_task_count);
    app_state_publish(&s_work_snapshot);
    if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
}

static void bridge_task(void *argument)
{
    (void)argument;
    publish_current_state();
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events,
            WIFI_CONNECTED_BIT | REFRESH_REQUESTED_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(CONFIG_STATE_DISPLAY_POLL_SECONDS * 1000));

        if ((bits & WIFI_CONNECTED_BIT) || s_wifi_connected) {
            bambu_command_t command;
            while (s_bambu_command_queue != NULL && xQueueReceive(s_bambu_command_queue, &command, 0) == pdTRUE) {
                send_bambu_command(command);
            }
            if (fetch_snapshot(&s_management_snapshot)) {
                if (s_state_lock != NULL) xSemaphoreTake(s_state_lock, portMAX_DELAY);
                management_snapshot_apply(&s_work_snapshot, &s_management_snapshot);
                if (!s_bridge_online) ESP_LOGI(TAG, "Bridge snapshot received");
                s_last_snapshot = s_work_snapshot;
                s_have_snapshot = true;
                s_bridge_online = true;
                strlcpy(s_bridge_note, s_work_snapshot.preview_data ? "管理中心在线 · 预览数据" : "管理中心在线", sizeof(s_bridge_note));
                app_state_publish(&s_work_snapshot);
                if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
            } else {
                if (s_state_lock != NULL) xSemaphoreTake(s_state_lock, portMAX_DELAY);
                if (s_bridge_online) {
                    s_bridge_online = false;
                    strlcpy(s_bridge_note, "管理中心不可达", sizeof(s_bridge_note));
                    codex_snapshot_apply_source_state(
                        &s_work_snapshot,
                        s_work_snapshot.codex_source_online,
                        false
                    );
                    s_last_snapshot = s_work_snapshot;
                    app_state_publish(&s_work_snapshot);
                } else if (s_have_snapshot) {
                    strlcpy(s_bridge_note, "正在重连 · 保留上次数据", sizeof(s_bridge_note));
                    app_state_publish(&s_last_snapshot);
                }
                if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
            }
        }

        bool codex_online = codex_bridge_client_is_online();
        if (s_state_lock != NULL) xSemaphoreTake(s_state_lock, portMAX_DELAY);
        if (codex_online != s_codex_was_online) {
            s_codex_was_online = codex_online;
            codex_snapshot_apply_source_state(
                &s_work_snapshot,
                codex_online,
                s_bridge_online
            );
            if (s_have_snapshot) {
                s_work_snapshot.stale = s_work_snapshot.generated_at > 0 &&
                    time(NULL) - s_work_snapshot.generated_at >
                        CONFIG_STATE_DISPLAY_STALE_SECONDS;
                s_last_snapshot = s_work_snapshot;
            }
            app_state_publish(&s_work_snapshot);
        }
        if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        strlcpy(s_ip, "--", sizeof(s_ip));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT | REFRESH_REQUESTED_BIT);
    }
}

static void wifi_start(void)
{
    const device_config_values_t *device_config = device_config_get();
    if (strlen(device_config->wifi_ssid) == 0) {
        strlcpy(s_bridge_note, "Wi-Fi 未配置", sizeof(s_bridge_note));
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t station = {0};
    strlcpy((char *)station.sta.ssid, device_config->wifi_ssid, sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, device_config->wifi_password, sizeof(station.sta.password));
    station.sta.threshold.authmode = strlen(device_config->wifi_password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    station.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
}

void connectivity_start(void)
{
    const device_config_values_t *device_config = device_config_get();
    ESP_LOGI(TAG, "Preparing bridge data channel");
    s_events = xEventGroupCreate();
    s_bambu_command_queue = xQueueCreate(4, sizeof(bambu_command_t));
    s_state_lock = xSemaphoreCreateMutex();
    s_work_tasks = heap_caps_calloc(CODEX_TASK_DETAIL_MAX, sizeof(*s_work_tasks),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK((s_events == NULL || s_bambu_command_queue == NULL ||
                     s_state_lock == NULL || s_work_tasks == NULL) ?
                    ESP_ERR_NO_MEM : ESP_OK);
    ESP_LOGI(TAG, "Bridge data buffers ready");
    setenv("TZ", CONFIG_STATE_DISPLAY_TIMEZONE, 1);
    tzset();
    strlcpy(s_bridge_note,
            strlen(device_config->bridge_url) == 0 ? "未配置" : "等待管理中心",
            sizeof(s_bridge_note));

    /* Reserve the polling task before the Wi-Fi driver fragments internal RAM. */
    BaseType_t task_created = xTaskCreate(bridge_task, "bridge", 6144, NULL, 5, NULL);
    if (task_created != pdPASS) {
        strlcpy(s_bridge_note, "管理中心任务启动失败", sizeof(s_bridge_note));
        ESP_LOGE(TAG, "Unable to create bridge task");
    }
    ESP_LOGI(TAG, "Starting Wi-Fi station");
    wifi_start();
    ESP_LOGI(TAG, "Connectivity services started");
}

bool connectivity_bambu_command(const char *action)
{
    if (s_bambu_command_queue == NULL || action == NULL) return false;
    bambu_command_t command;
    if (strcmp(action, "pause") == 0) command = BAMBU_COMMAND_PAUSE;
    else if (strcmp(action, "resume") == 0) command = BAMBU_COMMAND_RESUME;
    else if (strcmp(action, "stop") == 0) command = BAMBU_COMMAND_STOP;
    else return false;
    if (xQueueSend(s_bambu_command_queue, &command, 0) != pdTRUE) return false;
    xEventGroupSetBits(s_events, REFRESH_REQUESTED_BIT);
    return true;
}

void connectivity_request_refresh(void)
{
    if (s_events != NULL) xEventGroupSetBits(s_events, REFRESH_REQUESTED_BIT);
}

bool connectivity_is_wifi_connected(void)
{
    return s_wifi_connected;
}

void connectivity_get_summary(char *buffer, size_t buffer_size)
{
    const device_config_values_t *device_config = device_config_get();
    if (strlen(device_config->wifi_ssid) == 0) {
        snprintf(buffer, buffer_size, "未配置");
    } else {
        snprintf(buffer, buffer_size, "%s · %s", device_config->wifi_ssid,
                 s_wifi_connected ? "已连接" : "连接中");
    }
}

void connectivity_get_ip(char *buffer, size_t buffer_size)
{
    strlcpy(buffer, s_ip, buffer_size);
}

void connectivity_get_bridge_summary(char *buffer, size_t buffer_size)
{
    (void)s_bridge_online;
    strlcpy(buffer, s_bridge_note, buffer_size);
}
