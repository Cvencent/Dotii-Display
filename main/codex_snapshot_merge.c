#include "codex_snapshot_merge.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "codex_state";

typedef struct {
    const char *id;
    dotii_expression_t default_expression;
    uint16_t default_duration_ms;
} dotii_business_state_definition_t;

static const dotii_business_state_definition_t DOTII_BUSINESS_STATES[DOTII_STATE_COUNT] = {
    {"codex_waiting_user", DOTII_EXPRESSION_CURIOUS, 0},
    {"codex_working", DOTII_EXPRESSION_WORKING, 0},
    {"codex_completed", DOTII_EXPRESSION_COMPLETE, 0},
    {"codex_failure", DOTII_EXPRESSION_FAILURE, 0},
};

void codex_snapshot_reset_dotii_mapping(codex_snapshot_t *target)
{
    if (target == NULL) return;
    for (size_t index = 0; index < DOTII_STATE_COUNT; ++index) {
        target->dotii_expression_for_state[index] =
            DOTII_BUSINESS_STATES[index].default_expression;
        target->dotii_state_duration_for_state[index] =
            DOTII_BUSINESS_STATES[index].default_duration_ms;
    }
}

static uint32_t dotii_state_token(const char *value)
{
    uint32_t hash = 2166136261U;
    if (value == NULL) return hash;
    while (*value != '\0') {
        hash ^= (uint8_t)*value++;
        hash *= 16777619U;
    }
    return hash;
}

static void dotii_set_state(codex_snapshot_t *target, int state_index,
                            const char *state_id, bool assigned,
                            dotii_expression_t fallback_expression)
{
    const bool base_idle = !assigned;
    target->dotii_base_idle = base_idle;
    target->dotii_state_assigned = assigned;
    target->dotii_state_token = dotii_state_token(state_id);
    if (!assigned) {
        target->dotii_expression = DOTII_EXPRESSION_IDLE_BREATH;
        target->dotii_state_duration_ms = 0;
        return;
    }
    if (state_index >= 0 && state_index < DOTII_STATE_COUNT) {
        target->dotii_expression = target->dotii_expression_for_state[state_index];
        target->dotii_state_duration_ms =
            target->dotii_state_duration_for_state[state_index];
    } else {
        target->dotii_expression = fallback_expression;
        target->dotii_state_duration_ms = 0;
    }
}

void codex_snapshot_refresh_dotii_state(codex_snapshot_t *target)
{
    if (target == NULL) return;
    if (target->status == CODEX_STATUS_FAILED) {
        dotii_set_state(target, DOTII_STATE_CODEX_FAILURE, "codex_failure", true,
                        DOTII_EXPRESSION_FAILURE);
    } else if (target->status == CODEX_STATUS_OFFLINE) {
        dotii_set_state(target, -1, "connecting", true, DOTII_EXPRESSION_CONNECTING);
    } else if (target->status == CODEX_STATUS_WORKING) {
        dotii_set_state(target, DOTII_STATE_CODEX_WORKING, "codex_working", true,
                        DOTII_EXPRESSION_WORKING);
    } else if (target->status == CODEX_STATUS_COMPLETED) {
        dotii_set_state(target, DOTII_STATE_CODEX_COMPLETED, "codex_completed", true,
                        DOTII_EXPRESSION_COMPLETE);
    } else if (target->status == CODEX_STATUS_WAITING) {
        dotii_set_state(target, DOTII_STATE_CODEX_WAITING_USER, "codex_waiting_user", true,
                        DOTII_EXPRESSION_CURIOUS);
    } else {
        dotii_set_state(target, -1, "idle", false, DOTII_EXPRESSION_IDLE_BREATH);
    }
}

static bool json_number(const cJSON *parent, const char *name, cJSON **out)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (value == NULL || !cJSON_IsNumber(value)) return false;
    if (out != NULL) *out = value;
    return true;
}

static cJSON *json_object(const cJSON *parent, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsObject(value) ? value : NULL;
}

static cJSON *json_array(const cJSON *parent, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsArray(value) ? value : NULL;
}

static cJSON *json_string(const cJSON *parent, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsString(value) && value->valuestring != NULL ? value : NULL;
}

