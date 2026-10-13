#include "device_identity.h"

#include <stdio.h>

#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"

#define XIAOZHI_NAMESPACE "xiaozhi"

static char s_device_id[20];
static char s_mac[18];
static char s_client_id[37];

static void make_client_id(char *target, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[16] = {0};
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0F) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3F) | 0x80);
    size_t offset = 0;
    for (size_t i = 0; i < sizeof(bytes) && offset + 1 < capacity; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            if (offset + 1 < capacity) target[offset++] = '-';
        }
        if (offset + 1 < capacity) target[offset++] = hex[bytes[i] >> 4];
        if (offset + 1 < capacity) target[offset++] = hex[bytes[i] & 0x0F];
    }
    target[offset] = '\0';
}

esp_err_t device_identity_init(void)
{
    uint8_t mac[6] = {0};
    esp_err_t error = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (error != ESP_OK) return error;
    snprintf(s_mac, sizeof(s_mac),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_device_id, sizeof(s_device_id),
             "dotii-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t handle;
    error = nvs_open(XIAOZHI_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        make_client_id(s_client_id, sizeof(s_client_id));
        return ESP_OK;
    }
    size_t length = sizeof(s_client_id);
    error = nvs_get_str(handle, "client_id", s_client_id, &length);
    if (error != ESP_OK || length < 36) {
        make_client_id(s_client_id, sizeof(s_client_id));
        (void)nvs_set_str(handle, "client_id", s_client_id);
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
    return ESP_OK;
}

const char *device_identity_get(void)
{
    return s_device_id;
}

const char *device_identity_get_mac(void)
{
    return s_mac;
}

const char *device_identity_get_client_id(void)
{
    return s_client_id;
}
