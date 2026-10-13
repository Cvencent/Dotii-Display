#include "codex_snapshot_merge.h"

#include <string.h>

#include "esp_heap_caps.h"

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
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(tasks[index].message_id, message_id->valuestring) == 0 ||
            strcmp(tasks[index].result_message_id, message_id->valuestring) == 0) {
            tasks[index].read_count = read_count;
        }
    }
    if (strcmp(target->message_id, message_id->valuestring) == 0) {
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