static void copy_string(const cJSON *parent, const char *name, char *target, size_t size)
{
    cJSON *value = json_string(parent, name);
    if (value != NULL) strlcpy(target, value->valuestring, size);
}

static void copy_u32(const cJSON *parent, const char *name, uint32_t *target)
{
    cJSON *value = NULL;
    if (json_number(parent, name, &value) && value->valuedouble >= 0) {
        *target = (uint32_t)value->valuedouble;
    }
}

static void copy_time(const cJSON *parent, const char *name, time_t *target)
{
    cJSON *value = NULL;
    if (json_number(parent, name, &value)) *target = (time_t)value->valuedouble;
}

static void copy_bounded_u16(const cJSON *parent, const char *name, uint16_t *target)
{
    cJSON *value = NULL;
    if (!json_number(parent, name, &value)) return;
    const int number = (int)value->valuedouble;
    if (number < 0 || number > (int)UINT16_MAX) return;
    *target = (uint16_t)number;
}

static void copy_string_checked(const cJSON *parent, const char *name,
                                char *target, size_t size)
{
    cJSON *value = json_string(parent, name);
    if (value != NULL) strlcpy(target, value->valuestring, size);
}

/* Render an absolute reset instant as the "MM-DD HH:MM" string the on-device
   usage screens already display. */
static void format_epoch_date(time_t epoch, char *target, size_t size)
{
    if (epoch <= 0 || target == NULL || size == 0) return;
    struct tm local;
    if (localtime_r(&epoch, &local) == NULL) return;
    (void)strftime(target, size, "%m-%d %H:%M", &local);
}

/* Convert an absolute reset instant into the "MM-DD HH:MM" string the
   on-device usage screens already render. */
static void copy_epoch_date(const cJSON *parent, const char *name,
                            char *target, size_t size)
{
    cJSON *value = NULL;
    if (!json_number(parent, name, &value) || value->valuedouble <= 0) return;
    format_epoch_date((time_t)value->valuedouble, target, size);
}

static void apply_quota(const cJSON *quota, codex_snapshot_t *target)
{
    cJSON *five_hour = NULL;
    if (json_number(quota, "five_hour_remaining_percent", &five_hour) &&
        five_hour->valuedouble >= 0 && five_hour->valuedouble <= 100) {
        target->five_hour_remaining_percent = (int)five_hour->valuedouble;
        target->five_hour_available = true;
    }
    cJSON *weekly = NULL;
    if (json_number(quota, "weekly_remaining_percent", &weekly) &&
        weekly->valuedouble >= 0 && weekly->valuedouble <= 100) {
        target->weekly_remaining_percent = (int)weekly->valuedouble;
        target->weekly_available = true;
    }
    cJSON *weekly_tokens = NULL;
    if (json_number(quota, "weekly_tokens", &weekly_tokens) && weekly_tokens->valuedouble >= 0) {
        target->weekly_tokens = (uint32_t)weekly_tokens->valuedouble;
        target->weekly_tokens_available = true;
    } else {
        cJSON *seven_day = NULL;
        if (json_number(quota, "seven_day_tokens", &seven_day) && seven_day->valuedouble >= 0) {
            target->weekly_tokens = (uint32_t)seven_day->valuedouble;
            target->weekly_tokens_available = true;
        }
    }
    /* The protocol reports absolute reset instants; convert them into the
       "MM-DD HH:MM" strings the on-device usage screens already render. */
    copy_epoch_date(quota, "five_hour_reset_at", target->five_hour_reset_date,
                    sizeof(target->five_hour_reset_date));
    copy_epoch_date(quota, "weekly_reset_at", target->reset_date,
                    sizeof(target->reset_date));
    copy_string_checked(quota, "plan_type", target->plan_type, sizeof(target->plan_type));
    copy_string_checked(quota, "current_action", target->current_action,
                        sizeof(target->current_action));
    copy_string_checked(quota, "thread_id", target->thread_id, sizeof(target->thread_id));
    cJSON *token_count = NULL;
    if (json_number(quota, "token_count", &token_count) && token_count->valuedouble >= 0) {
        target->task_tokens = (uint32_t)token_count->valuedouble;
        target->task_tokens_available = true;
    }
    copy_bounded_u16(quota, "plan_completed", &target->plan_completed);
    copy_bounded_u16(quota, "plan_total", &target->plan_total);

    cJSON *task_status = json_string(quota, "task_status");
    if (task_status != NULL) {
        target->status = app_state_status_from_string(task_status->valuestring);
    }
}

