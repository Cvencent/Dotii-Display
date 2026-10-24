#include "codex_bridge_projection.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "codex_proj";

#define CBDP_PROJECTION_MAX_THREADS 8
#define CBDP_PROJECTION_MAX_PAGES 16

/* Which counter fields a home-counter datagram carried. */
#define CBDP_HOME_FIELD_RUNNING 0x01u

/* How often the unchanged counters are republished. The Bridge refreshes
   them every two seconds, but the panel only needs a slow heartbeat to
   recover from a lost frame. */
#define CBDP_HOME_COUNTERS_HEARTBEAT_MS 10000
#define CBDP_HOME_FIELD_COMPLETED 0x02u
#define CBDP_HOME_FIELD_FAILED 0x04u

/* Internal RAM is nearly exhausted after Wi-Fi, panel and audio, and cJSON
   lives in internal RAM. Projection text is capped; the full text stays on
   the Bridge and is fetched on demand with content.read. */
#define CBDP_TEXT_STORE_MAX 128
#define CBDP_TITLE_STORE_MAX 56

/* Set when ensure_thread() refuses a thread because the local panel list is
   already full. Full capacity is a benign skip, not an allocation failure. */
static bool s_projection_capacity_full;



/* cJSON nodes for the projection live in PSRAM: after Wi-Fi, the AMOLED panel
   and the audio stack only ~6 KB of internal RAM is free, which is not enough
   for a thread list, while ~3.4 MB of PSRAM sits idle. The projection tree is
   only ever touched from the CBDP task, so a private hook set is safe. */
static void *projection_malloc(size_t size)
{
    void *block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (block == NULL) block = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return block;
}

static void projection_free(void *pointer)
{
    heap_caps_free(pointer);
}

void codex_bridge_cjson_hooks_install(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = projection_malloc,
        .free_fn = projection_free,
    };
    cJSON_InitHooks(&hooks);
}

static uint16_t read_u16(const uint8_t *value)
{
    return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
}


static esp_err_t field_u8(const uint8_t *tlv, size_t length, uint16_t tag,
                          uint8_t *out, bool required)
{
    codex_bridge_tlv_t field = {0};
    esp_err_t error = codex_bridge_tlv_find(tlv, length, tag, &field);
    if (error == ESP_ERR_NOT_FOUND && !required) return error;
    if (error != ESP_OK) return required ? ESP_ERR_INVALID_ARG : error;
    return codex_bridge_tlv_u8(&field, out);
}

static esp_err_t field_u16(const uint8_t *tlv, size_t length, uint16_t tag,
                           uint16_t *out, bool required)
{
    codex_bridge_tlv_t field = {0};
    esp_err_t error = codex_bridge_tlv_find(tlv, length, tag, &field);
    if (error == ESP_ERR_NOT_FOUND && !required) return error;
    if (error != ESP_OK) return required ? ESP_ERR_INVALID_ARG : error;
    if (field.length == 1) {
        *out = field.value[0];
        return ESP_OK;
    }
    return codex_bridge_tlv_u16(&field, out);
}

static esp_err_t field_u32(const uint8_t *tlv, size_t length, uint16_t tag,
                           uint32_t *out, bool required)
{
    codex_bridge_tlv_t field = {0};
    esp_err_t error = codex_bridge_tlv_find(tlv, length, tag, &field);
    if (error == ESP_ERR_NOT_FOUND && !required) return error;
    if (error != ESP_OK) return required ? ESP_ERR_INVALID_ARG : error;
    return codex_bridge_tlv_u32(&field, out);
}

static esp_err_t field_u64(const uint8_t *tlv, size_t length, uint16_t tag,
                           uint64_t *out, bool required)
{
    codex_bridge_tlv_t field = {0};
    esp_err_t error = codex_bridge_tlv_find(tlv, length, tag, &field);
    if (error == ESP_ERR_NOT_FOUND && !required) return error;
    if (error != ESP_OK) return required ? ESP_ERR_INVALID_ARG : error;
    return codex_bridge_tlv_u64(&field, out);
}

static const char *field_string(const uint8_t *tlv, size_t length, uint16_t tag)
{
    codex_bridge_tlv_t field = {0};
    if (codex_bridge_tlv_find(tlv, length, tag, &field) != ESP_OK) return NULL;
    return (const char *)field.value;
}

static void copy_field_string(const uint8_t *tlv, size_t length, uint16_t tag,
                              char *target, size_t capacity)
{
    if (target == NULL || capacity == 0) return;
    codex_bridge_tlv_t field = {0};
    if (codex_bridge_tlv_find(tlv, length, tag, &field) != ESP_OK) return;
    if (field.length >= capacity) return;
    if (field.length > 0) memcpy(target, field.value, field.length);
    target[field.length] = '\0';
}

static cJSON *thread_by_ref(cJSON *threads, uint32_t ref)
{
    if (!cJSON_IsArray(threads)) return NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, threads) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "thread_ref");
        if (cJSON_IsNumber(value) && (uint32_t)value->valuedouble == ref) return item;
    }
    return NULL;
}

static cJSON *ensure_thread(cJSON *threads, uint32_t ref)
{
    cJSON *thread = thread_by_ref(threads, ref);
    if (thread != NULL) return thread;
    if (cJSON_GetArraySize(threads) >= CBDP_PROJECTION_MAX_THREADS) {
        s_projection_capacity_full = true;
        return NULL;
    }
    thread = cJSON_CreateObject();
    if (thread == NULL) return NULL;
    cJSON_AddNumberToObject(thread, "thread_ref", ref);
    cJSON_AddNumberToObject(thread, "topic_id", 0);
    char fallback[32];
    snprintf(fallback, sizeof(fallback), "thread-%lu", (unsigned long)ref);
    cJSON_AddStringToObject(thread, "thread_id", fallback);
    cJSON_AddStringToObject(thread, "title", "");
    cJSON_AddNumberToObject(thread, "thread_state", 6);
    cJSON_AddNumberToObject(thread, "owner_state", 0);
    cJSON_AddNumberToObject(thread, "revision", 0);
    cJSON_AddNumberToObject(thread, "updated_at_ms", 0);
    cJSON_AddNumberToObject(thread, "message_ref", 0);
    cJSON_AddStringToObject(thread, "message_id", "");
    cJSON_AddNumberToObject(thread, "result_message_ref", 0);
    cJSON_AddStringToObject(thread, "result_message_id", "");
    cJSON_AddStringToObject(thread, "last_user_message", "");
    cJSON_AddStringToObject(thread, "conversation_text", "");
    cJSON_AddNumberToObject(thread, "message_count", 0);
    cJSON_AddNumberToObject(thread, "message_status", 6);
    cJSON_AddItemToArray(threads, thread);
    return thread;
}

static void set_string(cJSON *object, const char *name, const char *value)
{
    if (value == NULL) return;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item != NULL) {
        cJSON_SetValuestring(item, value);
    } else {
        cJSON_AddStringToObject(object, name, value);
    }
}

static void set_number(cJSON *object, const char *name, double value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item != NULL) {
        cJSON_SetNumberValue(item, value);
    } else {
        cJSON_AddNumberToObject(object, name, value);
    }
}

