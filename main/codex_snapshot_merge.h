#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_state.h"
#include "cJSON.h"

void codex_snapshot_reset_dotii_mapping(codex_snapshot_t *target);
void codex_snapshot_refresh_dotii_state(codex_snapshot_t *target);
void codex_snapshot_merge_status(codex_snapshot_t *target, const cJSON *data);
void codex_snapshot_merge_codex_state(codex_snapshot_t *target, const cJSON *data);
void codex_snapshot_merge_quota(codex_snapshot_t *target, const cJSON *data);
/* Merge an account.changed / rate_limits.changed / token_usage.changed
   payload decoded by the CBDP projection. Only the families present in the
   object are touched; rate_limits replaces the window set wholesale so an
   unreported window renders as "--" instead of a stale percentage. */
void codex_snapshot_merge_account(codex_snapshot_t *target, const cJSON *data);
void codex_snapshot_merge_tasks(
    codex_snapshot_t *target,
    const cJSON *data,
    codex_task_detail_t *tasks,
    size_t *count,
    size_t capacity
);
void codex_snapshot_merge_result(codex_snapshot_t *target, const cJSON *data);
void codex_snapshot_attach_result(
    codex_task_detail_t *tasks,
    size_t count,
    const char *thread_id,
    const char *message_id
);
void codex_snapshot_apply_read_state(
    codex_snapshot_t *target,
    codex_task_detail_t *tasks,
    size_t count,
    const cJSON *data
);
void codex_snapshot_apply_source_state(
    codex_snapshot_t *target,
    bool codex_online,
    bool management_online
);