static void free_task_text(codex_task_detail_t *task)
{
    if (task == NULL) return;
    heap_caps_free(task->last_user_message);
    heap_caps_free(task->conversation_text);
    task->last_user_message = NULL;
    task->conversation_text = NULL;
}

static void copy_external_string(char **target, const char *source)
{
    heap_caps_free(*target);
    *target = NULL;
    if (source == NULL || source[0] == '\0') return;
    size_t size = strlen(source) + 1;
    char *copy = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) copy = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (copy == NULL) return;
    memcpy(copy, source, size);
    *target = copy;
}

static void copy_task(codex_task_detail_t *task, const cJSON *source, uint32_t read_count)
{
    if (task == NULL || source == NULL) return;
    free_task_text(task);
    memset(task, 0, sizeof(*task));
    cJSON *status = json_string(source, "status");
    task->status = app_state_status_from_string(status == NULL ? "idle" : status->valuestring);
    task->conversation_mode = task->status == CODEX_STATUS_WORKING ||
                              task->status == CODEX_STATUS_WAITING ?
                                  CODEX_CONVERSATION_PROGRESS :
                                  CODEX_CONVERSATION_FINAL;
    copy_string(source, "thread_id", task->thread_id, sizeof(task->thread_id));
    copy_string(source, "title", task->title, sizeof(task->title));
    copy_string(source, "message_id", task->message_id, sizeof(task->message_id));
    copy_string(source, "result_message_id", task->result_message_id,
                sizeof(task->result_message_id));
    copy_external_string(&task->last_user_message,
                         json_string(source, "last_user_message") != NULL ?
                             json_string(source, "last_user_message")->valuestring : NULL);
    copy_external_string(&task->conversation_text,
                         json_string(source, "conversation_text") != NULL ?
                             json_string(source, "conversation_text")->valuestring : NULL);
    copy_u32(source, "duration_seconds", &task->duration_seconds);
    copy_u32(source, "message_count", &task->message_count);
    copy_time(source, "started_at_epoch", &task->started_at);
    copy_time(source, "last_updated_epoch", &task->updated_at);
    task->read_count = read_count;
}