static const char *status_name(uint8_t state)
{
    switch (state) {
    case 1: return "working";
    case 2: return "completed";
    case 3:
    case 4: return "failed";
    case 0: return "idle";
    default: return "idle";
    }
}

static void add_string(cJSON *object, const char *name, const char *value)
{
    cJSON_AddStringToObject(object, name, value == NULL ? "" : value);
}

static cJSON *thread_task_json(const cJSON *thread)
{
    cJSON *task = cJSON_CreateObject();
    if (task == NULL) return NULL;
    cJSON *state = cJSON_GetObjectItemCaseSensitive(thread, "thread_state");
    cJSON *thread_id = cJSON_GetObjectItemCaseSensitive(thread, "thread_id");
    cJSON *title = cJSON_GetObjectItemCaseSensitive(thread, "title");
    cJSON *message_id = cJSON_GetObjectItemCaseSensitive(thread, "message_id");
    cJSON *result_message_id = cJSON_GetObjectItemCaseSensitive(thread, "result_message_id");
    cJSON *user = cJSON_GetObjectItemCaseSensitive(thread, "last_user_message");
    cJSON *conversation = cJSON_GetObjectItemCaseSensitive(thread, "conversation_text");
    cJSON *updated = cJSON_GetObjectItemCaseSensitive(thread, "updated_at_ms");
    cJSON *message_count = cJSON_GetObjectItemCaseSensitive(thread, "message_count");
    add_string(task, "thread_id", cJSON_IsString(thread_id) ? thread_id->valuestring : "");
    add_string(task, "title", cJSON_IsString(title) ? title->valuestring : "");
    add_string(task, "status", status_name(cJSON_IsNumber(state) ? (uint8_t)state->valueint : 6));
    add_string(task, "message_id", cJSON_IsString(message_id) ? message_id->valuestring : "");
    add_string(task, "result_message_id",
               cJSON_IsString(result_message_id) ? result_message_id->valuestring : "");
    const char *user_text = cJSON_IsString(user) ? user->valuestring : "";
    const char *conversation_text = cJSON_IsString(conversation) ? conversation->valuestring : "";
    char capped[CBDP_TEXT_STORE_MAX];
    size_t capped_length = strlen(user_text);
    if (capped_length > CBDP_TEXT_STORE_MAX - 1) capped_length = CBDP_TEXT_STORE_MAX - 1;
    memcpy(capped, user_text, capped_length);
    capped[capped_length] = 0;
    add_string(task, "last_user_message", capped);
    capped_length = strlen(conversation_text);
    if (capped_length > CBDP_TEXT_STORE_MAX - 1) capped_length = CBDP_TEXT_STORE_MAX - 1;
    memcpy(capped, conversation_text, capped_length);
    capped[capped_length] = 0;
    add_string(task, "conversation_text", capped);
    cJSON_AddNumberToObject(task, "last_updated_epoch",
                            cJSON_IsNumber(updated) ? updated->valuedouble / 1000.0 : 0);
    cJSON_AddNumberToObject(task, "message_count",
                            cJSON_IsNumber(message_count) ? message_count->valueint : 0);
    return task;
}

static void emit_message(codex_bridge_projection_t *projection,
                         const char *type, const char *kind, cJSON *data)
{
    if (projection == NULL || data == NULL) return;
    /* The Bridge replays a history window when it cannot honour our cursor.
       Every frame of that window carries an old counter value, so publishing
       them one by one would make the counters flicker. The window is flushed
       once, after it closed. */
    if (projection->quiet) {
        cJSON_Delete(data);
        return;
    }
    codex_bridge_message_t message = {0};
    strlcpy(message.type, type, sizeof(message.type));
    if (kind != NULL) strlcpy(message.kind, kind, sizeof(message.kind));
    message.data = data;
    if (projection->emit != NULL) projection->emit(&message, projection->emit_context);
    message.data = NULL;
    cJSON_Delete(data);
}

static cJSON *build_tasks_data(codex_bridge_projection_t *projection)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) return NULL;
    cJSON *tasks = cJSON_AddArrayToObject(data, "tasks");
    if (tasks == NULL) {
        cJSON_Delete(data);
        return NULL;
    }
    cJSON *active = thread_by_ref(projection->threads, projection->active_thread_ref);
    if (active != NULL) {
        cJSON *task = thread_task_json(active);
        if (task != NULL) cJSON_AddItemToArray(tasks, task);
    }
    cJSON *thread = NULL;
    cJSON_ArrayForEach(thread, projection->threads) {
        cJSON *ref = cJSON_GetObjectItemCaseSensitive(thread, "thread_ref");
        if (active != NULL && cJSON_IsNumber(ref) &&
            (uint32_t)ref->valuedouble == projection->active_thread_ref) {
            continue;
        }
        cJSON *task = thread_task_json(thread);
        if (task != NULL) cJSON_AddItemToArray(tasks, task);
    }
    return data;
}

static void emit_tasks(codex_bridge_projection_t *projection)
{
    cJSON *data = build_tasks_data(projection);
    if (data != NULL) emit_message(projection, "message", "task", data);
}

static void emit_status(codex_bridge_projection_t *projection)
{
    cJSON *thread = thread_by_ref(projection->threads, projection->active_thread_ref);
    if (thread == NULL) return;
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) return;
    cJSON *task = thread_task_json(thread);
    if (task == NULL) {
        cJSON_Delete(data);
        return;
    }
    cJSON_AddItemToObject(data, "task", task);
    emit_message(projection, "message", "status", data);
}

/* True while the UDP home-counter channel still delivers fresh numbers.
   The Bridge refreshes them every two seconds, so a gap of eight seconds
   means that channel is dead and the event channel has to take over. */
bool codex_bridge_projection_home_counters_fresh(
    const codex_bridge_projection_t *projection)
{
    if (projection == NULL || !projection->have_home_counters) return false;
    if (projection->home_counters_ms <= 0) return false;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    return (now_ms - projection->home_counters_ms) < CBDP_HOME_COUNTERS_FRESH_MS;
}

static void emit_state_ex(codex_bridge_projection_t *projection, bool baseline)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) return;
    uint16_t running = 0;
    uint16_t completed = 0;
    uint16_t failed = 0;
    if (codex_bridge_projection_home_counters_fresh(projection)) {
        /* The UDP home-counter channel is the authoritative source while it
           is alive. It is refreshed every two seconds and therefore always
           newer than anything the event channel can deliver. */
        if (projection->home_fields & CBDP_HOME_FIELD_RUNNING) {
            running = projection->home_running;
        }
        if (projection->home_fields & CBDP_HOME_FIELD_COMPLETED) {
            completed = projection->home_completed_unread;
        }
        if (projection->home_fields & CBDP_HOME_FIELD_FAILED) {
            failed = projection->home_failed_unread;
        }
    } else if (projection->have_summary) {
        /* The Bridge counts every task it tracks, while the local thread list
           only holds the threads this panel knows about. */
        running = projection->summary_running;
        completed = projection->summary_completed_unread;
        failed = projection->summary_failed_unread;
    } else {
        cJSON *thread = NULL;
        cJSON_ArrayForEach(thread, projection->threads) {
            cJSON *state = cJSON_GetObjectItemCaseSensitive(thread, "thread_state");
            if (!cJSON_IsNumber(state)) continue;
            if (state->valueint == 1) running++;
            else if (state->valueint == 2) completed++;
            else if (state->valueint == 3 || state->valueint == 4) failed++;
        }
    }
    cJSON_AddNumberToObject(data, "running", running);
    cJSON_AddNumberToObject(data, "completed_unread", completed);
    cJSON_AddNumberToObject(data, "failed_unread", failed);
    /* A state frame that only re-establishes the reference counters must not
       be mistaken for a freshly finished task by the panel. */
    if (baseline) cJSON_AddBoolToObject(data, "baseline", true);
    emit_message(projection, "codex_state", "", data);
}

