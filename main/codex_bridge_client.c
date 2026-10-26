#include "codex_bridge_client.h"

#include "connectivity.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "device_config.h"
#include "device_identity.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "codex_bridge_projection.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#define CBDP_DISCOVERY_PORT 8764
#define CBDP_TCP_PORT 8765
#define CBDP_DISCOVERY_MAX_BYTES 1024
#define CBDP_FRAME_BUFFER_MAX 4096
#define CBDP_TX_FRAME_MAX 1024
#define CBDP_PENDING_MAX 4
#define CBDP_UDP_TASK_STACK 4096
#define CBDP_TCP_TASK_STACK 8192
#define CBDP_HANDSHAKE_TIMEOUT_MS 10000
/* 11.8: a device with no token waits for the host admin to approve it, so the
   PAIRING wait is minutes rather than seconds. The presence task keeps
   broadcasting meanwhile, and the Bridge dials in once approved. */
#define CBDP_PAIRING_TIMEOUT_MS 180000
#define CBDP_SOCKET_POLL_MS 500
#define CBDP_ALIVE_LOG_MS 30000
#define CBDP_DEFAULT_HEARTBEAT_SECONDS 15
#define CBDP_DEFAULT_RETRANSMIT_MS 1000
#define CBDP_DEFAULT_MAX_RETRIES 6
#define CBDP_TOPIC_SYSTEM_THREAD_SUMMARIES 0x50000003u
#define CBDP_TOPIC_SYSTEM_RESYNC 0x50000002u
#define CBDP_SEND_DEADLINE_MS 5000
#define CBDP_SEND_RETRY_MS 20
#define CBDP_SOCKET_LOCK_TIMEOUT_MS 1000
#define CBDP_SOCKET_LOCK_WAIT_MS 6000
#define CBDP_PRESENCE_INTERVAL_MS 8000
#define CBDP_PRESENCE_TASK_STACK 4096
#define CBDP_LISTEN_BACKLOG 4
/* The Bridge publishes its home counters every two seconds. A restart of the
   Bridge process costs a few seconds before the new instance starts to
   publish again, so the freshness window has to be wide enough to survive
   that gap. An eight second window blanked the panel numbers on every
   restart. */
#define CBDP_HOME_COUNTERS_STALE_MS 15000

typedef struct {
    bool used;
    uint8_t bytes[CBDP_TX_FRAME_MAX];
    size_t length;
    uint32_t seq;
    uint32_t request_id;
    uint8_t retries;
    TickType_t next_retry;
} cbdp_pending_frame_t;

typedef struct {
    const uint64_t bit;
    const char *name;
} cbdp_capability_name_t;

static const cbdp_capability_name_t CAPABILITY_NAMES[] = {
    {CBDP_CAP_STATUS, "status"},
    {CBDP_CAP_TEXT, "text"},
    {CBDP_CAP_TASKS, "tasks"},
    {CBDP_CAP_RESULTS, "results"},
    {CBDP_CAP_APPROVAL, "approval"},
    {CBDP_CAP_AUDIO, "audio"},
    {CBDP_CAP_VOICE, "voice"},
    {CBDP_CAP_READ_ACK, "read_ack"},
    {CBDP_CAP_CONTROL, "control"},
    {CBDP_CAP_HISTORY, "history"},
    {CBDP_CAP_TELEMETRY, "telemetry"},
    {CBDP_CAP_SUBSCRIBE_LIVE, "subscribe_live"},
    {CBDP_CAP_SUBSCRIBE_THREAD, "subscribe_thread"},
    {CBDP_CAP_SUBSCRIBE_SUMMARY, "subscribe_summary"},
    {CBDP_CAP_WRITER_LEASE, "writer_lease"},
    {CBDP_CAP_MULTI_PAGE, "multi_page"},
    {CBDP_CAP_TLS, "tls"},
    {CBDP_CAP_DEVICE_CONFIG, "device_config"},
    {CBDP_CAP_COMMAND_STATUS, "command_status"},
    {CBDP_CAP_ACCOUNT, "quota"},
    {CBDP_CAP_CONTEXT_USAGE, "context_usage"},
    {CBDP_CAP_READ_UNREAD, "read_unread"},
    {CBDP_CAP_NOTICE, "notice"},
    {CBDP_CAP_MODEL_SWITCH, "model_switch"},
    {CBDP_CAP_CROSS_DEVICE_ACTIVE, "cross_device_active"},
    {CBDP_CAP_RESUME, "resume"},
    {CBDP_CAP_EVENTS_SINCE, "events_since"},
};

static const uint64_t SUPPORTED_CAPS =
    CBDP_CAP_STATUS |
    CBDP_CAP_TEXT |
    CBDP_CAP_TASKS |
    CBDP_CAP_RESULTS |
    CBDP_CAP_READ_ACK |
    CBDP_CAP_TELEMETRY |
    CBDP_CAP_SUBSCRIBE_LIVE |
    CBDP_CAP_SUBSCRIBE_THREAD |
    CBDP_CAP_SUBSCRIBE_SUMMARY |
    CBDP_CAP_MULTI_PAGE |
    CBDP_CAP_DEVICE_CONFIG |
    CBDP_CAP_ACCOUNT |
    CBDP_CAP_CONTEXT_USAGE |
    CBDP_CAP_READ_UNREAD;

static const char *TAG = "codex_bridge";
static const char *DEVICE_MODEL = "dotii";

/* The internal DRAM pool is tiny on this board and it is shared with the
   WiFi/LwIP stacks and the Xiaozhi TLS session. When it runs dry the mbedTLS
   writes fail ("Failed to allocate memory"), the Xiaozhi channel dies and the
   CBDP session stops answering. Keep the pool visible so that a drop is
   always explainable. */
