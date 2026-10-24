#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "codex_bridge_projection.h"
#include "codex_bridge_protocol.h"
#include "unity.h"

typedef struct {
    int count;
    cJSON *messages[16];
} capture_t;

static void capture_message(const codex_bridge_message_t *message, void *context)
{
    capture_t *capture = (capture_t *)context;
    if (capture == NULL || message == NULL || message->ignored) return;
    if (capture->count >= (int)(sizeof(capture->messages) / sizeof(capture->messages[0]))) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", message->type);
    cJSON_AddStringToObject(root, "kind", message->kind);
    cJSON_AddItemToObject(root, "data", cJSON_Duplicate(message->data, true));
    capture->messages[capture->count++] = root;
}

static void clear_capture(capture_t *capture)
{
    if (capture == NULL) return;
    for (int index = 0; index < capture->count; ++index) {
        cJSON_Delete(capture->messages[index]);
        capture->messages[index] = NULL;
    }
    capture->count = 0;
}

static size_t append_tlv(uint8_t *buffer, size_t offset, uint16_t tag,
                         const void *value, size_t length)
{
    buffer[offset++] = (uint8_t)(tag >> 8);
    buffer[offset++] = (uint8_t)tag;
    buffer[offset++] = (uint8_t)(length >> 8);
    buffer[offset++] = (uint8_t)length;
    if (length > 0) memcpy(buffer + offset, value, length);
    return offset + length;
}

static size_t build_object(uint8_t *buffer, size_t capacity,
                           uint8_t object_type, uint32_t thread_ref,
                           uint32_t message_ref, uint8_t kind, uint8_t status,
                           const char *title, const char *summary)
{
    uint8_t fields[256];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, fields, sizeof(fields));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_OBJECT_TYPE, object_type));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_THREAD_REF, thread_ref));
    if (message_ref != 0) {
        TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_MESSAGE_REF, message_ref));
    }
    if (object_type == 2) {
        TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_KIND, kind));
        TEST_ESP_OK(codex_bridge_tlv_builder_u16(&builder, CBDP_TLV_STATUS, status));
    } else if (status != 0xFF) {
        TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_THREAD_STATE, status));
    }
    if (title != NULL) TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, CBDP_TLV_TITLE, title));
    if (summary != NULL) TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, CBDP_TLV_SUMMARY, summary));
    TEST_ASSERT_TRUE(capacity >= 4 + builder.length);
    const size_t length = append_tlv(buffer, 0, CBDP_TLV_OBJECT, fields, builder.length);
    TEST_ASSERT_TRUE(capacity >= length);
    return length;
}

static size_t build_objects(uint8_t *buffer, size_t capacity,
                            const uint8_t *object, size_t object_length)
{
    TEST_ASSERT_TRUE(capacity >= object_length + 2);
    buffer[0] = 0;
    buffer[1] = 1;
    memcpy(buffer + 2, object, object_length);
    return object_length + 2;
}

static void build_snapshot_frame(uint8_t *frame, size_t capacity, size_t *frame_length,
                                 uint32_t snapshot_id, uint16_t page_index,
                                 uint16_t page_count, bool complete,
                                 const uint8_t *objects, size_t objects_length)
{
    uint8_t header_bytes[512];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&header, CBDP_TLV_SNAPSHOT_ID, snapshot_id));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&header, CBDP_TLV_SNAPSHOT_KIND, 2));
    TEST_ESP_OK(codex_bridge_tlv_builder_u64(&header, CBDP_TLV_SNAPSHOT_REVISION, 10));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&header, CBDP_TLV_PAGE_INDEX, page_index));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&header, CBDP_TLV_PAGE_COUNT, page_count));
    TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_OBJECTS, objects, objects_length));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&header, CBDP_TLV_COMPLETE, complete ? 1 : 0));
    TEST_ESP_OK(codex_bridge_tlv_builder_u64(&header, CBDP_TLV_CURSOR, 100));
    TEST_ESP_OK(codex_bridge_frame_build(
        frame, capacity, CBDP_MSG_SNAPSHOT, CBDP_FLAG_EVENT | CBDP_FLAG_SNAPSHOT_PAGE,
        0x50000003, 1, 0, 0, 1, &header, NULL, 0, frame_length));
}

