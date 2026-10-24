#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "codex_bridge_client.h"
#include "codex_bridge_protocol.h"
#include "device_config.h"
#include "device_identity.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "unity.h"

#define FIXTURE_DONE_BIT BIT0
#define FIXTURE_FAILED_BIT BIT1
#define FIXTURE_TOKEN "0123456789abcdef0123456789abcdef"

static EventGroupHandle_t s_fixture_events;
static char s_fixture_error[192];
static volatile bool s_fixture_online_seen;

static void capture_bridge_message(const codex_bridge_message_t *message, void *context)
{
    (void)context;
    if (message != NULL && strcmp(message->type, "bridge_online") == 0) {
        s_fixture_online_seen = true;
    }
}

static void fixture_fail(const char *message)
{
    snprintf(s_fixture_error, sizeof(s_fixture_error), "%s", message);
    xEventGroupSetBits(s_fixture_events, FIXTURE_DONE_BIT | FIXTURE_FAILED_BIT);
}

static bool fixture_recv_exact(int socket_fd, uint8_t *data, size_t length)
{
    size_t received = 0;
    while (received < length) {
        int result = recv(socket_fd, data + received, length - received, 0);
        if (result <= 0) return false;
        received += (size_t)result;
    }
    return true;
}

static bool fixture_read_frame(int socket_fd, uint8_t *buffer, size_t capacity,
                               codex_bridge_frame_t *frame)
{
    if (!fixture_recv_exact(socket_fd, buffer, CBDP_FIXED_HEADER_SIZE)) return false;
    const uint16_t header_len = (uint16_t)(((uint16_t)buffer[6] << 8) | buffer[7]);
    const uint32_t payload_len = ((uint32_t)buffer[8] << 24) |
                                 ((uint32_t)buffer[9] << 16) |
                                 ((uint32_t)buffer[10] << 8) |
                                 buffer[11];
    const size_t total = (size_t)header_len + payload_len;
    if (total > capacity || total < CBDP_FIXED_HEADER_SIZE) return false;
    if (!fixture_recv_exact(socket_fd, buffer + CBDP_FIXED_HEADER_SIZE,
                            total - CBDP_FIXED_HEADER_SIZE)) {
        return false;
    }
    return codex_bridge_frame_parse(buffer, total, frame) == ESP_OK;
}

static bool fixture_send_frame(int socket_fd, const uint8_t *buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        int result = send(socket_fd, buffer + sent, length - sent, 0);
        if (result <= 0) return false;
        sent += (size_t)result;
    }
    return true;
}

static bool fixture_build_welcome(uint8_t *frame, size_t capacity, size_t *length,
                                  const uint8_t session_id[16],
                                  const uint8_t server_nonce[16])
{
    uint8_t header_bytes[256];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_VERSION_MINOR, 0);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_SESSION_ID,
                                                                session_id, 16);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_SERVER_NONCE,
                                                                server_nonce, 16);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_AUTH_MODE, 1);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_MAX_FRAME_BYTES, 1024);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_MAX_TEXT_CHARS, 512);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_MAX_TEXT_BYTES, 1536);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_QUEUE_DEPTH, 32);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_HEARTBEAT_SECONDS, 15);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_MAX_CHUNK_BYTES, 384);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_WRITER_LEASE_SECONDS, 300);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, 1);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_SERVER_TIME_MS,
                                                              (uint64_t)time(NULL) * 1000ULL);
    if (error != ESP_OK) return false;
    return codex_bridge_frame_build(
        frame, capacity, CBDP_MSG_WELCOME, CBDP_FLAG_RESPONSE, 0, 0, 0, 0,
        CBDP_QOS_CRITICAL, &header, NULL, 0, length) == ESP_OK;
}

static bool fixture_build_auth_ok(uint8_t *frame, size_t capacity, size_t *length,
                                  uint32_t request_id)
{
    uint8_t header_bytes[256];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_DEVICE_ID,
                                                    device_identity_get());
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(
        &header, CBDP_TLV_EFFECTIVE_CAPS,
        CBDP_CAP_STATUS | CBDP_CAP_TEXT | CBDP_CAP_TASKS | CBDP_CAP_RESULTS |
        CBDP_CAP_READ_ACK | CBDP_CAP_TELEMETRY | CBDP_CAP_SUBSCRIBE_LIVE |
        CBDP_CAP_SUBSCRIBE_THREAD | CBDP_CAP_SUBSCRIBE_SUMMARY |
        CBDP_CAP_MULTI_PAGE | CBDP_CAP_DEVICE_CONFIG | CBDP_CAP_ACCOUNT |
        CBDP_CAP_CONTEXT_USAGE | CBDP_CAP_READ_UNREAD);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_POLICY_VERSION, 1);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_DEVICE_REF, 1);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID,
                                                              request_id);
    if (error != ESP_OK) return false;
    return codex_bridge_frame_build(
        frame, capacity, CBDP_MSG_AUTH_OK, CBDP_FLAG_RESPONSE, 0, 0, 0, 0,
        CBDP_QOS_CRITICAL, &header, NULL, 0, length) == ESP_OK;
}

