#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char bridge_url[256];
    char bridge_token[65];
    char codex_bridge_token[65];
    char qqmusic_cookie[8];
    bool provisioned;
} device_config_values_t;

esp_err_t device_config_init(void);
const device_config_values_t *device_config_get(void);
esp_err_t device_config_save(const device_config_values_t *values);

/* Persist only a short QQ Music login marker. The management snapshot signals
   that the account is signed in; the bridge owns the real cookie and streams
   the song, so the device never needs the credential text itself. */
esp_err_t device_config_store_qq_cookie(const char *cookie);

/* Persist a freshly paired CodexBridge token on its own, leaving every other
   provisioned field untouched. Used by the CBDP wireless pairing flow, which
   learns the token over the LAN instead of through Bluetooth provisioning. */
esp_err_t device_config_store_codex_token(const char *token);

/* Persist only the management bridge URL. Used when the device learns the
   Bridge's current address from a CBDP datagram, so a DHCP change does not
   require re-provisioning. */
esp_err_t device_config_store_bridge_url(const char *url);

/* Forget the CodexBridge token (revoked or rotated by the host). The next
   CBDP HELLO then carries pairing_state=request again. */
esp_err_t device_config_clear_codex_token(void);

esp_err_t device_config_clear_provisioning(void);