static void build_delta_frame(uint8_t *frame, size_t capacity, size_t *frame_length,
                              uint16_t event_type, uint32_t topic_seq,
                              const uint8_t *payload, size_t payload_length)
{
    uint8_t header_bytes[512];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    TEST_ESP_OK(codex_bridge_tlv_builder_u64(&header, CBDP_TLV_GLOBAL_SEQ, 200));
    TEST_ESP_OK(codex_bridge_tlv_builder_u64(&header, CBDP_TLV_TOPIC_SEQ, topic_seq));
    TEST_ESP_OK(codex_bridge_tlv_builder_u64(&header, CBDP_TLV_REVISION, 10 + topic_seq));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&header, CBDP_TLV_EVENT_TYPE, event_type));
    if (payload != NULL) {
        TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_EVENT_PAYLOAD, payload, payload_length));
    }
    TEST_ESP_OK(codex_bridge_frame_build(
        frame, capacity, CBDP_MSG_DELTA, CBDP_FLAG_EVENT,
        0x50000003, 2, 0, 0, 0, &header, NULL, 0, frame_length));
}

static codex_bridge_projection_t make_projection(capture_t *capture)
{
    codex_bridge_projection_t projection = {0};
    TEST_ESP_OK(codex_bridge_projection_init(&projection, capture_message, capture));
    return projection;
}

TEST_CASE("CBDP snapshot commits only after final page", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t object[256];
    const size_t object_length = build_object(
        object, sizeof(object), 0, 1, 0, 0, 1, "First", NULL);
    uint8_t objects[260];
    const size_t objects_length = build_objects(objects, sizeof(objects), object, object_length);
    uint8_t frame[1024];
    size_t frame_length = 0;

    build_snapshot_frame(frame, sizeof(frame), &frame_length, 7, 0, 2, false, objects, objects_length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ESP_OK(codex_bridge_projection_apply_snapshot(&projection, &parsed));
    TEST_ASSERT_EQUAL_INT(0, capture.count);

    const size_t second_length = build_object(
        object, sizeof(object), 0, 2, 0, 0, 2, "Second", NULL);
    const size_t second_objects_length = build_objects(objects, sizeof(objects), object, second_length);
    build_snapshot_frame(frame, sizeof(frame), &frame_length, 7, 1, 2, true, objects, second_objects_length);
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ESP_OK(codex_bridge_projection_apply_snapshot(&projection, &parsed));

    TEST_ASSERT_GREATER_THAN_INT(0, capture.count);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(data, "tasks");
    TEST_ASSERT_TRUE(cJSON_IsArray(tasks));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(tasks));
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}


static void build_delta_frame_minimal(uint8_t *frame, size_t capacity,
                                      size_t *frame_length, uint16_t event_type,
                                      uint32_t topic_seq,
                                      const uint8_t *payload,
                                      size_t payload_length)
{
    uint8_t header_bytes[512];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&header, CBDP_TLV_EVENT_TYPE, event_type));
    if (payload != NULL) {
        TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_EVENT_PAYLOAD,
                                                   payload, payload_length));
    }
    (void)topic_seq;
    TEST_ESP_OK(codex_bridge_frame_build(
        frame, capacity, CBDP_MSG_DELTA, CBDP_FLAG_EVENT,
        0, 0, 0, 0, 0, &header, NULL, 0, frame_length));
}

TEST_CASE("CBDP delta without outer seq fields is applied", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t object[256];
    uint8_t objects[512];
    const size_t object_length = build_object(
        object, sizeof(object), 0, 1, 0, 0, 1, "Thread one", NULL);
    const size_t objects_length = build_objects(objects, sizeof(objects), object, object_length);
    uint8_t frame[1024];
    size_t frame_length = 0;
    build_snapshot_frame(frame, sizeof(frame), &frame_length, 20, 0, 1, true,
                         objects, objects_length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ESP_OK(codex_bridge_projection_apply_snapshot(&projection, &parsed));
    clear_capture(&capture);

    uint8_t payload[256];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, payload, sizeof(payload));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_THREAD_REF, 1));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_MESSAGE_REF, 7));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_KIND, 0));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&builder, CBDP_TLV_STATUS, 2));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, CBDP_TLV_SUMMARY, "hi"));
    build_delta_frame_minimal(frame, sizeof(frame), &frame_length, 0x0301, 1,
                              payload, builder.length);
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ASSERT_EQUAL_UINT8(CBDP_MSG_DELTA, parsed.message_type);
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));
    TEST_ASSERT_GREATER_THAN_INT(0, capture.count);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(data, "tasks");
    cJSON *task = cJSON_GetArrayItem(tasks, 0);
    TEST_ASSERT_EQUAL_STRING("hi", cJSON_GetObjectItem(task, "last_user_message")->valuestring);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}


