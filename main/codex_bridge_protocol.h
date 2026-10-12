#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

#define CODEX_BRIDGE_LINE_MAX 768
#define CODEX_BRIDGE_BODY_MAX 4096
#define CODEX_BRIDGE_ID_MAX 80
#define CODEX_BRIDGE_CAPABILITY_MAX 8
#define CODEX_BRIDGE_CAPABILITY_LEN 16

typedef struct {
    char type[24];
    char message_id[CODEX_BRIDGE_ID_MAX];
    char thread_id[CODEX_BRIDGE_ID_MAX];
    char kind[16];
    char status[24];
    char title[161];
    char *body;
    int seq;
    int total;
    bool ignored;
    cJSON *data;
    char capabilities[CODEX_BRIDGE_CAPABILITY_MAX][CODEX_BRIDGE_CAPABILITY_LEN];
    size_t capability_count;
} codex_bridge_message_t;

typedef void (*codex_bridge_message_handler_t)(
    const codex_bridge_message_t *message,
    void *context
);

esp_err_t codex_bridge_protocol_feed(
    const char *line,
    size_t length,
    codex_bridge_message_t *message
);
void codex_bridge_protocol_reset_message(codex_bridge_message_t *message);
bool codex_bridge_protocol_has_capability(
    const codex_bridge_message_t *message,
    const char *capability
);
const char *codex_bridge_protocol_body(
    const codex_bridge_message_t *message,
    const char *message_id
);
size_t codex_bridge_protocol_body_length(const codex_bridge_message_t *message);