static void log_heap_status(const char *reason)
{
    ESP_LOGI(TAG,
             "heap[%s] internal free=%u largest=%u min=%u dma=%u psram free=%u largest=%u",
             (reason == NULL) ? "" : reason,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
static const char *DEVICE_NAME = "Dotii";

static TaskHandle_t s_udp_task;
static TaskHandle_t s_tcp_task;
static SemaphoreHandle_t s_socket_lock;
static volatile bool s_running;
static volatile bool s_online;
static volatile bool s_link_broken;
/* Set when a session ended, so the presence task announces the device again
   right away instead of waiting for the next periodic announcement. */
static volatile bool s_presence_wake;
static volatile TaskHandle_t s_socket_lock_owner;
static int s_client_socket = -1;
static int s_server_socket = -1;
static codex_bridge_message_handler_t s_handler;
static void *s_handler_context;
static codex_bridge_projection_t s_projection;

/* The UDP home-counter channel is a second, independent link to the
   Bridge. It delivers the authoritative counters every two seconds and it
   needs no authenticated TCP session, so the numbers on the panel must not
   depend on the TCP session. The projection is therefore written by the UDP
   task as well, and both sides share one recursive mutex. */
static SemaphoreHandle_t s_projection_lock;
static volatile bool s_home_counters_valid;
static volatile uint32_t s_home_counters_last_ms;

/* Counters published by the Bridge over the UDP home-counter channel. They
   live in the header TLVs of the 0x1E datagram. */
typedef struct {
    bool have_running;
    uint16_t running;
    bool have_completed;
    uint16_t completed;
    bool have_failed;
    uint16_t failed;
} cbdp_home_counters_t;
/* Address of the Bridge in host byte order, learned from the datagrams it
   sends. The presence announcements have to go there after a Bridge
   restart, because the device is the TCP server in this protocol and the
   Bridge can only dial in to a device that told it where to find it. */
static volatile uint32_t s_bridge_ip;
static volatile bool s_bridge_ip_known;
static TaskHandle_t s_presence_task;
/* Socket of the presence task. The task is deleted by its owner, so the
   owner closes the socket instead of the task itself. */
static volatile int s_presence_socket = -1;

/* The projection is written by the UDP task (home counters) and by the TCP
   task (session frames). A recursive mutex keeps both sides consistent
   without the risk of a self-deadlock in the nested helpers. */
static void projection_lock(void)
{
    if (s_projection_lock != NULL) {
        xSemaphoreTakeRecursive(s_projection_lock, portMAX_DELAY);
    }
}

static void projection_unlock(void)
{
    if (s_projection_lock != NULL) {
        xSemaphoreGiveRecursive(s_projection_lock);
    }
}

/* True while the Bridge keeps sending its home counters over UDP. */
static bool home_counters_fresh(void)
{
    if (!s_home_counters_valid) return false;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t age_ms = now_ms - s_home_counters_last_ms;
    return age_ms < (uint32_t)CBDP_HOME_COUNTERS_STALE_MS;
}

/* Log the thread counters exactly as the Bridge-side projection derives
   them, so the device view of "running / completed / failed" can be
   verified without touching the Bridge itself. */
static void log_projection_counts(const char *reason)
{
    projection_lock();
    if (s_projection.threads == NULL) {
        projection_unlock();
        return;
    }
    uint16_t running = 0;
    uint16_t completed = 0;
    uint16_t failed = 0;
    cJSON *thread = NULL;
    cJSON_ArrayForEach(thread, s_projection.threads) {
        cJSON *state = cJSON_GetObjectItemCaseSensitive(thread, "thread_state");
        if (!cJSON_IsNumber(state)) continue;
        if (state->valueint == 1) running++;
        else if (state->valueint == 2) completed++;
        else if (state->valueint == 3 || state->valueint == 4) failed++;
    }
    ESP_LOGI(TAG,
             "codex_state[%s] local running=%u completed_unread=%u failed_unread=%u | "
             "bridge summary_valid=%d running=%u completed_unread=%u failed_unread=%u",
             (reason == NULL) ? "" : reason,
             (unsigned)running, (unsigned)completed, (unsigned)failed,
             s_projection.have_summary ? 1 : 0,
             (unsigned)s_projection.summary_running,
             (unsigned)s_projection.summary_completed_unread,
             (unsigned)s_projection.summary_failed_unread);
    projection_unlock();
}

static uint8_t s_client_nonce[16];
static uint8_t s_session_id[16];
static uint8_t s_server_nonce[16];
static uint64_t s_effective_caps;
static uint32_t s_request_id;
static uint32_t s_tx_seq;
static uint32_t s_rx_expected;
static uint8_t s_gap_count;
static uint32_t s_last_ack_seq;
static uint32_t s_max_frame_bytes = 1024;
static uint32_t s_max_text_chars = 512;
static uint32_t s_max_text_bytes = 1536;
static uint16_t s_max_chunk_bytes = 384;
static uint16_t s_heartbeat_seconds = CBDP_DEFAULT_HEARTBEAT_SECONDS;
static uint32_t s_retransmit_timeout_ms = CBDP_DEFAULT_RETRANSMIT_MS;
static uint8_t s_max_retries = CBDP_DEFAULT_MAX_RETRIES;
/* Retransmit window. Each slot carries a full CBDP_TX_FRAME_MAX frame, so the
   array is ~4.2 KB. It is only touched from the bridge tasks (never an ISR),
   and the panel DMA strips and mbedTLS both compete for the same internal RAM,
   so it is allocated from PSRAM at start instead of living in .bss. */
static cbdp_pending_frame_t *s_pending;
static bool s_snapshot_seen;
static bool s_delta_seen;
static bool s_seq0_warned;
static uint32_t s_active_state_request_id;
static bool s_active_state_unsupported;
/* request_id of the in-flight account reads, so their RESULT can be routed to
   the account merge instead of being treated as an unknown command result. */
static uint32_t s_account_read_request_id;
static uint32_t s_rate_limits_read_request_id;
static uint32_t s_token_usage_read_request_id;

/* Bridge keys cursors by (device, topic_id, scope); a thread topic can be
   subscribed at both thread and summary scope, so track them separately. */
#define CBDP_CURSOR_MAX 12
typedef struct {
    uint32_t topic_id;
    uint8_t scope;
    uint64_t cursor;
    bool used;
} cbdp_cursor_t;
static cbdp_cursor_t s_cursors[CBDP_CURSOR_MAX];
#define CBDP_SUBSCRIBED_THREAD_MAX 8
static char s_subscribed_threads[CBDP_SUBSCRIBED_THREAD_MAX][64];
static size_t s_subscribed_thread_count;

/* --- subscription replay windows -----------------------------------------
   The Bridge answers every SUBSCRIBE with the events it still holds for the
   requested cursor. A subscription that starts at cursor 0 therefore replays
   an arbitrary history window whose frames carry old counter values. Those
   frames must update the projection but must not reach the panel: publishing
   them one by one makes the running counter flicker and rings the task chime
   for tasks that finished long ago. The window is opened before the SUBSCRIBE
   leaves the device and closed by the matching RESULT. */
#define CBDP_SUBSCRIBE_WINDOW_MAX 12
#define CBDP_REPLAY_WINDOW_TIMEOUT_MS 20000

typedef struct {
    bool used;
    uint32_t request_id;
    uint32_t topic_id;
    uint8_t scope;
    bool quiet;
    uint32_t suppressed;
    uint32_t opened_ms;
} cbdp_subscribe_window_t;

static cbdp_subscribe_window_t s_subscribe_windows[CBDP_SUBSCRIBE_WINDOW_MAX];
static SemaphoreHandle_t s_window_lock;
static bool s_replay_baseline_pending;
static uint32_t s_replay_suppressed;

static void window_lock(void)
{
    if (s_window_lock != NULL) xSemaphoreTake(s_window_lock, portMAX_DELAY);
}

static void window_unlock(void)
{
    if (s_window_lock != NULL) xSemaphoreGive(s_window_lock);
}

static uint32_t bridge_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* A replay window whose RESULT never arrives must not mute the topic
   forever. */
static void subscribe_window_expire(cbdp_subscribe_window_t *window, uint32_t now)
{
    if (!window->used || !window->quiet) return;
    if ((uint32_t)(now - window->opened_ms) < CBDP_REPLAY_WINDOW_TIMEOUT_MS) return;
    window->used = false;
    window->quiet = false;
    ESP_LOGW(TAG, "replay window %lu timed out (topic=0x%08lx)",
             (unsigned long)window->request_id, (unsigned long)window->topic_id);
}

static void subscribe_windows_clear(void)
{
    window_lock();
    memset(s_subscribe_windows, 0, sizeof(s_subscribe_windows));
    s_replay_baseline_pending = false;
    s_replay_suppressed = 0;
    window_unlock();
    projection_lock();
    codex_bridge_projection_set_quiet(&s_projection, false);
    codex_bridge_projection_set_catchup(&s_projection, false);
    projection_unlock();
}

static cbdp_subscribe_window_t *subscribe_window_find(uint32_t request_id)
{
    for (size_t index = 0; index < CBDP_SUBSCRIBE_WINDOW_MAX; ++index) {
        if (s_subscribe_windows[index].used &&
            s_subscribe_windows[index].request_id == request_id) {
            return &s_subscribe_windows[index];
        }
    }
    return NULL;
}

static void subscribe_window_open(uint32_t request_id, uint32_t topic_id,
                                  uint8_t scope, bool quiet)
{
    if (request_id == 0) return;
    window_lock();
    const uint32_t now = bridge_now_ms();
    cbdp_subscribe_window_t *window = subscribe_window_find(request_id);
    if (window == NULL) {
        size_t slot = CBDP_SUBSCRIBE_WINDOW_MAX;
        for (size_t index = 0; index < CBDP_SUBSCRIBE_WINDOW_MAX; ++index) {
            if (!s_subscribe_windows[index].used) {
                slot = index;
                break;
            }
        }
        if (slot == CBDP_SUBSCRIBE_WINDOW_MAX) {
            /* Every slot is taken: recycle the oldest one so a new
               subscription is never left untracked. */
            slot = 0;
            for (size_t index = 1; index < CBDP_SUBSCRIBE_WINDOW_MAX; ++index) {
                if (s_subscribe_windows[index].opened_ms <
                    s_subscribe_windows[slot].opened_ms) {
                    slot = index;
                }
            }
            ESP_LOGW(TAG, "replay window table full, recycling slot %u",
                     (unsigned)slot);
        }
        window = &s_subscribe_windows[slot];
        memset(window, 0, sizeof(*window));
        window->used = true;
        window->request_id = request_id;
        window->topic_id = topic_id;
        window->scope = scope;
        window->opened_ms = now;
    }
    window->quiet = quiet;
    if (quiet) {
        s_replay_baseline_pending = true;
        ESP_LOGI(TAG, "replay window %lu open (topic=0x%08lx scope=%u) quiet",
                 (unsigned long)request_id, (unsigned long)topic_id,
                 (unsigned)scope);
    }
    window_unlock();
}

static void subscribe_window_close(uint32_t request_id)
{
    if (request_id == 0) return;
    window_lock();
    cbdp_subscribe_window_t *window = subscribe_window_find(request_id);
    if (window != NULL) {
        window->used = false;
        window->quiet = false;
    }
    window_unlock();
}

/* True while the topic is inside a quiet replay window. */
static bool subscribe_quiet_for_topic(uint32_t topic_id)
{
    if (topic_id == 0) return false;
    bool quiet = false;
    window_lock();
    const uint32_t now = bridge_now_ms();
    for (size_t index = 0; index < CBDP_SUBSCRIBE_WINDOW_MAX; ++index) {
        cbdp_subscribe_window_t *window = &s_subscribe_windows[index];
        subscribe_window_expire(window, now);
        if (!window->used || !window->quiet) continue;
        if (window->topic_id != topic_id) continue;
        quiet = true;
    }
    window_unlock();
    return quiet;
}

static void replay_frame_suppressed(uint32_t topic_id)
{
    window_lock();
    s_replay_suppressed++;
    for (size_t index = 0; index < CBDP_SUBSCRIBE_WINDOW_MAX; ++index) {
        cbdp_subscribe_window_t *window = &s_subscribe_windows[index];
        if (!window->used || !window->quiet) continue;
        if (window->topic_id != topic_id) continue;
        window->suppressed++;
    }
    window_unlock();
}

/* The replay ended: publish the authoritative state once, marked as a
   baseline, so the panel refreshes its counters without ringing the task
   chime for tasks that finished before the device connected. */
static void replay_reconcile(void)
{
    bool flush = false;
    window_lock();
    const uint32_t now = bridge_now_ms();
    for (size_t index = 0; index < CBDP_SUBSCRIBE_WINDOW_MAX; ++index) {
        subscribe_window_expire(&s_subscribe_windows[index], now);
    }
    if (s_replay_baseline_pending && s_replay_suppressed > 0) {
        bool still_quiet = false;
        for (size_t index = 0; index < CBDP_SUBSCRIBE_WINDOW_MAX; ++index) {
            if (s_subscribe_windows[index].used &&
                s_subscribe_windows[index].quiet) {
                still_quiet = true;
                break;
            }
        }
        if (!still_quiet) {
            flush = true;
            s_replay_baseline_pending = false;
            s_replay_suppressed = 0;
        }
    }
    window_unlock();
    if (!flush) return;
    projection_lock();
    codex_bridge_projection_set_quiet(&s_projection, false);
    codex_bridge_projection_set_catchup(&s_projection, true);
    codex_bridge_projection_flush(&s_projection);
    codex_bridge_projection_set_catchup(&s_projection, false);
    projection_unlock();
    ESP_LOGI(TAG, "replay reconciled: baseline published");
}

static void close_socket(int *socket_fd)
{
    if (socket_fd == NULL || *socket_fd < 0) return;
    shutdown(*socket_fd, 0);
    close(*socket_fd);
    *socket_fd = -1;
}

/* The socket mutex is shared by the session task, the discovery task and the
   management path. A blocking acquire could park the session loop forever
   while another task is stuck, which freezes the heartbeat and with it every
   counter the panel shows. Ownership is tracked per task so a timed out
   acquire never releases a mutex that belongs to somebody else. */
static bool socket_lock_acquire(void)
{
    if (s_socket_lock == NULL) return false;
    if (xSemaphoreTake(s_socket_lock, pdMS_TO_TICKS(CBDP_SOCKET_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    s_socket_lock_owner = xTaskGetCurrentTaskHandle();
    return true;
}

static bool socket_lock_acquire_wait(uint32_t timeout_ms)
{
    if (s_socket_lock == NULL) return false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        if (xSemaphoreTake(s_socket_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_socket_lock_owner = xTaskGetCurrentTaskHandle();
            return true;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) return false;
    }
}

static void socket_lock_release(void)
{
    if (s_socket_lock == NULL) return;
    if (s_socket_lock_owner != xTaskGetCurrentTaskHandle()) return;
    s_socket_lock_owner = NULL;
    xSemaphoreGive(s_socket_lock);
}

/* The peer may stop reading at any moment. A blocking send() would then park
   this task forever while it still owns the socket mutex, which is what froze
   the session: the heartbeat stopped, the panel kept showing the old counters
   and the listener never accepted the next connection. Send without blocking
   and carry the frame out under a deadline; a frame that cannot be delivered
   in time marks the link broken so the session is torn down instead of
   retransmitting a half written frame. */
static bool send_all(int socket_fd, const uint8_t *payload, size_t length)
{
    if (socket_fd < 0 || payload == NULL) return false;
    const int64_t deadline = esp_timer_get_time() + (int64_t)CBDP_SEND_DEADLINE_MS * 1000;
    size_t sent = 0;
    while (sent < length) {
        int result = send(socket_fd, payload + sent, length - sent, MSG_DONTWAIT);
        if (result > 0) {
            sent += (size_t)result;
            continue;
        }
        if (result == 0) {
            s_link_broken = true;
            return false;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (esp_timer_get_time() >= deadline) {
                ESP_LOGW(TAG, "send stalled at %u/%u bytes",
                         (unsigned)sent, (unsigned)length);
                s_link_broken = true;
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(CBDP_SEND_RETRY_MS));
            continue;
        }
        ESP_LOGW(TAG, "send failed: errno=%d", errno);
        s_link_broken = true;
        return false;
    }
    return true;
}

static uint32_t next_request_id_locked(void)
{
    uint32_t value = s_request_id++;
    if (s_request_id == 0) s_request_id = 1;
    if (value == 0) value = s_request_id++;
    return value;
}

static uint32_t next_request_id(void)
{
    if (!socket_lock_acquire()) {
        return 0;
    }
    uint32_t value = next_request_id_locked();
    socket_lock_release();
    return value;
}

static uint32_t next_tx_seq_locked(void)
{
    uint32_t value = s_tx_seq++;
    if (s_tx_seq == 0) s_tx_seq = 1;
    if (value == 0) value = s_tx_seq++;
    return value;
}

static void clear_pending_locked(void)
{
    if (s_pending != NULL) memset(s_pending, 0, sizeof(cbdp_pending_frame_t) * CBDP_PENDING_MAX);
}

static void acknowledge_pending_locked(uint32_t ack_seq)
{
    if (ack_seq == 0) return;
    for (size_t index = 0; index < CBDP_PENDING_MAX; ++index) {
        if (s_pending[index].used && s_pending[index].seq <= ack_seq) {
            s_pending[index].used = false;
        }
    }
}

/* A peer ERROR that names a request_id is the terminal answer for that
   request; stop retransmitting it instead of hammering the Bridge. */
static void cancel_pending_request_locked(uint32_t request_id)
{
    if (request_id == 0) return;
    for (size_t index = 0; index < CBDP_PENDING_MAX; ++index) {
        if (s_pending[index].used && s_pending[index].request_id == request_id) {
            s_pending[index].used = false;
        }
    }
}

static esp_err_t store_pending_locked(
    const uint8_t *bytes,
    size_t length,
    uint32_t seq,
    uint32_t request_id
)
{
    if (length > CBDP_TX_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
    for (size_t index = 0; index < CBDP_PENDING_MAX; ++index) {
        if (s_pending[index].used) continue;
        s_pending[index].used = true;
        memcpy(s_pending[index].bytes, bytes, length);
        s_pending[index].length = length;
        s_pending[index].seq = seq;
        s_pending[index].request_id = request_id;
        s_pending[index].retries = 0;
        s_pending[index].next_retry =
            xTaskGetTickCount() + pdMS_TO_TICKS(s_retransmit_timeout_ms);
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t send_frame_locked(
    uint8_t message_type,
    uint16_t flags,
    uint32_t topic_id,
    uint32_t seq,
    uint16_t code,
    uint8_t qos,
    const codex_bridge_tlv_builder_t *header,
    const uint8_t *payload,
    size_t payload_len,
    bool track
)
{
    if (s_client_socket < 0) return ESP_ERR_INVALID_STATE;
    uint8_t frame[CBDP_TX_FRAME_MAX];
    size_t frame_length = 0;
    esp_err_t error = codex_bridge_frame_build(
        frame,
        sizeof(frame),
        message_type,
        flags,
        topic_id,
        seq,
        s_last_ack_seq,
        code,
        qos,
        header,
        payload,
        payload_len,
        &frame_length
    );
    if (error != ESP_OK) return error;
    if (!send_all(s_client_socket, frame, frame_length)) return ESP_ERR_INVALID_STATE;
    if (track && seq != 0) {
        uint32_t request_id = 0;
        codex_bridge_tlv_t field = {0};
        if (codex_bridge_tlv_find(frame + CBDP_FIXED_HEADER_SIZE,
                                  header->length,
                                  CBDP_TLV_REQUEST_ID, &field) == ESP_OK) {
            (void)codex_bridge_tlv_u32(&field, &request_id);
        }
        return store_pending_locked(frame, frame_length, seq, request_id);
    }
    return ESP_OK;
}

static esp_err_t send_control(
    uint8_t message_type,
    uint16_t flags,
    uint16_t code,
    uint8_t qos,
    const codex_bridge_tlv_builder_t *header
)
{
    if (!socket_lock_acquire()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = send_frame_locked(
        message_type, flags, 0, 0, code, qos, header, NULL, 0, false);
    socket_lock_release();
    return error;
}

static esp_err_t send_business(
    uint8_t message_type,
    uint16_t flags,
    uint32_t topic_id,
    uint8_t qos,
    const codex_bridge_tlv_builder_t *header
)
{
    if (!socket_lock_acquire()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t seq = next_tx_seq_locked();
    esp_err_t error = send_frame_locked(
        message_type, flags, topic_id, seq, 0, qos, header, NULL, 0, true);
    socket_lock_release();
    return error;
}

static uint64_t *cursor_slot(uint32_t topic_id, uint8_t scope, bool create)
{
    for (size_t index = 0; index < CBDP_CURSOR_MAX; ++index) {
        cbdp_cursor_t *entry = &s_cursors[index];
        if (entry->used && entry->topic_id == topic_id && entry->scope == scope) {
            return &entry->cursor;
        }
    }
    if (!create) return NULL;
    for (size_t index = 0; index < CBDP_CURSOR_MAX; ++index) {
        if (s_cursors[index].used) continue;
        s_cursors[index].used = true;
        s_cursors[index].topic_id = topic_id;
        s_cursors[index].scope = scope;
        return &s_cursors[index].cursor;
    }
    return NULL;
}

static void cursor_store(uint32_t topic_id, uint8_t scope, uint64_t cursor)
{
    if (topic_id == 0 || cursor == 0) return;
    uint64_t *slot = cursor_slot(topic_id, scope, true);
    /* Never move a cursor backwards: a replayed frame carries an older global
       sequence number and must not rewind the resubscribe position. */
    if (slot != NULL && cursor > *slot) *slot = cursor;
}

static uint64_t cursor_load(uint32_t topic_id, uint8_t scope)
{
    uint64_t *slot = cursor_slot(topic_id, scope, false);
    return slot == NULL ? 0 : *slot;
}

static uint64_t cursor_load(uint32_t topic_id, uint8_t scope);

/* The Bridge dropped the history of this topic, so the local cursor is
   meaningless now. Forget it: the next SUBSCRIBE starts a fresh, quiet
   window instead of asking for events the Bridge no longer has. */
static void cursor_forget(uint32_t topic_id, uint8_t scope)
{
    if (topic_id == 0) return;
    uint64_t *slot = cursor_slot(topic_id, scope, false);
    if (slot == NULL) return;
    ESP_LOGW(TAG, "cursor forgotten for topic=0x%08lx scope=%u",
             (unsigned long)topic_id, (unsigned)scope);
    *slot = 0;
}

static esp_err_t send_ack(uint32_t ack_seq, uint32_t topic_id, uint8_t scope,
                          uint64_t cursor)
{
    uint8_t header_bytes[64];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = ESP_OK;
    if (cursor != 0) {
        error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_TOPIC_ID, topic_id);
        if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_SCOPE, scope);
        if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_CURSOR, cursor);
    }
    if (error != ESP_OK) return error;
    if (!socket_lock_acquire()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client_socket < 0) {
        socket_lock_release();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t frame[128];
    size_t frame_length = 0;
    error = codex_bridge_frame_build(
        frame, sizeof(frame), CBDP_MSG_ACK, 0, 0, 0, ack_seq, 0,
        CBDP_QOS_TELEMETRY, &header, NULL, 0, &frame_length);
    if (error == ESP_OK && !send_all(s_client_socket, frame, frame_length)) {
        error = ESP_ERR_INVALID_STATE;
    }
    socket_lock_release();
    return error;
}
static esp_err_t send_pong(uint32_t echo)
{
    uint8_t header_bytes[16];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    if (echo != 0) {
        esp_err_t error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_ECHO, echo);
        if (error != ESP_OK) return error;
    }
    return send_control(CBDP_MSG_PONG, 0, 0, CBDP_QOS_TELEMETRY, &header);
}

static esp_err_t send_ping(uint32_t ping_id)
{
    uint8_t header_bytes[16];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_PING_ID, ping_id);
    if (error != ESP_OK) return error;
    return send_control(CBDP_MSG_PING, 0, 0, CBDP_QOS_TELEMETRY, &header);
}

static esp_err_t send_error(uint16_t code, uint32_t request_id, const char *message)
{
    uint8_t header_bytes[96];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = ESP_OK;
    if (request_id != 0) {
        error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, request_id);
    }
    if (error == ESP_OK) {
        error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_ERROR_MESSAGE,
                                              message == NULL ? "protocol error" : message);
    }
    if (error != ESP_OK) return error;
    return send_control(CBDP_MSG_ERROR, CBDP_FLAG_RESPONSE, code,
                        CBDP_QOS_CRITICAL, &header);
}

static esp_err_t send_close(const char *reason)
{
    uint8_t header_bytes[64];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_utf8(
        &header, CBDP_TLV_REASON, reason == NULL ? "close" : reason);
    if (error != ESP_OK) return error;
    return send_control(CBDP_MSG_CLOSE, 0, 0, CBDP_QOS_CRITICAL, &header);
}

static esp_err_t recv_exact(
    int socket_fd,
    uint8_t *data,
    size_t length,
    TickType_t deadline
)
{
    size_t received = 0;
    while (received < length) {
        if (!s_running) return ESP_ERR_INVALID_STATE;
        if (deadline != portMAX_DELAY &&
            (int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return ESP_ERR_TIMEOUT;
        }
        /* Block in recv(): the socket already carries SO_RCVTIMEO
           (CBDP_SOCKET_POLL_MS), so this parks for at most that long and
           then returns EAGAIN, which the deadline below turns into a clean
           timeout. A zero-delay spin here is what silently froze the whole
           session: every call is a round trip into the LwIP TCPIP thread, a
           busy loop floods its mailbox and the TCP timers plus the inbound
           path stop being served. The device then neither answers the
           bridge nor sends its own heartbeat, and the panel keeps showing
           the last known counters. */
        int result = recv(socket_fd, data + received, length - received, 0);
        if (result > 0) {
            received += (size_t)result;
            continue;
        }
        if (result == 0) return ESP_ERR_INVALID_STATE;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return ESP_ERR_INVALID_STATE;
        }
        if (deadline != portMAX_DELAY &&
            (int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static esp_err_t read_frame(
    int socket_fd,
    uint8_t *buffer,
    size_t capacity,
    codex_bridge_frame_t *frame,
    TickType_t deadline
)
{
    if (buffer == NULL || frame == NULL || capacity < CBDP_FIXED_HEADER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = CBDP_SOCKET_POLL_MS * 1000,
    };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    esp_err_t error = recv_exact(socket_fd, buffer, CBDP_FIXED_HEADER_SIZE, deadline);
    if (error != ESP_OK) return error;
    const uint16_t header_len = (uint16_t)(((uint16_t)buffer[6] << 8) | buffer[7]);
    const uint32_t payload_len = ((uint32_t)buffer[8] << 24) |
                                 ((uint32_t)buffer[9] << 16) |
                                 ((uint32_t)buffer[10] << 8) |
                                 buffer[11];
    if (header_len < CBDP_FIXED_HEADER_SIZE) return ESP_ERR_INVALID_SIZE;
    const size_t frame_length = (size_t)header_len + payload_len;
    if (frame_length > capacity ||
        frame_length > s_max_frame_bytes ||
        frame_length > CBDP_FRAME_BUFFER_MAX) {
        (void)send_error(0x000D, 0, "frame too large");
        return ESP_ERR_INVALID_SIZE;
    }
    error = recv_exact(
        socket_fd,
        buffer + CBDP_FIXED_HEADER_SIZE,
        frame_length - CBDP_FIXED_HEADER_SIZE,
        deadline
    );
    if (error != ESP_OK) return error;
    error = codex_bridge_frame_parse(buffer, frame_length, frame);
    if (error != ESP_OK) {
        (void)send_error(error == ESP_ERR_INVALID_VERSION ? 0x0016 : 0x0005, 0, "invalid frame");
        return error;
    }
    return ESP_OK;
}

/* True when a usable CodexBridge token is already stored. The pairing flow
   only runs for a device that has none. */
static bool codex_bridge_token_available(void)
{
    return strlen(device_config_get()->codex_bridge_token) >= CBDP_PAIRING_TOKEN_MIN_BYTES;
}

static esp_err_t send_hello(void)
{
    esp_fill_random(s_client_nonce, sizeof(s_client_nonce));
    const char *device_id = device_identity_get();
    const char *firmware = esp_app_get_description()->version;
    const uint32_t request_id = next_request_id();

    uint8_t header_bytes[384];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_VERSION_MINOR, 0);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_DEVICE_ID, device_id);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_MODEL, DEVICE_MODEL);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_FIRMWARE, firmware);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_NAME, DEVICE_NAME);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_PROFILE, CBDP_PROFILE_PANEL);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_SUPPORTED_CAPS, SUPPORTED_CAPS);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_TRANSPORT_SECURITY, 0);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_CLIENT_NONCE,
                                                                s_client_nonce, sizeof(s_client_nonce));
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, request_id);
    /* 11.8: ask for a token only while none is stored. A provisioned device
       omits the TLV entirely, so its HELLO stays identical to older builds. */
    if (error == ESP_OK && !codex_bridge_token_available()) {
        error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_PAIRING_STATE,
                                            CBDP_PAIRING_STATE_REQUEST);
    }
    if (error != ESP_OK) return error;

    if (!socket_lock_acquire()) {
        return ESP_ERR_INVALID_STATE;
    }
    error = send_frame_locked(CBDP_MSG_HELLO, CBDP_FLAG_REQUEST, 0, 0, 0,
                              CBDP_QOS_TELEMETRY, &header, NULL, 0, false);
    socket_lock_release();
    return error;
}