TEST_CASE("CBDP thread topic_id is stored and resumable", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);

    uint8_t fields[256];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, fields, sizeof(fields));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_OBJECT_TYPE, 0));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_THREAD_REF, 42));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, CBDP_TLV_THREAD_ID, "thread-abc"));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_THREAD_STATE, 1));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_TOPIC_ID, 0x1000002A));

    uint8_t object[512];
    size_t object_length = 0;
    object_length = append_tlv(object, 0, CBDP_TLV_OBJECT, fields, builder.length);
    uint8_t objects[520];
    const size_t objects_length = build_objects(objects, sizeof(objects), object, object_length);

    uint8_t frame[1024];
    size_t frame_length = 0;
    build_snapshot_frame(frame, sizeof(frame), &frame_length, 77, 0, 1, true,
                         objects, objects_length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ESP_OK(codex_bridge_projection_apply_snapshot(&projection, &parsed));

    uint32_t topic_id = 0;
    TEST_ASSERT_TRUE(codex_bridge_projection_thread_topic(&projection, "thread-abc", &topic_id));
    TEST_ASSERT_EQUAL_UINT32(0x1000002A, topic_id);
    TEST_ASSERT_FALSE(codex_bridge_projection_thread_topic(&projection, "missing", &topic_id));

    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

TEST_CASE("CBDP thread and message objects emit existing task JSON", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t object[256];
    uint8_t objects[512];
    size_t object_length = build_object(
        object, sizeof(object), 0, 1, 0, 0, 1, "Thread one", NULL);
    size_t objects_length = build_objects(objects, sizeof(objects), object, object_length);
    uint8_t frame[1024];
    size_t frame_length = 0;
    build_snapshot_frame(frame, sizeof(frame), &frame_length, 8, 0, 1, true, objects, objects_length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ESP_OK(codex_bridge_projection_apply_snapshot(&projection, &parsed));
    clear_capture(&capture);

    object_length = build_object(
        object, sizeof(object), 2, 1, 2, 1, 2, NULL, "Build passed");
    objects_length = build_objects(objects, sizeof(objects), object, object_length);
    build_snapshot_frame(frame, sizeof(frame), &frame_length, 9, 0, 1, true, objects, objects_length);
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ESP_OK(codex_bridge_projection_apply_snapshot(&projection, &parsed));

    TEST_ASSERT_GREATER_THAN_INT(0, capture.count);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(data, "tasks");
    TEST_ASSERT_TRUE(cJSON_IsArray(tasks));
    cJSON *task = cJSON_GetArrayItem(tasks, 0);
    TEST_ASSERT_EQUAL_STRING("thread-1", cJSON_GetObjectItem(task, "thread_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("msg-2", cJSON_GetObjectItem(task, "result_message_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("Build passed", cJSON_GetObjectItem(task, "conversation_text")->valuestring);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

TEST_CASE("CBDP message appended delta updates task and advances cursor", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t object[256];
    uint8_t objects[512];
    const size_t object_length = build_object(
        object, sizeof(object), 0, 1, 0, 0, 1, "Thread one", NULL);
    const size_t objects_length = build_objects(objects, sizeof(objects), object, object_length);
    uint8_t frame[1024];
    size_t frame_length = 0;
    build_snapshot_frame(frame, sizeof(frame), &frame_length, 10, 0, 1, true, objects, objects_length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ESP_OK(codex_bridge_projection_apply_snapshot(&projection, &parsed));
    clear_capture(&capture);

    uint8_t payload[256];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, payload, sizeof(payload));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_THREAD_REF, 1));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_MESSAGE_REF, 3));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_KIND, 0));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&builder, CBDP_TLV_STATUS, 2));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, CBDP_TLV_SUMMARY, "hello"));
    build_delta_frame(frame, sizeof(frame), &frame_length, 0x0301, 1, payload, builder.length);
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));
    TEST_ASSERT_TRUE(cursor == 200);
    TEST_ASSERT_GREATER_THAN_INT(0, capture.count);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(data, "tasks");
    cJSON *task = cJSON_GetArrayItem(tasks, 0);
    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetObjectItem(task, "last_user_message")->valuestring);
    TEST_ASSERT_EQUAL_STRING("msg-3", cJSON_GetObjectItem(task, "message_id")->valuestring);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

