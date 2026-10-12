#pragma once

#include <stdbool.h>

#include "codex_bridge_protocol.h"
#include "esp_err.h"

esp_err_t codex_bridge_client_start(
    codex_bridge_message_handler_t handler,
    void *context
);
void codex_bridge_client_stop(void);
bool codex_bridge_client_is_online(void);
esp_err_t codex_bridge_client_send_read_ack(const char *message_id);
