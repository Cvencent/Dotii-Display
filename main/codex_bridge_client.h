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
size_t codex_bridge_client_capability_count(void);
const char *codex_bridge_client_capability_at(size_t index);
bool codex_bridge_client_has_capability(const char *capability);
/* Subscribe to a thread's own topic so Bridge delivers message events.
   Safe to call repeatedly; the firmware dedupes by thread_id. */
esp_err_t codex_bridge_client_subscribe_thread(const char *thread_id);
esp_err_t codex_bridge_client_send_read_ack(const char *message_id);