/* Publish the counters with the baseline flag of the current session. */
static void emit_state(codex_bridge_projection_t *projection)
{
    emit_state_ex(projection, projection != NULL && projection->catchup);
}

static esp_err_t apply_thread_object(cJSON *threads, const uint8_t *object, size_t length)
{
    uint32_t thread_ref = 0;
    esp_err_t error = field_u32(object, length, CBDP_TLV_THREAD_REF, &thread_ref, true);
    if (error != ESP_OK || thread_ref == 0) return ESP_ERR_INVALID_ARG;
    cJSON *thread = ensure_thread(threads, thread_ref);
    if (thread == NULL) {
        return s_projection_capacity_full ? ESP_OK : ESP_ERR_NO_MEM;
    }

    codex_bridge_tlv_t field = {0};
    if (codex_bridge_tlv_find(object, length, CBDP_TLV_THREAD_ID, &field) == ESP_OK) {
        char value[128];
        if (field.length < sizeof(value)) {
            memcpy(value, field.value, field.length);
            value[field.length] = '\0';
            set_string(thread, "thread_id", value);
        }
    }
    uint32_t topic_id = 0;
    if (field_u32(object, length, CBDP_TLV_TOPIC_ID, &topic_id, false) == ESP_OK &&
        topic_id != 0) {
        set_number(thread, "topic_id", (double)topic_id);
    }

    char title[CBDP_TITLE_STORE_MAX];
    copy_field_string(object, length, CBDP_TLV_TITLE, title, sizeof(title));
    if (title[0] != '\0') set_string(thread, "title", title);
    uint8_t state = 6;
    if (field_u8(object, length, CBDP_TLV_THREAD_STATE, &state, false) != ESP_OK) {
        uint16_t status = 6;
        if (field_u16(object, length, CBDP_TLV_STATUS, &status, false) == ESP_OK) {
            state = (uint8_t)status;
        }
    }
    /* Also accept the generic STATE tag (0x0064) that some bridges emit. */
    if (state == 6) {
        uint8_t generic = 6;
        if (field_u8(object, length, CBDP_TLV_STATE, &generic, false) == ESP_OK) {
            state = generic;
        }
    }
    if (state <= 6) set_number(thread, "thread_state", state);
    uint8_t owner = 0;
    if (field_u8(object, length, CBDP_TLV_OWNER_STATE, &owner, false) == ESP_OK) {
        set_number(thread, "owner_state", owner);
    }
    uint64_t revision = 0;
    if (field_u64(object, length, CBDP_TLV_REVISION, &revision, false) == ESP_OK) {
        set_number(thread, "revision", (double)revision);
    }
    uint64_t updated = 0;
    if (field_u64(object, length, CBDP_TLV_UPDATED_AT_MS, &updated, false) == ESP_OK) {
        set_number(thread, "updated_at_ms", (double)updated);
    }
    return ESP_OK;
}

