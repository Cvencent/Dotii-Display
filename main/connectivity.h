#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codex_bridge_protocol.h"

void connectivity_start(void);
void connectivity_request_refresh(void);

/* Remember the Bridge address a CBDP datagram arrived from. It stays a candidate
   until a management request against it succeeds, so a multi-homed host cannot
   re-point the device at a NIC that does not serve 8787. */
void connectivity_note_bridge_ipv4(const char *ipv4);
bool connectivity_is_wifi_connected(void);
void connectivity_get_summary(char *buffer, size_t buffer_size);
void connectivity_get_ip(char *buffer, size_t buffer_size);
void connectivity_get_bridge_summary(char *buffer, size_t buffer_size);
void connectivity_apply_codex_bridge_message(
    const codex_bridge_message_t *message,
    void *context
);
