#include "management_snapshot.h"
#include "codex_snapshot_merge.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "device_config.h"

#define CUSTOM_IMAGE_BYTES (466 * 466 * 2)

static const char *TAG = "management";

static void copy_json_string(cJSON *parent, const char *name, char *destination,
                             size_t destination_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(destination, item->valuestring, destination_size);
    }
}

static uint32_t parse_custom_color(cJSON *custom, const char *name, uint32_t fallback)
{
    cJSON *accent = cJSON_GetObjectItemCaseSensitive(custom, name);
    if (!cJSON_IsString(accent) || accent->valuestring == NULL ||
        strlen(accent->valuestring) != 7 || accent->valuestring[0] != '#') {
        return fallback;
    }
    for (size_t index = 1; index < 7; index++) {
        if (!isxdigit((unsigned char)accent->valuestring[index])) return fallback;
    }
    return (uint32_t)strtoul(accent->valuestring + 1, NULL, 16);
}

static void reset_custom_config(codex_snapshot_t *snapshot)
{
    snapshot->custom_enabled = false;
    snapshot->custom_image_available = false;
    snapshot->custom_ring_enabled = false;
    snapshot->custom_accent = 0xF2C66D;
    snapshot->custom_ring_start = 0xF2C66D;
    snapshot->custom_ring_end = 0x5DA9FF;
    snapshot->custom_image_revision = 0;
    snapshot->custom_image_size = 0;
    snapshot->custom_title[0] = '\0';
    snapshot->custom_value[0] = '\0';
    snapshot->custom_body[0] = '\0';
    snapshot->custom_footer[0] = '\0';
}

static void copy_custom_config(cJSON *root, codex_snapshot_t *snapshot)
{
    reset_custom_config(snapshot);
    cJSON *custom = cJSON_GetObjectItemCaseSensitive(root, "custom");
    if (!cJSON_IsObject(custom)) return;

    snapshot->custom_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(custom, "enabled")
    );
    snapshot->custom_accent = parse_custom_color(custom, "accent", 0xF2C66D);
    snapshot->custom_ring_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(custom, "ring_enabled")
    );
    snapshot->custom_ring_start = parse_custom_color(
        custom, "ring_start", snapshot->custom_accent
    );
    snapshot->custom_ring_end = parse_custom_color(
        custom, "ring_end", snapshot->custom_ring_start
    );
    copy_json_string(custom, "title", snapshot->custom_title, sizeof(snapshot->custom_title));
    copy_json_string(custom, "value", snapshot->custom_value, sizeof(snapshot->custom_value));
    copy_json_string(custom, "body", snapshot->custom_body, sizeof(snapshot->custom_body));
    copy_json_string(custom, "footer", snapshot->custom_footer, sizeof(snapshot->custom_footer));

    cJSON *image_available = cJSON_GetObjectItemCaseSensitive(custom, "image_available");
    cJSON *image_revision = cJSON_GetObjectItemCaseSensitive(custom, "image_revision");
    cJSON *image_size = cJSON_GetObjectItemCaseSensitive(custom, "image_size");
    if (cJSON_IsTrue(image_available) && cJSON_IsNumber(image_revision) &&
        cJSON_IsNumber(image_size) && image_size->valuedouble == CUSTOM_IMAGE_BYTES) {
        snapshot->custom_image_available = true;
        snapshot->custom_image_revision = (uint32_t)image_revision->valuedouble;
        snapshot->custom_image_size = (uint32_t)image_size->valuedouble;
    }
}

