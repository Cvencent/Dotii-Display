#include "device_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "device_config";
static const char *NAMESPACE = "dotii_cfg";
static device_config_values_t s_config;

static void load_string(nvs_handle_t handle, const char *key, char *target, size_t capacity)
{
    size_t length = capacity;
    esp_err_t error = nvs_get_str(handle, key, target, &length);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Unable to read %s: %s", key, esp_err_to_name(error));
    }
}

esp_err_t device_config_init(void)
{
    memset(&s_config, 0, sizeof(s_config));
    strlcpy(s_config.wifi_ssid, CONFIG_STATE_DISPLAY_WIFI_SSID, sizeof(s_config.wifi_ssid));
    strlcpy(s_config.wifi_password, CONFIG_STATE_DISPLAY_WIFI_PASSWORD, sizeof(s_config.wifi_password));
    strlcpy(s_config.bridge_url, CONFIG_STATE_DISPLAY_BRIDGE_URL, sizeof(s_config.bridge_url));
    strlcpy(s_config.bridge_token, CONFIG_STATE_DISPLAY_BRIDGE_TOKEN, sizeof(s_config.bridge_token));

    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Using build-time defaults until Bluetooth provisioning completes");
        return ESP_OK;
    }
    if (error != ESP_OK) return error;
    load_string(handle, "ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    load_string(handle, "wifi_pass", s_config.wifi_password, sizeof(s_config.wifi_password));
    load_string(handle, "bridge_url", s_config.bridge_url, sizeof(s_config.bridge_url));
    load_string(handle, "bridge_tok", s_config.bridge_token, sizeof(s_config.bridge_token));
    load_string(handle, "codex_tok", s_config.codex_bridge_token, sizeof(s_config.codex_bridge_token));
    load_string(handle, "qq_cookie", s_config.qqmusic_cookie, sizeof(s_config.qqmusic_cookie));
    uint8_t provisioned = 0;
    (void)nvs_get_u8(handle, "provisioned", &provisioned);
    s_config.provisioned = provisioned == 1;
    nvs_close(handle);
    ESP_LOGI(TAG, "Runtime configuration loaded (provisioned=%s)", s_config.provisioned ? "yes" : "no");
    return ESP_OK;
}

const device_config_values_t *device_config_get(void)
{
    return &s_config;
}

esp_err_t device_config_save(const device_config_values_t *values)
{
    if (values == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    if ((error = nvs_set_str(handle, "ssid", values->wifi_ssid)) == ESP_OK &&
        (error = nvs_set_str(handle, "wifi_pass", values->wifi_password)) == ESP_OK &&
        (error = nvs_set_str(handle, "bridge_url", values->bridge_url)) == ESP_OK &&
        (error = nvs_set_str(handle, "bridge_tok", values->bridge_token)) == ESP_OK &&
        (error = nvs_set_str(handle, "codex_tok", values->codex_bridge_token)) == ESP_OK &&
        (error = nvs_set_str(handle, "qq_cookie", values->qqmusic_cookie)) == ESP_OK &&
        (error = nvs_set_u8(handle, "provisioned", 1)) == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t device_config_store_qq_cookie(const char *cookie)
{
    if (cookie == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_str(handle, "qq_cookie", cookie);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) {
        strlcpy(s_config.qqmusic_cookie, cookie, sizeof(s_config.qqmusic_cookie));
    }
    return error;
}

esp_err_t device_config_store_codex_token(const char *token)
{
    if (token == NULL) return ESP_ERR_INVALID_ARG;
    /* 11.8: the wire contract is 16..64 bytes; the buffer holds 64 + NUL. */
    const size_t length = strlen(token);
    if (length < 16 || length > 64) return ESP_ERR_INVALID_SIZE;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_str(handle, "codex_tok", token);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) {
        strlcpy(s_config.codex_bridge_token, token,
                sizeof(s_config.codex_bridge_token));
    }
    return error;
}

esp_err_t device_config_store_bridge_url(const char *url)
{
    if (url == NULL || url[0] == '\0') return ESP_ERR_INVALID_ARG;
    const size_t length = strlen(url);
    if (length >= sizeof(s_config.bridge_url)) return ESP_ERR_INVALID_SIZE;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_str(handle, "bridge_url", url);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) {
        strlcpy(s_config.bridge_url, url, sizeof(s_config.bridge_url));
    }
    return error;
}

esp_err_t device_config_clear_codex_token(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_erase_key(handle, "codex_tok");
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) memset(s_config.codex_bridge_token, 0,
                                sizeof(s_config.codex_bridge_token));
    return error;
}

esp_err_t device_config_clear_provisioning(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_erase_all(handle);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) memset(&s_config, 0, sizeof(s_config));
    return error;
}
