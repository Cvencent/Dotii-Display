/* Host implementations for the firmware services the pure xiaozhi logic uses.
   Nothing is persisted and the clock is a monotonic microsecond counter, which
   is all the parsing/state-mapping tests need. */

#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "host";
}

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out_handle)
{
    (void)name;
    (void)mode;
    if (out_handle != NULL) *out_handle = 0;
    return ESP_ERR_NVS_NOT_FOUND;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    (void)handle;
    (void)key;
    (void)out;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_FAIL;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length)
{
    (void)handle;
    (void)key;
    (void)out;
    (void)length;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_FAIL;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_FAIL;
}