static void copy_xiaozhi_config(cJSON *root, codex_snapshot_t *snapshot)
{
    snapshot->xiaozhi_server_url[0] = '\0';
    snapshot->xiaozhi_wake_word[0] = '\0';
    snapshot->xiaozhi_voice[0] = '\0';
    snapshot->xiaozhi_mcp_enabled = true;
    snapshot->xiaozhi_mcp_url[0] = '\0';
    cJSON *xiaozhi = cJSON_GetObjectItemCaseSensitive(root, "xiaozhi");
    if (!cJSON_IsObject(xiaozhi)) return;
    copy_json_string(xiaozhi, "server_url", snapshot->xiaozhi_server_url,
                     sizeof(snapshot->xiaozhi_server_url));
    copy_json_string(xiaozhi, "wake_word", snapshot->xiaozhi_wake_word,
                     sizeof(snapshot->xiaozhi_wake_word));
    copy_json_string(xiaozhi, "voice", snapshot->xiaozhi_voice,
                     sizeof(snapshot->xiaozhi_voice));
    cJSON *mcp = cJSON_GetObjectItemCaseSensitive(xiaozhi, "mcp_enabled");
    if (cJSON_IsBool(mcp)) snapshot->xiaozhi_mcp_enabled = cJSON_IsTrue(mcp);
    copy_json_string(xiaozhi, "mcp_url", snapshot->xiaozhi_mcp_url,
                     sizeof(snapshot->xiaozhi_mcp_url));
    ESP_LOGI("snapshot", "xiaozhi from json: mcp=%d mcp_url=%s",
             (int)snapshot->xiaozhi_mcp_enabled,
             snapshot->xiaozhi_mcp_url[0] ? "set" : "EMPTY");
}

static void copy_qqmusic_config(cJSON *root, codex_snapshot_t *snapshot)
{
    snapshot->qqmusic_logged_in = false;
    snapshot->qqmusic_cookie[0] = '\0';
    snapshot->qqmusic_nickname[0] = '\0';
    cJSON *qq = cJSON_GetObjectItemCaseSensitive(root, "qqmusic");
    if (!cJSON_IsObject(qq)) return;
    cJSON *logged = cJSON_GetObjectItemCaseSensitive(qq, "logged_in");
    if (cJSON_IsBool(logged)) snapshot->qqmusic_logged_in = cJSON_IsTrue(logged);
    copy_json_string(qq, "cookies", snapshot->qqmusic_cookie,
                     sizeof(snapshot->qqmusic_cookie));
    ESP_LOGI("snapshot", "qqmusic from json: logged_in=%d cookie_len=%u",
             (int)snapshot->qqmusic_logged_in,
             (unsigned)strlen(snapshot->qqmusic_cookie));
    /* Persist the scanned session right here: this parser runs on every poll,
       whereas the UI config hook only fires once at boot. */
    if (snapshot->qqmusic_cookie[0] != '\0' &&
        strcmp(device_config_get()->qqmusic_cookie, snapshot->qqmusic_cookie) != 0) {
        if (device_config_store_qq_cookie(snapshot->qqmusic_cookie) == ESP_OK) {
            ESP_LOGI("snapshot", "qqmusic cookie stored (%u bytes)",
                     (unsigned)strlen(snapshot->qqmusic_cookie));
        } else {
            ESP_LOGW("snapshot", "qqmusic cookie store failed");
        }
    }
    copy_json_string(qq, "nickname", snapshot->qqmusic_nickname,
                     sizeof(snapshot->qqmusic_nickname));
}

static void copy_module_config(cJSON *root, codex_snapshot_t *snapshot)
{
    /* Codex capabilities are owned by CodexBridge. The retired custom and
       standalone Dotii pages stay disabled even if an older management
       snapshot still contains their flags. */
    snapshot->codex_enabled = true;
    snapshot->xiaozhi_enabled = true;
    snapshot->dotii_enabled = false;
    snapshot->custom_enabled = false;
    cJSON *modules = cJSON_GetObjectItemCaseSensitive(root, "modules");
    if (!cJSON_IsObject(modules)) return;

    cJSON *xiaozhi = cJSON_GetObjectItemCaseSensitive(modules, "xiaozhi");
    if (cJSON_IsBool(xiaozhi)) snapshot->xiaozhi_enabled = cJSON_IsTrue(xiaozhi);
}

static uint32_t dotii_state_token_from_string(const char *value)
{
    uint32_t hash = 2166136261U;
    if (value == NULL) return hash;
    while (*value != '\0') {
        hash ^= (uint8_t)*value++;
        hash *= 16777619U;
    }
    return hash;
}