static esp_err_t parse_welcome(const codex_bridge_frame_t *frame)
{
    codex_bridge_tlv_t field = {0};
    esp_err_t error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_SESSION_ID, &field);
    if (error != ESP_OK || field.length != sizeof(s_session_id)) return ESP_ERR_INVALID_ARG;
    memcpy(s_session_id, field.value, sizeof(s_session_id));

    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_SERVER_NONCE, &field);
    if (error != ESP_OK || field.length != sizeof(s_server_nonce)) return ESP_ERR_INVALID_ARG;
    memcpy(s_server_nonce, field.value, sizeof(s_server_nonce));

    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_AUTH_MODE, &field);
    if (error != ESP_OK) return ESP_ERR_INVALID_ARG;
    uint8_t auth_mode = 0;
    error = codex_bridge_tlv_u8(&field, &auth_mode);
    if (error != ESP_OK || auth_mode != 1) return ESP_ERR_NOT_SUPPORTED;

    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_MAX_FRAME_BYTES, &field);
    if (error != ESP_OK) return ESP_ERR_INVALID_ARG;
    error = codex_bridge_tlv_u32(&field, &s_max_frame_bytes);
    if (error != ESP_OK || s_max_frame_bytes < CBDP_FIXED_HEADER_SIZE) return ESP_ERR_INVALID_SIZE;

    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_MAX_TEXT_CHARS, &field);
    if (error == ESP_OK) (void)codex_bridge_tlv_u32(&field, &s_max_text_chars);
    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_MAX_TEXT_BYTES, &field);
    if (error == ESP_OK) (void)codex_bridge_tlv_u32(&field, &s_max_text_bytes);
    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_MAX_CHUNK_BYTES, &field);
    if (error != ESP_OK) return ESP_ERR_INVALID_ARG;
    error = codex_bridge_tlv_u16(&field, &s_max_chunk_bytes);
    if (error != ESP_OK || s_max_chunk_bytes == 0) return ESP_ERR_INVALID_SIZE;
    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_HEARTBEAT_SECONDS, &field);
    if (error != ESP_OK) return ESP_ERR_INVALID_ARG;
    error = codex_bridge_tlv_u16(&field, &s_heartbeat_seconds);
    if (error != ESP_OK || s_heartbeat_seconds == 0) return ESP_ERR_INVALID_SIZE;

    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_RETRANSMIT_TIMEOUT_MS, &field);
    if (error == ESP_OK) (void)codex_bridge_tlv_u32(&field, &s_retransmit_timeout_ms);
    else s_retransmit_timeout_ms = CBDP_DEFAULT_RETRANSMIT_MS;
    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_MAX_RETRIES, &field);
    if (error == ESP_OK) (void)codex_bridge_tlv_u8(&field, &s_max_retries);
    else s_max_retries = CBDP_DEFAULT_MAX_RETRIES;
    return ESP_OK;
}

