#include <string.h>

#include "cJSON.h"
#include "codex_snapshot_merge.h"
#include "unity.h"

static cJSON *parse_json(const char *json)
{
    cJSON *value = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(value);
    return value;
}

static codex_snapshot_t *snapshot_for_test(void)
{
    static codex_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    return &snapshot;
}

TEST_CASE("status merges quota fields without erasing management data", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->bambu_configured = true;
    snapshot->bambu_status = BAMBU_STATUS_PRINTING;
    snapshot->custom_enabled = true;
    snapshot->display_revision = 42;
    cJSON *data = parse_json(
        "{\"generated_at_epoch\":123,"
        "\"account\":{\"weekly_remaining_percent\":68,\"weekly_tokens\":900,"
        "\"weekly_tokens_available\":true,\"plan_type\":\"plus\"},"
        "\"task\":{\"status\":\"working\",\"thread_id\":\"main-thread\","
        "\"title\":\"Build\",\"current_action\":\"Working\","
        "\"message_id\":\"task:main-thread\"}}"
    );

    codex_snapshot_merge_status(snapshot, data);

    TEST_ASSERT_EQUAL_INT(68, snapshot->weekly_remaining_percent);
    TEST_ASSERT_EQUAL_UINT(900, snapshot->weekly_tokens);
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_WORKING, snapshot->status);
    TEST_ASSERT_EQUAL_STRING("Build", snapshot->title);
    TEST_ASSERT_EQUAL_STRING("main-thread", snapshot->thread_id);
    TEST_ASSERT_EQUAL_STRING("task:main-thread", snapshot->message_id);
    TEST_ASSERT_TRUE(snapshot->bambu_configured);
    TEST_ASSERT_EQUAL_INT(BAMBU_STATUS_PRINTING, snapshot->bambu_status);
    TEST_ASSERT_TRUE(snapshot->custom_enabled);
    TEST_ASSERT_EQUAL_UINT(42, snapshot->display_revision);
    TEST_ASSERT_TRUE(snapshot->codex_source_online);
    TEST_ASSERT_TRUE(snapshot->source_online);
    cJSON_Delete(data);
}

TEST_CASE("tasks merge ordered details and preserve read state", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    codex_task_detail_t tasks[2] = {0};
    strlcpy(tasks[0].message_id, "task:one", sizeof(tasks[0].message_id));
    tasks[0].read_count = 2;
    cJSON *data = parse_json(
        "{\"tasks\":["
        "{\"thread_id\":\"one\",\"status\":\"waiting_user\",\"title\":\"One\","
        "\"message_id\":\"task:one\"},"
        "{\"thread_id\":\"two\",\"status\":\"completed\",\"title\":\"Two\","
        "\"message_id\":\"task:two\"}]}"
    );
    size_t count = 1;

    codex_snapshot_merge_tasks(snapshot, data, tasks, &count, 2);

    TEST_ASSERT_EQUAL_UINT(2, count);
    TEST_ASSERT_EQUAL_STRING("one", tasks[0].thread_id);
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_WAITING, tasks[0].status);
    TEST_ASSERT_EQUAL_UINT(2, tasks[0].read_count);
    TEST_ASSERT_EQUAL_STRING("two", tasks[1].thread_id);
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_COMPLETED, tasks[1].status);
    TEST_ASSERT_TRUE(snapshot->codex_source_online);
    TEST_ASSERT_TRUE(snapshot->source_online);
    cJSON_Delete(data);
}

TEST_CASE("task reordering preserves read state by message id", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    codex_task_detail_t tasks[2] = {0};
    strlcpy(tasks[0].message_id, "task:one", sizeof(tasks[0].message_id));
    tasks[0].read_count = 2;
    strlcpy(tasks[1].message_id, "task:two", sizeof(tasks[1].message_id));
    tasks[1].read_count = 3;
    cJSON *data = parse_json(
        "{\"tasks\":["
        "{\"thread_id\":\"two\",\"status\":\"completed\",\"title\":\"Two\","
        "\"message_id\":\"task:two\"},"
        "{\"thread_id\":\"one\",\"status\":\"waiting_user\",\"title\":\"One\","
        "\"message_id\":\"task:one\"}]}"
    );
    size_t count = 2;

    codex_snapshot_merge_tasks(snapshot, data, tasks, &count, 2);

    TEST_ASSERT_EQUAL_UINT(3, tasks[0].read_count);
    TEST_ASSERT_EQUAL_UINT(2, tasks[1].read_count);
    cJSON_Delete(data);
}

TEST_CASE("empty task list still marks CodexBridge online", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    codex_task_detail_t tasks[2] = {0};
    cJSON *data = parse_json("{\"tasks\":[]}");
    size_t count = 1;

    codex_snapshot_merge_tasks(snapshot, data, tasks, &count, 2);

    TEST_ASSERT_EQUAL_UINT(0, count);
    TEST_ASSERT_TRUE(snapshot->valid);
    TEST_ASSERT_TRUE(snapshot->codex_source_online);
    TEST_ASSERT_TRUE(snapshot->source_online);
    cJSON_Delete(data);
}