static bool fixture_build_thread_snapshot(
    uint8_t *frame, size_t capacity, size_t *length
)
{
    uint8_t fields[256];
    codex_bridge_tlv_builder_t object = {0};
    codex_bridge_tlv_builder_init(&object, fields, sizeof(fields));
    esp_err_t error = codex_bridge_tlv_builder_u8(&object, CBDP_TLV_OBJECT_TYPE, 0);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&object, CBDP_TLV_THREAD_REF, 1);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&object, CBDP_TLV_THREAD_ID, "thread-1");
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u32(&object, CBDP_TLV_TOPIC_ID, 0x10000001u);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&object, CBDP_TLV_TITLE, "Thread one");
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&object, CBDP_TLV_THREAD_STATE, 1);
    if (error != ESP_OK || object.length > 0xFFFFu) return false;

    uint8_t wrapped[512];
    const size_t wrapped_length = object.length + 4;
    if (wrapped_length > sizeof(wrapped)) return false;
    wrapped[0] = (uint8_t)(CBDP_TLV_OBJECT >> 8);
    wrapped[1] = (uint8_t)CBDP_TLV_OBJECT;
    wrapped[2] = (uint8_t)(object.length >> 8);
    wrapped[3] = (uint8_t)object.length;
    memcpy(wrapped + 4, object.data, object.length);

    uint8_t objects[520];
    if (wrapped_length + 2 > sizeof(objects)) return false;
    objects[0] = 0;
    objects[1] = 1;
    memcpy(objects + 2, wrapped, wrapped_length);

    uint8_t header_bytes[320];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    error = codex_bridge_tlv_builder_u32(&header, CBDP_TLV_SNAPSHOT_ID, 1);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_SNAPSHOT_KIND, 2);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_SNAPSHOT_REVISION, 1);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_PAGE_INDEX, 0);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_PAGE_COUNT, 1);
    if (error == ESP_OK) {
        error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_OBJECTS, objects,
                                               wrapped_length + 2);
    }
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_COMPLETE, 1);
    if (error != ESP_OK) return false;
    return codex_bridge_frame_build(
        frame, capacity, CBDP_MSG_SNAPSHOT,
        CBDP_FLAG_EVENT | CBDP_FLAG_SNAPSHOT_PAGE,
        0x50000003u, 1, 0, 0, CBDP_QOS_STATE,
        &header, NULL, 0, length) == ESP_OK;
}

static bool fixture_build_message_read_delta(
    uint8_t *frame, size_t capacity, size_t *length
)
{
    uint8_t payload_bytes[64];
    codex_bridge_tlv_builder_t payload = {0};
    codex_bridge_tlv_builder_init(&payload, payload_bytes, sizeof(payload_bytes));
    esp_err_t error = codex_bridge_tlv_builder_u32(&payload, CBDP_TLV_MESSAGE_REF, 3);
    if (error != ESP_OK) return false;

    uint8_t header_bytes[128];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_GLOBAL_SEQ, 200);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_TOPIC_SEQ, 2);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_REVISION, 2);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_EVENT_TYPE, 0x0303);
    if (error == ESP_OK) {
        error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_EVENT_PAYLOAD,
                                               payload.data, payload.length);
    }
    if (error != ESP_OK) return false;
    return codex_bridge_frame_build(
        frame, capacity, CBDP_MSG_DELTA, CBDP_FLAG_EVENT,
        0x10000001u, 2, 0, 0, CBDP_QOS_STATE, &header, NULL, 0, length) == ESP_OK;
}