static esp_err_t send_auth(void)
{
    uint8_t proof[32] = {0};
    esp_err_t error = codex_bridge_auth_proof(
        device_config_get()->codex_bridge_token,
        s_session_id,
        s_client_nonce,
        s_server_nonce,
        device_identity_get(),
        proof
    );
    if (error != ESP_OK) return error;
    const uint32_t request_id = next_request_id();
    uint8_t header_bytes[128];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_SESSION_ID,
                                           s_session_id, sizeof(s_session_id));
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_AUTH_PROOF,
                                                                proof, sizeof(proof));
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_AUTH_MODE, 1);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, request_id);
    if (error != ESP_OK) return error;

    if (!socket_lock_acquire()) {
        return ESP_ERR_INVALID_STATE;
    }
    error = send_frame_locked(CBDP_MSG_AUTH, CBDP_FLAG_REQUEST, 0, 0, 0,
                              CBDP_QOS_CRITICAL, &header, NULL, 0, false);
    socket_lock_release();
    return error;
}

static esp_err_t parse_auth_ok(const codex_bridge_frame_t *frame)
{
    codex_bridge_tlv_t field = {0};
    esp_err_t error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_EFFECTIVE_CAPS, &field);
    if (error != ESP_OK) return error;
    error = codex_bridge_tlv_u64(&field, &s_effective_caps);
    if (error != ESP_OK) return error;
    error = codex_bridge_tlv_find(
        frame->header_tlv, frame->header_tlv_len, CBDP_TLV_DEVICE_ID, &field);
    if (error != ESP_OK) return error;
    if (field.length != strlen(device_identity_get()) ||
        memcmp(field.value, device_identity_get(), field.length) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* Wait for one specific handshake message. A PAIRING(0x1F) offer may arrive
   between WELCOME and AUTH for a device being paired, so the caller can accept
   it via accept_pairing. */
static esp_err_t wait_for_message(
    int socket_fd,
    uint8_t message_type,
    uint8_t *buffer,
    size_t capacity,
    codex_bridge_frame_t *frame,
    uint32_t timeout_ms
)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_running) {
        esp_err_t error = read_frame(socket_fd, buffer, capacity, frame, deadline);
        if (error != ESP_OK) return error;
        /* Handshake replies may carry transport sequence numbers (WELCOME=1,
           AUTH_OK=2). Consume them so the first post-auth frame (seq=3) does
           not look like a gap. No ACK is sent here: the bridge's handshake
           state machine rejects ACKs before AUTH completes. */
        if (frame->seq != 0) {
            s_last_ack_seq = frame->seq;
            s_rx_expected = frame->seq + 1;
            if (s_rx_expected == 0) s_rx_expected = 1;
        }
        if (frame->message_type == message_type) return ESP_OK;
        if (frame->message_type == CBDP_MSG_PING) {
            codex_bridge_tlv_t ping = {0};
            uint32_t ping_id = 0;
            if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                      CBDP_TLV_PING_ID, &ping) == ESP_OK) {
                (void)codex_bridge_tlv_u32(&ping, &ping_id);
            }
            (void)send_pong(ping_id);
            continue;
        }
        if (frame->message_type == CBDP_MSG_ERROR ||
            frame->message_type == CBDP_MSG_CLOSE) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_ERR_INVALID_STATE;
}
static esp_err_t send_subscribe(uint32_t topic_id, uint8_t scope, uint8_t qos)
{
    const uint32_t request_id = next_request_id();
    const uint64_t cursor = cursor_load(topic_id, scope);
    /* Open the replay window before the request leaves the device: the
       Bridge may already have the answer queued, and a frame that arrives
       before the window exists would be treated as a live change. */
    subscribe_window_open(request_id, topic_id, scope, cursor == 0);
    uint8_t header_bytes[64];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_TOPIC_ID, topic_id);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_SCOPE, scope);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_QOS, qos);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_CURSOR, cursor);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, request_id);
    if (error != ESP_OK) {
        subscribe_window_close(request_id);
        return error;
    }
    esp_err_t sent = send_business(CBDP_MSG_SUBSCRIBE, CBDP_FLAG_REQUEST,
                                    topic_id, qos, &header);
    if (sent != ESP_OK) subscribe_window_close(request_id);
    return sent;
}

/* A reconnect drops the thread subscriptions the Bridge holds for us, so the
   thread detail page would stop updating. The cursors survive the reconnect,
   so the Bridge only replays what happened while the device was away. */
static void resubscribe_threads(void)
{
    if ((s_effective_caps & CBDP_CAP_SUBSCRIBE_THREAD) == 0) return;
    for (size_t index = 0; index < s_subscribed_thread_count; ++index) {
        uint32_t topic_id = 0;
        projection_lock();
        const bool known = codex_bridge_projection_thread_topic(
            &s_projection,
            s_subscribed_threads[index],
            &topic_id);
        projection_unlock();
        if (!known) continue;
        (void)send_subscribe(topic_id, CBDP_SCOPE_THREAD, CBDP_QOS_STATE);
        (void)send_subscribe(topic_id, CBDP_SCOPE_SUMMARY, CBDP_QOS_STATE);
    }
}

/* Issue one account read and return its request id so the RESULT can be
   recognised. The payload carries the same field set as the pushed
   account.changed / rate_limits.changed / token_usage.changed events, so both
   paths are decoded by the same projection helper. */
static esp_err_t send_account_read(uint16_t method_id, const char *label,
                                   uint32_t *out_request_id)
{
    if ((s_effective_caps & CBDP_CAP_ACCOUNT) == 0) return ESP_ERR_NOT_SUPPORTED;
    const uint32_t request_id = next_request_id();
    uint8_t idem[16];
    esp_fill_random(idem, sizeof(idem));
    uint8_t header_bytes[96];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, request_id);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_IDEM,
                                                                idem, sizeof(idem));
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_METHOD_ID,
                                                              method_id);
    /* The Bridge requires the params TLV on every COMMAND, even when the
       method takes no arguments; omitting it is answered with INVALID_PARAM. */
    if (error == ESP_OK) {
        uint8_t empty_params[8];
        codex_bridge_tlv_builder_t params = {0};
        codex_bridge_tlv_builder_init(&params, empty_params, sizeof(empty_params));
        error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_PARAMS,
                                               params.data, params.length);
    }
    if (error != ESP_OK) return error;
    esp_err_t send_result = send_business(CBDP_MSG_COMMAND,
                                          CBDP_FLAG_REQUEST | CBDP_FLAG_COMMAND,
                                          0, CBDP_QOS_STATE, &header);
    if (send_result == ESP_OK && out_request_id != NULL) {
        *out_request_id = request_id;
    }
    if (send_result == ESP_OK) {
        ESP_LOGI(TAG, "account read sent (%s) request=%lu", label,
                 (unsigned long)request_id);
    }
    return send_result;
}

static esp_err_t send_account_reads(void)
{
    if ((s_effective_caps & CBDP_CAP_ACCOUNT) == 0) return ESP_OK;
    /* Pull plan, window quotas and 7-day token usage once per attach. The
       events keep them current afterwards, and because both are served from
       the same committed store a pull can never contradict a push. */
    (void)send_account_read(CBDP_METHOD_ACCOUNT_READ, "account.read",
                            &s_account_read_request_id);
    (void)send_account_read(CBDP_METHOD_ACCOUNT_RATE_LIMITS_READ, "rate_limits.read",
                            &s_rate_limits_read_request_id);
    (void)send_account_read(CBDP_METHOD_ACCOUNT_TOKEN_USAGE_READ, "token_usage.read",
                            &s_token_usage_read_request_id);
    return ESP_OK;
}

static esp_err_t send_active_state_read(void)
{
    if (s_active_state_unsupported) return ESP_OK;
    if ((s_effective_caps & CBDP_CAP_CROSS_DEVICE_ACTIVE) == 0) return ESP_OK;
    const uint32_t request_id = next_request_id();
    uint8_t idem[16];
    esp_fill_random(idem, sizeof(idem));
    uint8_t params_bytes[8];
    codex_bridge_tlv_builder_t params = {0};
    codex_bridge_tlv_builder_init(&params, params_bytes, sizeof(params_bytes));
    uint8_t header_bytes[96];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, request_id);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_IDEM,
                                                                idem, sizeof(idem));
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_METHOD_ID,
                                                              CBDP_METHOD_ACTIVE_STATE_READ);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_PARAMS,
                                                                params.data, params.length);
    if (error != ESP_OK) return error;
    esp_err_t send_result = send_business(CBDP_MSG_COMMAND,
                                       CBDP_FLAG_REQUEST | CBDP_FLAG_COMMAND,
                                       0, CBDP_QOS_STATE, &header);
    if (send_result == ESP_OK) s_active_state_request_id = request_id;
    return send_result;
}

