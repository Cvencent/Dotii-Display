#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
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
static char s_fixture_error[160];
static cJSON *s_fixture_received[2];
static char s_client_message_body[128];
static int s_client_message_seq = -1;

static void capture_client_message(
    const codex_bridge_message_t *message,
    void *context
)
{
    (void)context;
    if (strcmp(message->type, "message") != 0) return;
    snprintf(
        s_client_message_body,
        sizeof(s_client_message_body),
        "%s",
        message->body == NULL ? "" : message->body
    );
    s_client_message_seq = message->seq;
}

static void provision_test_token(const char *token)
{
    device_config_values_t values = *device_config_get();
    snprintf(values.codex_bridge_token, sizeof(values.codex_bridge_token), "%s", token);
    TEST_ASSERT_EQUAL_INT(ESP_OK, device_config_save(&values));
    device_config_init();
}

static void fixture_fail(const char *message)
{
    snprintf(s_fixture_error, sizeof(s_fixture_error), "%s", message);
    xEventGroupSetBits(s_fixture_events, FIXTURE_DONE_BIT | FIXTURE_FAILED_BIT);
}

static bool fixture_read_line(int socket, char *line, size_t capacity)
{
    size_t length = 0;
    while (length + 1 < capacity) {
        char byte = 0;
        int received = recv(socket, &byte, 1, 0);
        if (received <= 0) return false;
        if (byte == '\n') {
            line[length] = '\0';
            return true;
        }
        line[length++] = byte;
    }
    return false;
}

static bool fixture_send_line(int socket, const char *json)
{
    size_t length = strlen(json);
    if (send(socket, json, length, 0) != (int)length) return false;
    return send(socket, "\n", 1, 0) == 1;
}

static bool fixture_discover(void)
{
    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(8764),
    };
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const char request[] = "CODEXBRIDGE_DISCOVER";
    struct timeval timeout = {.tv_sec = 2};
    char reply[1024] = {0};
    int length = 0;
    for (int attempt = 0; attempt < 50 && length <= 0; ++attempt) {
        int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) return false;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (sendto(socket_fd, request, sizeof(request) - 1, 0,
                   (struct sockaddr *)&target, sizeof(target)) >= 0) {
            length = recv(socket_fd, reply, sizeof(reply) - 1, 0);
        }
        close(socket_fd);
        if (length <= 0) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (length <= 0) return false;
    cJSON *root = cJSON_ParseWithLength(reply, (size_t)length);
    if (root == NULL) return false;
    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
    cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    bool valid = cJSON_IsString(device_id) &&
                 strcmp(device_id->valuestring, device_identity_get()) == 0 &&
                 cJSON_IsNumber(port) && port->valueint == 8765 &&
                 cJSON_IsArray(capabilities) &&
                 cJSON_GetArraySize(capabilities) == 5;
    cJSON_Delete(root);
    return valid;
}