TEST_CASE("CBDP unknown event is a no-op but advances cursor", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t frame[256];
    size_t frame_length = 0;
    build_delta_frame(frame, sizeof(frame), &frame_length, 0x7777, 1, NULL, 0);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));
    TEST_ASSERT_TRUE(cursor == 200);
    TEST_ASSERT_EQUAL_INT(0, capture.count);
    codex_bridge_projection_reset(&projection);
}

TEST_CASE("CBDP message read delta matches external bridge fields", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t object[256];
    uint8_t objects[512];
    const size_t object_length = build_object(
        object, sizeof(object), 0, 1, 0, 0, 1, "Thread one", NULL);
    const size_t objects_length = build_objects(objects, sizeof(objects), object, object_length);
    uint8_t frame[1024];
    size_t frame_length = 0;
    build_snapshot_frame(frame, sizeof(frame), &frame_length, 11, 0, 1, true, objects, objects_length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ESP_OK(codex_bridge_projection_apply_snapshot(&projection, &parsed));
    clear_capture(&capture);

    uint8_t payload[256];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, payload, sizeof(payload));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_THREAD_REF, 1));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&builder, CBDP_TLV_MESSAGE_REF, 3));
    TEST_ESP_OK(codex_bridge_tlv_builder_u64(&builder, CBDP_TLV_READ_AT_MS, 1234));
    build_delta_frame(frame, sizeof(frame), &frame_length, 0x0303, 2, payload, builder.length);
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));
    TEST_ASSERT_EQUAL_STRING("read_state", capture.messages[0]->child[0].valuestring);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    TEST_ASSERT_EQUAL_STRING("msg-3", cJSON_GetObjectItem(data, "message_id")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItem(data, "read_by")));
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

/* ---- account / rate_limits / token_usage events (0x0E01..0x0E03) ---- */

/* Build the rate_limits array exactly as §6.1.1 specifies: a u16 element count
   followed by count complete element TLVs. */
static size_t build_rate_limit_element(uint8_t *buffer, size_t offset,
                                       const char *window, int remaining_percent,
                                       uint64_t reset_at_ms, bool with_reset)
{
    uint8_t fields[64];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, fields, sizeof(fields));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, CBDP_TLV_RATE_LIMIT_WINDOW, window));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_REMAINING_PERCENT,
                                            (uint8_t)remaining_percent));
    if (with_reset) {
        TEST_ESP_OK(codex_bridge_tlv_builder_u64(&builder, CBDP_TLV_RESET_AT_MS, reset_at_ms));
    }
    return append_tlv(buffer, offset, CBDP_TLV_RATE_LIMITS, fields, builder.length);
}

TEST_CASE("account.changed publishes the plan type", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t payload[128];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, payload, sizeof(payload));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, CBDP_TLV_PLAN_TYPE, "pro"));
    uint8_t frame[512];
    size_t frame_length = 0;
    build_delta_frame_minimal(frame, sizeof(frame), &frame_length,
                              CBDP_EVENT_ACCOUNT_CHANGED, 1, payload, builder.length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));

    TEST_ASSERT_EQUAL_INT(1, capture.count);
    /* cJSON children are a linked list, not an array: index them by name.
       child[1] was an out-of-bounds read and panicked the board. */
    TEST_ASSERT_EQUAL_STRING("message",
        cJSON_GetObjectItem(capture.messages[0], "type")->valuestring);
    TEST_ASSERT_EQUAL_STRING("account",
        cJSON_GetObjectItem(capture.messages[0], "kind")->valuestring);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    TEST_ASSERT_EQUAL_STRING("pro", cJSON_GetObjectItem(data, "plan_type")->valuestring);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

