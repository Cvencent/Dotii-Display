#include <string.h>

#include "cJSON.h"
#include "management_snapshot.h"
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

TEST_CASE("management snapshot requires schema one", "[management_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    cJSON *data = parse_json("{\"schema_version\":2}");

    TEST_ASSERT_FALSE(management_snapshot_merge(snapshot, data));
    cJSON_Delete(data);
}