static void reset_dotii_state(codex_snapshot_t *snapshot)
{
    snapshot->dotii_expression = DOTII_EXPRESSION_IDLE_BREATH;
    snapshot->dotii_base_idle = true;
    snapshot->dotii_state_assigned = false;
    snapshot->dotii_state_duration_ms = 0;
    snapshot->dotii_state_token = dotii_state_token_from_string("idle");
    snapshot->dotii_touch_expression = DOTII_EXPRESSION_TOUCH_RESPONSE;
    snapshot->dotii_blink_expression = DOTII_EXPRESSION_BLINK;
    snapshot->dotii_long_idle_expression = DOTII_EXPRESSION_SLEEPY_YAWN;
    for (size_t index = 0; index < DOTII_EXPRESSION_COUNT; index++) {
        snapshot->dotii_expression_durations_ms[index] = 800;
    }
    snapshot->dotii_expression_durations_ms[DOTII_EXPRESSION_TOUCH_RESPONSE] = 1200;
    codex_snapshot_reset_dotii_mapping(snapshot);
}

static int dotii_business_state_index(const char *state_id)
{
    if (state_id == NULL) return -1;
    if (strcmp(state_id, "codex_waiting_user") == 0) return DOTII_STATE_CODEX_WAITING_USER;
    if (strcmp(state_id, "codex_working") == 0) return DOTII_STATE_CODEX_WORKING;
    if (strcmp(state_id, "codex_completed") == 0) return DOTII_STATE_CODEX_COMPLETED;
    if (strcmp(state_id, "codex_failure") == 0) return DOTII_STATE_CODEX_FAILURE;
    return -1;
}

static void copy_dotii_animation_map(cJSON *dotii, codex_snapshot_t *snapshot)
{
    cJSON *animations = cJSON_GetObjectItemCaseSensitive(dotii, "animations");
    if (!cJSON_IsObject(animations)) return;
    cJSON *animation = NULL;
    cJSON_ArrayForEach(animation, animations) {
        if (!cJSON_IsObject(animation) || animation->string == NULL) continue;
        dotii_expression_t expression = app_state_dotii_expression_from_string(
            animation->string
        );
        cJSON *states = cJSON_GetObjectItemCaseSensitive(animation, "states");
        if (!cJSON_IsArray(states)) continue;
        uint16_t duration_ms = 0;
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(animation, "state_duration_ms");
        if (cJSON_IsNumber(duration)) {
            const int value = duration->valueint;
            if (value == 0 || value == 1000 || value == 3000 || value == 5000 ||
                value == 30000) {
                duration_ms = (uint16_t)value;
            }
        }
        cJSON *state = NULL;
        cJSON_ArrayForEach(state, states) {
            const int index = dotii_business_state_index(
                cJSON_IsString(state) ? state->valuestring : NULL
            );
            if (index < 0) continue;
            snapshot->dotii_expression_for_state[index] = expression;
            snapshot->dotii_state_duration_for_state[index] = duration_ms;
        }
    }
}