TEST_CASE("rate_limits.changed keeps only the reported windows", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    /* Only the 5-hour window is reported: the weekly window must be absent so
       the panel renders "--" instead of a fabricated value. */
    uint8_t array_value[128];
    size_t array_length = 0;
    array_value[array_length++] = 0;
    array_value[array_length++] = 1;   /* u16 count = 1 */
    array_length = build_rate_limit_element(array_value, array_length,
                                            "five_hour", 42, 1792800000000ULL, true);
    uint8_t payload[192];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, payload, sizeof(payload));
    TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&builder, CBDP_TLV_RATE_LIMITS,
                                               array_value, array_length));
    uint8_t frame[512];
    size_t frame_length = 0;
    build_delta_frame_minimal(frame, sizeof(frame), &frame_length,
                              CBDP_EVENT_RATE_LIMITS_CHANGED, 1, payload, builder.length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));

    TEST_ASSERT_EQUAL_INT(1, capture.count);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    cJSON *windows = cJSON_GetObjectItem(data, "rate_limits");
    TEST_ASSERT_TRUE(cJSON_IsArray(windows));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(windows));
    cJSON *first = cJSON_GetArrayItem(windows, 0);
    TEST_ASSERT_EQUAL_STRING("five_hour", cJSON_GetObjectItem(first, "window")->valuestring);
    /* remaining_percent passes through unchanged: the Bridge already inverted
       usedPercent, so any second inversion here would be a bug. */
    TEST_ASSERT_EQUAL_INT(42, cJSON_GetObjectItem(first, "remaining_percent")->valueint);
    TEST_ASSERT_EQUAL_DOUBLE(1792800000000.0,
                             cJSON_GetObjectItem(first, "reset_at_ms")->valuedouble);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

TEST_CASE("token_usage.changed publishes the seven day total", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t payload[128];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, payload, sizeof(payload));
    TEST_ESP_OK(codex_bridge_tlv_builder_u64(&builder, CBDP_TLV_SEVEN_DAY_TOKENS, 123456));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, CBDP_TLV_SEVEN_DAY_AS_OF, "2026-10-24"));
    uint8_t frame[512];
    size_t frame_length = 0;
    build_delta_frame_minimal(frame, sizeof(frame), &frame_length,
                              CBDP_EVENT_TOKEN_USAGE_CHANGED, 1, payload, builder.length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));

    TEST_ASSERT_EQUAL_INT(1, capture.count);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    cJSON *usage = cJSON_GetObjectItem(data, "token_usage");
    TEST_ASSERT_EQUAL_DOUBLE(123456.0,
                             cJSON_GetObjectItem(usage, "seven_day_tokens")->valuedouble);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

TEST_CASE("account event with no reported field publishes nothing", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    /* Upstream reported nothing: the panel must keep its previous values
       instead of receiving an empty update. */
    uint8_t payload[16];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, payload, sizeof(payload));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_COMPLETE, 1));
    uint8_t frame[512];
    size_t frame_length = 0;
    build_delta_frame_minimal(frame, sizeof(frame), &frame_length,
                              CBDP_EVENT_ACCOUNT_CHANGED, 1, payload, builder.length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));

    TEST_ASSERT_EQUAL_INT(0, capture.count);
    codex_bridge_projection_reset(&projection);
}

TEST_CASE("account token read returns the usage totals", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    /* account.token_usage.read answers with a token_usage container rather
       than the 7-day aggregate. */
    uint8_t usage[64];
    codex_bridge_tlv_builder_t usage_builder = {0};
    codex_bridge_tlv_builder_init(&usage_builder, usage, sizeof(usage));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&usage_builder, CBDP_TLV_TOKEN_TOTAL, 900));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&usage_builder, CBDP_TLV_TOKEN_INPUT, 600));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&usage_builder, CBDP_TLV_TOKEN_OUTPUT, 300));
    uint8_t payload[128];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, payload, sizeof(payload));
    TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&builder, CBDP_TLV_TOKEN_USAGE,
                                               usage, usage_builder.length));
    uint8_t frame[512];
    size_t frame_length = 0;
    build_delta_frame_minimal(frame, sizeof(frame), &frame_length,
                              CBDP_EVENT_TOKEN_USAGE_CHANGED, 1, payload, builder.length);
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    uint64_t cursor = 0;
    TEST_ESP_OK(codex_bridge_projection_apply_delta(&projection, &parsed, &cursor));

    TEST_ASSERT_EQUAL_INT(1, capture.count);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    cJSON *usage_json = cJSON_GetObjectItem(data, "token_usage");
    TEST_ASSERT_EQUAL_INT(900, cJSON_GetObjectItem(usage_json, "token_total")->valueint);
    TEST_ASSERT_EQUAL_INT(600, cJSON_GetObjectItem(usage_json, "token_input")->valueint);
    TEST_ASSERT_EQUAL_INT(300, cJSON_GetObjectItem(usage_json, "token_output")->valueint);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

