#include "device_identity.h"

#include <stdio.h>

#include "esp_mac.h"

static char s_device_id[20];

esp_err_t device_identity_init(void)
{
    uint8_t mac[6] = {0};
    esp_err_t error = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (error != ESP_OK) return error;
    snprintf(s_device_id, sizeof(s_device_id),
             "dotii-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

const char *device_identity_get(void)
{
    return s_device_id;
}
