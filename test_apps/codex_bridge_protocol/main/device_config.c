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

/* The production management snapshot persists a short QQ Music login marker;
   the host stub keeps the same signature so the test binary links. */
esp_err_t device_config_store_qq_cookie(const char *cookie)
{
    if (cookie == NULL) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

/* Production persists the paired token in NVS. The host stub keeps it in
   memory so the pairing path is observable without a real flash write. */
esp_err_t device_config_store_codex_token(const char *token)
{
    if (token == NULL) return ESP_ERR_INVALID_ARG;
    const size_t length = strlen(token);
    if (length < 16 || length > 64) return ESP_ERR_INVALID_SIZE;
    memcpy(s_config.codex_bridge_token, token, length + 1);
    return ESP_OK;
}

/* Production rewrites the persisted bridge URL when the CBDP datagram
   reveals a new host address; the stub records it in memory. */
esp_err_t device_config_store_bridge_url(const char *url)
{
    if (url == NULL || url[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(url) >= sizeof(s_config.bridge_url)) return ESP_ERR_INVALID_SIZE;
    memcpy(s_config.bridge_url, url, strlen(url) + 1);
    return ESP_OK;
}

esp_err_t device_config_clear_codex_token(void)
{
    memset(s_config.codex_bridge_token, 0, sizeof(s_config.codex_bridge_token));
    return ESP_OK;
}