TEST_CASE("result merge fills final response and task identity", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->bambu_connected = true;
    cJSON *data = parse_json(
        "{\"task\":{\"thread_id\":\"done-thread\",\"status\":\"completed\","
        "\"title\":\"Done\",\"last_assistant_message\":\"finished\","
        "\"conversation_text\":\"final answer\","
        "\"message_id\":\"result:done-thread:123\"}}"
    );

    codex_snapshot_merge_result(snapshot, data);

    TEST_ASSERT_EQUAL_STRING("done-thread", snapshot->thread_id);
    TEST_ASSERT_EQUAL_STRING("final answer", snapshot->conversation_text);
    TEST_ASSERT_EQUAL_STRING("result:done-thread:123", snapshot->message_id);
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_COMPLETED, snapshot->status);
    TEST_ASSERT_TRUE(snapshot->bambu_connected);
    cJSON_Delete(data);
}

TEST_CASE("Codex offline does not clear the management snapshot", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->valid = true;
    snapshot->status = CODEX_STATUS_WORKING;
    snapshot->bambu_connected = true;
    snapshot->custom_enabled = true;

    codex_snapshot_apply_source_state(snapshot, false, true);

    TEST_ASSERT_FALSE(snapshot->codex_source_online);
    TEST_ASSERT_TRUE(snapshot->management_source_online);
    TEST_ASSERT_TRUE(snapshot->bambu_connected);
    TEST_ASSERT_TRUE(snapshot->custom_enabled);
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_OFFLINE, snapshot->status);
}

TEST_CASE("management offline keeps the last Codex result", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->valid = true;
    snapshot->status = CODEX_STATUS_COMPLETED;

    codex_snapshot_apply_source_state(snapshot, true, false);

    TEST_ASSERT_TRUE(snapshot->codex_source_online);
    TEST_ASSERT_FALSE(snapshot->management_source_online);
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_COMPLETED, snapshot->status);
}

TEST_CASE("codex working resolves the local working expression", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->status = CODEX_STATUS_WORKING;
    codex_snapshot_refresh_dotii_state(snapshot);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_WORKING, snapshot->dotii_expression);
    TEST_ASSERT_TRUE(snapshot->dotii_state_assigned);
    TEST_ASSERT_EQUAL_UINT(0, snapshot->dotii_state_duration_ms);
}

TEST_CASE("bambu fault overrides idle Codex while Codex failure wins", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->status = CODEX_STATUS_IDLE;
    snapshot->bambu_configured = true;
    snapshot->bambu_connected = true;
    snapshot->bambu_status = BAMBU_STATUS_FAULT;
    codex_snapshot_refresh_dotii_state(snapshot);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_FAILURE, snapshot->dotii_expression);

    snapshot->status = CODEX_STATUS_FAILED;
    codex_snapshot_refresh_dotii_state(snapshot);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_FAILURE, snapshot->dotii_expression);
    TEST_ASSERT_EQUAL_UINT(0, snapshot->dotii_state_duration_ms);
}

TEST_CASE("disconnected Bambu never drives an expression", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->status = CODEX_STATUS_IDLE;
    snapshot->bambu_configured = true;
    snapshot->bambu_connected = false;
    snapshot->bambu_status = BAMBU_STATUS_PRINTING;
    codex_snapshot_refresh_dotii_state(snapshot);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_IDLE_BREATH, snapshot->dotii_expression);
    TEST_ASSERT_FALSE(snapshot->dotii_state_assigned);
}

TEST_CASE("waiting maps to curious and offline maps to connecting", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->status = CODEX_STATUS_WAITING;
    codex_snapshot_refresh_dotii_state(snapshot);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_CURIOUS, snapshot->dotii_expression);

    snapshot->status = CODEX_STATUS_OFFLINE;
    codex_snapshot_refresh_dotii_state(snapshot);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_CONNECTING, snapshot->dotii_expression);
}

TEST_CASE("configured state duration is applied to the mapped expression", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    codex_snapshot_reset_dotii_mapping(snapshot);
    snapshot->dotii_expression_for_state[DOTII_STATE_CODEX_WORKING] =
        DOTII_EXPRESSION_COMPLETE;
    snapshot->dotii_state_duration_for_state[DOTII_STATE_CODEX_WORKING] = 3000;
    snapshot->status = CODEX_STATUS_WORKING;
    codex_snapshot_refresh_dotii_state(snapshot);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_COMPLETE, snapshot->dotii_expression);
    TEST_ASSERT_EQUAL_UINT(3000, snapshot->dotii_state_duration_ms);
}

TEST_CASE("read state updates every matching task detail", "[codex_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    codex_task_detail_t tasks[2] = {0};
    strlcpy(tasks[0].message_id, "task:thread-1", sizeof(tasks[0].message_id));
    strlcpy(tasks[1].message_id, "task:thread-2", sizeof(tasks[1].message_id));
    strlcpy(snapshot->message_id, "task:thread-1", sizeof(snapshot->message_id));
    cJSON *data = parse_json(
        "{\"message_id\":\"task:thread-1\","
        "\"read_by\":[\"dotii-a\",\"dotii-b\"]}"
    );

    codex_snapshot_apply_read_state(snapshot, tasks, 2, data);

    TEST_ASSERT_EQUAL_UINT(2, tasks[0].read_count);
    TEST_ASSERT_EQUAL_UINT(0, tasks[1].read_count);
    TEST_ASSERT_EQUAL_UINT(2, snapshot->read_count);
    cJSON_Delete(data);
}