static void fixture_task(void *context)
{
    (void)context;
    int socket_fd = -1;
    for (int attempt = 0; attempt < 100 && socket_fd < 0; ++attempt) {
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) break;
        struct sockaddr_in target = {
            .sin_family = AF_INET,
            .sin_port = htons(8765),
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };
        if (connect(socket_fd, (struct sockaddr *)&target, sizeof(target)) != 0) {
            close(socket_fd);
            socket_fd = -1;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (socket_fd < 0) {
        fixture_fail("unable to connect to device listener");
        vTaskDelete(NULL);
        return;
    }

    uint8_t buffer[2048];
    codex_bridge_frame_t frame = {0};
    if (!fixture_read_frame(socket_fd, buffer, sizeof(buffer), &frame)) {
        fixture_fail("HELLO not received");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    if (frame.message_type != CBDP_MSG_HELLO ||
        (frame.flags & CBDP_FLAG_REQUEST) == 0) {
        fixture_fail("first frame is not a HELLO request");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    codex_bridge_tlv_t field = {0};
    if (codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_PROFILE, &field) != ESP_OK) {
        fixture_fail("HELLO missing profile");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    uint8_t profile = 0;
    codex_bridge_tlv_u8(&field, &profile);
    if (profile != CBDP_PROFILE_PANEL) {
        fixture_fail("HELLO profile is not panel");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    if (codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_CLIENT_NONCE, &field) != ESP_OK ||
        field.length != 16) {
        fixture_fail("HELLO missing client nonce");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    uint8_t client_nonce[16];
    memcpy(client_nonce, field.value, sizeof(client_nonce));

    const uint8_t session_id[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    };
    const uint8_t server_nonce[16] = {
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
    };
    uint8_t reply[1024];
    size_t reply_length = 0;
    if (!fixture_build_welcome(reply, sizeof(reply), &reply_length,
                               session_id, server_nonce) ||
        !fixture_send_frame(socket_fd, reply, reply_length)) {
        fixture_fail("unable to send WELCOME");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    if (!fixture_read_frame(socket_fd, buffer, sizeof(buffer), &frame) ||
        frame.message_type != CBDP_MSG_AUTH ||
        (frame.flags & CBDP_FLAG_REQUEST) == 0) {
        fixture_fail("AUTH not received");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    if (codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_SESSION_ID, &field) != ESP_OK ||
        field.length != 16 ||
        memcmp(field.value, session_id, 16) != 0) {
        fixture_fail("AUTH session id mismatch");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    if (codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_AUTH_PROOF, &field) != ESP_OK ||
        field.length != 32) {
        fixture_fail("AUTH proof missing");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    uint8_t expected_proof[32] = {0};
    if (codex_bridge_auth_proof(FIXTURE_TOKEN, session_id, client_nonce,
                                server_nonce, device_identity_get(),
                                expected_proof) != ESP_OK ||
        memcmp(expected_proof, field.value, sizeof(expected_proof)) != 0) {
        fixture_fail("AUTH proof mismatch");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    uint32_t auth_request_id = 0;
    if (codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_REQUEST_ID, &field) != ESP_OK) {
        fixture_fail("AUTH request id missing");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    codex_bridge_tlv_u32(&field, &auth_request_id);

    if (!fixture_build_auth_ok(reply, sizeof(reply), &reply_length, auth_request_id) ||
        !fixture_send_frame(socket_fd, reply, reply_length)) {
        fixture_fail("unable to send AUTH_OK");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    int subscribe_count = 0;
    for (int index = 0; index < 2; ++index) {
        if (!fixture_read_frame(socket_fd, buffer, sizeof(buffer), &frame)) {
            fixture_fail("post-auth subscription missing");
            close(socket_fd);
            vTaskDelete(NULL);
            return;
        }
        if (frame.message_type == CBDP_MSG_SUBSCRIBE) subscribe_count++;
    }
    if (subscribe_count != 2) {
        fixture_fail("thread-summary and live subscriptions missing");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    if (!fixture_build_thread_snapshot(reply, sizeof(reply), &reply_length) ||
        !fixture_send_frame(socket_fd, reply, reply_length) ||
        !fixture_read_frame(socket_fd, buffer, sizeof(buffer), &frame) ||
        frame.message_type != CBDP_MSG_ACK) {
        fixture_fail("thread snapshot ACK missing");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    if (codex_bridge_client_subscribe_thread("thread-1") != ESP_OK ||
        !fixture_read_frame(socket_fd, buffer, sizeof(buffer), &frame) ||
        frame.message_type != CBDP_MSG_SUBSCRIBE ||
        frame.topic_id != 0x10000001u) {
        fixture_fail("thread-scope subscription missing");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    if (codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_SCOPE, &field) != ESP_OK ||
        field.length != 1 || field.value[0] != CBDP_SCOPE_THREAD) {
        fixture_fail("thread subscription scope mismatch");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    if (!fixture_read_frame(socket_fd, buffer, sizeof(buffer), &frame) ||
        frame.message_type != CBDP_MSG_SUBSCRIBE ||
        frame.topic_id != 0x10000001u ||
        codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_SCOPE, &field) != ESP_OK ||
        field.length != 1 || field.value[0] != CBDP_SCOPE_SUMMARY) {
        fixture_fail("message.read summary-scope subscription missing");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    if (!fixture_build_message_read_delta(reply, sizeof(reply), &reply_length) ||
        !fixture_send_frame(socket_fd, reply, reply_length) ||
        !fixture_read_frame(socket_fd, buffer, sizeof(buffer), &frame) ||
        frame.message_type != CBDP_MSG_ACK) {
        fixture_fail("message.read ACK missing");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    codex_bridge_tlv_t ack_topic = {0};
    codex_bridge_tlv_t ack_scope = {0};
    codex_bridge_tlv_t ack_cursor = {0};
    uint32_t topic_id = 0;
    uint64_t cursor = 0;
    if (codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_TOPIC_ID, &ack_topic) != ESP_OK ||
        codex_bridge_tlv_u32(&ack_topic, &topic_id) != ESP_OK ||
        topic_id != 0x10000001u ||
        codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_SCOPE, &ack_scope) != ESP_OK ||
        ack_scope.length != 1 || ack_scope.value[0] != CBDP_SCOPE_SUMMARY ||
        codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_CURSOR, &ack_cursor) != ESP_OK ||
        codex_bridge_tlv_u64(&ack_cursor, &cursor) != ESP_OK || cursor != 200) {
        fixture_fail("message.read ACK must target summary scope");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t ping_header_bytes[16];
    codex_bridge_tlv_builder_t ping_header = {0};
    codex_bridge_tlv_builder_init(&ping_header, ping_header_bytes, sizeof(ping_header_bytes));
    codex_bridge_tlv_builder_u32(&ping_header, CBDP_TLV_PING_ID, 7);
    size_t ping_length = 0;
    codex_bridge_frame_build(reply, sizeof(reply), CBDP_MSG_PING, 0, 0, 0, 0, 0,
                             CBDP_QOS_TELEMETRY, &ping_header, NULL, 0, &ping_length);
    if (!fixture_send_frame(socket_fd, reply, ping_length) ||
        !fixture_read_frame(socket_fd, buffer, sizeof(buffer), &frame) ||
        frame.message_type != CBDP_MSG_PONG) {
        fixture_fail("PONG not received");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    if (codex_bridge_tlv_find(frame.header_tlv, frame.header_tlv_len,
                              CBDP_TLV_ECHO, &field) != ESP_OK) {
        fixture_fail("PONG echo missing");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    uint32_t echo = 0;
    codex_bridge_tlv_u32(&field, &echo);
    if (echo != 7) {
        fixture_fail("PONG echo mismatch");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    close(socket_fd);
    xEventGroupSetBits(s_fixture_events, FIXTURE_DONE_BIT);
    vTaskDelete(NULL);
}

TEST_CASE("CBDP device handshakes with binary HELLO AUTH and subscriptions", "[cbdp_session]")
{
    s_fixture_events = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(s_fixture_events);
    s_fixture_error[0] = '\0';
    s_fixture_online_seen = false;
    TEST_ESP_OK(codex_bridge_client_start(capture_bridge_message, NULL));
    xTaskCreate(fixture_task, "cbdp_fixture", 6144, NULL, 5, NULL);
    EventBits_t bits = xEventGroupWaitBits(
        s_fixture_events,
        FIXTURE_DONE_BIT | FIXTURE_FAILED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(15000)
    );
    TEST_ASSERT_EQUAL_STRING("", s_fixture_error);
    TEST_ASSERT_TRUE((bits & FIXTURE_DONE_BIT) != 0);
    TEST_ASSERT_TRUE((bits & FIXTURE_FAILED_BIT) == 0);
    TEST_ASSERT_TRUE(s_fixture_online_seen);
    TEST_ASSERT_TRUE(codex_bridge_client_capability_count() > 0);
    codex_bridge_client_stop();
    TEST_ASSERT_FALSE(codex_bridge_client_is_online());
    vEventGroupDelete(s_fixture_events);
    s_fixture_events = NULL;
}

void app_main(void)
{
    esp_err_t nvs_error = nvs_flash_init();
    if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(device_identity_init());
    ESP_ERROR_CHECK(device_config_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    UNITY_BEGIN();
    unity_run_tests_by_tag("[cbdp_codec]", false);
    unity_run_tests_by_tag("[cbdp_auth]", false);
    unity_run_tests_by_tag("[cbdp_pairing]", false);
    unity_run_tests_by_tag("[cbdp_discovery]", false);
    unity_run_tests_by_tag("[cbdp_projection]", false);
    unity_run_tests_by_tag("[cbdp_session]", false);
    unity_run_tests_by_tag("[codex_read]", false);
    unity_run_tests_by_tag("[codex_merge]", false);
    unity_run_tests_by_tag("[management_merge]", false);
    UNITY_END();
}
