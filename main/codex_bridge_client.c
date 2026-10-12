#include "codex_bridge_client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_config.h"
#include "device_identity.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define CODEX_BRIDGE_DISCOVERY_PORT 8764
#define CODEX_BRIDGE_TCP_PORT 8765
#define CODEX_BRIDGE_DISCOVER_REQUEST "CODEXBRIDGE_DISCOVER"
#define CODEX_BRIDGE_SESSION_TIMEOUT_MS 15000
#define CODEX_BRIDGE_UDP_TASK_STACK 4096
#define CODEX_BRIDGE_TCP_TASK_STACK 6144
#define CODEX_BRIDGE_READ_ACK_MAX 96

static const char *TAG = "codex_bridge";
static const char *DEVICE_MODEL = "dotii";
static const char *DEVICE_NAME = "Dotii";
static const char *const CAPABILITIES[] = {
    "status", "text", "tasks", "results", "read_ack",
};

static TaskHandle_t s_udp_task;
static TaskHandle_t s_tcp_task;
static SemaphoreHandle_t s_socket_lock;
static volatile bool s_running;
static volatile bool s_online;
static int s_client_socket = -1;
static int s_server_socket = -1;
static codex_bridge_message_handler_t s_handler;
static void *s_handler_context;

static void close_socket(int *socket_fd)
{
    if (*socket_fd < 0) return;
    shutdown(*socket_fd, 0);
    close(*socket_fd);
    *socket_fd = -1;
}

static bool send_all(int socket_fd, const char *payload, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        int result = send(socket_fd, payload + sent, length - sent, 0);
        if (result <= 0) return false;
        sent += (size_t)result;
    }
    return true;
}

static bool send_json(int socket_fd, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) return false;
    bool sent = send_all(socket_fd, payload, strlen(payload)) &&
                send_all(socket_fd, "\n", 1);
    cJSON_free(payload);
    return sent;
}

static bool send_locked_json(cJSON *root)
{
    if (s_socket_lock == NULL || xSemaphoreTake(s_socket_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_Delete(root);
        return false;
    }
    bool sent = s_client_socket >= 0 && send_json(s_client_socket, root);
    cJSON_Delete(root);
    xSemaphoreGive(s_socket_lock);
    return sent;
}

static cJSON *capabilities_array(void)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) return NULL;
    for (size_t index = 0;
         index < sizeof(CAPABILITIES) / sizeof(CAPABILITIES[0]);
         ++index) {
        cJSON_AddItemToArray(array, cJSON_CreateString(CAPABILITIES[index]));
    }
    return array;
}

static void udp_task(void *context)
{
    (void)context;
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
        .sin_port = htons(CODEX_BRIDGE_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "UDP bind failed: %d", errno);
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Discovery listening on UDP %d", CODEX_BRIDGE_DISCOVERY_PORT);
    while (s_running) {
        char request[64] = {0};
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        int length = recvfrom(
            socket_fd,
            request,
            sizeof(request) - 1,
            0,
            (struct sockaddr *)&source,
            &source_length
        );
        if (length <= 0) {
            if (!s_running) break;
            continue;
        }
        if ((size_t)length != strlen(CODEX_BRIDGE_DISCOVER_REQUEST) ||
            strcmp(request, CODEX_BRIDGE_DISCOVER_REQUEST) != 0) {
            continue;
        }
        cJSON *reply = cJSON_CreateObject();
        if (reply == NULL) continue;
        cJSON_AddStringToObject(reply, "device", DEVICE_MODEL);
        cJSON_AddStringToObject(reply, "device_id", device_identity_get());
        cJSON_AddStringToObject(reply, "name", DEVICE_NAME);
        cJSON_AddNumberToObject(reply, "port", CODEX_BRIDGE_TCP_PORT);
        cJSON *capabilities = capabilities_array();
        if (capabilities != NULL) {
            cJSON_AddItemToObject(reply, "capabilities", capabilities);
        }
        char *payload = cJSON_PrintUnformatted(reply);
        if (payload != NULL) {
            sendto(
                socket_fd,
                payload,
                strlen(payload),
                0,
                (struct sockaddr *)&source,
                source_length
            );
            cJSON_free(payload);
        }
        cJSON_Delete(reply);
    }
    close(socket_fd);
    s_udp_task = NULL;
    vTaskDelete(NULL);
}

static bool authenticate_client(const char *line, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(line, length);
    if (root == NULL) return false;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
    bool valid = cJSON_IsString(type) && strcmp(type->valuestring, "bridge_auth") == 0 &&
                 cJSON_IsString(token) && token->valuestring != NULL &&
                 strcmp(token->valuestring, device_config_get()->codex_bridge_token) == 0;
    cJSON_Delete(root);
    if (!valid) return false;

    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) return false;
    cJSON_AddStringToObject(reply, "type", "bridge_auth_ok");
    cJSON_AddStringToObject(reply, "device_id", device_identity_get());
    cJSON_AddStringToObject(reply, "model", DEVICE_MODEL);
    cJSON_AddStringToObject(reply, "name", DEVICE_NAME);
    cJSON_AddStringToObject(reply, "firmware", esp_app_get_description()->version);
    cJSON *capabilities = capabilities_array();
    if (capabilities != NULL) {
        cJSON_AddItemToObject(reply, "capabilities", capabilities);
    }
    return send_locked_json(reply);
}