static esp_err_t apply_message_object(cJSON *threads, const uint8_t *object, size_t length)
{
    uint32_t thread_ref = 0;
    uint32_t message_ref = 0;
    esp_err_t error = field_u32(object, length, CBDP_TLV_THREAD_REF, &thread_ref, true);
    if (error != ESP_OK || thread_ref == 0) return ESP_ERR_INVALID_ARG;
    if (field_u32(object, length, CBDP_TLV_MESSAGE_REF, &message_ref, true) != ESP_OK ||
        message_ref == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *thread = ensure_thread(threads, thread_ref);
    if (thread == NULL) {
        return s_projection_capacity_full ? ESP_OK : ESP_ERR_NO_MEM;
    }

    uint8_t kind = 2;
    uint16_t status = 2;
    (void)field_u8(object, length, CBDP_TLV_KIND, &kind, false);
    (void)field_u16(object, length, CBDP_TLV_STATUS, &status, false);
    set_number(thread, "message_status", status);
    /* Task execution arrives as message objects (kind=turn/task) carrying the
       live status. Mirror it onto the thread so the running/completed/failed
       counters reflect what is actually happening. Status: 1=running,
       2=completed, 3/4=failed (matching the turn event mapping). */
    if (status == 1) {
        set_number(thread, "thread_state", 1);
    } else if (status == 2) {
        set_number(thread, "thread_state", 2);
    } else if (status == 3 || status == 4) {
        set_number(thread, "thread_state", status);
    }
    set_number(thread, "message_ref", message_ref);
    cJSON *count = cJSON_GetObjectItemCaseSensitive(thread, "message_count");
    if (cJSON_IsNumber(count) && count->valueint < 2) set_number(thread, "message_count", count->valueint + 1);

    char id[160];
    const char *external_id = field_string(object, length, CBDP_TLV_MESSAGE_ID);
    if (external_id != NULL) {
        codex_bridge_tlv_t field = {0};
        (void)codex_bridge_tlv_find(object, length, CBDP_TLV_MESSAGE_ID, &field);
        if (field.length < sizeof(id)) {
            memcpy(id, field.value, field.length);
            id[field.length] = '\0';
        } else {
            snprintf(id, sizeof(id), "msg-%lu", (unsigned long)message_ref);
        }
    } else {
        snprintf(id, sizeof(id), "msg-%lu", (unsigned long)message_ref);
    }
    const char *summary = field_string(object, length, CBDP_TLV_SUMMARY);
    const char *body = field_string(object, length, CBDP_TLV_BODY);
    char text[CBDP_TEXT_STORE_MAX];
    copy_field_string(object, length,
                      summary != NULL ? CBDP_TLV_SUMMARY : CBDP_TLV_BODY,
                      text, sizeof(text));

    if (kind == 0) {
        set_string(thread, "message_id", id);
        set_string(thread, "last_user_message", text);
    } else {
        set_string(thread, "result_message_id", id);
        set_number(thread, "result_message_ref", message_ref);
        set_string(thread, "conversation_text", text);
    }
    uint64_t updated = 0;
    if (field_u64(object, length, CBDP_TLV_UPDATED_AT_MS, &updated, false) != ESP_OK) {
        (void)field_u64(object, length, CBDP_TLV_CREATED_AT_MS, &updated, false);
    }
    if (updated > 0) set_number(thread, "updated_at_ms", (double)updated);
    (void)body;
    return ESP_OK;
}

static esp_err_t apply_object(cJSON *threads, const uint8_t *object, size_t length)
{
    uint8_t object_type = 0;
    esp_err_t error = field_u8(object, length, CBDP_TLV_OBJECT_TYPE, &object_type, true);
    if (error != ESP_OK) return error;
    if (object_type == 0) return apply_thread_object(threads, object, length);
    if (object_type == 2) return apply_message_object(threads, object, length);
    return ESP_OK;
}

static esp_err_t apply_objects(cJSON *threads, const uint8_t *array, size_t length)
{
    if (length < 2) return ESP_ERR_INVALID_SIZE;
    const uint16_t count = read_u16(array);
    const uint8_t *cursor = array + 2;
    const uint8_t *end = array + length;
    for (uint16_t index = 0; index < count; ++index) {
        codex_bridge_tlv_t element = {0};
        esp_err_t error = codex_bridge_tlv_next(&cursor, end, &element);
        if (error != ESP_OK) return error;
        if (element.tag != CBDP_TLV_OBJECT) return ESP_ERR_INVALID_ARG;
        error = apply_object(threads, element.value, element.length);
        if (error != ESP_OK) return error;
    }
    return cursor == end ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t codex_bridge_projection_init(
    codex_bridge_projection_t *projection,
    codex_bridge_projection_emit_t emit,
    void *context
)
{
    if (projection == NULL) return ESP_ERR_INVALID_ARG;
    memset(projection, 0, sizeof(*projection));
    projection->threads = cJSON_CreateArray();
    projection->staged_threads = cJSON_CreateArray();
    if (projection->threads == NULL || projection->staged_threads == NULL) {
        cJSON_Delete(projection->threads);
        cJSON_Delete(projection->staged_threads);
        memset(projection, 0, sizeof(*projection));
        return ESP_ERR_NO_MEM;
    }
    projection->emit = emit;
    projection->emit_context = context;
    return ESP_OK;
}

void codex_bridge_projection_reset(codex_bridge_projection_t *projection)
{
    if (projection == NULL) return;
    cJSON_Delete(projection->threads);
    cJSON_Delete(projection->staged_threads);
    codex_bridge_projection_emit_t emit = projection->emit;
    void *context = projection->emit_context;
    memset(projection, 0, sizeof(*projection));
    projection->threads = cJSON_CreateArray();
    projection->staged_threads = cJSON_CreateArray();
    projection->emit = emit;
    projection->emit_context = context;
}

void codex_bridge_projection_set_catchup(codex_bridge_projection_t *projection, bool catchup)
{
    if (projection == NULL) return;
    projection->catchup = catchup;
}

void codex_bridge_projection_set_quiet(codex_bridge_projection_t *projection, bool quiet)
{
    if (projection == NULL) return;
    projection->quiet = quiet;
}

void codex_bridge_projection_flush(codex_bridge_projection_t *projection)
{
    if (projection == NULL) return;
    emit_tasks(projection);
    emit_status(projection);
    emit_state(projection);
}

void codex_bridge_projection_forget_summary_seq(codex_bridge_projection_t *projection)
{
    if (projection == NULL) return;
    const uint64_t previous = projection->summary_global_seq;
    projection->summary_global_seq = 0;
    projection->have_summary_global_seq = false;
    ESP_LOGI(TAG, "state watermark released (previous gseq=%llu)",
             (unsigned long long)previous);
}

void codex_bridge_projection_forget_session_state(codex_bridge_projection_t *projection)
{
    if (projection == NULL) return;
    const uint64_t previous = projection->summary_global_seq;
    projection->summary_global_seq = 0;
    projection->have_summary_global_seq = false;
    projection->seen_count = 0;
    projection->seen_next = 0;
    /* The counters of the dead session are kept, so the change detection
       still works across the reconnect. They are marked stale instead, and
       the next home-counter datagram re-establishes the reference values
       and is published as a baseline. That is what brings the panel back to
       the numbers of the new Bridge: without it the very first datagram of
       the new session looked unchanged, nothing was published, and the
       counters stayed frozen on the values of the dead instance. */
    projection->home_counters_ms = 0;
    projection->home_counters_resync = true;
    projection->home_counters_guard_logged = false;
    projection->have_summary = false;
    projection->summary_running = 0;
    projection->summary_completed_unread = 0;
    projection->summary_failed_unread = 0;
    ESP_LOGI(TAG, "session state reset (previous summary gseq=%llu)",
             (unsigned long long)previous);
}

esp_err_t codex_bridge_projection_apply_snapshot(
    codex_bridge_projection_t *projection,
    const codex_bridge_frame_t *frame
)
{
    s_projection_capacity_full = false;
    if (projection == NULL || frame == NULL || frame->message_type != CBDP_MSG_SNAPSHOT) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t snapshot_id = 0;
    uint16_t page_index = 0;
    uint16_t page_count = 0;
    uint8_t complete = 0;
    esp_err_t error = field_u32(frame->header_tlv, frame->header_tlv_len,
                                CBDP_TLV_SNAPSHOT_ID, &snapshot_id, true);
    if (error == ESP_OK) {
        uint64_t revision = 0;
        error = field_u64(frame->header_tlv, frame->header_tlv_len,
                          CBDP_TLV_SNAPSHOT_REVISION, &revision, false);
        (void)revision;
        if (error == ESP_ERR_NOT_FOUND) error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = field_u16(frame->header_tlv, frame->header_tlv_len,
                          CBDP_TLV_PAGE_INDEX, &page_index, true);
    }
    if (error == ESP_OK) {
        error = field_u16(frame->header_tlv, frame->header_tlv_len,
                          CBDP_TLV_PAGE_COUNT, &page_count, true);
    }
    if (error == ESP_OK) {
        error = field_u8(frame->header_tlv, frame->header_tlv_len,
                         CBDP_TLV_COMPLETE, &complete, true);
    }
    if (error != ESP_OK || page_count == 0 || page_count > CBDP_PROJECTION_MAX_PAGES ||
        page_index >= page_count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (projection->snapshot_id != snapshot_id ||
        projection->snapshot_page_count != page_count) {
        cJSON_Delete(projection->staged_threads);
        projection->staged_threads = cJSON_CreateArray();
        if (projection->staged_threads == NULL) return ESP_ERR_NO_MEM;
        projection->snapshot_id = snapshot_id;
        projection->snapshot_page_count = page_count;
        projection->snapshot_page_mask = 0;
    }
    if (projection->snapshot_page_mask & (1u << page_index)) return ESP_OK;

    codex_bridge_tlv_t objects = {0};
    error = codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_OBJECTS, &objects);
    if (error != ESP_OK) return error;
    error = apply_objects(projection->staged_threads, objects.value, objects.length);
    if (error != ESP_OK) return error;
    projection->snapshot_page_mask |= (1u << page_index);

    const uint32_t all_pages = page_count == 32 ? UINT32_MAX :
                               ((1u << page_count) - 1u);
    if (!complete || (projection->snapshot_page_mask & all_pages) != all_pages) {
        return ESP_OK;
    }

    cJSON *old = projection->threads;
    projection->threads = projection->staged_threads;
    projection->staged_threads = old;
    cJSON_Delete(projection->staged_threads);
    projection->staged_threads = cJSON_CreateArray();
    projection->snapshot_page_mask = 0;
    if (projection->staged_threads == NULL) projection->staged_threads = cJSON_CreateArray();

    emit_tasks(projection);
    emit_status(projection);
    emit_state(projection);
    return ESP_OK;
}

static void apply_turn_payload(cJSON *thread, const uint8_t *payload, size_t length)
{
    uint16_t status = 0;
    const esp_err_t status_err =
        field_u16(payload, length, CBDP_TLV_STATUS, &status, false);
    if (status_err != ESP_OK) {
        /* No explicit status: also try the generic STATE tag, otherwise leave
           the thread's current state untouched instead of dropping the turn. */
        uint8_t generic = 0;
        if (field_u8(payload, length, CBDP_TLV_STATE, &generic, false) == ESP_OK) {
            status = generic;
        } else {
            return;
        }
    }
    uint8_t state = 1;
    if (status == 2) state = 2;
    else if (status == 3) state = 3;
    else if (status == 4) state = 4;
    else if (status == 0) state = 0;
    set_number(thread, "thread_state", state);
    char action[128];
    copy_field_string(payload, length, CBDP_TLV_CURRENT_ACTION, action, sizeof(action));
    if (action[0] != '\0') set_string(thread, "current_action", action);
}

/* The Bridge repeats frames after a reconnect and may also resend a frame
   with its original sequence numbers. Track the recently handled global
   sequence numbers so an exact duplicate is ignored instead of being applied
   twice. */
static bool seen_global_seq(const codex_bridge_projection_t *projection,
                            uint64_t global_seq)
{
    if (projection == NULL || global_seq == 0) return false;
    for (uint8_t index = 0; index < projection->seen_count; ++index) {
        if (projection->seen_global_seqs[index] == global_seq) return true;
    }
    return false;
}

static void remember_global_seq(codex_bridge_projection_t *projection,
                                uint64_t global_seq)
{
    if (projection == NULL || global_seq == 0) return;
    projection->seen_global_seqs[projection->seen_next] = global_seq;
    projection->seen_next =
        (uint8_t)((projection->seen_next + 1) % CBDP_PROJECTION_SEEN_RING);
    if (projection->seen_count < CBDP_PROJECTION_SEEN_RING) projection->seen_count++;
}

/* system.codex_state (0x0704) carries the Bridge's authoritative counters.
   The payload is a nested TLV blob holding the running / unread count tags.
   Returns true when the counters actually changed. */
static bool apply_summary_event(codex_bridge_projection_t *projection,
                                const uint8_t *payload,
                                size_t length)
{
    if (projection == NULL || payload == NULL || length == 0) return false;
    /* The UDP home-counter channel carries the same counters and refreshes
       them every two seconds. Mixing both sources would make the panel
       flicker between two different numbers, so the event channel is
       ignored as long as the home counters of this session are alive. */
    if (codex_bridge_projection_home_counters_fresh(projection)) {
        if (!projection->home_counters_guard_logged) {
            projection->home_counters_guard_logged = true;
            ESP_LOGI(TAG, "summary ev=0x0704 ignored (UDP home counters are authoritative)");
        }
        return false;
    }
    /* The home counters went silent, so the event channel takes the numbers
       over. The next datagram of the home channel has to re-establish the
       reference values, otherwise the two channels would fight over the
       numbers and the panel would flicker again. */
    projection->home_counters_resync = true;
    uint16_t running = 0;
    uint16_t completed = 0;
    uint16_t failed = 0;
    const bool have_running =
        field_u16(payload, length, CBDP_TLV_RUNNING_COUNT, &running, false) == ESP_OK;
    const bool have_completed =
        field_u16(payload, length, CBDP_TLV_COMPLETED_UNREAD, &completed, false) == ESP_OK;
    const bool have_failed =
        field_u16(payload, length, CBDP_TLV_FAILED_UNREAD, &failed, false) == ESP_OK;
    if (!have_running && !have_completed && !have_failed) {
        ESP_LOGW(TAG, "summary ev=0x0704 no counter tag found (len=%u)",
                 (unsigned)length);
        return false;
    }

    bool changed = !projection->have_summary;
    if (have_running && projection->summary_running != running) changed = true;
    if (have_completed && projection->summary_completed_unread != completed) changed = true;
    if (have_failed && projection->summary_failed_unread != failed) changed = true;

    if (have_running) projection->summary_running = running;
    if (have_completed) projection->summary_completed_unread = completed;
    if (have_failed) projection->summary_failed_unread = failed;
    projection->have_summary = true;
    ESP_LOGI(TAG,
             "summary ev=0x0704 running=%u completed_unread=%u failed_unread=%u "
             "(present r=%d c=%d f=%d) changed=%d",
             (unsigned)running, (unsigned)completed, (unsigned)failed,
             have_running ? 1 : 0, have_completed ? 1 : 0, have_failed ? 1 : 0,
             changed ? 1 : 0);
    return changed;
}

/* Decode the three account-scoped events into the object shape
   codex_snapshot_merge_account() consumes. Every field is optional: upstream
   may omit a window or the plan, and the device must then keep whatever it
   had instead of inventing a value. */
/* Map an account.* read method to the account event carrying the same field
   set, so a pull and a push decode through one path. */
static uint16_t account_event_for_method(uint16_t method_id)
{
    switch (method_id) {
    case CBDP_METHOD_ACCOUNT_READ: return CBDP_EVENT_ACCOUNT_CHANGED;
    case CBDP_METHOD_ACCOUNT_RATE_LIMITS_READ: return CBDP_EVENT_RATE_LIMITS_CHANGED;
    case CBDP_METHOD_ACCOUNT_TOKEN_USAGE_READ: return CBDP_EVENT_TOKEN_USAGE_CHANGED;
    default: return 0;
    }
}

/* Decode an account.* read RESULT.
 *
 * The running Bridge flattens the result fields straight into the RESULT
 * header next to request_id/method_id/state (its own tests assert
 * emitted[0].header[TAG_PLAN_TYPE]); it does not wrap them in the 0x0061
 * result TLV. Accept the flattened shape, and also the wrapped shape, so the
 * device works against either.
 */
esp_err_t codex_bridge_projection_apply_account_result(
    codex_bridge_projection_t *projection,
    uint16_t method_id,
    const uint8_t *header_tlv,
    size_t header_tlv_length)
{
    if (projection == NULL) return ESP_ERR_INVALID_ARG;
    const uint16_t event_type = account_event_for_method(method_id);
    if (event_type == 0) return ESP_ERR_INVALID_ARG;

    codex_bridge_tlv_t result = {0};
    const uint8_t *payload = header_tlv;
    size_t length = header_tlv_length;
    if (header_tlv != NULL &&
        codex_bridge_tlv_find(header_tlv, header_tlv_length, CBDP_TLV_RESULT,
                              &result) == ESP_OK) {
        payload = result.value;
        length = result.length;
    }
    return codex_bridge_projection_apply_account(projection, event_type,
                                                payload, length);
}

esp_err_t codex_bridge_projection_apply_account(codex_bridge_projection_t *projection,
                                     uint16_t event_type,
                                     const uint8_t *payload,
                                     size_t payload_length)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) return ESP_ERR_NO_MEM;
    bool have_field = false;

    if (event_type == CBDP_EVENT_ACCOUNT_CHANGED) {
        codex_bridge_tlv_t field = {0};
        if (codex_bridge_tlv_find(payload, payload_length, CBDP_TLV_PLAN_TYPE,
                                  &field) == ESP_OK) {
            char plan_type[32] = {0};
            if (codex_bridge_tlv_utf8(&field, plan_type, sizeof(plan_type)) == ESP_OK &&
                plan_type[0] != '\0') {
                cJSON_AddStringToObject(data, "plan_type", plan_type);
                have_field = true;
            }
        }
    } else if (event_type == CBDP_EVENT_RATE_LIMITS_CHANGED) {
        codex_bridge_tlv_t array = {0};
        if (codex_bridge_tlv_find(payload, payload_length, CBDP_TLV_RATE_LIMITS,
                                  &array) == ESP_OK) {
            cJSON *windows = cJSON_AddArrayToObject(data, "rate_limits");
            const uint8_t *cursor = array.value;
            const uint8_t *end = array.value + array.length;
            if (windows != NULL && (size_t)(end - cursor) >= sizeof(uint16_t)) {
                const uint16_t count = read_u16(cursor);
                cursor += sizeof(uint16_t);
                for (uint16_t index = 0; index < count && cursor < end; ++index) {
                    codex_bridge_tlv_t element = {0};
                    if (codex_bridge_tlv_next(&cursor, end, &element) != ESP_OK) break;
                    if (element.tag != CBDP_TLV_RATE_LIMITS) continue;
                    cJSON *window = cJSON_CreateObject();
                    if (window == NULL) break;
                    codex_bridge_tlv_t field = {0};
                    if (codex_bridge_tlv_find(element.value, element.length,
                                              CBDP_TLV_RATE_LIMIT_WINDOW,
                                              &field) == ESP_OK) {
                        char name[16] = {0};
                        if (codex_bridge_tlv_utf8(&field, name, sizeof(name)) == ESP_OK) {
                            cJSON_AddStringToObject(window, "window", name);
                        }
                    }
                    uint8_t remaining = 0;
                    if (codex_bridge_tlv_find(element.value, element.length,
                                              CBDP_TLV_REMAINING_PERCENT,
                                              &field) == ESP_OK &&
                        codex_bridge_tlv_u8(&field, &remaining) == ESP_OK) {
                        /* Sent through unchanged: the Bridge already applied
                           the usedPercent -> remaining_percent inversion. */
                        cJSON_AddNumberToObject(window, "remaining_percent", remaining);
                    }
                    uint64_t reset_at_ms = 0;
                    if (codex_bridge_tlv_find(element.value, element.length,
                                              CBDP_TLV_RESET_AT_MS, &field) == ESP_OK &&
                        codex_bridge_tlv_u64(&field, &reset_at_ms) == ESP_OK) {
                        cJSON_AddNumberToObject(window, "reset_at_ms",
                                                (double)reset_at_ms);
                    }
                    cJSON_AddItemToArray(windows, window);
                }
                have_field = true;
            }
        }
    } else if (event_type == CBDP_EVENT_TOKEN_USAGE_CHANGED) {
        cJSON *usage = cJSON_AddObjectToObject(data, "token_usage");
        if (usage != NULL) {
            uint64_t seven_day_tokens = 0;
            if (field_u64(payload, payload_length, CBDP_TLV_SEVEN_DAY_TOKENS,
                          &seven_day_tokens, false) == ESP_OK) {
                cJSON_AddNumberToObject(usage, "seven_day_tokens",
                                        (double)seven_day_tokens);
                have_field = true;
            }
            codex_bridge_tlv_t field = {0};
            if (codex_bridge_tlv_find(payload, payload_length,
                                      CBDP_TLV_SEVEN_DAY_AS_OF,
                                      &field) == ESP_OK) {
                char as_of[16] = {0};
                if (codex_bridge_tlv_utf8(&field, as_of, sizeof(as_of)) == ESP_OK) {
                    cJSON_AddStringToObject(usage, "seven_day_as_of", as_of);
                }
            }
            /* account.token_usage.read answers with a token_usage container
               rather than the 7-day aggregate. */
            codex_bridge_tlv_t container = {0};
            if (codex_bridge_tlv_find(payload, payload_length,
                                      CBDP_TLV_TOKEN_USAGE,
                                      &container) == ESP_OK) {
                const struct {
                    uint16_t tag;
                    const char *name;
                } fields[] = {
                    {CBDP_TLV_TOKEN_TOTAL, "token_total"},
                    {CBDP_TLV_TOKEN_INPUT, "token_input"},
                    {CBDP_TLV_TOKEN_OUTPUT, "token_output"},
                    {CBDP_TLV_TOKEN_CACHED, "token_cached"},
                    {CBDP_TLV_TOKEN_REASONING, "token_reasoning"},
                };
                for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
                    uint32_t value = 0;
                    if (field_u32(container.value, container.length,
                                  fields[index].tag, &value, false) == ESP_OK) {
                        cJSON_AddNumberToObject(usage, fields[index].name, value);
                        have_field = true;
                    }
                }
            }
            if (!have_field) {
                cJSON_DeleteItemFromObject(data, "token_usage");
            }
        }
    }

    if (!have_field) {
        /* Upstream reported nothing this round: keep the last known values
           rather than blanking the panel. */
        cJSON_Delete(data);
        return ESP_OK;
    }
    emit_message(projection, "message", "account", data);
    return ESP_OK;
}