static void copy_dotii_fixed_states(cJSON *dotii, codex_snapshot_t *snapshot)
{
    cJSON *fixed_states = cJSON_GetObjectItemCaseSensitive(dotii, "fixed_states");
    if (!cJSON_IsArray(fixed_states)) return;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, fixed_states) {
        if (!cJSON_IsObject(item)) continue;
        cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON *expression = cJSON_GetObjectItemCaseSensitive(item, "expression");
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(item, "duration_ms");
        if (!cJSON_IsString(id) || id->valuestring == NULL ||
            !cJSON_IsString(expression) || expression->valuestring == NULL) {
            continue;
        }
        dotii_expression_t parsed = app_state_dotii_expression_from_string(
            expression->valuestring
        );
        uint16_t duration_ms = 800;
        if (cJSON_IsNumber(duration) && duration->valueint >= 800 &&
            duration->valueint <= 10000) {
            duration_ms = (uint16_t)duration->valueint;
        }
        if (strcmp(id->valuestring, "touch") == 0) {
            snapshot->dotii_touch_expression = parsed;
            snapshot->dotii_expression_durations_ms[parsed] = duration_ms;
        } else if (strcmp(id->valuestring, "blink") == 0) {
            snapshot->dotii_blink_expression = parsed;
            snapshot->dotii_expression_durations_ms[parsed] = duration_ms;
        } else if (strcmp(id->valuestring, "long_idle") == 0) {
            snapshot->dotii_long_idle_expression = parsed;
            snapshot->dotii_expression_durations_ms[parsed] = duration_ms;
        } else if (strcmp(id->valuestring, "connecting") == 0) {
            snapshot->dotii_expression_durations_ms[DOTII_EXPRESSION_CONNECTING] = duration_ms;
        } else if (strcmp(id->valuestring, "idle") == 0) {
            snapshot->dotii_expression_durations_ms[DOTII_EXPRESSION_IDLE_BREATH] = duration_ms;
        }
    }
}

static void copy_dotii_state(cJSON *root, codex_snapshot_t *snapshot)
{
    reset_dotii_state(snapshot);
    cJSON *character = cJSON_GetObjectItemCaseSensitive(root, "character");
    if (cJSON_IsString(character) && character->valuestring != NULL) {
        strlcpy(snapshot->character_id, character->valuestring, sizeof(snapshot->character_id));
    }
    cJSON *dotii = cJSON_GetObjectItemCaseSensitive(root, "dotii");
    if (!cJSON_IsObject(dotii)) {
        codex_snapshot_refresh_dotii_state(snapshot);
        return;
    }

    copy_dotii_animation_map(dotii, snapshot);
    copy_dotii_fixed_states(dotii, snapshot);
    /* The management snapshot only carries the mapping. The live expression is
       resolved locally from CodexBridge and the xiaozhi session so this merge
       cannot
       overwrite CodexBridge-driven state. */
    codex_snapshot_refresh_dotii_state(snapshot);
}

static void reset_display_config(codex_snapshot_t *snapshot)
{
    snapshot->codex_ui_dual_limit = false;
    snapshot->display_revision = 0;
    snapshot->docked_rotation_tenths = 900;
    snapshot->screen_off_timeout_seconds = 60;
    snapshot->sleep_timeout_seconds = 300;
    snapshot->charging_screen_off_timeout_seconds = 60;
    snapshot->charging_sleep_timeout_seconds = 300;
    snapshot->screen_off_page = DISPLAY_SCREEN_OFF_PAGE_NONE;
}

