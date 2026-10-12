#pragma once

#include <stdbool.h>

#include "app_state.h"
#include "cJSON.h"

bool management_snapshot_merge(codex_snapshot_t *target, const cJSON *root);
bool management_snapshot_parse(const char *json, codex_snapshot_t *target);
void management_snapshot_apply(codex_snapshot_t *target, const codex_snapshot_t *source);