static esp_err_t apply_delta_event(codex_bridge_projection_t *projection,
                                   uint16_t event_type,
                                   const uint8_t *payload,
                                   size_t payload_length)
{
    uint32_t thread_ref = 0;
    uint32_t message_ref = 0;
    switch (event_type) {
    case CBDP_EVENT_ACCOUNT_CHANGED:
    case CBDP_EVENT_RATE_LIMITS_CHANGED:
    case CBDP_EVENT_TOKEN_USAGE_CHANGED:
        return codex_bridge_projection_apply_account(projection, event_type,
                                                     payload, payload_length);
    case 0x0101:
    case 0x0102:
    case 0x0103:
    case 0x0104:
    case 0x0105:
    case 0x0B01:
    case 0x0B02:
    case 0x0B03:
    case 0x0B04:
    case 0x0B05:
    case 0x0B06:
    case 0x0B07:
    case 0x0B08:
    case 0x0B0B:
    case 0x0B0C:
        return apply_thread_object(projection->threads, payload, payload_length) == ESP_OK
                   ? ESP_OK : ESP_ERR_INVALID_ARG;
    case 0x0201:
    case 0x0202:
    case 0x0203:
    case 0x0204:
    case 0x0205:
    case 0x0206: {
        if (field_u32(payload, payload_length, CBDP_TLV_THREAD_REF, &thread_ref, true) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON *thread = ensure_thread(projection->threads, thread_ref);
    if (thread == NULL) {
        return s_projection_capacity_full ? ESP_OK : ESP_ERR_NO_MEM;
    }
        apply_turn_payload(thread, payload, payload_length);
        return ESP_OK;
    }
    case 0x0301:
    case 0x0302:
        if (field_u32(payload, payload_length, CBDP_TLV_MESSAGE_REF, &message_ref, true) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        return apply_message_object(projection->threads, payload, payload_length);
    case 0x0303: {
        if (field_u32(payload, payload_length, CBDP_TLV_MESSAGE_REF, &message_ref, true) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON *data = cJSON_CreateObject();
        if (data == NULL) return ESP_ERR_NO_MEM;
        char id[80];
        snprintf(id, sizeof(id), "msg-%lu", (unsigned long)message_ref);
        cJSON_AddStringToObject(data, "message_id", id);
        cJSON *read_by = cJSON_AddArrayToObject(data, "read_by");
        if (read_by != NULL) cJSON_AddItemToArray(read_by, cJSON_CreateString("dotii"));
        emit_message(projection, "read_state", "", data);
        return ESP_OK;
    }
    case 0x0304:
        return ESP_OK;
    case 0x0401:
    case 0x0402:
    case 0x0403:
        if (field_u32(payload, payload_length, CBDP_TLV_THREAD_REF, &thread_ref, true) == ESP_OK) {
            cJSON *thread = ensure_thread(projection->threads, thread_ref);
            if (thread != NULL) {
                if (event_type == 0x0401) set_number(thread, "thread_state", 0);
            }
        }
        return ESP_OK;
    case 0x0704:
        if (apply_summary_event(projection, payload, payload_length)) {
            emit_state(projection);
        }
        return ESP_OK;
    case 0x0603:
    case 0x0604:
    case 0x0605:
    case 0x0701:
    case 0x0702:
    case 0x0703:
    case 0x0801:
    case 0x0802:
    case 0x0803:
    case 0x0804:
    case 0x0805:
    case 0x0806:
    case 0x0807:
    case 0x0808:
    case 0x0901:
    case 0x0902:
    case 0x0903:
    case 0x0904:
    case 0x0905:
    case 0x0A01:
    case 0x0A02:
    case 0x0A03:
    case 0x0A04:
    case 0x0A05:
    case 0x0A06:
    case 0x0C01:
    case 0x0C02:
    case 0x0C03:
    case 0x0C04:
    case 0x0C05:
    case 0x0C06:
    case 0x0C07:
    case 0x0C08:
    case 0x0C09:
    case 0x0C0A:
    case 0x0C0B:
    case 0x0C0C:
    case 0x0C0D:
    case 0x0C0E:
    case 0x0C0F:
    case 0x0C10:
    case 0x0C11:
    case 0x0C12:
    case 0x0C13:
    case 0x0D01:
    case 0x0D02:
    case 0x0D03:
    case 0x0D04:
    case 0x0D05:
    case 0x0D06:
    case 0x0D07:
    case 0x0E04:
    case 0x0E05:
    case 0x0E06:
    case 0x0E07:
    case 0x0E08:
    case 0x0E09:
    case 0x0E0A:
    case 0x0E0B:
    case 0x0E0C:
    case 0x0E0D:
    case 0x0E0E:
        return ESP_OK;
    default:
        return ESP_OK;
    }
}


bool codex_bridge_projection_thread_topic(
    const codex_bridge_projection_t *projection,
    const char *thread_id,
    uint32_t *topic_id
)
{
    if (projection == NULL || thread_id == NULL || topic_id == NULL ||
        thread_id[0] == '\0' || !cJSON_IsArray(projection->threads)) {
        return false;
    }
    cJSON *thread = NULL;
    cJSON_ArrayForEach(thread, projection->threads) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(thread, "thread_id");
        if (!cJSON_IsString(id) || id->valuestring == NULL) continue;
        if (strcmp(id->valuestring, thread_id) != 0) continue;
        cJSON *topic = cJSON_GetObjectItemCaseSensitive(thread, "topic_id");
        if (!cJSON_IsNumber(topic) || topic->valuedouble <= 0) return false;
        *topic_id = (uint32_t)topic->valuedouble;
        return true;
    }
    return false;
}

uint8_t codex_bridge_projection_delta_scope(const codex_bridge_frame_t *frame)
{
    if (frame == NULL || frame->message_type != CBDP_MSG_DELTA) {
        return CBDP_SCOPE_THREAD;
    }
    uint16_t event_type = 0;
    if (field_u16(frame->header_tlv, frame->header_tlv_len,
                  CBDP_TLV_EVENT_TYPE, &event_type, false) != ESP_OK) {
        return CBDP_SCOPE_THREAD;
    }
    /* The Bridge delivers these event types on the summary scope. The cursor
       has to be stored under the same scope that was used to subscribe,
       otherwise every reconnect asks for a full replay again. */
    switch (event_type) {
    case 0x0101:
    case 0x0102:
    case 0x0103:
    case 0x0104:
    case 0x0303:
    case 0x0601:
    case 0x0602:
    case 0x0603:
    case 0x0704:
        return CBDP_SCOPE_SUMMARY;
    default:
        break;
    }
    return CBDP_SCOPE_THREAD;
}

esp_err_t codex_bridge_projection_apply_delta(
    codex_bridge_projection_t *projection,
    const codex_bridge_frame_t *frame,
    uint64_t *cursor
)
{
    if (projection == NULL || frame == NULL || cursor == NULL ||
        frame->message_type != CBDP_MSG_DELTA) {
        return ESP_ERR_INVALID_ARG;
    }

    /* event_type is the only field this firmware requires on DELTA. The
       running Bridge omits topic_seq/global_seq/revision from the outer
       header and carries the payload only, so the remaining fields are
       parsed opportunistically and never cause the frame to be rejected. */
    s_projection_capacity_full = false;
    uint16_t event_type = 0;
    esp_err_t error = field_u16(frame->header_tlv, frame->header_tlv_len,
                                CBDP_TLV_EVENT_TYPE, &event_type, true);
    if (error != ESP_OK) return error;

    uint64_t topic_seq = 0;
    uint64_t global_seq = 0;
    uint64_t revision = 0;
    (void)field_u64(frame->header_tlv, frame->header_tlv_len,
                    CBDP_TLV_TOPIC_SEQ, &topic_seq, false);
    (void)field_u64(frame->header_tlv, frame->header_tlv_len,
                    CBDP_TLV_GLOBAL_SEQ, &global_seq, false);
    (void)field_u64(frame->header_tlv, frame->header_tlv_len,
                    CBDP_TLV_REVISION, &revision, false);

    codex_bridge_tlv_t payload = {0};
    const uint8_t *payload_data = NULL;
    size_t payload_length = 0;
    error = codex_bridge_tlv_find(frame->header_tlv, frame->header_tlv_len,
                                  CBDP_TLV_EVENT_PAYLOAD, &payload);
    if (error == ESP_OK) {
        payload_data = payload.value;
        payload_length = payload.length;
        if (topic_seq == 0) {
            (void)field_u64(payload_data, payload_length, CBDP_TLV_TOPIC_SEQ, &topic_seq, false);
        }
        if (global_seq == 0) {
            (void)field_u64(payload_data, payload_length, CBDP_TLV_GLOBAL_SEQ, &global_seq, false);
        }
        if (revision == 0) {
            (void)field_u64(payload_data, payload_length, CBDP_TLV_REVISION, &revision, false);
        }
    } else if (error != ESP_ERR_NOT_FOUND) {
        return error;
    }

    ESP_LOGI(TAG, "delta ev=0x%04x topic=0x%08lx tseq=%llu gseq=%llu plen=%u",
             (unsigned)event_type, (unsigned long)frame->topic_id,
             (unsigned long long)topic_seq, (unsigned long long)global_seq,
             (unsigned)payload_length);

    /* The per-topic sequence number is not a watermark: the Bridge resends a
       state event with its original sequence numbers whenever the counters
       did not change, and it interleaves other events on the same topic, so a
       "seq <= last_seq" check drops live updates. Only an exact global
       sequence match counts as a duplicate for the regular event types. */
    const bool is_state_event = (event_type == 0x0704);
    if (is_state_event && global_seq != 0 && projection->have_summary_global_seq &&
        global_seq <= projection->summary_global_seq) {
        /* system.codex_state is an aggregated snapshot: only the newest
           global sequence number describes the current counters. The Bridge
           replays its history window in ascending order and that replay
           carries the counter values of long ago. Applying it would make the
           panel counters flicker and would look like a task that just
           finished, so a stale frame is dropped without publishing it. */
        ESP_LOGI(TAG, "delta ev=0x0704 skipped stale gseq=%llu (last=%llu)",
                 (unsigned long long)global_seq,
                 (unsigned long long)projection->summary_global_seq);
        if (global_seq > projection->last_global_seq) {
            projection->last_global_seq = global_seq;
        }
        *cursor = global_seq;
        return ESP_OK;
    }
    if (!is_state_event && seen_global_seq(projection, global_seq)) {
        ESP_LOGI(TAG, "delta ev=0x%04x skipped duplicate gseq=%llu",
                 (unsigned)event_type, (unsigned long long)global_seq);
        if (global_seq > projection->last_global_seq) {
            projection->last_global_seq = global_seq;
        }
        *cursor = global_seq;
        return ESP_OK;
    }

    error = apply_delta_event(projection, event_type, payload_data, payload_length);
    if (error != ESP_OK) return error;

    if (is_state_event && global_seq != 0) {
        /* Remember the newest state snapshot so a later replay of an older
           snapshot can be recognised and dropped. */
        projection->summary_global_seq = global_seq;
        projection->have_summary_global_seq = true;
    }

    projection->last_topic_id = frame->topic_id;
    if (topic_seq != 0) projection->last_topic_seq = topic_seq;
    if (global_seq != 0) {
        remember_global_seq(projection, global_seq);
        if (global_seq > projection->last_global_seq) {
            projection->last_global_seq = global_seq;
        }
    }
    projection->last_revision = revision;
    projection->have_last_event = true;
    *cursor = global_seq;

    if (event_type >= 0x0101 && event_type <= 0x0206) {
        emit_tasks(projection);
        emit_status(projection);
        emit_state(projection);
    } else if (event_type == 0x0301 || event_type == 0x0302) {
        emit_tasks(projection);
        emit_status(projection);
        emit_state(projection);
    }
    return ESP_OK;
}

/* Apply the counters received over the UDP home-counter channel.

   The Bridge sends them every two seconds, no matter whether the numbers
   changed. The change detection below compares against the previous frame,
   so every real change is still seen and the completion chime follows it
   instead of a rare event frame. Only the frames that carry something new
   are published: the panel repaints itself completely when it is touched,
   and repainting it twice per second is what made the device unstable. */
bool codex_bridge_projection_apply_home_counters(
    codex_bridge_projection_t *projection,
    bool have_running,
    uint16_t running,
    bool have_completed,
    uint16_t completed,
    bool have_failed,
    uint16_t failed
)
{
    if (projection == NULL) return false;
    if (!have_running && !have_completed && !have_failed) return false;

    /* The first datagram of a session only establishes the reference
       counters. Publishing it without the baseline flag would look like a
       finished task to the panel. */
    const bool first = !projection->have_home_counters;
    /* A new Bridge instance has to re-establish the reference counters as
       well, even when its numbers are identical to the ones of the dead
       instance. Otherwise nothing is published and the panel keeps showing
       the numbers of a Bridge that no longer exists. */
    const bool resync = projection->home_counters_resync;

    /* The change detection compares against the values of the previous
       home-counter frame. Comparing against the event channel instead made
       the outcome depend on which channel spoke last, and that is exactly
       why the counters froze after a Bridge restart. */
    bool changed = false;
    if (have_running) {
        if (!(projection->home_fields & CBDP_HOME_FIELD_RUNNING) ||
            projection->home_running != running) changed = true;
    }
    if (have_completed) {
        if (!(projection->home_fields & CBDP_HOME_FIELD_COMPLETED) ||
            projection->home_completed_unread != completed) changed = true;
    }
    if (have_failed) {
        if (!(projection->home_fields & CBDP_HOME_FIELD_FAILED) ||
            projection->home_failed_unread != failed) changed = true;
    }

    if (have_running) {
        projection->home_running = running;
        projection->home_fields |= CBDP_HOME_FIELD_RUNNING;
    }
    if (have_completed) {
        projection->home_completed_unread = completed;
        projection->home_fields |= CBDP_HOME_FIELD_COMPLETED;
    }
    if (have_failed) {
        projection->home_failed_unread = failed;
        projection->home_fields |= CBDP_HOME_FIELD_FAILED;
    }
    projection->have_home_counters = true;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    projection->home_counters_ms = now_ms;

    /* The Bridge pushes the counters every two seconds, whether they changed
       or not. Publishing every frame forced a full panel repaint twice per
       second, which starved the SPI transfer of the display and made the
       device restart itself. A frame is therefore only published when it
       carries something new, plus a slow heartbeat so a panel that lost a
       frame converges again. The completion alert still sees every change,
       because the detection above compares against the previous frame and
       not against the published state. */
    const bool baseline = first || resync || projection->catchup;
    projection->home_counters_resync = false;

    const bool heartbeat_due =
        projection->home_counters_emit_ms > 0 &&
        (now_ms - projection->home_counters_emit_ms) >= CBDP_HOME_COUNTERS_HEARTBEAT_MS;
    if (!changed && !baseline && !heartbeat_due) {
        ESP_LOGD(TAG,
                 "home counters unchanged running=%u completed_unread=%u failed_unread=%u",
                 (unsigned)running, (unsigned)completed, (unsigned)failed);
        return false;
    }

    projection->home_counters_emit_ms = now_ms;
    emit_state_ex(projection, baseline);
    ESP_LOGI(TAG,
             "home counters applied running=%u completed_unread=%u failed_unread=%u "
             "changed=%d baseline=%d published=1",
             (unsigned)running, (unsigned)completed, (unsigned)failed,
             changed ? 1 : 0, baseline ? 1 : 0);
    return true;
}