static uint8_t scope_for_topic(uint32_t topic_id)
{
    for (size_t index = 0; index < CBDP_CURSOR_MAX; ++index) {
        if (s_cursors[index].used && s_cursors[index].topic_id == topic_id) {
            return s_cursors[index].scope;
        }
    }
    if (topic_id == CBDP_TOPIC_SYSTEM_RESYNC) return CBDP_SCOPE_LIVE;
    if (topic_id == CBDP_TOPIC_SYSTEM_THREAD_SUMMARIES) return CBDP_SCOPE_SUMMARY;
    if ((topic_id & 0xF0000000u) == 0x10000000u) return CBDP_SCOPE_THREAD;
    return CBDP_SCOPE_SUMMARY;
}

/* §21.1 codes that invalidate the session: only these tear the connection
   down. Everything else (INVALID_FRAME, BUSY, PAYLOAD_TOO_LARGE, ...) is a
   per-request failure the peer already handled. */
static bool error_is_fatal(uint16_t code)
{
    switch (code) {
    case 0x0003: /* UNAUTHORIZED */
    case 0x0016: /* PROTOCOL_VERSION */
    case 0x0017: /* AUTH_EXPIRED */
    case 0x0018: /* TOKEN_ROTATED */
        return true;
    default:
        return false;
    }
}

static void publish_online_state(void)
{
    codex_bridge_message_t message = {0};
    strlcpy(message.type, "bridge_online", sizeof(message.type));
    if (s_handler != NULL) s_handler(&message, s_handler_context);
}

static void publish_offline_state(void)
{
    /* The TCP session is only one of the two channels. The UDP home-counter
       channel keeps the authoritative counters flowing, so the Bridge is
       still reachable as long as those datagrams arrive. Announcing
       "offline" here would blank the numbers that are still being
       delivered, and the panel would flip between live and dead every time
       the session was rejected. */
    if (home_counters_fresh()) {
        ESP_LOGI(TAG, "CBDP session ended, UDP home counters still live");
        return;
    }
    codex_bridge_message_t message = {0};
    strlcpy(message.type, "bridge_offline", sizeof(message.type));
    if (s_handler != NULL) s_handler(&message, s_handler_context);
}

static esp_err_t handle_authenticated_frame(
    int socket_fd,
    const codex_bridge_frame_t *frame,
    uint8_t *buffer
)
{
    (void)socket_fd;
    (void)buffer;
    if (frame->ack_seq != 0 && socket_lock_acquire()) {
        acknowledge_pending_locked(frame->ack_seq);
        socket_lock_release();
    }

    if (frame->message_type == CBDP_MSG_PING) {
        codex_bridge_tlv_t ping = {0};
        uint32_t ping_id = 0;
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_PING_ID, &ping) == ESP_OK) {
            (void)codex_bridge_tlv_u32(&ping, &ping_id);
        }
        return send_pong(ping_id);
    }
    if (frame->message_type == CBDP_MSG_PONG ||
        frame->message_type == CBDP_MSG_ACK) return ESP_OK;
    if (frame->message_type == CBDP_MSG_CLOSE) return ESP_ERR_INVALID_STATE;
    if (frame->message_type == CBDP_MSG_ERROR) {
        char detail[128] = {0};
        uint32_t request_id = 0;
        codex_bridge_tlv_t field = {0};
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_REQUEST_ID, &field) == ESP_OK) {
            (void)codex_bridge_tlv_u32(&field, &request_id);
        }
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_ERROR_MESSAGE, &field) == ESP_OK &&
            field.length < sizeof(detail)) {
            memcpy(detail, field.value, field.length);
            detail[field.length] = 0;
        }
        if (socket_lock_acquire()) {
            cancel_pending_request_locked(request_id);
            socket_lock_release();
        }
        /* A rejected SUBSCRIBE never produces a RESULT, so its replay window
           has to be closed here. */
        subscribe_window_close(request_id);
        replay_reconcile();
        if (frame->code == 0x000E && request_id != 0 &&
            request_id == s_active_state_request_id) {
            s_active_state_unsupported = true;
            s_active_state_request_id = 0;
            ESP_LOGI(TAG, "active_state.read unsupported by this Bridge; disabled");
        }
        if (frame->code == 0x000E && request_id != 0) {
            if (request_id == s_account_read_request_id ||
                request_id == s_rate_limits_read_request_id ||
                request_id == s_token_usage_read_request_id) {
                s_account_read_request_id = 0;
                s_rate_limits_read_request_id = 0;
                s_token_usage_read_request_id = 0;
                ESP_LOGI(TAG, "account reads unsupported by this Bridge; disabled");
            }
        }
        if (error_is_fatal(frame->code)) {
            ESP_LOGE(TAG, "peer ERROR code=0x%04x request=%lu msg=%s",
                     frame->code, (unsigned long)request_id, detail);
            /* UNAUTHORIZED / TOKEN_ROTATED mean the stored token is dead
               (revoked or rotated). Clear it so the next HELLO requests
               pairing again; AUTH_EXPIRED only asks for a fresh AUTH, so the
               token is kept. */
            if (frame->code == CBDP_ERR_UNAUTHORIZED ||
                frame->code == CBDP_ERR_TOKEN_ROTATED) {
                ESP_LOGW(TAG, "clearing stored token; re-pairing required");
                esp_err_t cleared = device_config_clear_codex_token();
                if (cleared != ESP_OK) {
                    ESP_LOGE(TAG, "token clear failed (%d)", cleared);
                }
            }
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGW(TAG, "peer ERROR code=0x%04x request=%lu msg=%s (kept)",
                 frame->code, (unsigned long)request_id, detail);
        return ESP_OK;
    }

    /* seq==0 is reserved for control frames. Current Bridge DELTA uses a
       nonzero transport seq; retain the old seq=0 behavior only for legacy
       builds while enforcing §13.2 for all sequenced frames. */
    const bool sequenced = frame->seq != 0;
    if (sequenced) {
        if (frame->seq < s_rx_expected) return send_ack(s_last_ack_seq, 0, 0, 0);
        if (frame->seq > s_rx_expected) {
            /* Tolerate a bridge that skips transport sequence numbers (for
               example after AUTH_OK). Re-sync to the newest frame instead of
               stalling or dropping the session; snapshot frames are complete
               states, so accepting the newer base converges immediately. */
            ESP_LOGW(TAG, "sequence gap expected=%lu got=%lu (resyncing)",
                     (unsigned long)s_rx_expected, (unsigned long)frame->seq);
            s_rx_expected = frame->seq;
            s_gap_count = 0;
        }
    }

    esp_err_t error = ESP_OK;
    uint64_t cursor = 0;
    /* While a replay window is open the arriving frames belong to the
       catch-up, not to a live change: they update the projection but never
       reach the panel, otherwise the counters flicker and the task chime
       fires for tasks that finished long ago. */
    const bool replay_quiet =
        (frame->message_type == CBDP_MSG_SNAPSHOT ||
         frame->message_type == CBDP_MSG_DELTA) &&
        subscribe_quiet_for_topic(frame->topic_id);
    if (replay_quiet) replay_frame_suppressed(frame->topic_id);
    projection_lock();
    codex_bridge_projection_set_quiet(&s_projection, replay_quiet);
    switch (frame->message_type) {
    case CBDP_MSG_SNAPSHOT:
        error = codex_bridge_projection_apply_snapshot(&s_projection, frame);
        if (error == ESP_OK) {
            if (!s_snapshot_seen) {
                s_snapshot_seen = true;
                ESP_LOGI(TAG, "first SNAPSHOT applied (topic=0x%08lx seq=%lu)",
                         (unsigned long)frame->topic_id, (unsigned long)frame->seq);
            } else {
                ESP_LOGI(TAG, "SNAPSHOT applied (topic=0x%08lx seq=%lu)",
                         (unsigned long)frame->topic_id, (unsigned long)frame->seq);
            }
            log_projection_counts("snapshot");
        }
        break;
    case CBDP_MSG_DELTA:
        error = codex_bridge_projection_apply_delta(&s_projection, frame, &cursor);
        if (error == ESP_OK) {
            if (!s_delta_seen) {
                s_delta_seen = true;
                ESP_LOGI(TAG, "first DELTA applied (topic=0x%08lx seq=%lu cursor=%llu)",
                         (unsigned long)frame->topic_id, (unsigned long)frame->seq,
                         (unsigned long long)cursor);
            } else {
                ESP_LOGI(TAG, "DELTA applied (topic=0x%08lx seq=%lu cursor=%llu)",
                         (unsigned long)frame->topic_id, (unsigned long)frame->seq,
                         (unsigned long long)cursor);
            }
            log_projection_counts("delta");
        }
        break;
    case CBDP_MSG_CAPABILITY_UPDATE: {
        codex_bridge_tlv_t field = {0};
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_EFFECTIVE_CAPS, &field) == ESP_OK) {
            (void)codex_bridge_tlv_u64(&field, &s_effective_caps);
        }
        break;
    }
    case CBDP_MSG_RESYNC: {
        /* The Bridge lost the history of this topic and restarted its cursor
           there. Forget the local cursor and mute the topic for the rest of
           this subscription, because the Bridge re-sends the current state
           as part of the resync. */
        codex_bridge_tlv_t field = {0};
        uint32_t resync_topic = frame->topic_id;
        uint8_t resync_scope = scope_for_topic(frame->topic_id);
        uint32_t resync_request = 0;
        uint32_t resync_reason = 0;
        uint8_t resync_reason8 = 0;
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_TOPIC_ID, &field) == ESP_OK) {
            (void)codex_bridge_tlv_u32(&field, &resync_topic);
        }
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_SCOPE, &field) == ESP_OK) {
            (void)codex_bridge_tlv_u8(&field, &resync_scope);
        }
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_REQUEST_ID, &field) == ESP_OK) {
            (void)codex_bridge_tlv_u32(&field, &resync_request);
        }
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_RESYNC_REASON, &field) == ESP_OK) {
            if (codex_bridge_tlv_u8(&field, &resync_reason8) == ESP_OK) {
                resync_reason = resync_reason8;
            } else {
                (void)codex_bridge_tlv_u32(&field, &resync_reason);
            }
        }
        cursor_forget(resync_topic, resync_scope);
        /* The Bridge restarted its event history, so its global sequence
           numbers start over at a lower value. Release the stale-frame guard
           of the state event, otherwise the panel would never see a new
           counter again. The duplicate detection ring belongs to the same
           session, so it is released together with the watermark. */
        codex_bridge_projection_forget_session_state(&s_projection);
        if (resync_request != 0) {
            subscribe_window_open(resync_request, resync_topic, resync_scope, true);
        }
        ESP_LOGW(TAG, "RESYNC topic=0x%08lx scope=%u reason=%lu request=%lu",
                 (unsigned long)resync_topic, (unsigned)resync_scope,
                 (unsigned long)resync_reason, (unsigned long)resync_request);
        break;
    }
    case CBDP_MSG_RESULT: {
        codex_bridge_tlv_t req = {0};
        uint32_t result_request = 0;
        if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_REQUEST_ID, &req) == ESP_OK) {
            (void)codex_bridge_tlv_u32(&req, &result_request);
        }
        if (result_request != 0 && socket_lock_acquire()) {
            cancel_pending_request_locked(result_request);
            socket_lock_release();
        }
        /* A pull of the account families carries the same field set as the
           pushed events, so both are decoded by the same helper. Routing it
           here keeps the panel identical whether the value arrived by push or
           by pull. */
        uint16_t account_event = 0;
        uint16_t account_event_method = 0;
        if (result_request != 0 && result_request == s_account_read_request_id) {
            account_event = CBDP_EVENT_ACCOUNT_CHANGED;
            account_event_method = CBDP_METHOD_ACCOUNT_READ;
            s_account_read_request_id = 0;
        } else if (result_request != 0 && result_request == s_rate_limits_read_request_id) {
            account_event = CBDP_EVENT_RATE_LIMITS_CHANGED;
            account_event_method = CBDP_METHOD_ACCOUNT_RATE_LIMITS_READ;
            s_rate_limits_read_request_id = 0;
        } else if (result_request != 0 && result_request == s_token_usage_read_request_id) {
            account_event = CBDP_EVENT_TOKEN_USAGE_CHANGED;
            account_event_method = CBDP_METHOD_ACCOUNT_TOKEN_USAGE_READ;
            s_token_usage_read_request_id = 0;
        }
        if (account_event != 0) {
            projection_lock();
            const esp_err_t apply_error = codex_bridge_projection_apply_account_result(
                &s_projection, account_event_method, frame->header_tlv,
                frame->header_tlv_len);
            projection_unlock();
            if (apply_error != ESP_OK) {
                ESP_LOGW(TAG, "account read result rejected: %s",
                         esp_err_to_name(apply_error));
            }
        }
        /* The RESULT ends the replay of the matching SUBSCRIBE. */
        subscribe_window_close(result_request);
        replay_reconcile();
        if (frame->code != 0) {
            ESP_LOGW(TAG, "CBDP command failed code=0x%04x", frame->code);
        }
        break;
    }
    case CBDP_MSG_APPROVAL_REQUEST:
    case CBDP_MSG_DEVICE_STATUS:
        break;
    case CBDP_MSG_FRAGMENT:
        error = ESP_ERR_NOT_SUPPORTED;
        break;
    default:
        break;
    }

    /* The quiet gate only applies to the frame that was just applied. */
    codex_bridge_projection_set_quiet(&s_projection, false);
    projection_unlock();

    if (error != ESP_OK) {
        (void)send_error(error == ESP_ERR_NOT_SUPPORTED ? 0x000E : 0x0005,
                         0,
                         error == ESP_ERR_NOT_SUPPORTED
                             ? "fragmentation unsupported"
                             : "frame processing failed");
        return error;
    }
    if (!sequenced && frame->message_type == CBDP_MSG_DELTA && !s_seq0_warned) {
        s_seq0_warned = true;
        ESP_LOGW(TAG, "event type=0x%02x arrived with seq=0 topic=0x%08lx",
                 frame->message_type, (unsigned long)frame->topic_id);
    }
    const uint8_t event_scope = (frame->message_type == CBDP_MSG_DELTA)
        ? codex_bridge_projection_delta_scope(frame)
        : scope_for_topic(frame->topic_id);
    if (cursor != 0 && frame->topic_id != 0) {
        cursor_store(frame->topic_id, event_scope, cursor);
    }
    if (sequenced) {
        s_last_ack_seq = frame->seq;
        s_rx_expected = frame->seq + 1;
        if (s_rx_expected == 0) s_rx_expected = 1;
        s_gap_count = 0;
    }
    const uint32_t topic_id = frame->message_type == CBDP_MSG_DELTA ? frame->topic_id : 0;
    return send_ack(
        s_last_ack_seq,
        cursor != 0 ? topic_id : 0,
        cursor != 0 ? event_scope : 0,
        cursor
    );
}

