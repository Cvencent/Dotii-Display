#include "connectivity.h"

#include "bridge_url.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "cJSON.h"
#include "codex_bridge_client.h"
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

static const char *TAG = "connectivity";
static EventGroupHandle_t s_events;
static bool s_wifi_connected;
static bool s_bridge_online;
static char s_candidate_bridge_url[256];
static bool s_codex_was_online;
static char s_ip[24] = "--";
static char s_bridge_note[48] = "正在启动";
/* Snapshot payloads are large and do not participate in DMA. Keep them in
   PSRAM so the Wi-Fi driver retains enough internal RAM for its RX buffers. */
EXT_RAM_BSS_ATTR static codex_snapshot_t s_last_snapshot;
EXT_RAM_BSS_ATTR static codex_snapshot_t s_work_snapshot;
EXT_RAM_BSS_ATTR static codex_snapshot_t s_management_snapshot;
static bool s_have_snapshot;
static codex_task_detail_t *s_work_tasks;
static size_t s_work_task_count;
static SemaphoreHandle_t s_state_lock;

typedef struct {
    char *body;
    size_t capacity;
    size_t length;
    bool overflow;
} http_body_t;

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

static bool fetch_snapshot(const char *url, codex_snapshot_t *management)
{
    const device_config_values_t *device_config = device_config_get();
    if (url == NULL || url[0] == '\0') return false;

    char *body = heap_caps_calloc(1, HTTP_BODY_INITIAL_CAPACITY,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) return false;
    http_body_t response = {.body = body, .capacity = HTTP_BODY_INITIAL_CAPACITY};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    ESP_LOGI(TAG, "fetching management snapshot: %s", url);
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
    ESP_LOGI(TAG, "snapshot bytes=%u internal free=%u", (unsigned)response.length,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    bool parsed = management_snapshot_parse(response.body, management);
    heap_caps_free(response.body);
    /* The standalone custom page is gone, so its 434 KB framebuffer is no
       longer downloaded or kept resident: the Xiaozhi TLS session and the
       I2S DMA descriptors need that memory far more than a hidden page. */
    management->custom_image_available = false;
    return parsed;
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

/* The task list carries a per-task status, so the device can always derive
   the number of in-progress turns even if a bridge emits a stale or missing
   `running` field in its codex_state message. */
static uint16_t derive_running_tasks(void)
{
    uint16_t running = 0;
    for (size_t index = 0; index < s_work_task_count; ++index) {
        if (s_work_tasks[index].status == CODEX_STATUS_WORKING ||
            s_work_tasks[index].status == CODEX_STATUS_WAITING) {
            running++;
        }
    }
    return running;
}

void connectivity_apply_codex_bridge_message(
    const codex_bridge_message_t *message,
    void *context
)
{
    (void)context;
    if (message == NULL || message->ignored) return;
    if (s_state_lock != NULL) xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (strcmp(message->type, "codex_state") == 0) {
        codex_snapshot_merge_codex_state(&s_work_snapshot, message->data);
        /* The Bridge counters are authoritative and cover every task it
           tracks; only fall back to the local task list when the bridge did
           not deliver a summary. */
        if (!s_work_snapshot.codex_state_summary_valid && s_work_task_count > 0) {
            s_work_snapshot.running_tasks = derive_running_tasks();
        }
        s_last_snapshot = s_work_snapshot;
        app_state_publish(&s_work_snapshot);
        if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
        return;
    }
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
    if (strcmp(message->type, "bridge_online") == 0) {
        /* The Bridge session came back (fresh connect, or a reconnect after a
           restart). Flip the source-online flag so the panel leaves the
           offline state. The counters themselves are republished by the
           projection layer once the next home-counter datagram arrives, so
           nothing is cleared here. */
        codex_snapshot_apply_source_state(&s_work_snapshot, true, s_bridge_online);
        s_last_snapshot = s_work_snapshot;
        app_state_publish(&s_work_snapshot);
        if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
        return;
    }
    if (strcmp(message->type, "bridge_offline") == 0) {
        /* The bridge session died. Clear the source-online flag right away so
           the panel shows the offline state instead of a frozen task count. */
        codex_snapshot_apply_source_state(&s_work_snapshot, false, s_bridge_online);
        s_last_snapshot = s_work_snapshot;
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
        if (!s_work_snapshot.codex_state_summary_valid) {
            s_work_snapshot.running_tasks = derive_running_tasks();
        }
    } else if (strcmp(message->kind, "result") == 0) {
        codex_snapshot_merge_result(&s_work_snapshot, message->data);
        codex_snapshot_attach_result(
            s_work_tasks,
            s_work_task_count,
            message->thread_id,
            message->message_id
        );
    } else if (strcmp(message->kind, "codex_status") == 0) {
        codex_snapshot_merge_quota(&s_work_snapshot, message->data);
    } else if (strcmp(message->kind, "account") == 0) {
        /* account.changed / rate_limits.changed / token_usage.changed arrive
           as CBDP events and are served here from the same committed state as
           the account.* read commands. */
        codex_snapshot_merge_account(&s_work_snapshot, message->data);
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
    /* The management snapshot carries the xiaozhi config (wake word, voice and
       the MCP tool access point) as well as display settings. Those must keep
       arriving even when Codex data is also flowing over the direct CDBP
       session, so the only requirement here is a configured bridge URL.
       Gating this on the absence of a CodexBridge token left every xiaozhi
       setting permanently at its default. */
    const bool management_requested = strlen(device_config_get()->bridge_url) > 0;
    publish_current_state();
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events,
            WIFI_CONNECTED_BIT | REFRESH_REQUESTED_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(CONFIG_STATE_DISPLAY_POLL_SECONDS * 1000));

        /* The management snapshot carries Xiaozhi and display settings even
           when Codex data arrives over the direct session. Polling it used to
           stop as soon as a CodexBridge token existed, so later management
           changes never reached the device. */
        if (management_requested &&
            ((bits & WIFI_CONNECTED_BIT) || s_wifi_connected)) {
            const char *active_url = device_config_get()->bridge_url;
            bool fetched = fetch_snapshot(active_url, &s_management_snapshot);
            /* A DHCP change leaves the persisted URL at an address the Bridge
               no longer owns. Try the address CBDP reported; only a request
               that actually succeeds may become the new persisted URL. */
            if (!fetched && s_candidate_bridge_url[0] != '\0' &&
                strcmp(s_candidate_bridge_url, active_url) != 0 &&
                fetch_snapshot(s_candidate_bridge_url, &s_management_snapshot)) {
                esp_err_t stored = device_config_store_bridge_url(s_candidate_bridge_url);
                if (stored == ESP_OK) {
                    ESP_LOGI(TAG, "bridge url promoted to %s", s_candidate_bridge_url);
                } else {
                    ESP_LOGW(TAG, "unable to persist bridge url: %s",
                             esp_err_to_name(stored));
                }
                fetched = true;
            }
            if (fetched) {
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
    s_state_lock = xSemaphoreCreateMutex();
    s_work_tasks = heap_caps_calloc(CODEX_TASK_DETAIL_MAX, sizeof(*s_work_tasks),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK((s_events == NULL || s_state_lock == NULL ||
                     s_work_tasks == NULL) ?
                    ESP_ERR_NO_MEM : ESP_OK);
    ESP_LOGI(TAG, "Bridge data buffers ready");
    setenv("TZ", CONFIG_STATE_DISPLAY_TIMEZONE, 1);
    tzset();
    /* A provisioned direct CodexBridge token is the preferred transport. In
       that mode the legacy HTTP snapshot must not run at all, including the
       startup window before the direct session finishes its handshake. */
    const bool direct_requested = strlen(device_config->codex_bridge_token) >= 16;
    /* With no token the CBDP session starts in pairing mode and waits for the
       host admin to approve the device, so say that rather than showing a
       state the user cannot act on. Pairing needs no management URL, so that
       case takes priority over the legacy snapshot wording. */
    strlcpy(s_bridge_note,
            direct_requested
                ? "等待 CodexBridge"
                : (strlen(device_config->bridge_url) == 0 ? "等待电脑端批准" : "等待管理中心"),
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

void connectivity_note_bridge_ipv4(const char *ipv4)
{
    if (ipv4 == NULL || ipv4[0] == '\0') return;
    char rewritten[sizeof(s_candidate_bridge_url)];
    if (!bridge_url_replace_ipv4_host(device_config_get()->bridge_url, ipv4,
                                      rewritten, sizeof(rewritten))) {
        /* A hostname or non-IPv4 URL keeps its own resolution path. */
        return;
    }
    if (strcmp(rewritten, s_candidate_bridge_url) == 0) return;
    strlcpy(s_candidate_bridge_url, rewritten, sizeof(s_candidate_bridge_url));
    ESP_LOGI(TAG, "bridge address candidate: %s", rewritten);
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
    if (buffer == NULL || buffer_size == 0) return;
    if (s_codex_was_online) {
        const size_t count = codex_bridge_client_capability_count();
        if (count > 0) {
            snprintf(buffer, buffer_size, "CodexBridge 在线 · %u 项能力", (unsigned)count);
        } else {
            strlcpy(buffer, "CodexBridge 在线", buffer_size);
        }
        return;
    }
    strlcpy(buffer, s_bridge_note, buffer_size);
}
