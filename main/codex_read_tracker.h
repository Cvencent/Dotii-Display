#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CODEX_READ_TRACKER_DELAY_MS 2000

void codex_read_tracker_reset(void);
void codex_read_tracker_visit(
    const char *message_id,
    const char *result_message_id,
    uint32_t now_ms
);
bool codex_read_tracker_take_due(
    const char *message_id,
    const char *result_message_id,
    uint32_t now_ms
);
void codex_read_tracker_rearm(
    const char *message_id,
    const char *result_message_id,
    uint32_t now_ms
);