static bool retransmit_due(void)
{
    if (!socket_lock_acquire()) {
        /* The mutex is busy for a moment. That is not a broken link, so the
           session keeps running. */
        ESP_LOGW(TAG, "retransmit skipped: socket lock busy");
        return true;
    }
    const TickType_t now = xTaskGetTickCount();
    for (size_t index = 0; index < CBDP_PENDING_MAX; ++index) {
        cbdp_pending_frame_t *pending = &s_pending[index];
        if (!pending->used || now < pending->next_retry) continue;
        if (pending->retries >= s_max_retries || s_client_socket < 0) {
            pending->used = false;
            continue;
        }
        if (send_all(s_client_socket, pending->bytes, pending->length)) {
            pending->retries++;
            pending->next_retry = now + pdMS_TO_TICKS(s_retransmit_timeout_ms);
        } else {
            pending->used = false;
        }
    }
    socket_lock_release();
    return !s_link_broken;
}

static void session_run(int client_socket)
{
    /* The receive buffer is the single largest heap consumer of this task.
       Take it from PSRAM so the scarce internal DRAM stays available for
       WiFi, LwIP and the Xiaozhi TLS session; fall back to internal DRAM
       only when PSRAM cannot serve the request. */
    uint8_t *buffer = heap_caps_malloc(CBDP_FRAME_BUFFER_MAX,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(CBDP_FRAME_BUFFER_MAX, MALLOC_CAP_8BIT);
        if (buffer != NULL) {
            ESP_LOGW(TAG, "CBDP frame buffer fell back to internal DRAM");
        }
    }
    if (buffer == NULL) {
        ESP_LOGE(TAG, "CBDP frame buffer alloc failed (need %u bytes)",
                 (unsigned)CBDP_FRAME_BUFFER_MAX);
        log_heap_status("alloc-failed");
        publish_offline_state();
        return;
    }
    codex_bridge_frame_t frame = {0};
    s_tx_seq = 1;
    s_rx_expected = 1;
    s_gap_count = 0;
    s_last_ack_seq = 0;
    s_effective_caps = 0;
    s_active_state_request_id = 0;
    s_account_read_request_id = 0;
    s_rate_limits_read_request_id = 0;
    s_token_usage_read_request_id = 0;
    s_seq0_warned = false;
    /* The cursors and the projection survive a reconnect: the Bridge then
       only has to deliver what happened while the device was away instead of
       the whole history, and the panel keeps the last known counters instead
       of blanking them. */
    s_snapshot_seen = false;
    s_delta_seen = false;
    clear_pending_locked();
    subscribe_windows_clear();

    esp_err_t error = send_hello();
    if (error != ESP_OK) { ESP_LOGW(TAG, "handshake: send HELLO failed (%d)", error); goto done; }
    error = wait_for_message(client_socket, CBDP_MSG_WELCOME, buffer,
                             CBDP_FRAME_BUFFER_MAX, &frame,
                             CBDP_HANDSHAKE_TIMEOUT_MS);
    if (error != ESP_OK) { ESP_LOGW(TAG, "handshake: WELCOME failed (%d)", error); goto done; }
    ESP_LOGI(TAG, "handshake: WELCOME received");
    if ((frame.flags & CBDP_FLAG_RESPONSE) == 0) {
        (void)send_error(0x0005, 0, "WELCOME missing response flag");
        goto done;
    }
    error = parse_welcome(&frame);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "handshake: parse WELCOME failed (%d)", error);
        (void)send_error(error == ESP_ERR_NOT_SUPPORTED ? 0x000E : 0x0005, 0,
                         "WELCOME rejected");
        goto done;
    }
    if (!codex_bridge_token_available()) {
        /* 11.8: no token yet. Stay on this connection and wait for the
           Bridge's PAIRING offer; sending AUTH without a key would just be
           rejected. The approval itself happens out of band on the host. */
        ESP_LOGI(TAG, "handshake: awaiting PAIRING offer");
        error = wait_for_message(client_socket, CBDP_MSG_PAIRING, buffer,
                                 CBDP_FRAME_BUFFER_MAX, &frame,
                                 CBDP_PAIRING_TIMEOUT_MS);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "handshake: PAIRING offer failed (%d)", error);
            goto done;
        }
        char paired_token[CBDP_PAIRING_TOKEN_MAX_BYTES + 1] = {0};
        error = codex_bridge_parse_pairing_offer(
            frame.header_tlv, frame.header_tlv_len,
            paired_token, sizeof(paired_token));
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "handshake: PAIRING offer rejected (%d)", error);
            (void)send_error(0x0006, 0, "PAIRING offer rejected");
            goto done;
        }
        error = device_config_store_codex_token(paired_token);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "handshake: pairing token save failed (%d)", error);
            goto done;
        }
        /* Never log the token itself, only its length. */
        ESP_LOGI(TAG, "handshake: PAIRING accepted, token_bytes=%u",
                 (unsigned)strlen(paired_token));
    }
    error = send_auth();
    if (error != ESP_OK) { ESP_LOGW(TAG, "handshake: send AUTH failed (%d)", error); goto done; }
    error = wait_for_message(client_socket, CBDP_MSG_AUTH_OK, buffer,
                             CBDP_FRAME_BUFFER_MAX, &frame,
                             CBDP_HANDSHAKE_TIMEOUT_MS);
    if (error != ESP_OK) { ESP_LOGW(TAG, "handshake: AUTH_OK failed (%d)", error); goto done; }
    ESP_LOGI(TAG, "handshake: AUTH_OK received");
    if ((frame.flags & CBDP_FLAG_RESPONSE) == 0) {
        ESP_LOGW(TAG, "handshake: AUTH_OK missing response flag");
        (void)send_error(0x0005, 0, "AUTH_OK missing response flag");
        goto done;
    }
    error = parse_auth_ok(&frame);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "handshake: parse AUTH_OK failed (%d)", error);
        (void)send_error(0x0003, 0, "authentication failed");
        goto done;
    }

    /* A new authenticated session can be served by a different Bridge
       process. The Bridge restarts its global sequence numbers when it
       rebuilds its event store, so the watermark of the previous session
       would mark every snapshot of the new instance as stale and the panel
       would keep the counters of the dead instance forever. Release the
       watermark together with the duplicate detection ring of that session,
       so the fresh attach snapshot of the new instance is accepted. */
    projection_lock();
    codex_bridge_projection_forget_session_state(&s_projection);
    projection_unlock();

    s_online = true;
    ESP_LOGI(TAG, "CBDP v1 online profile=panel caps=0x%llx heap=%u psram=%u",
             (unsigned long long)s_effective_caps,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    publish_online_state();
    if (s_effective_caps & CBDP_CAP_SUBSCRIBE_SUMMARY) {
        (void)send_subscribe(CBDP_TOPIC_SYSTEM_THREAD_SUMMARIES,
                             CBDP_SCOPE_SUMMARY, CBDP_QOS_STATE);
    }
    if (s_effective_caps & CBDP_CAP_SUBSCRIBE_LIVE) {
        (void)send_subscribe(CBDP_TOPIC_SYSTEM_RESYNC,
                             CBDP_SCOPE_LIVE, CBDP_QOS_CRITICAL);
    }
    resubscribe_threads();
    (void)send_active_state_read();
    (void)send_account_reads();

    const TickType_t heartbeat_ticks = pdMS_TO_TICKS((uint32_t)s_heartbeat_seconds * 1000u);
    TickType_t next_ping = xTaskGetTickCount() + heartbeat_ticks;
    TickType_t pong_deadline = 0;
    bool awaiting_pong = false;
    uint32_t ping_id = 0;
    TickType_t next_alive = xTaskGetTickCount() + pdMS_TO_TICKS(CBDP_ALIVE_LOG_MS);
    log_heap_status("online");

    while (s_running) {
        if (s_link_broken) break;
        error = read_frame(
            client_socket,
            buffer,
            CBDP_FRAME_BUFFER_MAX,
            &frame,
            xTaskGetTickCount() + pdMS_TO_TICKS(CBDP_SOCKET_POLL_MS)
        );
        if (error == ESP_OK) {
            error = handle_authenticated_frame(client_socket, &frame, buffer);
            if (error != ESP_OK) break;
            if (frame.message_type == CBDP_MSG_PONG) {
                awaiting_pong = false;
                pong_deadline = 0;
            }
        } else if (error != ESP_ERR_TIMEOUT) {
            break;
        }

        const TickType_t now = xTaskGetTickCount();
        /* The counters that arrive over UDP are applied by the UDP task
           itself, so they reach the panel even while no authenticated TCP
           session exists. */
        if (!retransmit_due()) break;
        if (now >= next_ping) {
            ping_id++;
            if (ping_id == 0) ping_id = 1;
            esp_err_t ping_error = send_ping(ping_id);
            if (ping_error == ESP_OK) {
                awaiting_pong = true;
                pong_deadline = now + heartbeat_ticks * 3;
            } else {
                /* A failed ping used to be swallowed unless the link was
                   already flagged broken, so a dead uplink produced no log
                   line at all. Report it and keep the session only while the
                   link is still considered usable. */
                ESP_LOGW(TAG, "CBDP ping failed: err=%d link_broken=%d",
                         (int)ping_error, s_link_broken ? 1 : 0);
                if (s_link_broken) {
                    ESP_LOGW(TAG, "CBDP ping failed, link broken");
                    break;
                }
            }
            next_ping = now + heartbeat_ticks;
        }
        if (now >= next_alive) {
            ESP_LOGI(TAG, "CBDP session alive (ping_id=%lu awaiting_pong=%d)",
                     (unsigned long)ping_id, awaiting_pong ? 1 : 0);
            log_heap_status("alive");
            next_alive = now + pdMS_TO_TICKS(CBDP_ALIVE_LOG_MS);
        }
        if (awaiting_pong && pong_deadline != 0 && now >= pong_deadline) {
            ESP_LOGW(TAG, "CBDP heartbeat timed out");
            break;
        }
    }

done:
    s_online = false;
    publish_offline_state();
    clear_pending_locked();
    if (!s_link_broken) (void)send_close("session ended");
    heap_caps_free(buffer);
    ESP_LOGI(TAG, "CBDP session closed");
}
/* Resolve the local IPv4 address in host byte order so that the subnet math
   below stays readable. */
