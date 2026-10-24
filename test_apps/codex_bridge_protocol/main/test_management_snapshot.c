#include <string.h>

#include "cJSON.h"
#include "management_snapshot.h"
#include "codex_snapshot_merge.h"
#include "unity.h"

static codex_snapshot_t *snapshot_for_test(void)
{
    static codex_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    codex_snapshot_reset_dotii_mapping(&snapshot);
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
        "\"modules\":{\"codex\":false,\"xiaozhi\":true,\"dotii\":true},"
        "\"custom\":{\"enabled\":true,\"title\":\"Desk\"},"
        "\"xiaozhi\":{\"server_url\":\"wss://xiaozhi.example\",\"mcp_enabled\":false}}"
    );

    TEST_ASSERT_TRUE(management_snapshot_merge(snapshot, data));
    TEST_ASSERT_TRUE(snapshot->management_source_online);
    TEST_ASSERT_TRUE(snapshot->custom_enabled);
    TEST_ASSERT_EQUAL_STRING("wss://xiaozhi.example", snapshot->xiaozhi_server_url);
    TEST_ASSERT_FALSE(snapshot->xiaozhi_mcp_enabled);
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
        "\"codex\":{\"weekly_remaining_percent\":1,\"task\":{\"status\":\"completed\"}}}"
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
    snapshot->display_revision = 42;
    strlcpy(snapshot->xiaozhi_server_url, "wss://stale.example",
            sizeof(snapshot->xiaozhi_server_url));
    snapshot->xiaozhi_mcp_enabled = false;
    cJSON *data = parse_json("{\"schema_version\":1}");

    TEST_ASSERT_TRUE(management_snapshot_merge(snapshot, data));
    TEST_ASSERT_FALSE(snapshot->custom_enabled);
    TEST_ASSERT_FALSE(snapshot->custom_image_available);
    TEST_ASSERT_EQUAL_UINT(0, snapshot->display_revision);
    TEST_ASSERT_EQUAL_STRING("", snapshot->xiaozhi_server_url);
    TEST_ASSERT_TRUE(snapshot->xiaozhi_mcp_enabled);
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
    static codex_snapshot_t target;
    static codex_snapshot_t source;
    memset(&target, 0, sizeof(target));
    memset(&source, 0, sizeof(source));
    target.status = CODEX_STATUS_WORKING;
    target.running_tasks = 3;
    target.completed_unread_tasks = 2;
    target.failed_unread_tasks = 1;
    codex_snapshot_reset_dotii_mapping(&target);
    codex_snapshot_refresh_dotii_state(&target);
    source.custom_enabled = true;
    strlcpy(source.xiaozhi_server_url, "wss://new.example/xiaozhi/v1/",
            sizeof(source.xiaozhi_server_url));
    source.display_revision = 17;
    source.screen_off_timeout_seconds = 30;
    codex_snapshot_reset_dotii_mapping(&source);

    management_snapshot_apply(&target, &source);

    TEST_ASSERT_EQUAL_INT(CODEX_STATUS_WORKING, target.status);
    TEST_ASSERT_EQUAL_UINT16(3, target.running_tasks);
    TEST_ASSERT_EQUAL_UINT16(2, target.completed_unread_tasks);
    TEST_ASSERT_EQUAL_UINT16(1, target.failed_unread_tasks);
    TEST_ASSERT_EQUAL_INT(DOTII_EXPRESSION_WORKING, target.dotii_expression);
    TEST_ASSERT_TRUE(target.dotii_state_assigned);
    TEST_ASSERT_TRUE(target.custom_enabled);
    TEST_ASSERT_EQUAL_STRING("wss://new.example/xiaozhi/v1/",
                             target.xiaozhi_server_url);
    TEST_ASSERT_EQUAL_UINT(17, target.display_revision);
    TEST_ASSERT_EQUAL_UINT(30, target.screen_off_timeout_seconds);
}

TEST_CASE("management snapshot requires schema one", "[management_merge]")
{
    codex_snapshot_t *snapshot = snapshot_for_test();
    cJSON *data = parse_json("{\"schema_version\":2}");

    TEST_ASSERT_FALSE(management_snapshot_merge(snapshot, data));
    cJSON_Delete(data);
}

/* ---- preview scaffolding must never masquerade as measured data ---- */

TEST_CASE("preview snapshot never claims a measured quota", "[management_merge]")
{
    /* app_state_make_preview() used to hard-code 68%, 18420 tokens and the
       fixed dates "08-18 14:40" / "08-18 10:00" while marking every window
       available. With no Codex account signed in those numbers are pure
       fiction, and the panel rendered them exactly like real usage. */
    codex_snapshot_t snapshot;
    app_state_make_preview(&snapshot);

    TEST_ASSERT_TRUE(snapshot.preview_data);
    TEST_ASSERT_FALSE(snapshot.five_hour_available);
    TEST_ASSERT_FALSE(snapshot.weekly_available);
    TEST_ASSERT_FALSE(snapshot.weekly_tokens_available);
    TEST_ASSERT_FALSE(snapshot.task_tokens_available);
    TEST_ASSERT_EQUAL_STRING("", snapshot.five_hour_reset_date);
    TEST_ASSERT_EQUAL_STRING("", snapshot.reset_date);
    /* The scaffolding the layout needs is still filled in. */
    TEST_ASSERT_TRUE(snapshot.screen_off_timeout_seconds > 0);
    TEST_ASSERT_TRUE(snapshot.character_id[0] != '\0');
}

TEST_CASE("real account data replaces the preview scaffolding", "[management_merge]")
{
    /* The preview used to be sticky: management snapshots never carried quota
       fields, so once 68% was seeded it survived every poll and the panel
       looked like it had live usage. A real account message must take over. */
    codex_snapshot_t *target = snapshot_for_test();
    app_state_make_preview(target);
    TEST_ASSERT_FALSE(target->weekly_available);
    cJSON *data = parse_json(
        "{\"plan_type\":\"plus\",\"rate_limits\":["
        "{\"window\":\"weekly\",\"remaining_percent\":73}],"
        "\"token_usage\":{\"seven_day_tokens\":5000}}");

    codex_snapshot_merge_account(target, data);

    TEST_ASSERT_TRUE(target->weekly_available);
    TEST_ASSERT_EQUAL_INT(73, target->weekly_remaining_percent);
    TEST_ASSERT_EQUAL_UINT(5000, target->weekly_tokens);
    TEST_ASSERT_TRUE(target->weekly_tokens_available);
    TEST_ASSERT_EQUAL_STRING("plus", target->plan_type);
    cJSON_Delete(data);
}