static void fixture_task(void *context)
{
    (void)context;
    if (!fixture_discover()) {
        fixture_fail("discovery response invalid");
        vTaskDelete(NULL);
        return;
    }

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        fixture_fail("fixture socket failed");
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
    };
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 1) != 0) {
        close(server);
        fixture_fail("fixture listen failed");
        vTaskDelete(NULL);
        return;
    }
    socklen_t address_length = sizeof(address);
    getsockname(server, (struct sockaddr *)&address, &address_length);

    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(8765),
    };
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int client = -1;
    for (int attempt = 0; attempt < 50 && client < 0; ++attempt) {
        client = socket(AF_INET, SOCK_STREAM, 0);
        if (client < 0) break;
        if (connect(client, (struct sockaddr *)&target, sizeof(target)) != 0) {
            close(client);
            client = -1;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (client < 0) {
        close(server);
        fixture_fail("client port unavailable");
        vTaskDelete(NULL);
        return;
    }

    char line[CODEX_BRIDGE_LINE_MAX + 1] = {0};
    if (!fixture_send_line(
            client,
            "{\"type\":\"bridge_auth\",\"token\":\"" FIXTURE_TOKEN "\"}")) {
        close(client);
        close(server);
        fixture_fail("bridge_auth send failed");
        vTaskDelete(NULL);
        return;
    }
    if (!fixture_read_line(client, line, sizeof(line))) {
        close(client);
        close(server);
        fixture_fail("bridge_auth_ok missing");
        vTaskDelete(NULL);
        return;
    }
    s_fixture_received[0] = cJSON_ParseWithLength(line, strlen(line));
    if (s_fixture_received[0] == NULL ||
        !fixture_send_line(
            client,
            "{\"type\":\"bridge_ready\",\"bridge\":\"CodexBridge\","
            "\"version\":\"test\",\"capabilities\":["
            "\"status\",\"text\",\"tasks\",\"results\",\"read_ack\"]}")) {
        close(client);
        close(server);
        fixture_fail("bridge_ready send failed");
        vTaskDelete(NULL);
        return;
    }
    if (!fixture_send_line(client,
            "{\"type\":\"message\",\"message_id\":\"m1\",\"thread_id\":\"t1\","
            "\"kind\":\"status\",\"status\":\"working\",\"title\":\"Task\","
            "\"body\":\"first\",\"seq\":0,\"total\":2}") ||
        !fixture_send_line(client,
            "{\"type\":\"message\",\"message_id\":\"m1\",\"thread_id\":\"t1\","
            "\"kind\":\"status\",\"status\":\"working\",\"title\":\"Task\","
            "\"body\":\"second\",\"seq\":1,\"total\":2}")) {
        close(client);
        close(server);
        fixture_fail("message send failed");
        vTaskDelete(NULL);
        return;
    }
    TickType_t wait_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    while ((!codex_bridge_client_is_online() || s_client_message_seq != 1) &&
           xTaskGetTickCount() < wait_deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!codex_bridge_client_is_online()) {
        close(client);
        close(server);
        fixture_fail("client did not become online");
        vTaskDelete(NULL);
        return;
    }
    if (s_client_message_seq != 1) {
        close(client);
        close(server);
        fixture_fail("message chunks were not delivered");
        vTaskDelete(NULL);
        return;
    }
    xEventGroupSetBits(s_fixture_events, FIXTURE_DONE_BIT);
    vTaskDelay(pdMS_TO_TICKS(2000));
    close(client);
    close(server);
    vTaskDelete(NULL);
}

TEST_CASE("bridge_ready exposes only granted capabilities", "[codex_bridge]")
{
    codex_bridge_message_t message = {0};
    const char *line =
        "{\"type\":\"bridge_ready\",\"bridge\":\"CodexBridge\","
        "\"version\":\"0.1.0\",\"capabilities\":[\"status\",\"tasks\",\"read_ack\"]}\n";
    TEST_ESP_OK(codex_bridge_protocol_feed(line, strlen(line), &message));
    TEST_ASSERT_EQUAL_STRING("bridge_ready", message.type);
    TEST_ASSERT_TRUE(codex_bridge_protocol_has_capability(&message, "tasks"));
    TEST_ASSERT_FALSE(codex_bridge_protocol_has_capability(&message, "voice"));
    codex_bridge_protocol_reset_message(&message);
}

TEST_CASE("unknown message type is ignored without closing", "[codex_bridge]")
{
    codex_bridge_message_t message = {0};
    const char *line = "{\"type\":\"future_optional_event\",\"value\":1}\n";
    TEST_ESP_OK(codex_bridge_protocol_feed(line, strlen(line), &message));
    TEST_ASSERT_TRUE(message.ignored);
    codex_bridge_protocol_reset_message(&message);
}

TEST_CASE("message chunks reassemble in sequence order", "[codex_bridge]")
{
    codex_bridge_message_t message = {0};
    const char *second =
        "{\"type\":\"message\",\"message_id\":\"m1\",\"thread_id\":\"t1\","
        "\"kind\":\"result\",\"status\":\"completed\",\"title\":\"done\","
        "\"body\":\"second\",\"seq\":1,\"total\":2}\n";
    const char *first =
        "{\"type\":\"message\",\"message_id\":\"m1\",\"thread_id\":\"t1\","
        "\"kind\":\"result\",\"status\":\"completed\",\"title\":\"done\","
        "\"body\":\"first\",\"seq\":0,\"total\":2}\n";
    TEST_ESP_OK(codex_bridge_protocol_feed(second, strlen(second), &message));
    TEST_ASSERT_EQUAL_STRING("second", message.body);
    TEST_ESP_OK(codex_bridge_protocol_feed(first, strlen(first), &message));
    TEST_ASSERT_EQUAL_STRING("firstsecond", codex_bridge_protocol_body(&message, "m1"));
    TEST_ASSERT_EQUAL_INT(11, codex_bridge_protocol_body_length(&message));
    codex_bridge_protocol_reset_message(&message);
}

TEST_CASE("oversized line is rejected without allocation", "[codex_bridge]")
{
    codex_bridge_message_t message = {0};
    char line[900];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_SIZE,
        codex_bridge_protocol_feed(line, strlen(line), &message)
    );
    TEST_ASSERT_NULL(message.body);
}