static bool local_ipv4_address(uint32_t *out)
{
    if (out == NULL) return false;
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif == NULL) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    if (info.ip.addr == 0) return false;
    *out = ntohl((uint32_t)info.ip.addr);
    return true;
}

/* Broadcast address of the subnet the device is attached to. */
static bool subnet_broadcast_address(uint32_t *out)
{
    if (out == NULL) return false;
    uint32_t address = 0;
    if (!local_ipv4_address(&address)) return false;
    uint32_t mask = 0xFFFFFF00u;
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif != NULL) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.netmask.addr != 0) {
            mask = ntohl((uint32_t)info.netmask.addr);
        }
    }
    *out = address | (~mask & 0xFFFFFFFFu);
    return true;
}

/* Remember where the Bridge lives. Every datagram the Bridge sends on the
   discovery port reveals its address, which is exactly the address the
   presence announcements have to be sent to. */
static void note_bridge_source(const struct sockaddr_in *source)
{
    if (source == NULL) return;
    uint32_t source_ip = ntohl(source->sin_addr.s_addr);
    if (source_ip == 0) return;
    uint32_t local_ip = 0;
    if (local_ipv4_address(&local_ip) && source_ip == local_ip) return;
    if (s_bridge_ip_known && s_bridge_ip == source_ip) return;
    s_bridge_ip = source_ip;
    s_bridge_ip_known = true;
    char learned[16];
    snprintf(learned, sizeof(learned), "%u.%u.%u.%u",
             (unsigned)((source_ip >> 24) & 0xFFu),
             (unsigned)((source_ip >> 16) & 0xFFu),
             (unsigned)((source_ip >> 8) & 0xFFu),
             (unsigned)(source_ip & 0xFFu));
    ESP_LOGI(TAG, "bridge seen at %s", learned);

    /* Hand the address to connectivity. It stays a candidate until a
       management request against it succeeds, so a multi-homed host cannot
       re-point the device at a NIC that does not serve 8787. */
    connectivity_note_bridge_ipv4(learned);
}

/* The Bridge publishes its authoritative task counters over UDP on the
   discovery port (message 0x1E). The counters live in the header TLVs, not in
   the payload. This channel updates the panel every two seconds even when the
   counters did not change, so the panel never shows a frozen number. */
static void apply_home_counters_datagram(const codex_bridge_frame_t *frame)
{
    if (frame == NULL || frame->header_tlv == NULL || frame->header_tlv_len == 0) return;
    cbdp_home_counters_t item;
    memset(&item, 0, sizeof(item));
    codex_bridge_tlv_t tlv;
    if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                              CBDP_TLV_RUNNING_COUNT, &tlv) == ESP_OK &&
        codex_bridge_tlv_u16(&tlv, &item.running) == ESP_OK) {
        item.have_running = true;
    }
    if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                              CBDP_TLV_COMPLETED_UNREAD, &tlv) == ESP_OK &&
        codex_bridge_tlv_u16(&tlv, &item.completed) == ESP_OK) {
        item.have_completed = true;
    }
    if (codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                              CBDP_TLV_FAILED_UNREAD, &tlv) == ESP_OK &&
        codex_bridge_tlv_u16(&tlv, &item.failed) == ESP_OK) {
        item.have_failed = true;
    }
    if (!item.have_running && !item.have_completed && !item.have_failed) {
        ESP_LOGW(TAG, "home counters 0x1E without counter tag");
        return;
    }
    /* The counters are authoritative and they have to reach the panel even
       when the TCP session cannot be authenticated. They are therefore
       applied to the projection right here instead of being queued for the
       TCP task, which would only drain the queue after a successful
       handshake. */
    s_home_counters_valid = true;
    s_home_counters_last_ms = (uint32_t)(esp_timer_get_time() / 1000);
    projection_lock();
    const bool published = codex_bridge_projection_apply_home_counters(
        &s_projection,
        item.have_running, item.running,
        item.have_completed, item.completed,
        item.have_failed, item.failed);
    projection_unlock();
    /* "published" tells whether this frame reached the panel. A zero here
       only means the numbers did not change since the previous frame, it is
       not an error and not a stall. */
    ESP_LOGI(TAG,
             "home counters 0x1E running=%u completed_unread=%u failed_unread=%u published=%d",
             (unsigned)item.running, (unsigned)item.completed,
             (unsigned)item.failed, published ? 1 : 0);
}

/* Announce this device to the Bridge. The Bridge listens on the discovery
   port for devices that announce themselves and dials them right away, which
   is the only way for the two to find each other again after the Bridge
   process was restarted. */
static void send_presence_datagram(int socket_fd)
{
    if (socket_fd < 0) return;
    uint8_t packet[512];
    size_t packet_length = 0;
    esp_err_t error = codex_bridge_build_presence_reply(
        device_identity_get(),
        DEVICE_MODEL,
        esp_app_get_description()->version,
        CBDP_PROFILE_PANEL,
        SUPPORTED_CAPS,
        CBDP_TCP_PORT,
        0,
        packet,
        sizeof(packet),
        &packet_length
    );
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "presence build failed: %d", (int)error);
        return;
    }

    struct sockaddr_in targets[3];
    size_t target_count = 0;
    if (s_bridge_ip_known) {
        targets[target_count].sin_family = AF_INET;
        targets[target_count].sin_port = htons(CBDP_DISCOVERY_PORT);
        targets[target_count].sin_addr.s_addr = htonl(s_bridge_ip);
        target_count++;
    }
    uint32_t broadcast = 0;
    if (subnet_broadcast_address(&broadcast)) {
        targets[target_count].sin_family = AF_INET;
        targets[target_count].sin_port = htons(CBDP_DISCOVERY_PORT);
        targets[target_count].sin_addr.s_addr = htonl(broadcast);
        target_count++;
    }
    targets[target_count].sin_family = AF_INET;
    targets[target_count].sin_port = htons(CBDP_DISCOVERY_PORT);
    targets[target_count].sin_addr.s_addr = htonl(INADDR_BROADCAST);
    target_count++;

    size_t sent = 0;
    for (size_t index = 0; index < target_count; index++) {
        int written = sendto(socket_fd, packet, packet_length, 0,
                             (struct sockaddr *)&targets[index],
                             sizeof(targets[index]));
        if (written >= 0) sent++;
    }
    ESP_LOGI(TAG, "presence announced (%u bytes, %u/%u target(s) reached)",
             (unsigned)packet_length, (unsigned)sent, (unsigned)target_count);
}

/* Keep announcing this device so that a restarted Bridge finds it again. The
   task never blocks on anything, so a stop request is honoured at once. */