void codex_snapshot_merge_status(codex_snapshot_t *target, const cJSON *data)
{
    if (target == NULL || !cJSON_IsObject(data)) return;
    target->valid = true;
    target->codex_source_online = true;
    target->source_online = true;
    copy_time(data, "generated_at_epoch", &target->generated_at);

    cJSON *account = json_object(data, "account");
    if (account != NULL) {
        cJSON *weekly = NULL;
        if (json_number(account, "weekly_remaining_percent", &weekly) &&
            weekly->valueint >= 0 && weekly->valueint <= 100) {
            target->weekly_remaining_percent = weekly->valueint;
            cJSON *weekly_available = cJSON_GetObjectItemCaseSensitive(account, "weekly_available");
            target->weekly_available = weekly_available == NULL ? true : cJSON_IsTrue(weekly_available);
        }
        copy_u32(account, "weekly_tokens", &target->weekly_tokens);
        cJSON *weekly_tokens_available = cJSON_GetObjectItemCaseSensitive(account, "weekly_tokens_available");
        if (weekly_tokens_available != NULL) {
            target->weekly_tokens_available = cJSON_IsTrue(weekly_tokens_available);
        }
        cJSON *five_hour = NULL;
        if (json_number(account, "five_hour_remaining_percent", &five_hour) &&
            five_hour->valueint >= 0 && five_hour->valueint <= 100) {
            target->five_hour_remaining_percent = five_hour->valueint;
            cJSON *five_hour_available = cJSON_GetObjectItemCaseSensitive(account, "five_hour_available");
            target->five_hour_available = five_hour_available == NULL ? true : cJSON_IsTrue(five_hour_available);
        }
        copy_string(account, "five_hour_reset_date", target->five_hour_reset_date,
                    sizeof(target->five_hour_reset_date));
        copy_string(account, "reset_date", target->reset_date, sizeof(target->reset_date));
        copy_string(account, "plan_type", target->plan_type, sizeof(target->plan_type));
    }

    cJSON *task = json_object(data, "task");
    if (task == NULL) return;
    cJSON *status = json_string(task, "status");
    target->status = app_state_status_from_string(status == NULL ? "idle" : status->valuestring);
    copy_string(task, "thread_id", target->thread_id, sizeof(target->thread_id));
    copy_string(task, "title", target->title, sizeof(target->title));
    copy_string(task, "current_action", target->current_action, sizeof(target->current_action));
    copy_string(task, "message_id", target->message_id, sizeof(target->message_id));
    copy_u32(task, "duration_seconds", &target->duration_seconds);
    copy_u32(task, "token_count", &target->task_tokens);
    cJSON *tokens_available = cJSON_GetObjectItemCaseSensitive(task, "token_count_available");
    if (tokens_available != NULL) target->task_tokens_available = cJSON_IsTrue(tokens_available);
    cJSON *plan_completed = NULL;
    cJSON *plan_total = NULL;
    if (json_number(task, "plan_completed", &plan_completed) && plan_completed->valueint >= 0) {
        target->plan_completed = (uint16_t)plan_completed->valueint;
    }
    if (json_number(task, "plan_total", &plan_total) && plan_total->valueint >= 0) {
        target->plan_total = (uint16_t)plan_total->valueint;
    }
    copy_time(task, "last_updated_epoch", &target->task_updated_at);
    cJSON *mode = json_string(task, "conversation_mode");
    if (mode != NULL) {
        if (strcmp(mode->valuestring, "progress") == 0) target->conversation_mode = CODEX_CONVERSATION_PROGRESS;
        else if (strcmp(mode->valuestring, "history") == 0) target->conversation_mode = CODEX_CONVERSATION_HISTORY;
        else if (strcmp(mode->valuestring, "final") == 0) target->conversation_mode = CODEX_CONVERSATION_FINAL;
        else target->conversation_mode = CODEX_CONVERSATION_EMPTY;
    } else {
        target->conversation_mode = target->status == CODEX_STATUS_WORKING ||
                                    target->status == CODEX_STATUS_WAITING ?
                                        CODEX_CONVERSATION_PROGRESS :
                                        CODEX_CONVERSATION_FINAL;
    }
}

/* Add two counters without wrapping around. */
static uint16_t saturating_add_u16(uint16_t base, uint16_t delta)
{
    const uint32_t sum = (uint32_t)base + (uint32_t)delta;
    return sum > 0xFFFFu ? 0xFFFFu : (uint16_t)sum;
}