TEST_CASE("body is truncated on a UTF-8 boundary with a suffix", "[codex_bridge]")
{
    codex_bridge_message_t message = {0};
    char body[481];
    const size_t chunk_size = CODEX_BRIDGE_BODY_MAX / 10 + 1;
    memset(body, 'a', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
    for (int chunk_index = 0; chunk_index < 10; ++chunk_index) {
        char line[CODEX_BRIDGE_LINE_MAX + 1] = {0};
        snprintf(
            line,
            sizeof(line),
            "{\"type\":\"message\",\"message_id\":\"long\","
            "\"body\":\"%.*s\",\"seq\":%d,\"total\":10}",
            (int)chunk_size,
            body,
            chunk_index
        );
        TEST_ESP_OK(codex_bridge_protocol_feed(line, strlen(line), &message));
    }
    TEST_ASSERT_EQUAL_UINT(CODEX_BRIDGE_BODY_MAX - 1, strlen(message.body));
    TEST_ASSERT_TRUE(strstr(message.body, "\n...[已截断]") != NULL);
    TEST_ASSERT_EQUAL_MEMORY(
        "a",
        message.body + strlen(message.body) - strlen("\n...[已截断]") - 1,
        1
    );
    codex_bridge_protocol_reset_message(&message);
}

TEST_CASE("client handshakes and reassembles message chunks", "[codex_bridge]")
{
    provision_test_token(FIXTURE_TOKEN);
    s_fixture_events = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(s_fixture_events);
    memset(s_fixture_received, 0, sizeof(s_fixture_received));
    s_client_message_body[0] = '\0';
    s_client_message_seq = -1;
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        codex_bridge_client_start(capture_client_message, NULL)
    );
    xTaskCreate(fixture_task, "fixture", 4096, NULL, 5, NULL);
    EventBits_t bits = xEventGroupWaitBits(
        s_fixture_events,
        FIXTURE_DONE_BIT | FIXTURE_FAILED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(5000)
    );
    TEST_ASSERT_EQUAL_STRING("", s_fixture_error);
    TEST_ASSERT_TRUE((bits & FIXTURE_DONE_BIT) != 0);
    TEST_ASSERT_TRUE((bits & FIXTURE_FAILED_BIT) == 0);
    TEST_ASSERT_NOT_NULL(s_fixture_received[0]);
    TEST_ASSERT_EQUAL_STRING(
        "bridge_auth_ok",
        cJSON_GetObjectItemCaseSensitive(s_fixture_received[0], "type")->valuestring
    );
    TEST_ASSERT_EQUAL_STRING(
        "dotii",
        cJSON_GetObjectItemCaseSensitive(s_fixture_received[0], "model")->valuestring
    );
    TEST_ASSERT_NULL(
        cJSON_GetObjectItemCaseSensitive(s_fixture_received[0], "protocol")
    );
    TEST_ASSERT_TRUE(codex_bridge_client_is_online());
    TEST_ASSERT_EQUAL_STRING("firstsecond", s_client_message_body);
    TEST_ASSERT_EQUAL_INT(1, s_client_message_seq);
    codex_bridge_client_stop();
    cJSON_Delete(s_fixture_received[0]);
    s_fixture_received[0] = NULL;
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
    unity_run_tests_by_tag("[codex_bridge]", false);
    UNITY_END();
}