static void presence_task(void *context)
{
    (void)context;
    /* Pairing mode starts this task before lwIP exists; socket() would assert. */
    while (s_running && !connectivity_is_wifi_connected()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    int socket_fd = -1;
    while (s_running) {
        if (socket_fd < 0) {
            socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (socket_fd < 0) {
                ESP_LOGW(TAG, "presence socket failed: %d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int broadcast = 1;
            setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
            s_presence_socket = socket_fd;
        }
        send_presence_datagram(socket_fd);
        for (int elapsed = 0; elapsed < CBDP_PRESENCE_INTERVAL_MS && s_running; elapsed += 250) {
            vTaskDelay(pdMS_TO_TICKS(250));
            if (s_presence_wake) {
                s_presence_wake = false;
                break;
            }
        }
    }
    /* The owner of this task (codex_bridge_client_stop) deletes it and
       closes the socket. Self deletion would free the task handle that the
       owner still holds, and a later vTaskDelete on that handle would
       corrupt the heap. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* One udp_task instance owns these buffers for its whole life, so keeping
   them out of the task stack is safe and buys about 1.5 kB of headroom for
   the counter path, which ends in a vprintf call several frames deep. */
static uint8_t s_udp_request_bytes[CBDP_DISCOVERY_MAX_BYTES];
static uint8_t s_udp_reply[512];

static void udp_task(void *context)
{
    (void)context;
    /* Pairing mode starts the listener before Wi-Fi has configured lwIP.
       socket() asserts with an invalid mbox until the TCP/IP task exists, so
       wait for the station instead of racing it. */
    while (s_running && !connectivity_is_wifi_connected()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!s_running) return;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "UDP socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CBDP_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "UDP bind failed: %d", errno);
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "CBDP discovery listening on UDP %d", CBDP_DISCOVERY_PORT);
    while (s_running) {
        uint8_t *request_bytes = s_udp_request_bytes;
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        int length = recvfrom(socket_fd, request_bytes, CBDP_DISCOVERY_MAX_BYTES, 0,
                              (struct sockaddr *)&source, &source_length);
        if (length <= 0) {
            if (!s_running) break;
            continue;
        }
        codex_bridge_frame_t request = {0};
        if (codex_bridge_frame_parse(request_bytes, (size_t)length, &request) != ESP_OK) {
            continue;
        }
        /* The discovery requests and the counter datagrams both reveal the
           address of the Bridge, which is where the presence announcements
           have to go after the Bridge was restarted. */
        if (request.message_type == CBDP_MSG_DISCOVERY ||
            request.message_type == CBDP_MSG_HOME_COUNTERS) {
            note_bridge_source(&source);
        }
        if (request.message_type == CBDP_MSG_HOME_COUNTERS) {
            apply_home_counters_datagram(&request);
            continue;
        }
        if (request.message_type != CBDP_MSG_DISCOVERY) {
            continue;
        }
        uint8_t *reply = s_udp_reply;
        size_t reply_length = 0;
        esp_err_t error = codex_bridge_build_discovery_reply(
            &request,
            device_identity_get(),
            DEVICE_MODEL,
            esp_app_get_description()->version,
            CBDP_PROFILE_PANEL,
            SUPPORTED_CAPS,
            CBDP_TCP_PORT,
            0,
            reply,
            sizeof(s_udp_reply),
            &reply_length
        );
        if (error == ESP_OK) {
            sendto(socket_fd, reply, reply_length, 0,
                   (struct sockaddr *)&source, source_length);
        }
    }
    close(socket_fd);
    s_udp_task = NULL;
    vTaskDelete(NULL);
}

static void tcp_task(void *context)
{
    (void)context;
    /* Same pairing-mode race as udp_task: lwIP must exist before socket(). */
    while (s_running && !connectivity_is_wifi_connected()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!s_running) return;
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        ESP_LOGE(TAG, "TCP socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CBDP_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server_socket, CBDP_LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "TCP listen failed: %d", errno);
        close(server_socket);
        vTaskDelete(NULL);
        return;
    }
    (void)socket_lock_acquire_wait(CBDP_SOCKET_LOCK_WAIT_MS);
    s_server_socket = server_socket;
    if (s_socket_lock != NULL) socket_lock_release();
    ESP_LOGI(TAG, "CBDP device listener on TCP %d", CBDP_TCP_PORT);

    while (s_running) {
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        int client_socket = accept(server_socket,
                                   (struct sockaddr *)&source,
                                   &source_length);
        if (client_socket < 0) {
            if (!s_running) break;
            /* A failing accept() returns immediately, so a tight loop would
               starve the other tasks.  Back off a little instead. */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        (void)socket_lock_acquire_wait(CBDP_SOCKET_LOCK_WAIT_MS);
        close_socket(&s_client_socket);
        s_client_socket = client_socket;
        if (s_socket_lock != NULL) socket_lock_release();

        s_link_broken = false;
        session_run(client_socket);
        s_online = false;
        /* The device is the TCP server in this protocol, so the Bridge can
           only dial back in after the device announced itself.  Ask the
           presence task to do that now, so a restarted Bridge is picked up
           within a fraction of a second instead of a full presence period. */
        s_presence_wake = true;
        (void)socket_lock_acquire_wait(CBDP_SOCKET_LOCK_WAIT_MS);
        if (s_client_socket == client_socket) s_client_socket = -1;
        close_socket(&client_socket);
        if (s_socket_lock != NULL) socket_lock_release();
    }
    (void)socket_lock_acquire_wait(CBDP_SOCKET_LOCK_WAIT_MS);
    close_socket(&s_server_socket);
    if (s_socket_lock != NULL) socket_lock_release();
    s_tcp_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t codex_bridge_client_start(
    codex_bridge_message_handler_t handler,
    void *context
)
{
    if (s_running) return ESP_OK;
    if (s_pending == NULL) {
        s_pending = heap_caps_calloc(CBDP_PENDING_MAX, sizeof(cbdp_pending_frame_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pending == NULL) {
            ESP_LOGE(TAG, "CBDP retransmit window allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    /* A device with no token must still start: it listens on 8765, keeps
       broadcasting UDP presence so the admin page can show it as pending, and
       requests pairing in its HELLO. Refusing to start here made the device
       invisible and pairing impossible. */
    if (!codex_bridge_token_available()) {
        ESP_LOGI(TAG, "No CodexBridge token yet; starting in pairing mode");
    }
    s_handler = handler;
    s_handler_context = context;
    s_socket_lock = xSemaphoreCreateMutex();
    if (s_socket_lock == NULL) return ESP_ERR_NO_MEM;
    s_window_lock = xSemaphoreCreateMutex();
    if (s_window_lock == NULL) {
        vSemaphoreDelete(s_socket_lock);
        s_socket_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = codex_bridge_projection_init(&s_projection, handler, context);
    if (error != ESP_OK) {
        vSemaphoreDelete(s_window_lock);
        s_window_lock = NULL;
        vSemaphoreDelete(s_socket_lock);
        s_socket_lock = NULL;
        return error;
    }
    s_projection_lock = xSemaphoreCreateRecursiveMutex();
    if (s_projection_lock == NULL) {
        vSemaphoreDelete(s_window_lock);
        s_window_lock = NULL;
        vSemaphoreDelete(s_socket_lock);
        s_socket_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_home_counters_valid = false;
    s_home_counters_last_ms = 0;
    memset(s_cursors, 0, sizeof(s_cursors));
    subscribe_windows_clear();
    s_request_id = 1;
    s_running = true;
    /* Stacks must stay in internal DRAM. A PSRAM stack is unavailable while
       the flash cache is frozen (NVS commits, flash writes) and the firmware
       asserts with s_task_stack_is_sane_when_cache_frozen() if such a task
       runs then. */
    if (xTaskCreate(udp_task, "cbdp_udp", CBDP_UDP_TASK_STACK, NULL, 4, &s_udp_task) != pdPASS ||
        xTaskCreate(tcp_task, "cbdp_tcp", CBDP_TCP_TASK_STACK, NULL, 4, &s_tcp_task) != pdPASS ||
        xTaskCreate(presence_task, "cbdp_presence", CBDP_PRESENCE_TASK_STACK, NULL, 4, &s_presence_task) != pdPASS) {
        codex_bridge_client_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void codex_bridge_client_stop(void)
{
    s_running = false;
    s_online = false;
    (void)socket_lock_acquire_wait(CBDP_SOCKET_LOCK_WAIT_MS);
    close_socket(&s_client_socket);
    close_socket(&s_server_socket);
    if (s_socket_lock != NULL) socket_lock_release();
    if (s_tcp_task != NULL) {
        vTaskDelete(s_tcp_task);
        s_tcp_task = NULL;
    }
    if (s_udp_task != NULL) {
        vTaskDelete(s_udp_task);
        s_udp_task = NULL;
    }
    /* The presence task clears no handle of its own: vTaskDelete(NULL)
       would delete the calling task instead of the presence task. */
    if (s_presence_task != NULL) {
        TaskHandle_t presence = s_presence_task;
        s_presence_task = NULL;
        vTaskDelete(presence);
    }
    if (s_presence_socket >= 0) {
        close(s_presence_socket);
        s_presence_socket = -1;
    }
    if (s_socket_lock != NULL) {
        s_socket_lock_owner = NULL;
        vSemaphoreDelete(s_socket_lock);
        s_socket_lock = NULL;
    }
    /* The projection lock is released after every task that could still
       touch the projection. */
    if (s_projection_lock != NULL) {
        vSemaphoreDelete(s_projection_lock);
        s_projection_lock = NULL;
    }
    s_home_counters_valid = false;
    s_home_counters_last_ms = 0;
    codex_bridge_projection_reset(&s_projection);
    memset(s_cursors, 0, sizeof(s_cursors));
    subscribe_windows_clear();
    if (s_window_lock != NULL) {
        vSemaphoreDelete(s_window_lock);
        s_window_lock = NULL;
    }
    s_handler = NULL;
    s_handler_context = NULL;
}

bool codex_bridge_client_is_online(void)
{
    /* Either channel is enough to consider the Bridge reachable: an
       authenticated TCP session, or home counters that are still arriving
       over UDP. The latter is what keeps the panel alive after a Bridge
       restart, where the device reconnects within a couple of seconds. */
    if (s_online) return true;
    return home_counters_fresh();
}

size_t codex_bridge_client_capability_count(void)
{
    size_t count = 0;
    for (size_t index = 0;
         index < sizeof(CAPABILITY_NAMES) / sizeof(CAPABILITY_NAMES[0]);
         ++index) {
        if (s_effective_caps & CAPABILITY_NAMES[index].bit) count++;
    }
    return count;
}

const char *codex_bridge_client_capability_at(size_t index)
{
    for (size_t current = 0;
         current < sizeof(CAPABILITY_NAMES) / sizeof(CAPABILITY_NAMES[0]);
         ++current) {
        if ((s_effective_caps & CAPABILITY_NAMES[current].bit) == 0) continue;
        if (index == 0) return CAPABILITY_NAMES[current].name;
        index--;
    }
    return NULL;
}

bool codex_bridge_client_has_capability(const char *capability)
{
    if (capability == NULL) return false;
    if (strcmp(capability, "account") == 0) {
        return (s_effective_caps & CBDP_CAP_ACCOUNT) != 0;
    }
    for (size_t index = 0;
         index < sizeof(CAPABILITY_NAMES) / sizeof(CAPABILITY_NAMES[0]);
         ++index) {
        if (strcmp(CAPABILITY_NAMES[index].name, capability) == 0) {
            return (s_effective_caps & CAPABILITY_NAMES[index].bit) != 0;
        }
    }
    return false;
}

static bool parse_prefixed_ref(const char *value, const char *prefix, uint32_t *out)
{
    if (value == NULL || prefix == NULL || out == NULL) return false;
    const size_t prefix_length = strlen(prefix);
    if (strncmp(value, prefix, prefix_length) != 0) return false;
    const char *number = value + prefix_length;
    if (*number == '\0') return false;
    char *end = NULL;
    unsigned long parsed = strtoul(number, &end, 10);
    if (end == number || *end != '\0' || parsed == 0 || parsed > 0x0FFFFFFFUL) {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
}

esp_err_t codex_bridge_client_subscribe_thread(const char *thread_id)
{
    if (thread_id == NULL || thread_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!s_online) return ESP_ERR_INVALID_STATE;
    if ((s_effective_caps & CBDP_CAP_SUBSCRIBE_THREAD) == 0) return ESP_ERR_NOT_SUPPORTED;
    for (size_t index = 0; index < s_subscribed_thread_count; ++index) {
        if (strcmp(s_subscribed_threads[index], thread_id) == 0) return ESP_OK;
    }
    uint32_t topic_id = 0;
    projection_lock();
    const bool known = codex_bridge_projection_thread_topic(&s_projection,
                                                           thread_id,
                                                           &topic_id);
    projection_unlock();
    if (!known) return ESP_ERR_NOT_FOUND;
    esp_err_t error = send_subscribe(topic_id, CBDP_SCOPE_THREAD, CBDP_QOS_STATE);
    if (error == ESP_OK) {
        (void)send_subscribe(topic_id, CBDP_SCOPE_SUMMARY, CBDP_QOS_STATE);
    }
    if (error != ESP_OK) return error;
    if (s_subscribed_thread_count < CBDP_SUBSCRIBED_THREAD_MAX) {
        strlcpy(s_subscribed_threads[s_subscribed_thread_count], thread_id,
                sizeof(s_subscribed_threads[0]));
        s_subscribed_thread_count++;
    }
    ESP_LOGI(TAG, "subscribed thread topic=0x%08lx scopes=thread+summary",
             (unsigned long)topic_id);
    return ESP_OK;
}

esp_err_t codex_bridge_client_send_read_ack(const char *message_id)
{
    if (!s_online || message_id == NULL || message_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t params_bytes[256];
    codex_bridge_tlv_builder_t params = {0};
    codex_bridge_tlv_builder_init(&params, params_bytes, sizeof(params_bytes));
    uint32_t message_ref = 0;
    esp_err_t error = ESP_OK;
    if (parse_prefixed_ref(message_id, "msg-", &message_ref)) {
        error = codex_bridge_tlv_builder_u32(&params, CBDP_TLV_MESSAGE_REF, message_ref);
    } else {
        error = codex_bridge_tlv_builder_utf8(&params, CBDP_TLV_MESSAGE_ID, message_id);
    }
    const uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&params, CBDP_TLV_READ_AT_MS, now_ms);
    if (error != ESP_OK) return error;

    const uint32_t request_id = next_request_id();
    uint8_t idem[16];
    esp_fill_random(idem, sizeof(idem));
    uint8_t header_bytes[384];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, request_id);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_IDEM,
                                                                idem, sizeof(idem));
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_METHOD_ID,
                                                              CBDP_METHOD_MESSAGE_READ_UPDATE);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_PARAMS,
                                                                params.data, params.length);
    if (error != ESP_OK) return error;
    return send_business(CBDP_MSG_COMMAND,
                         CBDP_FLAG_REQUEST | CBDP_FLAG_COMMAND,
                         0, CBDP_QOS_STATE, &header);
}