/* The running Bridge answers account.* reads with the result fields flattened
   into the RESULT header (see tests/test_cbdp_account_read.py, which asserts
   emitted[0].header[TAG_PLAN_TYPE]). The device must accept that exact shape,
   not only a nested result TLV -- getting this wrong is what made every read
   come back INVALID_PARAM on hardware. */
static size_t build_result_header(uint8_t *buffer, size_t capacity,
                                  uint16_t method_id,
                                  const uint8_t *fields, size_t fields_length)
{
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, buffer, capacity);
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&header, CBDP_TLV_REQUEST_ID, 9));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&header, CBDP_TLV_METHOD_ID, method_id));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&header, CBDP_TLV_STATE, 1));
    if (fields_length > 0) {
        memcpy(buffer + header.length, fields, fields_length);
        header.length += fields_length;
    }
    return header.length;
}

TEST_CASE("flattened account read result is decoded", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t fields[64];
    codex_bridge_tlv_builder_t fb = {0};
    codex_bridge_tlv_builder_init(&fb, fields, sizeof(fields));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&fb, CBDP_TLV_PLAN_TYPE, "plus"));

    uint8_t header[128];
    const size_t header_length = build_result_header(header, sizeof(header),
                                                     CBDP_METHOD_ACCOUNT_READ,
                                                     fields, fb.length);
    TEST_ESP_OK(codex_bridge_projection_apply_account_result(
        &projection, CBDP_METHOD_ACCOUNT_READ, header, header_length));

    TEST_ASSERT_EQUAL_INT(1, capture.count);
    cJSON *data = cJSON_GetObjectItem(capture.messages[0], "data");
    TEST_ASSERT_EQUAL_STRING("plus", cJSON_GetObjectItem(data, "plan_type")->valuestring);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

TEST_CASE("flattened rate limits read result is decoded", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    uint8_t elements[128];
    size_t n = 0;
    elements[n++] = 0; elements[n++] = 1;
    n = build_rate_limit_element(elements, n, "weekly", 64, 0, false);
    uint8_t fields[192];
    codex_bridge_tlv_builder_t fb = {0};
    codex_bridge_tlv_builder_init(&fb, fields, sizeof(fields));
    TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&fb, CBDP_TLV_RATE_LIMITS, elements, n));

    uint8_t header[256];
    const size_t header_length = build_result_header(header, sizeof(header),
                                                     CBDP_METHOD_ACCOUNT_RATE_LIMITS_READ,
                                                     fields, fb.length);
    TEST_ESP_OK(codex_bridge_projection_apply_account_result(
        &projection, CBDP_METHOD_ACCOUNT_RATE_LIMITS_READ, header, header_length));

    TEST_ASSERT_EQUAL_INT(1, capture.count);
    cJSON *windows = cJSON_GetObjectItem(capture.messages[0], "data");
    windows = cJSON_GetObjectItem(windows, "rate_limits");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(windows));
    TEST_ASSERT_EQUAL_INT(64,
        cJSON_GetObjectItem(cJSON_GetArrayItem(windows, 0), "remaining_percent")->valueint);
    codex_bridge_projection_reset(&projection);
    clear_capture(&capture);
}

TEST_CASE("empty account read result publishes nothing", "[cbdp_projection]")
{
    capture_t capture = {0};
    codex_bridge_projection_t projection = make_projection(&capture);
    /* The Bridge returns an empty success when it has no committed snapshot
       yet; the device must render "--" and not publish an empty event. */
    uint8_t header[64];
    const size_t header_length = build_result_header(header, sizeof(header),
                                                     CBDP_METHOD_ACCOUNT_READ, NULL, 0);
    TEST_ESP_OK(codex_bridge_projection_apply_account_result(
        &projection, CBDP_METHOD_ACCOUNT_READ, header, header_length));

    TEST_ASSERT_EQUAL_INT(0, capture.count);
    codex_bridge_projection_reset(&projection);
}
