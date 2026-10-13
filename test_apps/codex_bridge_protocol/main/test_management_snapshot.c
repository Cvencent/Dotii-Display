#include <string.h>

#include "cJSON.h"
#include "management_snapshot.h"
#include "codex_snapshot_merge.h"
#include "unity.h"

static codex_snapshot_t *snapshot_for_test(void)
{
    static codex_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    return &snapshot;
}

static cJSON *parse_json(const char *json)
{
    cJSON *value = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(value);
    return value;
}

TEST_CASE("management snapshot without codex preserves CodexBridge state",
          "[management_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->codex_source_online = true;
    snapshot->status = CODEX_STATUS_WORKING;
    snapshot->weekly_remaining_percent = 68;
    strlcpy(snapshot->title, "Build", sizeof(snapshot->title));
    cJSON *data = parse_json(
        "{\"schema_version\":1,"
        "\"modules\":{\"codex\":false,\"bambu\":true,\"dotii\":true},"
        "\"custom\":{\"enabled\":true,\"title\":\"Desk\"},"
        "\"bambu\":{\"configured\":true,\"connected\":true,\"status\":\"printing\"}}"
    );

    TEST_ASSERT_TRUE(management_snapshot_merge(snapshot, data));
    TEST_ASSERT_TRUE(snapshot->management_source_online);
    TEST_ASSERT_TRUE(snapshot->custom_enabled);
    TEST_ASSERT_TRUE(snapshot->bambu_connected);
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_WORKING, snapshot->status);
    TEST_ASSERT_EQUAL_INT(68, snapshot->weekly_remaining_percent);
    TEST_ASSERT_EQUAL_STRING("Build", snapshot->title);
    cJSON_Delete(data);
}

TEST_CASE("management snapshot ignores legacy codex fields",
          "[management_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->codex_source_online = true;
    snapshot->status = CODEX_STATUS_WAITING;
    snapshot->weekly_remaining_percent = 54;
    cJSON *data = parse_json(
        "{\"schema_version\":1,"
        "\"codex\":{\"weekly_remaining_percent\":1,\"task\":{\"status\":\"completed\"}},"
        "\"modules\":{\"bambu\":true}}"
    );

    TEST_ASSERT_TRUE(management_snapshot_merge(snapshot, data));
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_WAITING, snapshot->status);
    TEST_ASSERT_EQUAL_INT(54, snapshot->weekly_remaining_percent);
    cJSON_Delete(data);
}

TEST_CASE("management snapshot resets omitted management fields",
          "[management_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->custom_enabled = true;
    snapshot->custom_image_available = true;
    snapshot->bambu_configured = true;
    snapshot->bambu_connected = true;
    snapshot->bambu_commandable = true;
    snapshot->bambu_camera_available = true;
    snapshot->display_revision = 42;
    cJSON *data = parse_json("{\"schema_version\":1}");

    TEST_ASSERT_TRUE(management_snapshot_merge(snapshot, data));
    TEST_ASSERT_FALSE(snapshot->custom_enabled);
    TEST_ASSERT_FALSE(snapshot->custom_image_available);
    TEST_ASSERT_FALSE(snapshot->bambu_configured);
    TEST_ASSERT_FALSE(snapshot->bambu_connected);
    TEST_ASSERT_FALSE(snapshot->bambu_commandable);
    TEST_ASSERT_FALSE(snapshot->bambu_camera_available);
    TEST_ASSERT_EQUAL_UINT(0, snapshot->display_revision);
    cJSON_Delete(data);
}

TEST_CASE("management snapshot applies mapping without overwriting live expression",
          "[management_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    snapshot->codex_source_online = true;
    snapshot->status = CODEX_STATUS_WORKING;
    codex_snapshot_reset_dotii_mapping(snapshot);
    codex_snapshot_refresh_dotii_state(snapshot);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_WORKING, snapshot->dotii_expression);
    cJSON *data = parse_json(
        "{\"schema_version\":1,"
        "\"dotii\":{\"animations\":{"
        "\"complete\":{\"states\":[\"codex_working\"],\"state_duration_ms\":3000}}}}"
    );

    TEST_ASSERT_TRUE(management_snapshot_merge(snapshot, data));
    /* The mapping moves codex_working to "complete", but the expression stays
       live-driven instead of being forced to idle by the management snapshot. */
    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_WORKING, snapshot->status);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_COMPLETE, snapshot->dotii_expression);
    TEST_ASSERT_EQUAL_UINT(3000, snapshot->dotii_state_duration_ms);
    TEST_ASSERT_TRUE(snapshot->dotii_state_assigned);
    cJSON_Delete(data);
}

TEST_CASE("management apply preserves CodexBridge-derived expression",
          "[management_merge]")
{
    codex_snapshot_t target = {0};
    codex_snapshot_t source = {0};
    target.status = CODEX_STATUS_WORKING;
    codex_snapshot_reset_dotii_mapping(&target);
    codex_snapshot_refresh_dotii_state(&target);
    source.custom_enabled = true;
    codex_snapshot_reset_dotii_mapping(&source);

    management_snapshot_apply(&target, &source);

    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_WORKING, target.status);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_WORKING, target.dotii_expression);
    TEST_ASSERT_TRUE(target.dotii_state_assigned);
    TEST_ASSERT_TRUE(target.custom_enabled);
}

TEST_CASE("management snapshot requires schema one", "[management_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    cJSON *data = parse_json("{\"schema_version\":2}");

    TEST_ASSERT_FALSE(management_snapshot_merge(snapshot, data));
    cJSON_Delete(data);
}