static void copy_display_config(cJSON *root, codex_snapshot_t *snapshot)
{
    reset_display_config(snapshot);
    cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (!cJSON_IsObject(display)) return;

    cJSON *revision = cJSON_GetObjectItemCaseSensitive(display, "revision");
    cJSON *codex_ui = cJSON_GetObjectItemCaseSensitive(display, "codex_ui");
    cJSON *angle = cJSON_GetObjectItemCaseSensitive(display, "docked_rotation_tenths");
    cJSON *screen_off_timeout =
        cJSON_GetObjectItemCaseSensitive(display, "screen_off_timeout_seconds");
    cJSON *sleep_timeout =
        cJSON_GetObjectItemCaseSensitive(display, "sleep_timeout_seconds");
    cJSON *charging_screen_off_timeout =
        cJSON_GetObjectItemCaseSensitive(display, "charging_screen_off_timeout_seconds");
    cJSON *charging_sleep_timeout =
        cJSON_GetObjectItemCaseSensitive(display, "charging_sleep_timeout_seconds");
    cJSON *screen_off_page = cJSON_GetObjectItemCaseSensitive(display, "screen_off_page");

    if (cJSON_IsNumber(revision) && revision->valuedouble >= 0) {
        snapshot->display_revision = (uint32_t)revision->valuedouble;
    }
    if (cJSON_IsString(codex_ui) && codex_ui->valuestring != NULL) {
        snapshot->codex_ui_dual_limit = strcmp(codex_ui->valuestring, "dual_limit") == 0;
    }
    if (cJSON_IsNumber(angle) && angle->valueint >= 800 && angle->valueint <= 1000) {
        snapshot->docked_rotation_tenths = (int16_t)angle->valueint;
    }
    if (cJSON_IsNumber(screen_off_timeout) && screen_off_timeout->valuedouble >= 0 &&
        screen_off_timeout->valuedouble <= 86400) {
        snapshot->screen_off_timeout_seconds = (uint32_t)screen_off_timeout->valuedouble;
    }
    if (cJSON_IsNumber(sleep_timeout) && sleep_timeout->valuedouble >= 0 &&
        sleep_timeout->valuedouble <= 86400) {
        snapshot->sleep_timeout_seconds = (uint32_t)sleep_timeout->valuedouble;
    }
    snapshot->charging_screen_off_timeout_seconds = snapshot->screen_off_timeout_seconds;
    snapshot->charging_sleep_timeout_seconds = snapshot->sleep_timeout_seconds;
    if (cJSON_IsNumber(charging_screen_off_timeout) &&
        charging_screen_off_timeout->valuedouble >= 0 &&
        charging_screen_off_timeout->valuedouble <= 86400) {
        snapshot->charging_screen_off_timeout_seconds =
            (uint32_t)charging_screen_off_timeout->valuedouble;
    }
    if (cJSON_IsNumber(charging_sleep_timeout) &&
        charging_sleep_timeout->valuedouble >= 0 &&
        charging_sleep_timeout->valuedouble <= 86400) {
        snapshot->charging_sleep_timeout_seconds =
            (uint32_t)charging_sleep_timeout->valuedouble;
    }
    if (cJSON_IsString(screen_off_page) && screen_off_page->valuestring != NULL) {
        if (strcmp(screen_off_page->valuestring, "custom") == 0) {
            snapshot->screen_off_page = DISPLAY_SCREEN_OFF_PAGE_CUSTOM;
        } else if (strcmp(screen_off_page->valuestring, "dotii") == 0) {
            snapshot->screen_off_page = DISPLAY_SCREEN_OFF_PAGE_DOTII;
        }
    }
    if ((snapshot->screen_off_timeout_seconds == 0 && snapshot->sleep_timeout_seconds != 0) ||
        (snapshot->screen_off_timeout_seconds != 0 && snapshot->sleep_timeout_seconds != 0 &&
         snapshot->sleep_timeout_seconds < snapshot->screen_off_timeout_seconds)) {
        ESP_LOGW(TAG, "Ignoring invalid display timeout order: off=%lu sleep=%lu",
                 (unsigned long)snapshot->screen_off_timeout_seconds,
                 (unsigned long)snapshot->sleep_timeout_seconds);
        snapshot->screen_off_timeout_seconds = 60;
        snapshot->sleep_timeout_seconds = 300;
    }
    if ((snapshot->charging_screen_off_timeout_seconds == 0 &&
         snapshot->charging_sleep_timeout_seconds != 0) ||
        (snapshot->charging_screen_off_timeout_seconds != 0 &&
         snapshot->charging_sleep_timeout_seconds != 0 &&
         snapshot->charging_sleep_timeout_seconds <
             snapshot->charging_screen_off_timeout_seconds)) {
        ESP_LOGW(TAG, "Ignoring invalid charging timeout order: off=%lu sleep=%lu",
                 (unsigned long)snapshot->charging_screen_off_timeout_seconds,
                 (unsigned long)snapshot->charging_sleep_timeout_seconds);
        snapshot->charging_screen_off_timeout_seconds = snapshot->screen_off_timeout_seconds;
        snapshot->charging_sleep_timeout_seconds = snapshot->sleep_timeout_seconds;
    }
}

