#include "management_snapshot.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define CUSTOM_IMAGE_BYTES (466 * 466 * 2)
#define BAMBU_CAMERA_BYTES (320 * 180 * 2)

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

static void copy_module_config(cJSON *root, codex_snapshot_t *snapshot)
{
    /* Codex capabilities are owned by CodexBridge, not the management center. */
    snapshot->codex_enabled = true;
    snapshot->bambu_enabled = true;
    snapshot->dotii_enabled = true;
    cJSON *modules = cJSON_GetObjectItemCaseSensitive(root, "modules");
    if (!cJSON_IsObject(modules)) return;

    cJSON *bambu = cJSON_GetObjectItemCaseSensitive(modules, "bambu");
    cJSON *dotii = cJSON_GetObjectItemCaseSensitive(modules, "dotii");
    if (cJSON_IsBool(bambu)) snapshot->bambu_enabled = cJSON_IsTrue(bambu);
    if (cJSON_IsBool(dotii)) snapshot->dotii_enabled = cJSON_IsTrue(dotii);
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
}

static void copy_dotii_state(cJSON *root, codex_snapshot_t *snapshot)
{
    reset_dotii_state(snapshot);
    cJSON *dotii = cJSON_GetObjectItemCaseSensitive(root, "dotii");
    if (!cJSON_IsObject(dotii)) return;

    cJSON *expression = cJSON_GetObjectItemCaseSensitive(dotii, "expression");
    if (cJSON_IsString(expression) && expression->valuestring != NULL) {
        snapshot->dotii_expression = app_state_dotii_expression_from_string(
            expression->valuestring
        );
        snapshot->dotii_base_idle =
            snapshot->dotii_expression == DOTII_EXPRESSION_IDLE_BREATH;
        snapshot->dotii_state_assigned = !snapshot->dotii_base_idle;
    }
    cJSON *state = cJSON_GetObjectItemCaseSensitive(dotii, "state");
    if (cJSON_IsString(state) && state->valuestring != NULL) {
        snapshot->dotii_base_idle = strcmp(state->valuestring, "idle") == 0;
        snapshot->dotii_state_token = dotii_state_token_from_string(state->valuestring);
    }
    cJSON *assigned = cJSON_GetObjectItemCaseSensitive(dotii, "state_assigned");
    if (cJSON_IsBool(assigned)) snapshot->dotii_state_assigned = cJSON_IsTrue(assigned);
    if (!snapshot->dotii_state_assigned) {
        snapshot->dotii_expression = DOTII_EXPRESSION_IDLE_BREATH;
        snapshot->dotii_base_idle = true;
    }
    cJSON *state_duration = cJSON_GetObjectItemCaseSensitive(dotii, "state_duration_ms");
    if (cJSON_IsNumber(state_duration)) {
        const int duration = state_duration->valueint;
        if (duration == 0 || duration == 1000 || duration == 3000 ||
            duration == 5000 || duration == 30000) {
            snapshot->dotii_state_duration_ms = (uint32_t)duration;
        }
    }
    cJSON *state_token = cJSON_GetObjectItemCaseSensitive(dotii, "state_token");
    if (cJSON_IsNumber(state_token) && state_token->valuedouble >= 0 &&
        state_token->valuedouble <= UINT32_MAX) {
        snapshot->dotii_state_token = (uint32_t)state_token->valuedouble;
    }
}

