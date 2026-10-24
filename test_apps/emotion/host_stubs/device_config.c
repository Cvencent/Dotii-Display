#include "device_config.h"
#include "esp_err.h"
#include <string.h>

static device_config_values_t s_values;

const device_config_values_t *device_config_get(void) { return &s_values; }
esp_err_t device_config_init(void) { memset(&s_values, 0, sizeof(s_values)); return ESP_OK; }
esp_err_t device_config_save(const device_config_values_t *values) { (void)values; return ESP_FAIL; }
esp_err_t device_config_clear_provisioning(void) { return ESP_FAIL; }