bool management_snapshot_merge(codex_snapshot_t *target, const cJSON *root)
{
    if (target == NULL || !cJSON_IsObject(root)) return false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(version) || version->valueint != 1) return false;

    target->valid = true;
    target->stale = false;
    target->preview_data = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(root, "preview_data")
    );
    copy_module_config((cJSON *)root, target);
    copy_xiaozhi_config((cJSON *)root, target);
    copy_qqmusic_config((cJSON *)root, target);
    copy_display_config((cJSON *)root, target);
    copy_custom_config((cJSON *)root, target);
    copy_dotii_state((cJSON *)root, target);
    target->management_source_online = true;
    target->source_online = target->codex_source_online || target->management_source_online;
    return true;
}

bool management_snapshot_parse(const char *json, codex_snapshot_t *target)
{
    if (json == NULL || target == NULL) return false;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return false;
    bool merged = management_snapshot_merge(target, root);
    cJSON_Delete(root);
    return merged;
}

void management_snapshot_apply(codex_snapshot_t *target, const codex_snapshot_t *source)
{
    if (target == NULL || source == NULL) return;
    target->valid = target->valid || source->valid;
    target->preview_data = source->preview_data;
    target->stale = false;
    target->codex_enabled = source->codex_enabled;
    target->xiaozhi_enabled = source->xiaozhi_enabled;
    memcpy(target->xiaozhi_server_url, source->xiaozhi_server_url,
           sizeof(target->xiaozhi_server_url));
    memcpy(target->xiaozhi_wake_word, source->xiaozhi_wake_word,
           sizeof(target->xiaozhi_wake_word));
    memcpy(target->xiaozhi_voice, source->xiaozhi_voice,
           sizeof(target->xiaozhi_voice));
    target->xiaozhi_mcp_enabled = source->xiaozhi_mcp_enabled;
    memcpy(target->xiaozhi_mcp_url, source->xiaozhi_mcp_url,
           sizeof(target->xiaozhi_mcp_url));
    target->custom_enabled = source->custom_enabled;
    target->dotii_enabled = source->dotii_enabled;
    target->custom_image_available = source->custom_image_available;
    target->custom_ring_enabled = source->custom_ring_enabled;
    target->custom_accent = source->custom_accent;
    target->custom_ring_start = source->custom_ring_start;
    target->custom_ring_end = source->custom_ring_end;
    target->custom_image_revision = source->custom_image_revision;
    target->custom_image_size = source->custom_image_size;
    memcpy(target->custom_title, source->custom_title, sizeof(target->custom_title));
    memcpy(target->custom_value, source->custom_value, sizeof(target->custom_value));
    memcpy(target->custom_body, source->custom_body, sizeof(target->custom_body));
    memcpy(target->custom_footer, source->custom_footer, sizeof(target->custom_footer));
    target->display_revision = source->display_revision;
    target->codex_ui_dual_limit = source->codex_ui_dual_limit;
    target->docked_rotation_tenths = source->docked_rotation_tenths;
    target->screen_off_timeout_seconds = source->screen_off_timeout_seconds;
    target->sleep_timeout_seconds = source->sleep_timeout_seconds;
    target->charging_screen_off_timeout_seconds = source->charging_screen_off_timeout_seconds;
    target->charging_sleep_timeout_seconds = source->charging_sleep_timeout_seconds;
    target->screen_off_page = source->screen_off_page;
    /* CodexBridge owns the live expression; the management snapshot only
       supplies the state mapping and local animation configuration. */
    target->dotii_touch_expression = source->dotii_touch_expression;
    target->dotii_blink_expression = source->dotii_blink_expression;
    target->dotii_long_idle_expression = source->dotii_long_idle_expression;
    memcpy(target->dotii_expression_durations_ms, source->dotii_expression_durations_ms,
           sizeof(target->dotii_expression_durations_ms));
    memcpy(target->dotii_expression_for_state, source->dotii_expression_for_state,
           sizeof(target->dotii_expression_for_state));
    memcpy(target->dotii_state_duration_for_state, source->dotii_state_duration_for_state,
           sizeof(target->dotii_state_duration_for_state));
    codex_snapshot_refresh_dotii_state(target);
    target->management_source_online = source->management_source_online;
    target->source_online = target->codex_source_online || target->management_source_online;
}