static void reset_display_config(codex_snapshot_t *snapshot)
{
    snapshot->codex_ui_dual_limit = false;
    snapshot->display_revision = 0;
    snapshot->docked_rotation_tenths = 840;
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

static void reset_bambu_status(codex_snapshot_t *snapshot)
{
    snapshot->bambu_configured = false;
    snapshot->bambu_connected = false;
    snapshot->bambu_commandable = false;
    snapshot->bambu_camera_available = false;
    snapshot->bambu_camera_revision = 0;
    snapshot->bambu_camera_size = 0;
    snapshot->bambu_status = BAMBU_STATUS_OFFLINE;
    snapshot->bambu_progress = 0;
    snapshot->bambu_remaining_minutes = 0;
    snapshot->bambu_layer_current = 0;
    snapshot->bambu_layer_total = 0;
    snapshot->bambu_nozzle_temperature = 0;
    snapshot->bambu_bed_temperature = 0;
    snapshot->bambu_finish_at = 0;
    snapshot->bambu_updated_at = 0;
    strlcpy(snapshot->bambu_status_text, "离线", sizeof(snapshot->bambu_status_text));
    snapshot->bambu_name[0] = '\0';
    snapshot->bambu_filename[0] = '\0';
    snapshot->bambu_filament[0] = '\0';
}

static void copy_bambu_status(cJSON *root, codex_snapshot_t *snapshot)
{
    reset_bambu_status(snapshot);
    cJSON *bambu = cJSON_GetObjectItemCaseSensitive(root, "bambu");
    if (!cJSON_IsObject(bambu)) return;

    snapshot->bambu_configured = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(bambu, "configured")
    );
    snapshot->bambu_connected = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(bambu, "connected")
    );
    snapshot->bambu_commandable = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(bambu, "commandable")
    );
    copy_json_string(bambu, "name", snapshot->bambu_name, sizeof(snapshot->bambu_name));
    copy_json_string(bambu, "status_text", snapshot->bambu_status_text,
                     sizeof(snapshot->bambu_status_text));
    copy_json_string(bambu, "filename", snapshot->bambu_filename,
                     sizeof(snapshot->bambu_filename));
    copy_json_string(bambu, "filament", snapshot->bambu_filament,
                     sizeof(snapshot->bambu_filament));
    cJSON *status = cJSON_GetObjectItemCaseSensitive(bambu, "status");
    snapshot->bambu_status = app_state_bambu_status_from_string(
        cJSON_IsString(status) ? status->valuestring : "offline"
    );
    cJSON *progress = cJSON_GetObjectItemCaseSensitive(bambu, "progress");
    cJSON *remaining = cJSON_GetObjectItemCaseSensitive(bambu, "remaining_minutes");
    cJSON *finish = cJSON_GetObjectItemCaseSensitive(bambu, "finish_epoch");
    cJSON *updated = cJSON_GetObjectItemCaseSensitive(bambu, "updated_at_epoch");
    cJSON *layer_current = cJSON_GetObjectItemCaseSensitive(bambu, "layer_current");
    cJSON *layer_total = cJSON_GetObjectItemCaseSensitive(bambu, "layer_total");
    cJSON *nozzle = cJSON_GetObjectItemCaseSensitive(bambu, "nozzle_temperature");
    cJSON *bed = cJSON_GetObjectItemCaseSensitive(bambu, "bed_temperature");
    snapshot->bambu_progress = cJSON_IsNumber(progress) ? (uint8_t)progress->valueint : 0;
    snapshot->bambu_remaining_minutes =
        cJSON_IsNumber(remaining) ? (uint16_t)remaining->valueint : 0;
    snapshot->bambu_finish_at = cJSON_IsNumber(finish) ? (time_t)finish->valuedouble : 0;
    snapshot->bambu_updated_at = cJSON_IsNumber(updated) ? (time_t)updated->valuedouble : 0;
    snapshot->bambu_layer_current =
        cJSON_IsNumber(layer_current) ? (uint16_t)layer_current->valueint : 0;
    snapshot->bambu_layer_total =
        cJSON_IsNumber(layer_total) ? (uint16_t)layer_total->valueint : 0;
    snapshot->bambu_nozzle_temperature =
        cJSON_IsNumber(nozzle) ? (int16_t)nozzle->valuedouble : 0;
    snapshot->bambu_bed_temperature =
        cJSON_IsNumber(bed) ? (int16_t)bed->valuedouble : 0;

    cJSON *camera_available = cJSON_GetObjectItemCaseSensitive(bambu, "camera_available");
    cJSON *camera_revision = cJSON_GetObjectItemCaseSensitive(bambu, "camera_revision");
    cJSON *camera_size = cJSON_GetObjectItemCaseSensitive(bambu, "camera_size");
    if (cJSON_IsTrue(camera_available) && cJSON_IsNumber(camera_revision) &&
        cJSON_IsNumber(camera_size) && camera_size->valuedouble == BAMBU_CAMERA_BYTES) {
        snapshot->bambu_camera_available = true;
        snapshot->bambu_camera_revision = (uint32_t)camera_revision->valuedouble;
        snapshot->bambu_camera_size = (uint32_t)camera_size->valuedouble;
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
    copy_display_config((cJSON *)root, target);
    copy_custom_config((cJSON *)root, target);
    copy_bambu_status((cJSON *)root, target);
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
    target->bambu_enabled = source->bambu_enabled;
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
    target->dotii_base_idle = source->dotii_base_idle;
    target->dotii_state_assigned = source->dotii_state_assigned;
    target->dotii_state_duration_ms = source->dotii_state_duration_ms;
    target->dotii_state_token = source->dotii_state_token;
    target->dotii_expression = source->dotii_expression;
    target->dotii_touch_expression = source->dotii_touch_expression;
    target->dotii_blink_expression = source->dotii_blink_expression;
    target->dotii_long_idle_expression = source->dotii_long_idle_expression;
    memcpy(target->dotii_expression_durations_ms, source->dotii_expression_durations_ms,
           sizeof(target->dotii_expression_durations_ms));
    target->bambu_configured = source->bambu_configured;
    target->bambu_connected = source->bambu_connected;
    target->bambu_commandable = source->bambu_commandable;
    target->bambu_camera_available = source->bambu_camera_available;
    target->bambu_camera_revision = source->bambu_camera_revision;
    target->bambu_camera_size = source->bambu_camera_size;
    target->bambu_status = source->bambu_status;
    target->bambu_progress = source->bambu_progress;
    target->bambu_remaining_minutes = source->bambu_remaining_minutes;
    target->bambu_layer_current = source->bambu_layer_current;
    target->bambu_layer_total = source->bambu_layer_total;
    target->bambu_nozzle_temperature = source->bambu_nozzle_temperature;
    target->bambu_bed_temperature = source->bambu_bed_temperature;
    target->bambu_finish_at = source->bambu_finish_at;
    target->bambu_updated_at = source->bambu_updated_at;
    memcpy(target->bambu_name, source->bambu_name, sizeof(target->bambu_name));
    memcpy(target->bambu_status_text, source->bambu_status_text,
           sizeof(target->bambu_status_text));
    memcpy(target->bambu_filename, source->bambu_filename, sizeof(target->bambu_filename));
    memcpy(target->bambu_filament, source->bambu_filament, sizeof(target->bambu_filament));
    target->management_source_online = source->management_source_online;
    target->source_online = target->codex_source_online || target->management_source_online;
}
