#include "device_config.h"

#include <string.h>

static device_config_values_t s_config = {
    .codex_bridge_token = "0123456789abcdef0123456789abcdef",
};

esp_err_t device_config_init(void)
{
    return ESP_OK;
}

const device_config_values_t *device_config_get(void)
{
    return &s_config;
}

esp_err_t device_config_save(const device_config_values_t *values)
{
    if (values == NULL) return ESP_ERR_INVALID_ARG;
    s_config = *values;
    return ESP_OK;
}