void codex_snapshot_merge_codex_state(codex_snapshot_t *target, const cJSON *data)
{
    if (target == NULL || !cJSON_IsObject(data)) return;

    target->valid = true;
    target->codex_source_online = true;
    target->source_online = true;

    cJSON *running_value = NULL;
    if (json_number(data, "running", &running_value)) {
        copy_bounded_u16(data, "running", &target->running_tasks);
        /* The Bridge sent an explicit counter summary: it is authoritative and
           must not be replaced by a locally derived task count. */
        target->codex_state_summary_valid = true;
    }

    /* A replayed frame only re-establishes the reference counters. The panel
       refreshes the numbers from it but must not ring the task chime, because
       those tasks finished before the device connected. */
    cJSON *baseline = cJSON_GetObjectItemCaseSensitive(data, "baseline");
    target->codex_state_baseline = cJSON_IsTrue(baseline);

    /* Remember the previous raw counters.  The Bridge counters are "unread"
       counters that drop back to zero as soon as any client reads a result,
       so the device derives its own monotonic totals from the growth of
       these values. */
    const bool have_previous = target->codex_state_raw_valid;
    const uint16_t previous_completed = target->completed_unread_tasks;
    const uint16_t previous_failed = target->failed_unread_tasks;

    cJSON *completed = NULL;
    if (json_number(data, "completed_unread", &completed)) {
        copy_bounded_u16(data, "completed_unread", &target->completed_unread_tasks);
    } else {
        /* Older bridges only expose the terminal aliases. */
        copy_bounded_u16(data, "done", &target->completed_unread_tasks);
    }

    cJSON *failed = NULL;
    if (json_number(data, "failed_unread", &failed)) {
        copy_bounded_u16(data, "failed_unread", &target->failed_unread_tasks);
    } else {
        copy_bounded_u16(data, "failed", &target->failed_unread_tasks);
    }

    /* The Bridge counters are "unread" counters: they collapse back to zero
       as soon as the desktop or the phone reads a result.  The panel needs a
       stable number, so the device keeps a total that only ever grows.
       The first summary seeds the total with whatever the Bridge already
       knows, so the panel does not jump when the device connects.  A replayed
       baseline frame only re-establishes the reference counters and never
       adds to the total. */
    if (!have_previous) {
        target->completed_total_tasks = target->completed_unread_tasks;
        target->failed_total_tasks = target->failed_unread_tasks;
    } else if (!target->codex_state_baseline) {
        if (target->completed_unread_tasks > previous_completed) {
            target->completed_total_tasks = saturating_add_u16(
                target->completed_total_tasks,
                (uint16_t)(target->completed_unread_tasks - previous_completed));
        }
        if (target->failed_unread_tasks > previous_failed) {
            target->failed_total_tasks = saturating_add_u16(
                target->failed_total_tasks,
                (uint16_t)(target->failed_unread_tasks - previous_failed));
        }
    }
    target->codex_state_raw_valid = true;

    ESP_LOGI(TAG, "codex_state merged running=%u completed_unread=%u failed_unread=%u totals=%u/%u summary_valid=%d",
             (unsigned)target->running_tasks,
             (unsigned)target->completed_unread_tasks,
             (unsigned)target->failed_unread_tasks,
             (unsigned)target->completed_total_tasks,
             (unsigned)target->failed_total_tasks,
             target->codex_state_summary_valid ? 1 : 0);
}

void codex_snapshot_merge_quota(codex_snapshot_t *target, const cJSON *data)
{
    if (target == NULL || !cJSON_IsObject(data)) return;
    const cJSON *quota = json_object(data, "quota");
    if (quota == NULL) return;
    target->valid = true;
    target->codex_source_online = true;
    target->source_online = true;
    copy_time(data, "generated_at_epoch", &target->generated_at);
    apply_quota(quota, target);
    codex_snapshot_refresh_dotii_state(target);
}

void codex_snapshot_merge_account(codex_snapshot_t *target, const cJSON *data)
{
    if (target == NULL || !cJSON_IsObject(data)) return;
    target->valid = true;
    target->codex_source_online = true;
    target->source_online = true;

    /* account.changed. An absent plan_type means upstream did not report one;
       keep the previous text instead of inventing a tier. */
    copy_string_checked(data, "plan_type", target->plan_type, sizeof(target->plan_type));

    /* rate_limits.changed. The array lists exactly the windows upstream
       reported, so a missing window must render as "--" instead of keeping a
       stale percentage. Both availabilities are therefore cleared first and
       only re-armed by a window that is actually present. */
    cJSON *rate_limits = json_array(data, "rate_limits");
    if (rate_limits != NULL) {
        target->five_hour_available = false;
        target->weekly_available = false;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, rate_limits) {
            cJSON *window = json_string(item, "window");
            cJSON *remaining = NULL;
            if (window == NULL) continue;
            /* remaining_percent is already the REMAINING percentage. The
               Bridge inverted usedPercent; inverting it again here would
               show a full bar to an exhausted account. */
            if (!json_number(item, "remaining_percent", &remaining)) continue;
            if (remaining->valuedouble < 0 || remaining->valuedouble > 100) continue;
            cJSON *reset = NULL;
            time_t reset_epoch = 0;
            if (json_number(item, "reset_at_ms", &reset) && reset->valuedouble > 0) {
                reset_epoch = (time_t)(reset->valuedouble / 1000.0);
            }
            if (strcmp(window->valuestring, "five_hour") == 0) {
                target->five_hour_remaining_percent = (int)remaining->valuedouble;
                target->five_hour_available = true;
                format_epoch_date(reset_epoch, target->five_hour_reset_date,
                                  sizeof(target->five_hour_reset_date));
            } else if (strcmp(window->valuestring, "weekly") == 0) {
                target->weekly_remaining_percent = (int)remaining->valuedouble;
                target->weekly_available = true;
                format_epoch_date(reset_epoch, target->reset_date,
                                  sizeof(target->reset_date));
            }
        }
    }

    /* token_usage.changed. seven_day_tokens is the 7-day running total. */
    cJSON *token_usage = json_object(data, "token_usage");
    if (token_usage != NULL) {
        cJSON *seven_day = NULL;
        if (json_number(token_usage, "seven_day_tokens", &seven_day) &&
            seven_day->valuedouble >= 0) {
            target->weekly_tokens = (uint32_t)seven_day->valuedouble;
            target->weekly_tokens_available = true;
        }
        /* account.token_usage.read reports this turn/thread's total rather
           than the 7-day aggregate; it belongs on the per-task metric. */
        cJSON *token_total = NULL;
        if (json_number(token_usage, "token_total", &token_total) &&
            token_total->valuedouble >= 0) {
            target->task_tokens = (uint32_t)token_total->valuedouble;
            target->task_tokens_available = true;
        }
    }

    codex_snapshot_refresh_dotii_state(target);
}