static size_t read_line(int socket_fd, char *line, size_t capacity, TickType_t deadline)
{
    size_t length = 0;
    while (s_running && xTaskGetTickCount() < deadline) {
        TickType_t remaining = deadline - xTaskGetTickCount();
        struct timeval timeout = {
            .tv_sec = (time_t)(remaining / configTICK_RATE_HZ),
            .tv_usec = (suseconds_t)((remaining % configTICK_RATE_HZ) * 1000000 / configTICK_RATE_HZ),
        };
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char byte = 0;
        int received = recv(socket_fd, &byte, 1, 0);
        if (received <= 0) return 0;
        if (byte == '\n') {
            line[length] = '\0';
            return length;
        }
        if (length + 1 >= capacity) return 0;
        line[length++] = byte;
    }
    return 0;
}

static void tcp_session(int client_socket)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CODEX_BRIDGE_SESSION_TIMEOUT_MS);
    char line[CODEX_BRIDGE_LINE_MAX + 1] = {0};
    size_t length = read_line(client_socket, line, sizeof(line), deadline);
    if (length == 0 || !authenticate_client(line, length)) return;

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CODEX_BRIDGE_SESSION_TIMEOUT_MS);
    length = read_line(client_socket, line, sizeof(line), deadline);
    if (length == 0) return;
    codex_bridge_message_t message = {0};
    if (codex_bridge_protocol_feed(line, length, &message) != ESP_OK ||
        strcmp(message.type, "bridge_ready") != 0 || message.ignored) {
        codex_bridge_protocol_reset_message(&message);
        return;
    }
    s_online = true;
    ESP_LOGI(TAG, "Online with CodexBridge");
    if (s_handler != NULL) s_handler(&message, s_handler_context);
    codex_bridge_protocol_reset_message(&message);

    while (s_running) {
        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CODEX_BRIDGE_SESSION_TIMEOUT_MS);
        length = read_line(client_socket, line, sizeof(line), deadline);
        if (length == 0) break;
        if (codex_bridge_protocol_feed(line, length, &message) != ESP_OK) {
            codex_bridge_protocol_reset_message(&message);
            break;
        }
        if (s_handler != NULL && !message.ignored) {
            s_handler(&message, s_handler_context);
        }
    }
    codex_bridge_protocol_reset_message(&message);
}

static void tcp_task(void *context)
{
    (void)context;
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
        .sin_port = htons(CODEX_BRIDGE_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server_socket, 1) != 0) {
        ESP_LOGE(TAG, "TCP listen failed: %d", errno);
        close(server_socket);
        vTaskDelete(NULL);
        return;
    }
    if (s_socket_lock != NULL) xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    s_server_socket = server_socket;
    if (s_socket_lock != NULL) xSemaphoreGive(s_socket_lock);
    ESP_LOGI(TAG, "Device service listening on TCP %d", CODEX_BRIDGE_TCP_PORT);

    while (s_running) {
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        int client_socket = accept(
            server_socket,
            (struct sockaddr *)&source,
            &source_length
        );
        if (client_socket < 0) {
            if (!s_running) break;
            continue;
        }
        if (s_socket_lock != NULL) xSemaphoreTake(s_socket_lock, portMAX_DELAY);
        close_socket(&s_client_socket);
        s_client_socket = client_socket;
        if (s_socket_lock != NULL) xSemaphoreGive(s_socket_lock);
        tcp_session(client_socket);
        s_online = false;
        if (s_socket_lock != NULL) xSemaphoreTake(s_socket_lock, portMAX_DELAY);
        if (s_client_socket == client_socket) s_client_socket = -1;
        close_socket(&client_socket);
        if (s_socket_lock != NULL) xSemaphoreGive(s_socket_lock);
        ESP_LOGI(TAG, "CodexBridge session closed");
    }
    if (s_socket_lock != NULL) xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    close_socket(&s_server_socket);
    if (s_socket_lock != NULL) xSemaphoreGive(s_socket_lock);
    s_tcp_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t codex_bridge_client_start(
    codex_bridge_message_handler_t handler,
    void *context
)
{
    if (s_running) return ESP_OK;
    if (strlen(device_config_get()->codex_bridge_token) < 16) {
        ESP_LOGW(TAG, "CodexBridge token is not provisioned");
        return ESP_ERR_INVALID_STATE;
    }
    s_handler = handler;
    s_handler_context = context;
    s_socket_lock = xSemaphoreCreateMutex();
    if (s_socket_lock == NULL) return ESP_ERR_NO_MEM;
    s_running = true;
    if (xTaskCreate(udp_task, "codexbridge_udp", CODEX_BRIDGE_UDP_TASK_STACK, NULL, 4, &s_udp_task) != pdPASS ||
        xTaskCreate(tcp_task, "codexbridge_tcp", CODEX_BRIDGE_TCP_TASK_STACK, NULL, 4, &s_tcp_task) != pdPASS) {
        codex_bridge_client_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void codex_bridge_client_stop(void)
{
    s_running = false;
    s_online = false;
    if (s_socket_lock != NULL) xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    close_socket(&s_client_socket);
    close_socket(&s_server_socket);
    if (s_socket_lock != NULL) xSemaphoreGive(s_socket_lock);
    if (s_tcp_task != NULL) {
        vTaskDelete(s_tcp_task);
        s_tcp_task = NULL;
    }
    if (s_udp_task != NULL) {
        vTaskDelete(s_udp_task);
        s_udp_task = NULL;
    }
    if (s_socket_lock != NULL) {
        vSemaphoreDelete(s_socket_lock);
        s_socket_lock = NULL;
    }
    s_handler = NULL;
    s_handler_context = NULL;
}

bool codex_bridge_client_is_online(void)
{
    return s_online;
}

esp_err_t codex_bridge_client_send_read_ack(const char *message_id)
{
    if (!s_online || message_id == NULL || message_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(message_id) >= CODEX_BRIDGE_READ_ACK_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "type", "read_ack");
    cJSON_AddStringToObject(root, "message_id", message_id);
    return send_locked_json(root) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