void codex_snapshot_merge_tasks(
    codex_snapshot_t *target,
    const cJSON *data,
    codex_task_detail_t *tasks,
    size_t *count,
    size_t capacity
)
{
    if (target == NULL || count == NULL || tasks == NULL || capacity == 0) return;
    cJSON *source = json_array(data, "tasks");
    if (source == NULL) return;

    char previous_ids[CODEX_TASK_DETAIL_MAX][sizeof(tasks[0].message_id)] = {{0}};
    uint32_t previous_read_counts[CODEX_TASK_DETAIL_MAX] = {0};
    size_t previous_count = *count < capacity ? *count : capacity;
    for (size_t index = 0; index < previous_count; ++index) {
        strlcpy(previous_ids[index], tasks[index].message_id, sizeof(previous_ids[index]));
        previous_read_counts[index] = tasks[index].read_count;
    }

    size_t next_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, source) {
        if (!cJSON_IsObject(item) || next_count >= capacity) break;
        uint32_t read_count = 0;
        cJSON *message_id = json_string(item, "message_id");
        for (size_t index = 0; index < previous_count; ++index) {
            if (message_id != NULL &&
                strcmp(previous_ids[index], message_id->valuestring) == 0) {
                read_count = previous_read_counts[index];
                break;
            }
        }
        copy_task(&tasks[next_count], item, read_count);
        next_count++;
    }
    for (size_t index = next_count; index < capacity; ++index) {
        free_task_text(&tasks[index]);
        memset(&tasks[index], 0, sizeof(tasks[index]));
    }
    *count = next_count;
    target->valid = true;
    target->codex_source_online = true;
    target->source_online = true;

    cJSON *first = cJSON_GetArrayItem(source, 0);
    if (first != NULL) {
        cJSON *status = json_string(first, "status");
        if (status != NULL) target->status = app_state_status_from_string(status->valuestring);
        copy_string(first, "message_id", target->message_id, sizeof(target->message_id));
    }
    codex_snapshot_refresh_dotii_state(target);
}

void codex_snapshot_attach_result(
    codex_task_detail_t *tasks,
    size_t count,
    const char *thread_id,
    const char *message_id
)
{
    if (tasks == NULL || thread_id == NULL || thread_id[0] == '\0' ||
        message_id == NULL || message_id[0] == '\0') {
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(tasks[index].thread_id, thread_id) == 0) {
            strlcpy(tasks[index].result_message_id, message_id,
                    sizeof(tasks[index].result_message_id));
            return;
        }
    }
}

void codex_snapshot_merge_result(codex_snapshot_t *target, const cJSON *data)
{
    if (target == NULL || !cJSON_IsObject(data)) return;
    cJSON *task = json_object(data, "task");
    if (task == NULL) return;

    target->valid = true;
    target->codex_source_online = true;
    target->source_online = true;
    cJSON *status = json_string(task, "status");
    target->status = app_state_status_from_string(status == NULL ? "idle" : status->valuestring);
    copy_string(task, "thread_id", target->thread_id, sizeof(target->thread_id));
    copy_string(task, "title", target->title, sizeof(target->title));
    copy_string(task, "message_id", target->message_id, sizeof(target->message_id));
    copy_u32(task, "duration_seconds", &target->duration_seconds);
    copy_u32(task, "token_count", &target->task_tokens);
    cJSON *tokens_available = cJSON_GetObjectItemCaseSensitive(task, "token_count_available");
    if (tokens_available != NULL) target->task_tokens_available = cJSON_IsTrue(tokens_available);
    copy_time(task, "last_updated_epoch", &target->task_updated_at);

    cJSON *user = json_string(task, "last_user_message");
    cJSON *assistant = json_string(task, "last_assistant_message");
    cJSON *conversation = json_string(task, "conversation_text");
    if (user != NULL) strlcpy(target->last_user_message, user->valuestring, sizeof(target->last_user_message));
    if (assistant != NULL) {
        strlcpy(target->last_assistant_message, assistant->valuestring,
                sizeof(target->last_assistant_message));
    }
    if (conversation != NULL) {
        strlcpy(target->conversation_text, conversation->valuestring,
                sizeof(target->conversation_text));
    } else if (assistant != NULL) {
        strlcpy(target->conversation_text, assistant->valuestring,
                sizeof(target->conversation_text));
    }
    target->conversation_message_count = target->conversation_text[0] ? 1 : 0;
    target->conversation_mode = CODEX_CONVERSATION_FINAL;
    codex_snapshot_refresh_dotii_state(target);
}

static void decrement_terminal_unread(codex_snapshot_t *target,
                                      codex_task_status_t status)
{
    if (target == NULL) return;
    if (status == CODEX_STATUS_COMPLETED && target->completed_unread_tasks > 0) {
        target->completed_unread_tasks--;
    } else if (status == CODEX_STATUS_FAILED && target->failed_unread_tasks > 0) {
        target->failed_unread_tasks--;
    }
}

void codex_snapshot_apply_read_state(
    codex_snapshot_t *target,
    codex_task_detail_t *tasks,
    size_t count,
    const cJSON *data
)
{
    if (target == NULL || !cJSON_IsObject(data)) return;
    cJSON *message_id = json_string(data, "message_id");
    cJSON *read_by = json_array(data, "read_by");
    if (message_id == NULL || read_by == NULL) return;
    uint32_t read_count = (uint32_t)cJSON_GetArraySize(read_by);
    bool decremented = false;
    for (size_t index = 0; index < count; ++index) {
        const bool message_matches =
            strcmp(tasks[index].message_id, message_id->valuestring) == 0;
        const bool result_matches =
            strcmp(tasks[index].result_message_id, message_id->valuestring) == 0;
        if (!message_matches && !result_matches) continue;
        /* When the Bridge delivered a counter summary it owns the raw
           counters.  Decrementing them locally would make the next summary
           look like a larger growth and ring the task chime twice for a
           single finished task, so the local decrement is only used when no
           summary was received. */
        if (result_matches && tasks[index].read_count == 0 && read_count > 0 &&
            !decremented && !target->codex_state_raw_valid) {
            decrement_terminal_unread(target, tasks[index].status);
            decremented = true;
        }
        tasks[index].read_count = read_count;
    }
    if (strcmp(target->message_id, message_id->valuestring) == 0) {
        if (target->read_count == 0 && read_count > 0 && !decremented &&
            !target->codex_state_raw_valid) {
            decrement_terminal_unread(target, target->status);
        }
        target->read_count = read_count;
    }
}

void codex_snapshot_apply_source_state(
    codex_snapshot_t *target,
    bool codex_online,
    bool management_online
)
{
    if (target == NULL) return;
    target->codex_source_online = codex_online;
    target->management_source_online = management_online;
    target->source_online = codex_online || management_online;
    if (!codex_online) target->status = CODEX_STATUS_OFFLINE;
    codex_snapshot_refresh_dotii_state(target);
}
