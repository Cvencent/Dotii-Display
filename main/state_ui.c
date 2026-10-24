#include "state_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "ble_bridge.h"
#include "board_input.h"
#include "bsp/display.h"
#include "codex_bridge_client.h"
#include "connectivity.h"
#include "emotion/dotii_data.h"
#include "emotion/dotii_engine.h"
#include "emotion/emotion_mood.h"
#include "emotion/emotion_mood_store.h"
#include "emotion/dotii_render.h"
#include "emotion/emotion_router.h"
#include "xiaozhi_client.h"
#include "qqmusic_client.h"
#include "device_config.h"
#include "device_config.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "ui_icons.h"

LV_FONT_DECLARE(ui_font_cjk_24);
LV_FONT_DECLARE(ui_font_cjk_28);
LV_FONT_DECLARE(ui_font_digits_64);
LV_FONT_DECLARE(ui_font_cjk_20);
LV_FONT_DECLARE(ui_font_fixed_20);

#define SCREEN_SIZE 466

static const char *TAG = "state_ui";
#define SAFE_SIZE 358
#define COLOR_BG 0x000000
#define COLOR_SURFACE 0x141A19
#define COLOR_TEXT 0xF1F8F5
#define COLOR_MUTED 0x8E9B97
#define COLOR_BLUE 0x5DA9FF
#define COLOR_BLUE_DARK 0x2F65FF
#define COLOR_BLUE_USAGE 0x3A8DFF
#define COLOR_ORANGE 0xFF9D3D
#define COLOR_CYAN 0x20D6E6
#define COLOR_GREEN 0x42D98C
#define COLOR_XIAOZHI 0x20D6E6
#define COLOR_WARNING 0xF2C66D
#define COLOR_DANGER 0xFF766F
#define PAGE_COUNT 2
#define TITLE_HEIGHT 30
#define DISPLAY_ANGLE_DEFAULT_TENTHS 900
#define DISPLAY_ANGLE_MIN_TENTHS 800
#define DISPLAY_ANGLE_MAX_TENTHS 1000

static QueueHandle_t s_snapshot_queue;
/* The UI reads this snapshot from task context; it is not a DMA buffer. */
EXT_RAM_BSS_ATTR static codex_snapshot_t s_snapshot;
static lv_obj_t *s_main;
static lv_obj_t *s_main_content;
static lv_obj_t *s_plus_main;
static lv_obj_t *s_plus_five_arc;
static lv_obj_t *s_plus_weekly_arc;
static lv_obj_t *s_plus_five_percent;
static lv_obj_t *s_plus_weekly_percent;
static lv_obj_t *s_plus_five_reset;
static lv_obj_t *s_plus_weekly_reset;
static lv_obj_t *s_plus_weekly_reset_day;
static lv_obj_t *s_plus_running_count;
static lv_obj_t *s_plus_completed_count;
static lv_obj_t *s_plus_failed_count;
static lv_obj_t *s_plus_status_pill;
static lv_obj_t *s_xiaozhi_page;
static lv_obj_t *s_control;
static lv_obj_t *s_settings;
static lv_obj_t *s_power;
static lv_obj_t *s_before_power;
static lv_obj_t *s_before_control;
static lv_obj_t *s_before_screen_saver;
static lv_obj_t *s_current;
static lv_obj_t *s_main_arc;
static lv_obj_t *s_percent;
static lv_obj_t *s_status_pill;
static lv_obj_t *s_running_count;
static lv_obj_t *s_completed_count;
static lv_obj_t *s_failed_count;
static lv_obj_t *s_weekly_tokens;
static lv_obj_t *s_reset_day;
static lv_obj_t *s_reset_time;
static lv_obj_t *s_time_main;
static lv_obj_t *s_time_xiaozhi;
static lv_obj_t *s_xiaozhi_status_pill;
static lv_obj_t *s_xiaozhi_status_dot;
static lv_obj_t *s_xiaozhi_status;
static lv_obj_t *s_xiaozhi_emotion;
static lv_obj_t *s_xiaozhi_server;
static lv_obj_t *s_xiaozhi_listen_badge;
static lv_obj_t *s_settings_list;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_settings_wifi;
static lv_obj_t *s_settings_ip;
static lv_obj_t *s_settings_bridge;
static lv_obj_t *s_settings_reset_button;
static lv_obj_t *s_settings_reset_detail;
static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_volume_slider;
static lv_obj_t *s_volume_label;
static dotii_engine_t s_xiaozhi_engine;
static emotion_router_t s_xiaozhi_router;
static dotii_render_t s_xiaozhi_render;
static bool s_xiaozhi_engine_ready;
static emotion_mood_t s_xiaozhi_mood;
/* Turn-scoped face semantics: while Xiaozhi speaks, the face only changes on
   a genuinely new server emotion or a session-state change. Emotion expiry
   (the server emotion field times out after 5 s), mood drift and streaming
   text growth never switch the face mid-sentence. */
#define XIAOZHI_SERVER_EMOTION_MIN_GAP_MS 700u
#define XIAOZHI_SPEAK_AFTERGLOW_MS 1200u
static char s_xz_turn_emotion[3] = {0, 0, 0};
static uint32_t s_xz_turn_emotion_ms;
static uint32_t s_xz_handled_emotion_rev;
static bool s_xz_turn_server_applied;
static xiaozhi_session_state_t s_xz_face_session = XIAOZHI_STATE_DISABLED;
static uint32_t s_xz_afterglow_until_ms;
static uint32_t s_xiaozhi_mood_user_revision;
static uint32_t s_xiaozhi_mood_assistant_revision;
static uint32_t s_xiaozhi_mood_server_revision;

static xiaozhi_client_t s_xiaozhi;
/* Set by the page loader when the Xiaozhi screen is loaded, so the first frame
   after becoming visible repaints the whole canvas. The canvas lives in PSRAM
   and is normally only repainted inside the previous frame's dirty rectangle,
   so the pixels outside it are stale and would show as a garbled pattern when
   the page is shown again. An explicit flag is used instead of a time
   heuristic: a full repaint itself takes ~200 ms, so any "long gap" test would
   re-trigger on its own output and loop forever. */
static bool s_xiaozhi_face_needs_full;
/* Websocket start/stop can block for a long time (TLS connect, close
   handshake). Those calls must never run inside an LVGL timer: a stalled close
   used to hold the LVGL lock forever, which froze the screen and both hardware
   keys. The UI thread only publishes the desired state here; the supervisor
   task below owns the client lifecycle. */
static SemaphoreHandle_t s_xiaozhi_request_lock;
static bool s_xiaozhi_want_enabled;
static xiaozhi_config_t s_xiaozhi_want_config;

const xiaozhi_client_t *state_ui_xiaozhi_client(void)
{
    return &s_xiaozhi;
}
static lv_obj_t *s_codex_quick;
static lv_obj_t *s_xiaozhi_quick;
static lv_obj_t *s_page_dots[PAGE_COUNT][PAGE_COUNT];
static lv_point_t s_press_point;
static uint8_t s_battery = 0xFF;
static int s_brightness = 64;
static int16_t s_display_angle_tenths = DISPLAY_ANGLE_DEFAULT_TENTHS;
static uint32_t s_display_revision;
static SemaphoreHandle_t s_display_save_signal;
static uint32_t s_screen_off_timeout_seconds = 60;
static uint32_t s_sleep_timeout_seconds = 300;
static uint32_t s_charging_screen_off_timeout_seconds = 60;
/* Docked: only the panel turns off so Wi-Fi, BLE and the Codex Bridge link
   stay alive. Deep sleep stays on the battery profile. */
static uint32_t s_charging_sleep_timeout_seconds = 0;
static display_screen_off_page_t s_screen_off_page = DISPLAY_SCREEN_OFF_PAGE_NONE;
static bool s_external_power;
static uint32_t s_last_activity;
static uint32_t s_last_press_event;
static bool s_screen_on = true;
static bool s_screen_saver_active;
static bool s_wake_touch_in_progress;
static bool s_xiaozhi_touch_listening;
static bool s_voice_wake_transition_pending;
static bool s_screen_load_pending;
static lv_obj_t *s_pending_screen;
typedef enum {
    TRANSITION_PAGE_FORWARD,
    TRANSITION_PAGE_BACK,
    TRANSITION_CONTROL_DOWN,
    TRANSITION_CONTROL_UP,
    TRANSITION_DIRECT,
} transition_t;
static transition_t s_pending_transition;
static lv_font_t s_ui_font;
static uint32_t s_xiaozhi_touch_started;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *target;
} page_scroll_pair_t;

static page_scroll_pair_t s_page_scroll_pairs[2];
static size_t s_page_scroll_pair_count;

static bool screen_enabled(lv_obj_t *screen);
static bool page_enabled(uint8_t page);
static lv_obj_t *page_screen(uint8_t page);
static lv_obj_t *first_enabled_screen(void);
static lv_obj_t *enabled_return_screen(lv_obj_t *screen);
static void render_xiaozhi_expression(void);
static void xiaozhi_sync_config(void);
static void xiaozhi_supervisor_task(void *argument);
static void xiaozhi_touch_event(lv_event_t *event);
static lv_color_t color(uint32_t hex);
static void set_screen_background(lv_obj_t *screen);
static void screen_off(void);
static void screen_wake(void);
static bool enter_screen_saver(void);
static bool exit_screen_saver(void);
/* LVGL frees and re-allocates the text buffer of a label on every
   lv_label_set_text() call. Writing the same text again on every UI tick
   therefore fragments the heap, which is what made the panel die with a
   malloc error. Only touch a label when its text really changed. */
static void set_label_text_cached(lv_obj_t *label, const char *text)
{
    if (label == NULL) return;
    if (text == NULL) text = "";
    const char *current = lv_label_get_text(label);
    if (current != NULL && strcmp(current, text) == 0) return;
    lv_label_set_text(label, text);
}

static void set_label_text_fmt_cached(lv_obj_t *label, const char *fmt, ...)
{
    if (label == NULL || fmt == NULL) return;
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    set_label_text_cached(label, buffer);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t text_color);

static lv_color_t color(uint32_t hex)
{
    return lv_color_hex(hex);
}

static void set_screen_background(lv_obj_t *screen)
{
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, color(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, color(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(screen, &ui_font_cjk_20, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t text_color)
{
    lv_obj_t *label = lv_label_create(parent);
    set_label_text_cached(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color(text_color), 0);
    return label;
}

static lv_obj_t *make_task_counter(lv_obj_t *parent, const char *caption,
                                   uint32_t accent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 94, 44);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *value = make_label(box, "--", &lv_font_montserrat_22, accent);
    lv_obj_set_size(value, 88, 24);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, -1);

    lv_obj_t *caption_label = make_label(box, caption, &s_ui_font, COLOR_MUTED);
    lv_obj_set_size(caption_label, 88, 20);
    lv_obj_set_style_text_align(caption_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(caption_label, LV_ALIGN_BOTTOM_MID, 0, 1);
    return value;
}

static void make_task_counters(lv_obj_t *pill, lv_obj_t **running,
                               lv_obj_t **completed, lv_obj_t **failed)
{
    lv_obj_set_size(pill, 300, 52);
    lv_obj_set_layout(pill, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_set_style_pad_column(pill, 0, 0);
    *running = make_task_counter(pill, "运行中", COLOR_GREEN);
    *completed = make_task_counter(pill, "已完成", COLOR_BLUE);
    *failed = make_task_counter(pill, "失败", COLOR_DANGER);
}

static void update_task_counter(lv_obj_t *label, bool available, uint16_t value)
{
    if (label == NULL) return;
    if (!available) {
        set_label_text_cached(label, "--");
    } else {
        set_label_text_fmt_cached(label, "%u", (unsigned)value);
    }
}

/* The usage arc covers the whole panel, so writing its value or its colour
   invalidates every pixel of the screen and turns a cheap partial update
   into a full panel flush of about 78 milliseconds. The management snapshot
   arrives every couple of seconds while the numbers hardly ever change, so
   the arc is only touched when it really changes. */
typedef struct {
    lv_obj_t *arc;
    int32_t value;
    uint32_t color;
    bool used;
    bool valid;
} arc_display_slot_t;

static arc_display_slot_t s_arc_slots[3];

static void arc_display_update(lv_obj_t *arc, int32_t value, uint32_t hex)
{
    if (arc == NULL) return;
    arc_display_slot_t *slot = NULL;
    for (size_t i = 0; i < sizeof(s_arc_slots) / sizeof(s_arc_slots[0]); ++i) {
        if (s_arc_slots[i].used && s_arc_slots[i].arc == arc) {
            slot = &s_arc_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        for (size_t i = 0; i < sizeof(s_arc_slots) / sizeof(s_arc_slots[0]); ++i) {
            if (!s_arc_slots[i].used) {
                slot = &s_arc_slots[i];
                slot->used = true;
                slot->arc = arc;
                break;
            }
        }
    }
    if (slot == NULL) {
        lv_arc_set_value(arc, value);
        lv_obj_set_style_arc_color(arc, color(hex), LV_PART_INDICATOR);
        return;
    }
    if (slot->valid && slot->value == value && slot->color == hex) return;
    slot->valid = true;
    slot->value = value;
    slot->color = hex;
    lv_arc_set_value(arc, value);
    lv_obj_set_style_arc_color(arc, color(hex), LV_PART_INDICATOR);
}

/* Centering a scaled label by its glyph bounds is unstable because Chinese
   glyph bearings and the transform scale both change the visual center.  Give
   every page title a fixed box and scale around that box's geometric center. */
static lv_obj_t *make_centered_title(lv_obj_t *parent, const char *text,
                                     const lv_font_t *font, int32_t width,
                                     int32_t scale, int32_t outline_width,
                                     int32_t y)
{
    lv_obj_t *title = make_label(parent, text, font, COLOR_TEXT);
    lv_obj_set_size(title, width, TITLE_HEIGHT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    if (outline_width > 0) {
        lv_obj_set_style_text_outline_stroke_color(title, color(COLOR_TEXT), 0);
        lv_obj_set_style_text_outline_stroke_width(title, outline_width, 0);
        lv_obj_set_style_text_outline_stroke_opa(title, LV_OPA_COVER, 0);
    }
    if (scale != 256) lv_obj_set_style_transform_scale(title, scale, 0);
    lv_obj_set_style_transform_pivot_x(title, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(title, TITLE_HEIGHT / 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, y);
    return title;
}

static lv_obj_t *make_safe(lv_obj_t *screen)
{
    lv_obj_t *safe = lv_obj_create(screen);
    lv_obj_remove_style_all(safe);
    lv_obj_set_size(safe, SAFE_SIZE, SAFE_SIZE);
    lv_obj_center(safe);
    lv_obj_remove_flag(safe, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(safe, LV_OBJ_FLAG_CLICKABLE);
    return safe;
}

static void page_scroll_step(page_scroll_pair_t *pair, bool down)
{
    if (pair == NULL || pair->target == NULL) return;
    lv_obj_update_layout(pair->target);
    if ((down && lv_obj_get_scroll_bottom(pair->target) <= 0) ||
        (!down && lv_obj_get_scroll_top(pair->target) <= 0)) {
        return;
    }
    int32_t step = lv_obj_get_height(pair->target) * 3 / 4;
    int32_t destination = lv_obj_get_scroll_y(pair->target) + (down ? step : -step);
    /* Scrolling moves already-rendered content. Force one complete transformed
       frame for every vertically scrolling page; a rotated partial refresh can
       otherwise leave stale/jagged strips in the newly exposed area. The
       physical reveal direction follows the vertical gesture. */
    bsp_display_transform_set_next_sweep(down ? BSP_DISPLAY_SWEEP_UP :
                                            BSP_DISPLAY_SWEEP_DOWN);
    bsp_display_transform_force_full_refresh();
    lv_obj_scroll_to_y(pair->target, destination, LV_ANIM_OFF);
}

static bool page_scroll_current(bool down)
{
    for (size_t index = 0; index < s_page_scroll_pair_count; ++index) {
        page_scroll_pair_t *pair = &s_page_scroll_pairs[index];
        if (pair->screen == s_current) {
            page_scroll_step(pair, down);
            return true;
        }
    }
    return false;
}

static void register_page_scroll_target(lv_obj_t *screen, lv_obj_t *target)
{
    if (s_page_scroll_pair_count >= sizeof(s_page_scroll_pairs) / sizeof(s_page_scroll_pairs[0])) return;
    page_scroll_pair_t *pair = &s_page_scroll_pairs[s_page_scroll_pair_count++];
    pair->screen = screen;
    pair->target = target;
    /* Native free scrolling is disabled. Each vertical gesture advances a
       deterministic 75% viewport step through page_scroll_step(). */
    lv_obj_remove_flag(target, LV_OBJ_FLAG_SCROLLABLE);
}

static void load_screen_commit(void *user_data)
{
    (void)user_data;
    lv_obj_t *screen = s_pending_screen;
    if (screen == NULL) {
        s_screen_load_pending = false;
        return;
    }

    if (s_pending_transition == TRANSITION_DIRECT) {
        lv_screen_load(screen);
        lv_obj_invalidate(screen);
        if (screen == s_xiaozhi_page) s_xiaozhi_face_needs_full = true;
        lv_refr_now(lv_obj_get_display(screen));
        s_screen_load_pending = false;
        if (s_voice_wake_transition_pending && screen == s_xiaozhi_page) {
            s_voice_wake_transition_pending = false;
            ESP_LOGI(TAG, "voice wake: Xiaozhi face is active");
        }
        return;
    }

    if (screen == s_xiaozhi_page) s_xiaozhi_face_needs_full = true;
    lv_screen_load_anim(screen,
                        s_pending_transition == TRANSITION_CONTROL_DOWN ? LV_SCREEN_LOAD_ANIM_OVER_BOTTOM :
                        s_pending_transition == TRANSITION_CONTROL_UP ? LV_SCREEN_LOAD_ANIM_OUT_TOP :
                        s_pending_transition == TRANSITION_PAGE_FORWARD ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT :
                        LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
                        240, 0, false);
}

static void load_screen_with_transition(lv_obj_t *screen, transition_t transition)
{
    if (screen == NULL || screen == s_current || s_screen_load_pending) return;

    const bsp_display_sweep_t sweep =
        transition == TRANSITION_PAGE_BACK ? BSP_DISPLAY_SWEEP_RIGHT :
        transition == TRANSITION_CONTROL_DOWN ? BSP_DISPLAY_SWEEP_DOWN :
        transition == TRANSITION_CONTROL_UP ? BSP_DISPLAY_SWEEP_UP :
        BSP_DISPLAY_SWEEP_LEFT;
    bsp_display_transform_set_next_sweep(sweep);
    transition = TRANSITION_DIRECT;

    s_current = screen;
    s_last_activity = lv_tick_get();
    s_screen_load_pending = true;
    s_pending_screen = screen;
    s_pending_transition = transition;
    /* Gestures bubble through their old object tree.  Deferring the actual
       switch until that dispatch has finished prevents the old and new screen
       from being flushed during the same gesture transaction. */
    if (lv_async_call(load_screen_commit, NULL) != LV_RESULT_OK) {
        load_screen_commit(NULL);
    }
}

static void load_screen(lv_obj_t *screen, bool forward)
{
    load_screen_with_transition(screen, forward ? TRANSITION_PAGE_FORWARD : TRANSITION_PAGE_BACK);
}

static void screen_loaded_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED) return;
    /* The load animation has already submitted its final frame.  Forcing a
       second full-screen refresh here creates a visible hitch at the end of
       every swipe and is unnecessary with reliable DMA strip buffers. */
    s_pending_screen = NULL;
    s_screen_load_pending = false;
}

static void activity_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        if (s_screen_saver_active) {
            exit_screen_saver();
            s_wake_touch_in_progress = true;
            return;
        }
        if (!s_screen_on) {
            screen_wake();
            s_wake_touch_in_progress = true;
            return;
        }
        lv_indev_t *input = lv_indev_active();
        if (input != NULL) lv_indev_get_point(input, &s_press_point);
        uint32_t now = lv_tick_get();
        if (s_last_press_event != 0 && lv_tick_elaps(s_last_press_event) < 80) return;
        s_last_press_event = now;
        s_last_activity = now;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_wake_touch_in_progress = false;
    }
}

static void add_activity_event(lv_obj_t *screen)
{
    lv_obj_add_event_cb(screen, activity_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(screen, screen_loaded_event, LV_EVENT_SCREEN_LOADED, NULL);
}

static void return_from_current(void)
{
    if (s_current == s_settings) load_screen(first_enabled_screen(), false);
    else if (s_current == s_power) load_screen(enabled_return_screen(s_before_power), false);
}

static void swipe_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
    if (s_wake_touch_in_progress) return;
    lv_indev_t *input = lv_indev_active();
    if (input == NULL) return;

    lv_dir_t direction = lv_indev_get_gesture_dir(input);
    if (s_current == s_control && direction == LV_DIR_TOP) {
        load_screen_with_transition(enabled_return_screen(s_before_control), TRANSITION_CONTROL_UP);
    } else if (s_current != s_control && s_current != s_power &&
               direction == LV_DIR_BOTTOM && s_press_point.y <= 80) {
        s_before_control = s_current;
        load_screen_with_transition(s_control, TRANSITION_CONTROL_DOWN);
    } else if (direction == LV_DIR_TOP && page_scroll_current(true)) {
        /* One upward swipe advances one viewport step. */
    } else if (direction == LV_DIR_BOTTOM && page_scroll_current(false)) {
        /* One downward swipe returns one viewport step. */
    } else if (direction == LV_DIR_RIGHT) {
        return_from_current();
    }
}

static void quick_clicked(lv_event_t *event)
{
    lv_obj_t *target = (lv_obj_t *)lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        if (target == s_main) target = page_screen(0);
        load_screen_with_transition(target, TRANSITION_CONTROL_UP);
    }
}

static void brightness_changed(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    s_brightness = lv_slider_get_value(lv_event_get_target_obj(event));
    if (s_screen_on) bsp_display_brightness_set(s_brightness);
}

static void volume_changed(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
    const int requested = lv_slider_get_value(lv_event_get_target_obj(event));
    const uint8_t volume = xiaozhi_client_set_volume(
        &s_xiaozhi, requested, code == LV_EVENT_RELEASED);
    if (s_volume_label != NULL) {
        set_label_text_fmt_cached(s_volume_label, "音量 %u%%", (unsigned)volume);
    }
}

static void screen_off(void)
{
    if (!s_screen_on) return;
    bsp_display_brightness_set(0);
    s_screen_on = false;
}

static void screen_wake(void)
{
    if (s_screen_on) return;
    bsp_display_brightness_set(s_brightness);
    s_screen_on = true;
    s_last_activity = lv_tick_get();
}

static lv_obj_t *screen_saver_target(void)
{
    return NULL;
}

static bool enter_screen_saver(void)
{
    if (s_screen_saver_active) return true;
    lv_obj_t *target = screen_saver_target();
    if (target == NULL || s_current == NULL) return false;
    s_before_screen_saver = s_current;
    s_screen_saver_active = true;
    if (target != s_current) {
        /* Loading the saver must not restart the inactivity clock: the
           configured sleep deadline is measured from the user's last input. */
        const uint32_t last_activity = s_last_activity;
        load_screen_with_transition(target, TRANSITION_DIRECT);
        s_last_activity = last_activity;
    } else {
        lv_obj_invalidate(target);
    }
    return true;
}

static bool exit_screen_saver(void)
{
    if (!s_screen_saver_active) return false;
    s_screen_saver_active = false;
    lv_obj_t *return_screen = enabled_return_screen(s_before_screen_saver);
    s_before_screen_saver = NULL;
    s_last_activity = lv_tick_get();
    if (return_screen != NULL && return_screen != s_current) {
        load_screen_with_transition(return_screen, TRANSITION_DIRECT);
    } else if (s_current != NULL) {
        lv_obj_invalidate(s_current);
    }
    return true;
}

static void load_persisted_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("display_ui", NVS_READONLY, &handle) != ESP_OK) return;
    if (nvs_get_u32(handle, "sleep_sec", &s_sleep_timeout_seconds) != ESP_OK) {
        static const uint32_t legacy_seconds[] = {5, 10, 30, 60, 300, 0, 300};
        uint8_t legacy_option = 4;
        if (nvs_get_u8(handle, "timeout", &legacy_option) == ESP_OK && legacy_option < 7) {
            s_sleep_timeout_seconds = legacy_seconds[legacy_option];
        }
    }
    if (s_sleep_timeout_seconds == 5) s_sleep_timeout_seconds = 10;
    if (nvs_get_u32(handle, "off_sec", &s_screen_off_timeout_seconds) != ESP_OK) {
        s_screen_off_timeout_seconds = s_sleep_timeout_seconds == 0 ? 60 :
                                       s_sleep_timeout_seconds;
    }
    if (s_screen_off_timeout_seconds == 5) s_screen_off_timeout_seconds = 10;
    if (s_screen_off_timeout_seconds == 0 && s_sleep_timeout_seconds != 0) {
        s_screen_off_timeout_seconds = s_sleep_timeout_seconds;
    } else if (s_screen_off_timeout_seconds != 0 && s_sleep_timeout_seconds != 0 &&
               s_sleep_timeout_seconds < s_screen_off_timeout_seconds) {
        s_screen_off_timeout_seconds = s_sleep_timeout_seconds;
    }
    if (nvs_get_u32(handle, "chg_off_sec", &s_charging_screen_off_timeout_seconds) != ESP_OK) {
        s_charging_screen_off_timeout_seconds = s_screen_off_timeout_seconds;
    }
    if (nvs_get_u32(handle, "chg_sleep_sec", &s_charging_sleep_timeout_seconds) != ESP_OK) {
        s_charging_sleep_timeout_seconds = s_sleep_timeout_seconds;
    }
    if (s_charging_screen_off_timeout_seconds == 5) s_charging_screen_off_timeout_seconds = 10;
    if (s_charging_sleep_timeout_seconds == 5) s_charging_sleep_timeout_seconds = 10;
    if (s_charging_screen_off_timeout_seconds == 0 && s_charging_sleep_timeout_seconds != 0) {
        s_charging_screen_off_timeout_seconds = s_charging_sleep_timeout_seconds;
    } else if (s_charging_screen_off_timeout_seconds != 0 && s_charging_sleep_timeout_seconds != 0 &&
               s_charging_sleep_timeout_seconds < s_charging_screen_off_timeout_seconds) {
        s_charging_screen_off_timeout_seconds = s_charging_sleep_timeout_seconds;
    }
    /* Legacy custom/dotii ambient pages are no longer selectable. */
    s_screen_off_page = DISPLAY_SCREEN_OFF_PAGE_NONE;
    int16_t angle;
    if (nvs_get_i16(handle, "dock_angle", &angle) == ESP_OK &&
        angle >= DISPLAY_ANGLE_MIN_TENTHS && angle <= DISPLAY_ANGLE_MAX_TENTHS) {
        /* 84.0 deg was the firmware default before the pixel-exact 90 deg
           transform. Devices upgrading in place still have it persisted in
           NVS, which would keep the face blurry, so migrate that legacy
           default while leaving deliberate custom angles alone. */
        s_display_angle_tenths = (angle == 840) ? DISPLAY_ANGLE_DEFAULT_TENTHS : angle;
    }
    nvs_get_u32(handle, "display_rev", &s_display_revision);
    nvs_close(handle);
}

static void save_display_settings(void);

/* NVS writes must not run on the LVGL worker: that task's stack lives in
   PSRAM, and the flash driver freezes the external-memory cache while a
   page is written. The timer only signals this worker, which owns an
   internal stack for the actual commit. */
static void display_settings_writer_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (xSemaphoreTake(s_display_save_signal, portMAX_DELAY) != pdTRUE) continue;
        save_display_settings();
    }
}

static void request_display_settings_save(void)
{
    if (s_display_save_signal != NULL) xSemaphoreGive(s_display_save_signal);
}

static void save_display_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("display_ui", NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t angle_result = nvs_set_i16(handle, "dock_angle", s_display_angle_tenths);
    esp_err_t revision_result = nvs_set_u32(handle, "display_rev", s_display_revision);
    esp_err_t off_result = nvs_set_u32(handle, "off_sec", s_screen_off_timeout_seconds);
    esp_err_t sleep_result = nvs_set_u32(handle, "sleep_sec", s_sleep_timeout_seconds);
    esp_err_t charging_off_result = nvs_set_u32(handle, "chg_off_sec", s_charging_screen_off_timeout_seconds);
    esp_err_t charging_sleep_result = nvs_set_u32(handle, "chg_sleep_sec", s_charging_sleep_timeout_seconds);
    esp_err_t screen_off_page_result = nvs_set_u8(handle, "off_page", (uint8_t)s_screen_off_page);
    if (angle_result == ESP_OK && revision_result == ESP_OK &&
        off_result == ESP_OK && sleep_result == ESP_OK && charging_off_result == ESP_OK &&
        charging_sleep_result == ESP_OK && screen_off_page_result == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static void refresh_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) connectivity_request_refresh();
}

static void restart_hold_anim_exec(void *object, int32_t value)
{
    lv_color_t mixed = lv_color_mix(color(0x286348), color(0x173D2D), (uint8_t)value);
    lv_obj_set_style_bg_color((lv_obj_t *)object, mixed, 0);
    lv_obj_set_style_bg_color((lv_obj_t *)object, mixed, LV_STATE_PRESSED);
}

static void shutdown_hold_anim_exec(void *object, int32_t value)
{
    lv_color_t mixed = lv_color_mix(color(0x682A28), color(0x3A1D1D), (uint8_t)value);
    lv_obj_set_style_bg_color((lv_obj_t *)object, mixed, 0);
    lv_obj_set_style_bg_color((lv_obj_t *)object, mixed, LV_STATE_PRESSED);
}

static void provisioning_reset_hold_anim_exec(void *object, int32_t value)
{
    lv_color_t mixed = lv_color_mix(color(0x682A28), color(0x321A1A), (uint8_t)value);
    lv_obj_set_style_bg_color((lv_obj_t *)object, mixed, 0);
    lv_obj_set_style_bg_color((lv_obj_t *)object, mixed, LV_STATE_PRESSED);
}

static void provisioning_reset_anim_done(lv_anim_t *animation)
{
    (void)animation;
    esp_err_t config_error = device_config_clear_provisioning();
    if (config_error != ESP_OK) {
        set_label_text_cached(s_settings_reset_detail, "重置失败，请重试");
        lv_obj_set_style_bg_color(s_settings_reset_button, color(0x321A1A), 0);
        lv_obj_set_style_bg_color(s_settings_reset_button, color(0x4A2422), LV_STATE_PRESSED);
        return;
    }
    esp_err_t bonds_error = ble_bridge_clear_bonds();
    if (bonds_error != ESP_OK) {
        set_label_text_cached(s_settings_reset_detail, "重置失败，请重试");
        lv_obj_set_style_bg_color(s_settings_reset_button, color(0x321A1A), 0);
        lv_obj_set_style_bg_color(s_settings_reset_button, color(0x4A2422), LV_STATE_PRESSED);
        return;
    }
    set_label_text_cached(s_settings_reset_detail, "已清除，正在重新启动");
    esp_restart();
}

static void provisioning_reset_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *button = lv_event_get_current_target(event);
    if (code == LV_EVENT_PRESSED) {
        set_label_text_cached(s_settings_reset_detail, "继续按住以确认");
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, button);
        lv_anim_set_values(&animation, 0, 255);
        lv_anim_set_duration(&animation, 1200);
        lv_anim_set_exec_cb(&animation, provisioning_reset_hold_anim_exec);
        lv_anim_set_completed_cb(&animation, provisioning_reset_anim_done);
        lv_anim_start(&animation);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_anim_delete(button, provisioning_reset_hold_anim_exec);
        lv_obj_set_style_bg_color(button, color(0x321A1A), 0);
        lv_obj_set_style_bg_color(button, color(0x4A2422), LV_STATE_PRESSED);
        set_label_text_cached(s_settings_reset_detail, "长按 1.2 秒，清除配网信息");
    }
}

static void restart_anim_done(lv_anim_t *animation)
{
    (void)animation;
    esp_restart();
}

static void restart_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *button = lv_event_get_current_target(event);
    if (code == LV_EVENT_PRESSED) {
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, button);
        lv_anim_set_values(&animation, 0, 255);
        lv_anim_set_duration(&animation, 1200);
        lv_anim_set_exec_cb(&animation, restart_hold_anim_exec);
        lv_anim_set_completed_cb(&animation, restart_anim_done);
        lv_anim_start(&animation);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_anim_delete(button, restart_hold_anim_exec);
        lv_obj_set_style_bg_color(button, color(0x173D2D), 0);
        lv_obj_set_style_bg_color(button, color(0x214F3A), LV_STATE_PRESSED);
    }
}

static void shutdown_anim_done(lv_anim_t *animation)
{
    (void)animation;
    board_input_request_shutdown();
}

static void shutdown_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *button = lv_event_get_current_target(event);
    if (code == LV_EVENT_PRESSED) {
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, button);
        lv_anim_set_values(&animation, 0, 255);
        lv_anim_set_duration(&animation, 1200);
        lv_anim_set_exec_cb(&animation, shutdown_hold_anim_exec);
        lv_anim_set_completed_cb(&animation, shutdown_anim_done);
        lv_anim_start(&animation);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_anim_delete(button, shutdown_hold_anim_exec);
        lv_obj_set_style_bg_color(button, color(0x3A1D1D), 0);
        lv_obj_set_style_bg_color(button, color(0x572725), LV_STATE_PRESSED);
    }
}

static lv_obj_t *make_metric(lv_obj_t *parent, const char *value, const char *caption, int x)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 132, 56);
    lv_obj_set_pos(box, x, 346);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *value_label = make_label(box, value, &lv_font_montserrat_22, COLOR_TEXT);
    lv_obj_align(value_label, LV_ALIGN_TOP_MID, 0, 0);
    /* Fixed captions use one compact font instead of crossing the four large
       conversation-font fragments.  This keeps glyph metrics and bitmap
       decoding identical across the whole label. */
    lv_obj_t *caption_label = make_label(box, caption, &s_ui_font, COLOR_MUTED);
    lv_obj_set_style_transform_scale(caption_label, 282, 0);
    lv_obj_align(caption_label, LV_ALIGN_BOTTOM_MID, 0, 1);
    return value_label;
}

static void make_reset_metric(lv_obj_t *parent, int x)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 132, 56);
    lv_obj_set_pos(box, x, 346);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *value_row = lv_obj_create(box);
    lv_obj_remove_style_all(value_row);
    lv_obj_set_size(value_row, 132, 25);
    lv_obj_align(value_row, LV_ALIGN_TOP_MID, 4, 0);
    lv_obj_set_layout(value_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(value_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(value_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(value_row, 4, 0);
    lv_obj_remove_flag(value_row, LV_OBJ_FLAG_SCROLLABLE);

    s_reset_day = make_label(value_row, "08-18", &lv_font_montserrat_22, COLOR_TEXT);
    s_reset_time = make_label(value_row, "10:00", &lv_font_montserrat_18, COLOR_TEXT);
    lv_obj_t *caption = make_label(box, "额度重置", &s_ui_font, COLOR_MUTED);
    lv_obj_set_style_transform_scale(caption, 282, 0);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, 1);
}

static bool page_enabled(uint8_t page)
{
    if (!s_snapshot.valid) return page < 2;
    if (page == 0) return s_snapshot.codex_enabled;
    return s_snapshot.xiaozhi_enabled;
}

static lv_obj_t *page_screen(uint8_t page)
{
    if (page == 0) return s_snapshot.codex_ui_dual_limit ? s_plus_main : s_main;
    return s_xiaozhi_page;
}

static lv_obj_t *first_enabled_screen(void)
{
    for (uint8_t page = 0; page < PAGE_COUNT; ++page) {
        if (page_enabled(page)) return page_screen(page);
    }
    return page_screen(0);
}

static bool screen_enabled(lv_obj_t *screen)
{
    if (screen == s_main || screen == s_plus_main) return page_enabled(0);
    if (screen == s_xiaozhi_page) return page_enabled(1);
    return true;
}

static lv_obj_t *enabled_return_screen(lv_obj_t *screen)
{
    if (screen == s_main || screen == s_plus_main) return page_screen(0);
    return screen != NULL && screen_enabled(screen) ? screen : first_enabled_screen();
}

static void update_page_dots(void)
{
    static const uint32_t active_colors[PAGE_COUNT] = {COLOR_BLUE, COLOR_XIAOZHI};
    for (uint8_t row = 0; row < PAGE_COUNT; ++row) {
        int total_width = 0;
        uint8_t count = 0;
        for (uint8_t page = 0; page < PAGE_COUNT; ++page) {
            if (!page_enabled(page)) continue;
            total_width += page == row ? 16 : 5;
            count++;
        }
        if (count > 1) total_width += (count - 1) * 8;
        int cursor = -total_width / 2;
        for (uint8_t page = 0; page < PAGE_COUNT; ++page) {
            lv_obj_t *dot = s_page_dots[row][page];
            if (dot == NULL) continue;
            if (!page_enabled(page)) {
                lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
            int width = page == row ? 16 : 5;
            lv_obj_set_size(dot, width, 5);
            lv_obj_set_style_bg_color(dot, color(page == row ? active_colors[row] : 0x59605E), 0);
            lv_obj_set_style_radius(dot, page == row ? 3 : LV_RADIUS_CIRCLE, 0);
            lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, cursor + width / 2, -29);
            cursor += width + 8;
        }
    }
}

static void make_page_dots(lv_obj_t *parent, uint8_t active_page)
{
    for (uint8_t page = 0; page < PAGE_COUNT; ++page) {
        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        s_page_dots[active_page][page] = dot;
    }
    update_page_dots();
}

static void build_main(void)
{
    /* Waveshare's known-good LVGL demo builds directly on the display's
       current active screen.  Reusing it also guarantees that the first
       complete invalidation belongs to the screen attached by the BSP. */
    s_main = lv_screen_active();
    lv_obj_clean(s_main);
    set_screen_background(s_main);
    add_activity_event(s_main);

    s_main_content = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_main_content);
    lv_obj_set_size(s_main_content, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_main_content);
    lv_obj_remove_flag(s_main_content, LV_OBJ_FLAG_SCROLLABLE);

    s_main_arc = lv_arc_create(s_main_content);
    lv_obj_set_size(s_main_arc, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(s_main_arc);
    lv_arc_set_range(s_main_arc, 0, 100);
    lv_arc_set_value(s_main_arc, 68);
    lv_arc_set_bg_angles(s_main_arc, 140, 400);
    lv_obj_set_style_arc_width(s_main_arc, 26, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_main_arc, color(0x232928), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_main_arc, 26, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_main_arc, color(COLOR_BLUE), LV_PART_INDICATOR);
    lv_obj_remove_style(s_main_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_main_arc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = make_label(s_main_content, "Codex", &ui_font_cjk_28, COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);
    s_time_main = make_label(s_main_content, "--:--", &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_align(s_time_main, LV_ALIGN_TOP_MID, 0, 82);

    lv_obj_t *kicker = make_label(s_main_content, "周剩余", &ui_font_cjk_24, COLOR_MUTED);
    lv_obj_align(kicker, LV_ALIGN_CENTER, 0, -82);
    s_percent = make_label(s_main_content, "68%", &ui_font_digits_64, COLOR_TEXT);
    lv_obj_set_style_transform_scale(s_percent, 282, 0);
    lv_obj_align(s_percent, LV_ALIGN_CENTER, 0, -19);

    s_status_pill = lv_obj_create(s_main_content);
    lv_obj_set_style_radius(s_status_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_pill, color(0x152A42), 0);
    lv_obj_set_style_bg_opa(s_status_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_pill, 0, 0);
    lv_obj_remove_flag(s_status_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_status_pill, LV_OBJ_FLAG_CLICKABLE);
    make_task_counters(s_status_pill, &s_running_count, &s_completed_count,
                       &s_failed_count);
    lv_obj_align(s_status_pill, LV_ALIGN_CENTER, 0, 50);

    s_weekly_tokens = make_metric(s_main_content, "18.4k", "近7日 Token", 92);
    make_reset_metric(s_main_content, 242);

    lv_obj_t *divider = lv_obj_create(s_main_content);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 1, 48);
    lv_obj_set_style_bg_color(divider, color(0x38403E), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_pos(divider, 233, 350);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    make_page_dots(s_main_content, 0);

    add_activity_event(s_main_content);
    lv_obj_add_event_cb(s_main, swipe_event, LV_EVENT_GESTURE, NULL);
}

static lv_obj_t *make_usage_arc(lv_obj_t *parent, int32_t start_angle,
                                int32_t end_angle, uint32_t accent)
{
    lv_obj_t *arc = lv_arc_create(parent);
    /* Match the classic Codex ring geometry exactly. */
    lv_obj_set_size(arc, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(arc);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_arc_set_bg_angles(arc, start_angle, end_angle);
    lv_obj_set_style_arc_width(arc, 26, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color(0x222725), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 26, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color(accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

static void make_five_hour_title(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 180, 32);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

    make_label(row, "5H", &lv_font_montserrat_22, COLOR_ORANGE);
    lv_obj_t *title_gap = lv_obj_create(row);
    lv_obj_remove_style_all(title_gap);
    lv_obj_set_size(title_gap, 2, 1);
    make_label(row, "剩余", &ui_font_cjk_24, COLOR_ORANGE);
}

static lv_obj_t *make_usage_reset_row(lv_obj_t *parent, int32_t y,
                                      lv_obj_t **weekday_label)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 320, 28);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

    if (weekday_label != NULL) {
        *weekday_label = make_label(row, "", &s_ui_font, COLOR_MUTED);
    }
    lv_obj_t *value = make_label(row, "--", &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_t *suffix_gap = lv_obj_create(row);
    lv_obj_remove_style_all(suffix_gap);
    lv_obj_set_size(suffix_gap, 6, 1);
    make_label(row, "重置", &s_ui_font, COLOR_MUTED);
    return value;
}

static void build_plus_main(void)
{
    s_plus_main = lv_obj_create(NULL);
    set_screen_background(s_plus_main);
    add_activity_event(s_plus_main);

    s_plus_five_arc = make_usage_arc(s_plus_main, 188, 352, COLOR_ORANGE);
    s_plus_weekly_arc = make_usage_arc(s_plus_main, 8, 172, COLOR_BLUE_USAGE);
    /* Anchor the blue indicator at the left endpoint. As the remaining value
     * falls, its right endpoint retreats from right to left. */
    lv_arc_set_mode(s_plus_weekly_arc, LV_ARC_MODE_REVERSE);

    make_five_hour_title(s_plus_main, 64);
    s_plus_five_percent = make_label(s_plus_main, "--", &ui_font_digits_64, COLOR_TEXT);
    lv_obj_set_style_transform_scale(s_plus_five_percent, 260, 0);
    lv_obj_align(s_plus_five_percent, LV_ALIGN_TOP_MID, 0, 104);
    s_plus_five_reset = make_usage_reset_row(s_plus_main, 166, NULL);

    s_plus_status_pill = lv_obj_create(s_plus_main);
    lv_obj_align(s_plus_status_pill, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_plus_status_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_plus_status_pill, color(0x152A42), 0);
    lv_obj_set_style_bg_opa(s_plus_status_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_plus_status_pill, 0, 0);
    lv_obj_remove_flag(s_plus_status_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_plus_status_pill, LV_OBJ_FLAG_CLICKABLE);
    make_task_counters(s_plus_status_pill, &s_plus_running_count,
                       &s_plus_completed_count, &s_plus_failed_count);

    lv_obj_t *weekly_title = make_label(s_plus_main, "周剩余", &ui_font_cjk_24,
                                        COLOR_BLUE_USAGE);
    lv_obj_align(weekly_title, LV_ALIGN_TOP_MID, 0, 278);
    s_plus_weekly_percent = make_label(s_plus_main, "--", &ui_font_digits_64, COLOR_TEXT);
    lv_obj_set_style_transform_scale(s_plus_weekly_percent, 260, 0);
    lv_obj_align(s_plus_weekly_percent, LV_ALIGN_TOP_MID, 0, 318);
    s_plus_weekly_reset = make_usage_reset_row(s_plus_main, 378, &s_plus_weekly_reset_day);

    lv_obj_add_event_cb(s_plus_main, swipe_event, LV_EVENT_GESTURE, NULL);
}

static void set_usage_reset_text(lv_obj_t *label, const char *reset_date, bool time_only)
{
    if (reset_date == NULL || reset_date[0] == '\0') {
        set_label_text_cached(label, "--");
        return;
    }
    const char *value = reset_date;
    if (time_only) {
        const char *space = strchr(reset_date, ' ');
        if (space != NULL && space[1] != '\0') value = space + 1;
    }
    set_label_text_cached(label, value);
}

static void set_weekly_reset_text(lv_obj_t *day_label, lv_obj_t *time_label,
                                  const char *reset_date)
{
    if (reset_date == NULL || reset_date[0] == '\0') {
        set_label_text_cached(day_label, "--");
        set_label_text_cached(time_label, "");
        return;
    }

    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    if (sscanf(reset_date, "%d-%d %d:%d", &month, &day, &hour, &minute) != 4 ||
        month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        set_label_text_cached(day_label, "--");
        set_label_text_cached(time_label, reset_date);
        return;
    }

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year < 120) {
        set_label_text_cached(day_label, "--");
    } else {
        struct tm reset = local;
        reset.tm_mon = month - 1;
        reset.tm_mday = day;
        reset.tm_hour = hour;
        reset.tm_min = minute;
        reset.tm_sec = 0;
        reset.tm_isdst = -1;
        time_t reset_at = mktime(&reset);
        double offset = difftime(reset_at, now);
        if (offset < -15552000.0) {
            reset.tm_year += 1;
            reset_at = mktime(&reset);
        } else if (offset > 15552000.0) {
            reset.tm_year -= 1;
            reset_at = mktime(&reset);
        }
        static const char *const weekdays[] = {
            "周日", "周一", "周二", "周三", "周四", "周五", "周六",
        };
        struct tm reset_local;
        localtime_r(&reset_at, &reset_local);
        set_label_text_cached(day_label, weekdays[reset_local.tm_wday]);
    }

    char time_text[6];
    snprintf(time_text, sizeof(time_text), "%02d:%02d", hour, minute);
    set_label_text_cached(time_label, time_text);
}

static void xiaozhi_listen_badge_set_visible(bool visible)
{
    if (s_xiaozhi_listen_badge == NULL) return;
    if (visible) {
        lv_obj_remove_flag(s_xiaozhi_listen_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_xiaozhi_listen_badge, LV_OBJ_FLAG_HIDDEN);
    }
}
static void build_xiaozhi(void)
{
    s_xiaozhi_page = lv_obj_create(NULL);
    set_screen_background(s_xiaozhi_page);
    add_activity_event(s_xiaozhi_page);
    lv_obj_add_event_cb(s_xiaozhi_page, swipe_event, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_xiaozhi_page, xiaozhi_touch_event, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_xiaozhi_page, xiaozhi_touch_event, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_xiaozhi_page, xiaozhi_touch_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_flag(s_xiaozhi_page, LV_OBJ_FLAG_CLICKABLE);
    make_page_dots(s_xiaozhi_page, 1);

    lv_obj_t *content = lv_obj_create(s_xiaozhi_page);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    /* The Xiaozhi page is an immersive face: the character fills the whole
       466 px round panel, so the bezel itself becomes the face outline. No
       title sits on top; the status pill only appears when there is something
       worth saying (pairing code, error, or an active session). */
    lv_obj_t *face = lv_obj_create(content);
    lv_obj_remove_style_all(face);
    lv_obj_set_size(face, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_center(face);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE);
    dotii_render_attach(&s_xiaozhi_render, face, SCREEN_SIZE);
    /* Keep the selected character palette visible. A previous pure-black body
       override made every character look identical on the Xiaozhi page. */

    /* Listening gets its own compact badge so the current microphone state is
       readable without covering the face with the full session status pill. */
    s_xiaozhi_listen_badge = lv_obj_create(content);
    lv_obj_set_size(s_xiaozhi_listen_badge, 164, 44);
    lv_obj_set_style_radius(s_xiaozhi_listen_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_xiaozhi_listen_badge, color(0x071416), 0);
    lv_obj_set_style_bg_opa(s_xiaozhi_listen_badge, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_xiaozhi_listen_badge, 1, 0);
    lv_obj_set_style_border_color(s_xiaozhi_listen_badge, color(COLOR_XIAOZHI), 0);
    lv_obj_set_style_pad_column(s_xiaozhi_listen_badge, 9, 0);
    lv_obj_set_layout(s_xiaozhi_listen_badge, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_xiaozhi_listen_badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_xiaozhi_listen_badge, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_xiaozhi_listen_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_xiaozhi_listen_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_xiaozhi_listen_badge, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_add_flag(s_xiaozhi_listen_badge, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ear = lv_obj_create(s_xiaozhi_listen_badge);
    lv_obj_remove_style_all(ear);
    lv_obj_set_size(ear, 28, 30);
    lv_obj_remove_flag(ear, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ear_outer = lv_arc_create(ear);
    lv_obj_set_size(ear_outer, 23, 27);
    lv_obj_align(ear_outer, LV_ALIGN_LEFT_MID, 0, -1);
    lv_arc_set_bg_angles(ear_outer, 35, 315);
    lv_obj_set_style_arc_width(ear_outer, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ear_outer, color(COLOR_XIAOZHI), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ear_outer, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ear_outer, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_flag(ear_outer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ear_inner = lv_arc_create(ear);
    lv_obj_set_size(ear_inner, 12, 15);
    lv_obj_align(ear_inner, LV_ALIGN_LEFT_MID, 6, -2);
    lv_arc_set_bg_angles(ear_inner, 55, 295);
    lv_obj_set_style_arc_width(ear_inner, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ear_inner, color(COLOR_XIAOZHI), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ear_inner, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ear_inner, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_flag(ear_inner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ear_lobe = lv_obj_create(ear);
    lv_obj_remove_style_all(ear_lobe);
    lv_obj_set_size(ear_lobe, 4, 4);
    lv_obj_set_style_radius(ear_lobe, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ear_lobe, color(COLOR_XIAOZHI), 0);
    lv_obj_set_style_bg_opa(ear_lobe, LV_OPA_COVER, 0);
    lv_obj_align(ear_lobe, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_remove_flag(ear_lobe, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *listen_text = make_label(s_xiaozhi_listen_badge, "聆听中",
                                      &ui_font_cjk_20, COLOR_XIAOZHI);
    lv_obj_set_width(listen_text, 82);
    lv_obj_set_style_text_align(listen_text, LV_TEXT_ALIGN_CENTER, 0);
    s_xiaozhi_status_pill = lv_obj_create(content);
    lv_obj_set_size(s_xiaozhi_status_pill, 252, 56);
    lv_obj_set_style_radius(s_xiaozhi_status_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_xiaozhi_status_pill, color(0x05090A), 0);
    lv_obj_set_style_bg_opa(s_xiaozhi_status_pill, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_xiaozhi_status_pill, 0, 0);
    lv_obj_set_style_pad_row(s_xiaozhi_status_pill, 3, 0);
    lv_obj_set_style_pad_all(s_xiaozhi_status_pill, 7, 0);
    lv_obj_set_layout(s_xiaozhi_status_pill, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_xiaozhi_status_pill, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_xiaozhi_status_pill, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_xiaozhi_status_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_xiaozhi_status_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_xiaozhi_status_pill, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_add_flag(s_xiaozhi_status_pill, LV_OBJ_FLAG_HIDDEN);

    /* Row 1: connection dot, session state, current expression. */
    lv_obj_t *status_row = lv_obj_create(s_xiaozhi_status_pill);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, 236, 20);
    lv_obj_set_layout(status_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 6, 0);
    lv_obj_remove_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(status_row, LV_OBJ_FLAG_CLICKABLE);
    s_xiaozhi_status_dot = lv_obj_create(status_row);
    lv_obj_remove_style_all(s_xiaozhi_status_dot);
    lv_obj_set_size(s_xiaozhi_status_dot, 8, 8);
    lv_obj_set_style_radius(s_xiaozhi_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_xiaozhi_status_dot, LV_OPA_COVER, 0);
    s_xiaozhi_status = make_label(status_row, "未连接", &ui_font_cjk_20, COLOR_MUTED);
    lv_obj_set_width(s_xiaozhi_status, 80);
    lv_label_set_long_mode(s_xiaozhi_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_xiaozhi_status, LV_TEXT_ALIGN_CENTER, 0);
    s_xiaozhi_emotion = make_label(status_row, "等待配对", &ui_font_cjk_20, COLOR_TEXT);
    lv_obj_set_width(s_xiaozhi_emotion, 148);
    lv_label_set_long_mode(s_xiaozhi_emotion, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_xiaozhi_emotion, LV_TEXT_ALIGN_CENTER, 0);

    /* Row 2: clock and server hint. */
    lv_obj_t *server_row = lv_obj_create(s_xiaozhi_status_pill);
    lv_obj_remove_style_all(server_row);
    lv_obj_set_size(server_row, 236, 20);
    lv_obj_set_layout(server_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(server_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(server_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(server_row, 6, 0);
    lv_obj_remove_flag(server_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(server_row, LV_OBJ_FLAG_CLICKABLE);
    s_time_xiaozhi = make_label(server_row, "--:--", &lv_font_montserrat_20, COLOR_MUTED);
    lv_obj_set_width(s_time_xiaozhi, 44);
    lv_label_set_long_mode(s_time_xiaozhi, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_time_xiaozhi, LV_TEXT_ALIGN_CENTER, 0);
    s_xiaozhi_server = make_label(server_row, "等待服务连接", &ui_font_cjk_20, COLOR_MUTED);
    lv_obj_set_width(s_xiaozhi_server, 186);
    lv_label_set_long_mode(s_xiaozhi_server, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_xiaozhi_server, LV_TEXT_ALIGN_CENTER, 0);
}



/* Touch feedback shows the greeting face briefly; the expression itself owns
   its animation length, this only bounds the touch-source hold window. */
#define XIAOZHI_TOUCH_HOLD_MS 900u

static bool xiaozhi_touch_active(void)
{
    return s_xiaozhi_touch_started != 0 &&
           lv_tick_elaps(s_xiaozhi_touch_started) < XIAOZHI_TOUCH_HOLD_MS;
}







static const char *business_emotion_id(void)
{
    if (!s_snapshot.codex_enabled) {
        return NULL;
    }
    switch (s_snapshot.status) {
    case CODEX_STATUS_WORKING:   return "30"; /* coding 处理任务 */
    case CODEX_STATUS_WAITING:   return "32"; /* waiting 等待输入 */
    case CODEX_STATUS_COMPLETED: return "33"; /* success 完成 */
    case CODEX_STATUS_FAILED:    return "34"; /* error 出错 */
    case CODEX_STATUS_IDLE:      return "00"; /* idle 待机 */
    case CODEX_STATUS_OFFLINE:   return "38"; /* offline 离线 */
    default:                     return NULL;
    }
}

static const char *touch_emotion_id(void)
{
    if (!xiaozhi_touch_active()) {
        return NULL;
    }
    return "26"; /* greeting 打招呼 */
}

static void xiaozhi_engine_ensure(void)
{
    if (s_xiaozhi_engine_ready) {
        return;
    }
    dotii_engine_init(&s_xiaozhi_engine, lv_tick_get());
    emotion_router_init(&s_xiaozhi_router, &s_xiaozhi_engine);
    emotion_mood_init(&s_xiaozhi_mood, lv_tick_get());
    (void)emotion_mood_store_load(&s_xiaozhi_mood);
    s_xiaozhi_mood_user_revision = s_xiaozhi.user_text_revision;
    s_xiaozhi_mood_assistant_revision = s_xiaozhi.assistant_text_revision;
    s_xiaozhi_mood_server_revision = s_xiaozhi.server_emotion_revision;
    s_xiaozhi_engine_ready = true;
}

static void xiaozhi_sync_config(void)
{
    xiaozhi_config_t config = {0};
    strlcpy(config.server_url, s_snapshot.xiaozhi_server_url,
            sizeof(config.server_url));
    strlcpy(config.wake_word,
            s_snapshot.xiaozhi_wake_word[0] ? s_snapshot.xiaozhi_wake_word : "你好小智",
            sizeof(config.wake_word));
    /* The device registers the wake words through MultiNet, which takes the
       whole ';' separated pinyin list. Without this the recogniser starts with
       an empty vocabulary and falls back to WakeNet. */
    strlcpy(config.wake_words, s_snapshot.xiaozhi_wake_word,
            sizeof(config.wake_words));
    strlcpy(config.voice,
            s_snapshot.xiaozhi_voice[0] ? s_snapshot.xiaozhi_voice : "default",
            sizeof(config.voice));
    config.mcp_enabled = s_snapshot.xiaozhi_mcp_enabled;
    strlcpy(config.mcp_url, s_snapshot.xiaozhi_mcp_url, sizeof(config.mcp_url));

    ESP_LOGI(TAG, "xiaozhi config: mcp=%d mcp_url=%s", (int)config.mcp_enabled,
             config.mcp_url[0] ? "set" : "EMPTY");

    if (s_xiaozhi_request_lock == NULL) return;
    if (xSemaphoreTake(s_xiaozhi_request_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    /* A paired device must keep working when the management center is
       temporarily unreachable. Only an explicit disabled module from a valid
       snapshot should suppress the cached Xiaozhi session. */
    s_xiaozhi_want_enabled = s_snapshot.valid ? s_snapshot.xiaozhi_enabled : true;
    s_xiaozhi_want_config = config;
    xSemaphoreGive(s_xiaozhi_request_lock);
}

/* The wake word, voice and MCP flag travel in the websocket hello, so a change
   to any of them needs a fresh session rather than a live field update. */
static bool xiaozhi_config_differs(const xiaozhi_config_t *left, const xiaozhi_config_t *right)
{
    return strcmp(left->server_url, right->server_url) != 0 ||
           strcmp(left->wake_word, right->wake_word) != 0 ||
           strcmp(left->voice, right->voice) != 0 ||
           strcmp(left->mcp_url, right->mcp_url) != 0 ||
           left->mcp_enabled != right->mcp_enabled;
}

static void xiaozhi_supervisor_task(void *argument)
{
    (void)argument;
    for (;;) {
        bool want_enabled = false;
        xiaozhi_config_t want_config = {0};
        if (s_xiaozhi_request_lock != NULL &&
            xSemaphoreTake(s_xiaozhi_request_lock, portMAX_DELAY) == pdTRUE) {
            want_enabled = s_xiaozhi_want_enabled;
            want_config = s_xiaozhi_want_config;
            xSemaphoreGive(s_xiaozhi_request_lock);
        }

        const bool want_run = want_enabled;
        static uint32_t s_last_sup_log;
        const uint32_t sup_now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if ((uint32_t)(sup_now - s_last_sup_log) >= 10000u) {
            s_last_sup_log = sup_now;
            ESP_LOGI(TAG, "sup: want_run=%d running=%d cur_mcp_url=%d want_mcp_url=%d differ=%d",
                     (int)want_run, (int)s_xiaozhi.running,
                     s_xiaozhi.config.mcp_url[0] ? 1 : 0,
                     want_config.mcp_url[0] ? 1 : 0,
                     (int)xiaozhi_config_differs(&s_xiaozhi.config, &want_config));
        }
        /* Starting the websocket before the station has an address only
           produces a DNS failure and a five second reconnect timer. Wait for
           the link; the loop retries every two hundred milliseconds. */
        const bool network_ready = connectivity_is_wifi_connected();
        if (want_run && network_ready && !s_xiaozhi.running) {
            ESP_LOGI(TAG, "sup: starting xiaozhi session");
            xiaozhi_client_configure(&s_xiaozhi, &want_config);
            xiaozhi_client_start(&s_xiaozhi);
        } else if (!want_run && s_xiaozhi.running) {
            xiaozhi_client_stop(&s_xiaozhi);
        } else if (want_run && s_xiaozhi.running &&
                   xiaozhi_config_differs(&s_xiaozhi.config, &want_config)) {
            ESP_LOGI(TAG, "sup: config changed, restarting xiaozhi session");
            xiaozhi_client_stop(&s_xiaozhi);
            xiaozhi_client_configure(&s_xiaozhi, &want_config);
            xiaozhi_client_start(&s_xiaozhi);
        }

        /* Built-in MCP tool calls arrive on the conversation socket, so no
           extra channel needs reconciling here. */


        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ---- human-like fatigue cycle -------------------------------------------
   A cartoon person gets sleepy after a long awake stretch, yawns, falls
   asleep, and wakes up when spoken to or touched. Night time shortens the
   awake window so the character behaves like a real person's daily rhythm. */
#define XZ_FATIGUE_NIGHT_LIMIT_MS 120000u  /* ~2 min awake at night */
#define XZ_FATIGUE_DAY_LIMIT_MS   480000u  /* ~8 min awake by day */
#define XZ_YAWN_MS                 3200u
#define XZ_WAKING_MS               1800u

typedef enum {
    XZ_BODY_AWAKE = 0,
    XZ_BODY_YAWN,
    XZ_BODY_ASLEEP,
    XZ_BODY_WAKING,
} xz_body_t;

static xz_body_t s_body_state = XZ_BODY_AWAKE;
static uint32_t s_body_state_ms;
static uint32_t s_body_awake_since;
static bool s_body_activity_pending;

static void xiaozhi_face_activity(void)
{
    s_body_activity_pending = true;
}

static const char *xiaozhi_body_expression(uint32_t now)
{
    if (s_body_activity_pending) {
        s_body_activity_pending = false;
        if (s_body_state == XZ_BODY_ASLEEP) {
            s_body_state = XZ_BODY_WAKING;
            s_body_state_ms = now;
        } else if (s_body_state == XZ_BODY_YAWN) {
            s_body_state = XZ_BODY_AWAKE;
            s_body_awake_since = now;
        } else if (s_body_state == XZ_BODY_AWAKE) {
            s_body_awake_since = now;
        }
    }

    switch (s_body_state) {
    case XZ_BODY_ASLEEP:
        return "02"; /* sleep: closed eyes + drifting Zzz */
    case XZ_BODY_WAKING:
        if (now - s_body_state_ms >= XZ_WAKING_MS) {
            s_body_state = XZ_BODY_AWAKE;
            s_body_awake_since = now;
        }
        return "01"; /* wake: eyes fly open */
    case XZ_BODY_YAWN:
        if (now - s_body_state_ms >= XZ_YAWN_MS) {
            s_body_state = XZ_BODY_ASLEEP;
            s_body_state_ms = now;
        }
        return "24"; /* tired: drooping lids before sleep */
    case XZ_BODY_AWAKE:
    default: {
        time_t wall = time(NULL);
        struct tm local;
        localtime_r(&wall, &local);
        const bool night = local.tm_hour >= 22 || local.tm_hour < 6;
        const uint32_t limit = night ? XZ_FATIGUE_NIGHT_LIMIT_MS
                                     : XZ_FATIGUE_DAY_LIMIT_MS;
        if (now - s_body_awake_since >= limit) {
            s_body_state = XZ_BODY_YAWN;
            s_body_state_ms = now;
            return "24";
        }
        return NULL; /* normal idle / mood */
    }
    }
}

static void render_xiaozhi_expression(void)
{
    if (s_xiaozhi_page == NULL || !s_xiaozhi_render.attached ||
        (s_current != s_xiaozhi_page && s_current != NULL)) return;


    if (s_xiaozhi_face_needs_full) {
        s_xiaozhi_face_needs_full = false;
        dotii_render_invalidate_all(&s_xiaozhi_render);
    }
    xiaozhi_engine_ensure();

    const uint32_t now = lv_tick_get();
    const char *touch = touch_emotion_id();
    if (touch != NULL) {
        emotion_router_set_source(&s_xiaozhi_router, EMOTION_SRC_TOUCH, touch,
                                  XIAOZHI_TOUCH_HOLD_MS, now);
    } else {
        emotion_router_clear_source(&s_xiaozhi_router, EMOTION_SRC_TOUCH, now);
    }

    const char *business = business_emotion_id();
    if (business != NULL) {
        /* A live Codex turn is activity: stay awake and show the working face
           instead of dozing off mid-task. */
        if (strcmp(business, "30") == 0 || strcmp(business, "32") == 0) {
            xiaozhi_face_activity();
        }
        emotion_router_set_source(&s_xiaozhi_router, EMOTION_SRC_BUSINESS, business, 0, now);
    } else {
        emotion_router_clear_source(&s_xiaozhi_router, EMOTION_SRC_BUSINESS, now);
    }

    if (s_snapshot.xiaozhi_enabled) {
        /* Feed only newly arrived turn text into the local affect model.
           Reading a buffer repeatedly must not turn one compliment into
           permanently higher trust. */
        static float s_saved_positive_bias;
        static float s_saved_correction_bias;
        if (s_xiaozhi.last_user_text[0] != '\0' &&
            s_xiaozhi.user_text_revision != s_xiaozhi_mood_user_revision) {
            /* Playback is driven solely by the MCP music tools. A second local
               auto-start here raced the model's own tool call: the song began
               immediately and was then interrupted by the assistant asking
               which version to play. */
            emotion_mood_ingest_user(&s_xiaozhi_mood, s_xiaozhi.last_user_text, now);
            s_xiaozhi_mood_user_revision = s_xiaozhi.user_text_revision;
            const float positive_bias = emotion_mood_positive_bias(&s_xiaozhi_mood);
            const float correction_bias = emotion_mood_correction_bias(&s_xiaozhi_mood);
            if (positive_bias != s_saved_positive_bias ||
                correction_bias != s_saved_correction_bias) {
                if (emotion_mood_store_save(&s_xiaozhi_mood)) {
                    s_saved_positive_bias = positive_bias;
                    s_saved_correction_bias = correction_bias;
                }
            }
        }
        if (s_xiaozhi.last_assistant_text[0] != '\0' &&
            s_xiaozhi.assistant_text_revision != s_xiaozhi_mood_assistant_revision) {
            emotion_mood_ingest_assistant(&s_xiaozhi_mood, s_xiaozhi.last_assistant_text, now);
            s_xiaozhi_mood_assistant_revision = s_xiaozhi.assistant_text_revision;
        }
        const char *server_emotion = xiaozhi_client_last_emotion(&s_xiaozhi);
        if (server_emotion != NULL &&
            s_xiaozhi.server_emotion_revision != s_xiaozhi_mood_server_revision) {
            emotion_mood_ingest_server_emotion(&s_xiaozhi_mood, server_emotion, now);
            s_xiaozhi_mood_server_revision = s_xiaozhi.server_emotion_revision;
        }
        emotion_mood_tick(&s_xiaozhi_mood, now);

        const xiaozhi_session_state_t session = xiaozhi_client_state(&s_xiaozhi);
        server_emotion = xiaozhi_client_last_emotion(&s_xiaozhi);
        const char *xiaozhi_emotion = NULL;

        if (session == XIAOZHI_STATE_SPEAKING) {
            const bool entering = s_xz_face_session != XIAOZHI_STATE_SPEAKING;
            if (entering) {
                /* Turn start: server emotion > text intent > plain speaking. */
                const char *initial = xiaozhi_client_emotion_policy(
                    session, server_emotion, s_xiaozhi.last_text);
                if (initial != NULL) {
                    s_xz_turn_emotion[0] = initial[0];
                    s_xz_turn_emotion[1] = initial[1];
                    s_xz_turn_emotion_ms = now;
                }
                s_xz_handled_emotion_rev = s_xiaozhi.server_emotion_revision;
                s_xz_turn_server_applied = server_emotion != NULL;
                s_xz_afterglow_until_ms = 0;
            } else if (s_xiaozhi.server_emotion_revision != s_xz_handled_emotion_rev) {
                s_xz_handled_emotion_rev = s_xiaozhi.server_emotion_revision;
                /* Only a NEW server emotion is a semantic cue. The 5 s expiry of
                   the previous emotion (NULL with unchanged revision) never
                   reaches this branch, so sentences no longer flicker. */
                if (server_emotion != NULL &&
                    dotii_catalog_by_id(server_emotion) != NULL &&
                    (!s_xz_turn_server_applied ||
                     now - s_xz_turn_emotion_ms >= XIAOZHI_SERVER_EMOTION_MIN_GAP_MS)) {
                    s_xz_turn_emotion[0] = server_emotion[0];
                    s_xz_turn_emotion[1] = server_emotion[1];
                    s_xz_turn_emotion_ms = now;
                    s_xz_turn_server_applied = true;
                }
            }
            xiaozhi_emotion = s_xz_turn_emotion[0] ? s_xz_turn_emotion : "05";
        } else if (session == XIAOZHI_STATE_ACTIVATING ||
                   session == XIAOZHI_STATE_CONNECTING ||
                   session == XIAOZHI_STATE_LISTENING ||
                   session == XIAOZHI_STATE_THINKING ||
                   session == XIAOZHI_STATE_MUSIC ||
                   session == XIAOZHI_STATE_ERROR) {
            /* Transport states carry their own meaning, and being spoken to
               keeps the character awake. */
            xiaozhi_face_activity();
            xiaozhi_emotion = xiaozhi_client_emotion_policy(
                session, server_emotion, s_xiaozhi.last_text);
            if (session == XIAOZHI_STATE_ERROR && xiaozhi_emotion != NULL &&
                !(xiaozhi_emotion[0] == '3' && xiaozhi_emotion[1] == '4')) {
                xiaozhi_emotion = "34";
            }
            s_xz_afterglow_until_ms = 0;
        } else {
            /* Idle: the body cycle (awake -> yawn -> asleep -> waking) has the
               strongest say. Otherwise hold the finished turn's emotion briefly
               (a meaningful afterglow), then let the mood model drive subtle
               presence. */
            const char *body = xiaozhi_body_expression(now);
            if (body != NULL) {
                xiaozhi_emotion = body;
            } else if (s_xz_face_session == XIAOZHI_STATE_SPEAKING &&
                       s_xz_turn_emotion[0] != '\0') {
                s_xz_afterglow_until_ms = now + XIAOZHI_SPEAK_AFTERGLOW_MS;
                xiaozhi_emotion = s_xz_turn_emotion;
            } else if ((int32_t)(s_xz_afterglow_until_ms - now) > 0 &&
                       s_xz_turn_emotion[0] != '\0') {
                xiaozhi_emotion = s_xz_turn_emotion;
            } else {
                s_xz_afterglow_until_ms = 0;
                /* Passive mood drift must not invent expressions: at idle the
                   face only reflects a mood that came from a recent real event
                   (user text / server emotion); otherwise it just idles. */
                if (emotion_mood_has_active_event(&s_xiaozhi_mood)) {
                    xiaozhi_emotion = emotion_mood_current_id(&s_xiaozhi_mood);
                }
                if (xiaozhi_emotion == NULL) xiaozhi_emotion = "00";
            }
        }
        s_xz_face_session = session;

        if (xiaozhi_emotion != NULL) {
            emotion_router_set_source(&s_xiaozhi_router, EMOTION_SRC_XIAOZHI,
                                      xiaozhi_emotion, 0, now);
        } else {
            emotion_router_clear_source(&s_xiaozhi_router, EMOTION_SRC_XIAOZHI, now);
        }
    } else {
        emotion_router_clear_source(&s_xiaozhi_router, EMOTION_SRC_XIAOZHI, now);
    }
    emotion_router_tick(&s_xiaozhi_router, now);

    /* The Dotii face ignores the legacy character picker; management may still
       send character_id but it no longer changes the look. */
    const char *active = emotion_router_active_id(&s_xiaozhi_router);
    static char s_last_expression[3];
    if (strcmp(active, s_last_expression) != 0) {
        memcpy(s_last_expression, active, 3);
        const dotii_expr_t *logged = dotii_catalog_by_id(active);
        ESP_LOGI(TAG, "xiaozhi expression -> %s %s", active, logged ? logged->name : "?");
    }
    const dotii_expr_t *expr = dotii_catalog_by_id(active);
    if (expr == NULL) expr = dotii_catalog_idle();
    (void)dotii_engine_set(&s_xiaozhi_engine, active, now);
    (void)dotii_engine_tick(&s_xiaozhi_engine, now);
    dotii_render_update(&s_xiaozhi_render, expr, &s_xiaozhi_engine.state.pose);
}
static void xiaozhi_listen_hold(void)
{
    if (!s_snapshot.xiaozhi_enabled || !s_xiaozhi.running) return;
    s_xiaozhi_touch_started = lv_tick_get();
    if (s_xiaozhi_touch_started == 0) s_xiaozhi_touch_started = 1;
    s_xiaozhi_touch_listening = xiaozhi_client_send_listen(&s_xiaozhi, true);
    render_xiaozhi_expression();
}

static void xiaozhi_listen_release(void)
{
    if (s_xiaozhi_touch_listening) {
        xiaozhi_client_send_listen(&s_xiaozhi, false);
        s_xiaozhi_touch_listening = false;
    }
}

static void xiaozhi_touch_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED ||
        code == LV_EVENT_RELEASED || code == LV_EVENT_SHORT_CLICKED) {
        xiaozhi_face_activity();
    }
    if (code == LV_EVENT_LONG_PRESSED) {
        xiaozhi_listen_hold();
    } else if (code == LV_EVENT_RELEASED) {
        xiaozhi_listen_release();
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        s_xiaozhi_touch_started = lv_tick_get();
        if (s_xiaozhi_touch_started == 0) s_xiaozhi_touch_started = 1;
        render_xiaozhi_expression();
    }
}


static void xiaozhi_face_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!s_screen_load_pending &&
        xiaozhi_client_take_wake_event(&s_xiaozhi)) {
        if (s_screen_saver_active) {
            s_screen_saver_active = false;
            s_before_screen_saver = NULL;
        }
        screen_wake();
        s_last_activity = lv_tick_get();
        if (s_current != s_xiaozhi_page) {
            s_voice_wake_transition_pending = true;
            load_screen(s_xiaozhi_page, true);
        } else {
            ESP_LOGI(TAG, "voice wake: Xiaozhi face already active");
        }
        xiaozhi_face_activity();
        xiaozhi_listen_badge_set_visible(true);
        render_xiaozhi_expression();
        return;
    }
    if (s_current == s_xiaozhi_page && s_screen_on) {
        xiaozhi_listen_badge_set_visible(
            xiaozhi_client_state(&s_xiaozhi) == XIAOZHI_STATE_LISTENING);
        render_xiaozhi_expression();
    }
}


static lv_obj_t *make_quick_button(lv_obj_t *parent,
                                   const lv_image_dsc_t *icon_source, uint32_t accent)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 78, 78);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_bg_color(button, color(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, color(accent), 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    if (icon_source != NULL) {
        lv_obj_t *icon = lv_image_create(button);
        lv_image_set_src(icon, icon_source);
        lv_obj_set_style_image_recolor(icon, color(accent), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_center(icon);
    } else {
        lv_obj_t *icon = lv_obj_create(button);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, 36, 36);
        lv_obj_center(icon);
        static const int16_t positions[4][2] = {{1, 1}, {20, 1}, {1, 20}, {20, 20}};
        for (size_t i = 0; i < 4; ++i) {
            lv_obj_t *tile = lv_obj_create(icon);
            lv_obj_remove_style_all(tile);
            lv_obj_set_size(tile, 15, 15);
            lv_obj_set_pos(tile, positions[i][0], positions[i][1]);
            lv_obj_set_style_border_width(tile, 3, 0);
            lv_obj_set_style_border_color(tile, color(accent), 0);
            lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(tile, 4, 0);
        }
    }
    return button;
}

static void build_control(void)
{
    s_control = lv_obj_create(NULL);
    set_screen_background(s_control);
    add_activity_event(s_control);
    lv_obj_add_event_cb(s_control, swipe_event, LV_EVENT_GESTURE, NULL);
    lv_obj_t *safe = make_safe(s_control);
    make_centered_title(safe, "控制中心", &s_ui_font, 200, 346, 2, 7);

    lv_obj_t *brightness = make_label(safe, "屏幕亮度", &s_ui_font, COLOR_MUTED);
    lv_obj_set_style_transform_scale(brightness, 282, 0);
    lv_obj_set_pos(brightness, 12, 59);
    s_brightness_slider = lv_slider_create(safe);
    lv_obj_set_size(s_brightness_slider, 330, 22);
    lv_obj_set_pos(s_brightness_slider, 14, 96);
    lv_slider_set_range(s_brightness_slider, 10, 100);
    lv_slider_set_value(s_brightness_slider, s_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_brightness_slider, color(0x272D2C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_brightness_slider, color(COLOR_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_brightness_slider, color(0xE9FFF7), LV_PART_KNOB);
    lv_obj_add_event_cb(s_brightness_slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_volume_label = make_label(safe, "音量", &s_ui_font, COLOR_MUTED);
    lv_obj_set_style_transform_scale(s_volume_label, 282, 0);
    lv_obj_set_pos(s_volume_label, 12, 131);
    set_label_text_fmt_cached(s_volume_label, "音量 %u%%",
                          (unsigned)xiaozhi_client_get_volume(&s_xiaozhi));
    s_volume_slider = lv_slider_create(safe);
    lv_obj_set_size(s_volume_slider, 330, 22);
    lv_obj_set_pos(s_volume_slider, 14, 168);
    lv_slider_set_range(s_volume_slider, 0, 100);
    lv_slider_set_value(s_volume_slider,
                        xiaozhi_client_get_volume(&s_xiaozhi), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_volume_slider, color(0x272D2C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_volume_slider, color(COLOR_XIAOZHI), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_volume_slider, color(0xE9FFF7), LV_PART_KNOB);
    lv_obj_add_event_cb(s_volume_slider, volume_changed, LV_EVENT_ALL, NULL);

    lv_obj_t *row = lv_obj_create(safe);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 340, 82);
    lv_obj_set_pos(row, 9, 210);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 9, 0);
    s_codex_quick = make_quick_button(row, &ui_icon_openai_36, COLOR_BLUE);
    s_xiaozhi_quick = make_quick_button(row, &ui_icon_openai_36, COLOR_XIAOZHI);
    lv_obj_set_style_pad_column(row, 32, 0);
    lv_obj_add_event_cb(s_codex_quick, quick_clicked, LV_EVENT_CLICKED, s_main);
    lv_obj_add_event_cb(s_xiaozhi_quick, quick_clicked, LV_EVENT_CLICKED, s_xiaozhi_page);

    s_battery_label = make_label(safe, "电量 --", &s_ui_font, COLOR_MUTED);
    lv_obj_set_width(s_battery_label, 300);
    lv_label_set_long_mode(s_battery_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_battery_label, LV_ALIGN_BOTTOM_MID, 0, -5);
}

static lv_obj_t *make_setting_card(lv_obj_t *parent, const char *title, const char *detail)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, 88);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_bg_color(card, color(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title_label = make_label(card, title, &ui_font_cjk_20, COLOR_TEXT);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *detail_label = make_label(card, detail, &ui_font_cjk_20, COLOR_MUTED);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_label, LV_PCT(100));
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_LEFT, 0);
    return detail_label;
}

static void build_settings(void)
{
    s_settings = lv_obj_create(NULL);
    set_screen_background(s_settings);
    add_activity_event(s_settings);
    lv_obj_add_event_cb(s_settings, swipe_event, LV_EVENT_GESTURE, NULL);
    lv_obj_t *safe = make_safe(s_settings);
    make_centered_title(safe, "设置", &ui_font_cjk_20, 336, 256, 1, -2);

    s_settings_list = lv_obj_create(safe);
    lv_obj_remove_style_all(s_settings_list);
    lv_obj_set_size(s_settings_list, 334, 310);
    lv_obj_align(s_settings_list, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_flex_flow(s_settings_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_settings_list, 10, 0);
    lv_obj_set_scroll_dir(s_settings_list, LV_DIR_VER);

    lv_obj_t *refresh = lv_button_create(s_settings_list);
    lv_obj_set_width(refresh, LV_PCT(100));
    lv_obj_set_height(refresh, 54);
    lv_obj_set_style_radius(refresh, 18, 0);
    lv_obj_set_style_bg_color(refresh, color(COLOR_BLUE_DARK), 0);
    lv_obj_t *refresh_text = make_label(refresh, "立即刷新数据", &ui_font_cjk_20, COLOR_TEXT);
    lv_obj_center(refresh_text);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);

    s_settings_wifi = make_setting_card(s_settings_list, "网络", "未配置");
    s_settings_ip = make_setting_card(s_settings_list, "设备地址", "IP --");
    s_settings_bridge = make_setting_card(s_settings_list, "Codex 数据源", "未配置");
    char device_info[72];
    snprintf(device_info, sizeof(device_info), "Dotii %s · ESP-IDF %s",
             esp_app_get_description()->version, esp_get_idf_version());
    make_setting_card(s_settings_list, "设备", device_info);

    s_settings_reset_button = lv_button_create(s_settings_list);
    lv_obj_remove_style_all(s_settings_reset_button);
    lv_obj_set_width(s_settings_reset_button, LV_PCT(100));
    lv_obj_set_height(s_settings_reset_button, 88);
    lv_obj_set_style_radius(s_settings_reset_button, 20, 0);
    lv_obj_set_style_bg_color(s_settings_reset_button, color(0x321A1A), 0);
    lv_obj_set_style_bg_color(s_settings_reset_button, color(0x4A2422), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_settings_reset_button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_settings_reset_button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_settings_reset_button, 0, 0);
    lv_obj_set_style_pad_all(s_settings_reset_button, 12, 0);
    lv_obj_set_flex_flow(s_settings_reset_button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_settings_reset_button, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_settings_reset_button, 4, 0);
    lv_obj_remove_flag(s_settings_reset_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *reset_title = make_label(s_settings_reset_button, "重置配网", &ui_font_cjk_20, COLOR_DANGER);
    lv_obj_set_width(reset_title, LV_PCT(100));
    lv_label_set_long_mode(reset_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(reset_title, LV_TEXT_ALIGN_LEFT, 0);
    s_settings_reset_detail = make_label(s_settings_reset_button,
                                         "长按 1.2 秒，清除配网信息",
                                         &ui_font_cjk_20, COLOR_MUTED);
    lv_obj_set_width(s_settings_reset_detail, LV_PCT(100));
    lv_label_set_long_mode(s_settings_reset_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_settings_reset_detail, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_event_cb(s_settings_reset_button, provisioning_reset_event, LV_EVENT_ALL, NULL);

    register_page_scroll_target(s_settings, s_settings_list);

}

static void build_power(void)
{
    s_power = lv_obj_create(NULL);
    set_screen_background(s_power);
    add_activity_event(s_power);
    lv_obj_add_event_cb(s_power, swipe_event, LV_EVENT_GESTURE, NULL);
    lv_obj_t *safe = make_safe(s_power);
    make_centered_title(safe, "电源", &s_ui_font, 200, 346, 2, 7);

    lv_obj_t *restart = lv_button_create(safe);
    lv_obj_set_size(restart, 156, 132);
    lv_obj_align(restart, LV_ALIGN_CENTER, -86, 0);
    lv_obj_set_style_radius(restart, 28, 0);
    lv_obj_set_style_bg_color(restart, color(0x173D2D), 0);
    lv_obj_set_style_bg_color(restart, color(0x214F3A), LV_STATE_PRESSED);
    lv_obj_t *restart_icon = make_label(restart, LV_SYMBOL_REFRESH, &lv_font_montserrat_32, COLOR_GREEN);
    lv_obj_align(restart_icon, LV_ALIGN_CENTER, 0, -24);
    lv_obj_t *restart_text = make_label(restart, "重新启动", &ui_font_cjk_20, COLOR_GREEN);
    lv_obj_set_width(restart_text, 132);
    lv_obj_set_style_text_align(restart_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(restart_text, LV_ALIGN_CENTER, 0, 24);
    lv_obj_add_event_cb(restart, restart_event, LV_EVENT_ALL, NULL);

    lv_obj_t *shutdown = lv_button_create(safe);
    lv_obj_set_size(shutdown, 156, 132);
    lv_obj_align(shutdown, LV_ALIGN_CENTER, 86, 0);
    lv_obj_set_style_radius(shutdown, 28, 0);
    lv_obj_set_style_bg_color(shutdown, color(0x3A1D1D), 0);
    lv_obj_set_style_bg_color(shutdown, color(0x572725), LV_STATE_PRESSED);
    lv_obj_t *shutdown_icon = make_label(shutdown, LV_SYMBOL_POWER, &lv_font_montserrat_32, COLOR_DANGER);
    lv_obj_align(shutdown_icon, LV_ALIGN_CENTER, 0, -24);
    lv_obj_t *shutdown_text = make_label(shutdown, "关机", &ui_font_cjk_20, COLOR_DANGER);
    lv_obj_set_width(shutdown_text, 132);
    lv_obj_set_style_text_align(shutdown_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(shutdown_text, LV_ALIGN_CENTER, 0, 24);
    lv_obj_add_event_cb(shutdown, shutdown_event, LV_EVENT_ALL, NULL);

    lv_obj_t *hint = make_label(safe, "按住 1.2 秒确认\n向右滑动取消", &ui_font_cjk_20, COLOR_MUTED);
    lv_obj_set_width(hint, 330);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(hint, 6, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
}

/* A finished task raises the Bridge's "completed unread" counter, but that
   counter oscillates while other clients read the same result
   (2 -> 3 -> 2 -> 3), so a plain "greater than last" test rang several times
   within a few seconds. The OnePlus passport device solves the same problem
   with a settle window: a rise arms a pending alert and the alert only rings
   when the counter is still elevated after four seconds. A genuine
   completion stays unread, while the oscillation falls back within a second
   and disarms the alert again. */
#define CODEX_ALERT_SETTLE_MS 4000

static bool s_alert_seeded;
static uint16_t s_alert_last_completed;
static uint16_t s_alert_last_failed;
static int64_t s_alert_armed_ms;
static uint16_t s_alert_armed_completed;
static uint16_t s_alert_armed_failed;
static bool s_alert_armed_is_failure;

static void codex_alert_update(const codex_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->valid || !snapshot->codex_state_raw_valid) {
        return;
    }
    const uint16_t completed = snapshot->completed_unread_tasks;
    const uint16_t failed = snapshot->failed_unread_tasks;
    if (snapshot->codex_state_baseline) {
        /* A replayed frame only re-establishes the reference counters, so it
           must not ring. */
        s_alert_seeded = true;
        s_alert_last_completed = completed;
        s_alert_last_failed = failed;
        s_alert_armed_ms = 0;
        return;
    }
    if (!s_alert_seeded) {
        s_alert_seeded = true;
        s_alert_last_completed = completed;
        s_alert_last_failed = failed;
        return;
    }
    const bool rose_completed = completed > s_alert_last_completed;
    const bool rose_failed = failed > s_alert_last_failed;
    s_alert_last_completed = completed;
    s_alert_last_failed = failed;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (rose_completed || rose_failed) {
        /* Arm the alert and wait for the counters to settle. */
        s_alert_armed_ms = now_ms;
        s_alert_armed_completed = completed;
        s_alert_armed_failed = failed;
        s_alert_armed_is_failure = rose_failed;
        return;
    }
    if (s_alert_armed_ms == 0) return;
    if (completed < s_alert_armed_completed || failed < s_alert_armed_failed) {
        /* The counters fell back: another client read the result, so this was
           not a new task. */
        s_alert_armed_ms = 0;
        return;
    }
    if (now_ms - s_alert_armed_ms >= CODEX_ALERT_SETTLE_MS) {
        const bool is_failure = s_alert_armed_is_failure;
        s_alert_armed_ms = 0;
        xiaozhi_client_notify_task_completed(&s_xiaozhi, is_failure);
    }
}

static void update_snapshot(const codex_snapshot_t *snapshot)
{
    /* Follow the Bridge's authoritative counters, exactly like the OnePlus
       passport device does.  The Bridge computes the globally unread
       results, so a result that is read on the phone or the desktop
       lowers the number again.  A device-side "only ever grows" total
       would latch the badge forever, which is the "the number is never
       right" complaint.  The monotonic totals stay in the snapshot: the
       completion chime uses them, because another client reading the
       same result must not silence our sound notification. */
    const uint16_t completed_display = snapshot->completed_unread_tasks;
    const uint16_t failed_display = snapshot->failed_unread_tasks;
    s_snapshot = *snapshot;
    if (snapshot->docked_rotation_tenths >= DISPLAY_ANGLE_MIN_TENTHS &&
        snapshot->docked_rotation_tenths <= DISPLAY_ANGLE_MAX_TENTHS &&
        (snapshot->docked_rotation_tenths != s_display_angle_tenths ||
         snapshot->screen_off_timeout_seconds != s_screen_off_timeout_seconds ||
         snapshot->sleep_timeout_seconds != s_sleep_timeout_seconds ||
         snapshot->charging_screen_off_timeout_seconds != s_charging_screen_off_timeout_seconds ||
         snapshot->charging_sleep_timeout_seconds != s_charging_sleep_timeout_seconds ||
         snapshot->screen_off_page != s_screen_off_page ||
         snapshot->display_revision != s_display_revision)) {
        const bool timeout_changed =
            snapshot->screen_off_timeout_seconds != s_screen_off_timeout_seconds ||
            snapshot->sleep_timeout_seconds != s_sleep_timeout_seconds ||
            snapshot->charging_screen_off_timeout_seconds != s_charging_screen_off_timeout_seconds ||
            snapshot->charging_sleep_timeout_seconds != s_charging_sleep_timeout_seconds;
        const bool screen_off_page_changed = snapshot->screen_off_page != s_screen_off_page;
        s_display_angle_tenths = snapshot->docked_rotation_tenths;
        s_screen_off_timeout_seconds = snapshot->screen_off_timeout_seconds;
        s_sleep_timeout_seconds = snapshot->sleep_timeout_seconds;
        s_charging_screen_off_timeout_seconds = snapshot->charging_screen_off_timeout_seconds;
        s_charging_sleep_timeout_seconds = snapshot->charging_sleep_timeout_seconds;
        s_screen_off_page = snapshot->screen_off_page <= DISPLAY_SCREEN_OFF_PAGE_DOTII
            ? snapshot->screen_off_page : DISPLAY_SCREEN_OFF_PAGE_NONE;
        s_display_revision = snapshot->display_revision;
        request_display_settings_save();
        bsp_display_transform_set_angle(s_display_angle_tenths);
        if (timeout_changed || screen_off_page_changed) {
            if (s_screen_saver_active) exit_screen_saver();
            s_last_activity = lv_tick_get();
        }
        if (s_current != NULL) lv_obj_invalidate(s_current);
    }
    if (!snapshot->valid || snapshot->stale || !snapshot->weekly_available) {
        set_label_text_cached(s_percent, "--");
        arc_display_update(s_main_arc, 0, 0x59605E);
    } else {
        set_label_text_fmt_cached(s_percent, "%d%%", snapshot->weekly_remaining_percent);
        arc_display_update(s_main_arc, snapshot->weekly_remaining_percent, COLOR_BLUE);
    }

    /* The counters stay on the screen while the Bridge is restarting. They
       hold the last known values, and a blank panel would look like a dead
       device instead of a Bridge that is coming back. */
    const bool task_counts_available =
        snapshot->valid && snapshot->codex_state_raw_valid;
    update_task_counter(s_running_count, task_counts_available,
                        snapshot->running_tasks);
    update_task_counter(s_completed_count, task_counts_available,
                        completed_display);
    update_task_counter(s_failed_count, task_counts_available,
                        failed_display);

    if (!snapshot->weekly_tokens_available) set_label_text_cached(s_weekly_tokens, "--");
    else if (snapshot->weekly_tokens >= 1000) set_label_text_fmt_cached(s_weekly_tokens, "%.1fk", snapshot->weekly_tokens / 1000.0);
    else set_label_text_fmt_cached(s_weekly_tokens, "%lu", (unsigned long)snapshot->weekly_tokens);
    if (snapshot->weekly_available && snapshot->reset_date[0]) {
        const char *space = strchr(snapshot->reset_date, ' ');
        if (space != NULL) {
            set_label_text_fmt_cached(s_reset_day, "%.*s", (int)(space - snapshot->reset_date), snapshot->reset_date);
            set_label_text_cached(s_reset_time, space + 1);
        } else {
            set_label_text_cached(s_reset_day, snapshot->reset_date);
            set_label_text_cached(s_reset_time, "");
        }
    } else {
        set_label_text_cached(s_reset_day, "--");
        set_label_text_cached(s_reset_time, "");
    }

    if (!snapshot->valid || snapshot->stale || !snapshot->five_hour_available) {
        set_label_text_cached(s_plus_five_percent, "--");
        arc_display_update(s_plus_five_arc, 0, 0x59605E);
        set_usage_reset_text(s_plus_five_reset, "", true);
    } else {
        set_label_text_fmt_cached(s_plus_five_percent, "%d%%", snapshot->five_hour_remaining_percent);
        arc_display_update(s_plus_five_arc, snapshot->five_hour_remaining_percent, COLOR_ORANGE);
        set_usage_reset_text(s_plus_five_reset, snapshot->five_hour_reset_date, true);
    }
    if (!snapshot->valid || snapshot->stale || !snapshot->weekly_available) {
        set_label_text_cached(s_plus_weekly_percent, "--");
        arc_display_update(s_plus_weekly_arc, 0, 0x59605E);
        set_weekly_reset_text(s_plus_weekly_reset_day, s_plus_weekly_reset, "");
    } else {
        set_label_text_fmt_cached(s_plus_weekly_percent, "%d%%", snapshot->weekly_remaining_percent);
        arc_display_update(s_plus_weekly_arc, snapshot->weekly_remaining_percent, COLOR_BLUE_USAGE);
        set_weekly_reset_text(s_plus_weekly_reset_day, s_plus_weekly_reset,
                              snapshot->reset_date);
    }
    update_task_counter(s_plus_running_count, task_counts_available,
                        snapshot->running_tasks);
    update_task_counter(s_plus_completed_count, task_counts_available,
                        completed_display);
    update_task_counter(s_plus_failed_count, task_counts_available,
                        failed_display);

    const xiaozhi_session_state_t xiaozhi_session = xiaozhi_client_state(&s_xiaozhi);
    const char *xiaozhi_state = xiaozhi_client_state_name(xiaozhi_session);
    const char *xiaozhi_emotion = "等待配对";
    uint32_t xiaozhi_color = COLOR_MUTED;
    switch (xiaozhi_session) {
    case XIAOZHI_STATE_CONNECTING:
    case XIAOZHI_STATE_ACTIVATING:
        xiaozhi_color = COLOR_XIAOZHI;
        break;
    case XIAOZHI_STATE_LISTENING:
        xiaozhi_emotion = "聆听";
        xiaozhi_color = COLOR_XIAOZHI;
        break;
    case XIAOZHI_STATE_THINKING:
        xiaozhi_emotion = "思考";
        xiaozhi_color = COLOR_XIAOZHI;
        break;
    case XIAOZHI_STATE_SPEAKING:
        xiaozhi_emotion = "回应";
        xiaozhi_color = COLOR_XIAOZHI;
        break;
    case XIAOZHI_STATE_MUSIC:
        xiaozhi_emotion = "音乐律动";
        xiaozhi_color = COLOR_XIAOZHI;
        break;
    case XIAOZHI_STATE_ERROR:
        xiaozhi_emotion = "出错";
        xiaozhi_color = COLOR_DANGER;
        break;
    case XIAOZHI_STATE_IDLE:
        if (snapshot->xiaozhi_enabled) {
            xiaozhi_state = "已就绪";
            xiaozhi_color = COLOR_XIAOZHI;
        }
        break;
    default:
        break;
    }
    const char *reported_emotion = xiaozhi_client_last_emotion(&s_xiaozhi);
    if (reported_emotion != NULL) {
        const dotii_expr_t *reported_def = dotii_catalog_by_id(reported_emotion);
        if (reported_def == NULL) reported_def = dotii_catalog_by_name(reported_emotion);
        xiaozhi_emotion = reported_def != NULL ? reported_def->label : reported_emotion;
    }
    set_label_text_cached(s_xiaozhi_status, xiaozhi_state);
    set_label_text_cached(s_xiaozhi_emotion, xiaozhi_emotion);
    lv_obj_set_style_text_color(s_xiaozhi_status, color(xiaozhi_color), 0);
    lv_obj_set_style_bg_color(s_xiaozhi_status_dot, color(xiaozhi_color), 0);
    lv_obj_set_style_bg_color(s_xiaozhi_status_pill, color(xiaozhi_color), 0);
    lv_obj_set_style_arc_color(s_xiaozhi_status_pill, color(xiaozhi_color), 0);
    const char *activation_code = xiaozhi_client_activation_code(&s_xiaozhi);
    const bool xiaozhi_busy =
        xiaozhi_session == XIAOZHI_STATE_LISTENING ||
        xiaozhi_session == XIAOZHI_STATE_THINKING ||
        xiaozhi_session == XIAOZHI_STATE_SPEAKING ||
        xiaozhi_session == XIAOZHI_STATE_MUSIC;
    /* The face owns the whole panel. A normal connected session is expressed
       through the eyes, so the pill only appears when the user actually has
       something to read or act on: a pairing code or a connection error. */
    const bool xiaozhi_pill_visible =
        activation_code != NULL ||
        xiaozhi_session == XIAOZHI_STATE_ACTIVATING ||
        xiaozhi_session == XIAOZHI_STATE_ERROR;
    if (xiaozhi_pill_visible) {
        lv_obj_remove_flag(s_xiaozhi_status_pill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_xiaozhi_status_pill, LV_OBJ_FLAG_HIDDEN);
    }
    xiaozhi_listen_badge_set_visible(xiaozhi_session == XIAOZHI_STATE_LISTENING);
    if (activation_code != NULL) {
        /* Pairing needs the widest single hint row we have. */
        lv_obj_add_flag(s_time_xiaozhi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_xiaozhi_server, 236);
        set_label_text_fmt_cached(s_xiaozhi_emotion, "设备码 %s", activation_code);
        set_label_text_cached(s_xiaozhi_server, "xiaozhi.me 输入设备码");
    } else if (xiaozhi_session == XIAOZHI_STATE_ACTIVATING) {
        lv_obj_add_flag(s_time_xiaozhi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_xiaozhi_server, 236);
        set_label_text_cached(s_xiaozhi_emotion, "获取设备码中");
        set_label_text_cached(s_xiaozhi_server, s_xiaozhi.connected ?
                          "等待 xiaozhi.me 完成配对" : "正在连接小智服务");
    } else if (xiaozhi_busy) {
        lv_obj_remove_flag(s_time_xiaozhi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_xiaozhi_server, 186);
        if (xiaozhi_session == XIAOZHI_STATE_MUSIC &&
            s_xiaozhi.last_assistant_text[0] != '\0') {
            set_label_text_cached(s_xiaozhi_server, s_xiaozhi.last_assistant_text);
        } else {
            set_label_text_cached(s_xiaozhi_server, s_xiaozhi.connected ? "长按说话，松开发送" : "等待小智服务连接");
        }
    } else {
        lv_obj_remove_flag(s_time_xiaozhi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_xiaozhi_server, 186);
        set_label_text_cached(s_xiaozhi_server, "长按说话");
    }
    /* Hand the desired lifecycle to the supervisor task; starting or stopping
       the websocket here would block the LVGL worker. */
    xiaozhi_sync_config();

    if (snapshot->codex_enabled) lv_obj_remove_flag(s_codex_quick, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_codex_quick, LV_OBJ_FLAG_HIDDEN);
    if (snapshot->xiaozhi_enabled) lv_obj_remove_flag(s_xiaozhi_quick, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_xiaozhi_quick, LV_OBJ_FLAG_HIDDEN);
    render_xiaozhi_expression();
    update_page_dots();

    if ((s_current == s_main || s_current == s_plus_main) && s_current != page_screen(0)) {
        load_screen(page_screen(0), false);
    } else if (!s_screen_saver_active && !screen_enabled(s_current)) {
        load_screen(first_enabled_screen(), false);
    }
}

static void ui_timer(lv_timer_t *timer)
{
    (void)timer;
    if (xQueueReceive(s_snapshot_queue, &s_snapshot, 0) == pdTRUE) update_snapshot(&s_snapshot);
    /* The completion alert runs on the UI beat and not on the snapshot, so a
       result that stays unread still rings after the settle window even when
       no new snapshot arrived in between. */
    codex_alert_update(&s_snapshot);


    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char clock[8];
    if (local.tm_year >= 120) snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
    else strlcpy(clock, "--:--", sizeof(clock));
    set_label_text_cached(s_time_main, clock);
    set_label_text_cached(s_time_xiaozhi, clock);

    char summary[80];
    connectivity_get_summary(summary, sizeof(summary));
    set_label_text_cached(s_settings_wifi, summary);
    connectivity_get_ip(summary, sizeof(summary));
    set_label_text_fmt_cached(s_settings_ip, "IP %s", summary);
    connectivity_get_bridge_summary(summary, sizeof(summary));
    set_label_text_cached(s_settings_bridge, summary);

    uint32_t inactive_ms = lv_tick_elaps(s_last_activity);
    /* Docked means the panel is on a desk with power attached. Treat it as an
       always-visible desk device: do not enter the screen saver or blank the
       panel. Wi-Fi, BLE and CodexBridge stay resident as before. */
    const uint32_t active_screen_off_timeout = s_external_power ?
        0u : s_screen_off_timeout_seconds;
    /* Docked (USB powered) is always network-resident: the panel may still
       follow the screen-off timeout, but deep sleep stays disabled no matter
       what the stored profile or the management center asks for. Deep sleep
       drops the USB serial port and takes Wi-Fi, BLE and CodexBridge offline,
       which is never what a plugged-in desk device should do. */
    uint32_t active_sleep_timeout = s_external_power ?
        s_charging_sleep_timeout_seconds : s_sleep_timeout_seconds;
    if (s_external_power) active_sleep_timeout = 0;
    if (s_screen_on && !s_screen_saver_active && active_screen_off_timeout > 0 &&
        inactive_ms > active_screen_off_timeout * 1000U) {
        if (!enter_screen_saver()) screen_off();
    }
    if (active_sleep_timeout > 0 && inactive_ms > active_sleep_timeout * 1000U) {
        s_last_activity = lv_tick_get();
        board_input_request_sleep();
    }
}

void state_ui_start(QueueHandle_t snapshot_queue)
{
    s_snapshot_queue = snapshot_queue;
    load_persisted_settings();
    s_display_save_signal = xSemaphoreCreateBinary();
    if (s_display_save_signal != NULL) {
        if (xTaskCreate(display_settings_writer_task, "ui_nvs", 3072, NULL, 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Unable to create the display settings writer task");
        }
    }
    ESP_ERROR_CHECK(bsp_display_transform_set_angle(s_display_angle_tenths));
    s_ui_font = ui_font_fixed_20;
    s_ui_font.fallback = &ui_font_cjk_20;
    xiaozhi_client_init(&s_xiaozhi);
    s_xiaozhi_request_lock = xSemaphoreCreateMutex();
    if (s_xiaozhi_request_lock == NULL) {
        ESP_LOGE(TAG, "Unable to create the xiaozhi request lock");
    } else if (xTaskCreate(xiaozhi_supervisor_task, "xz_sup", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create the xiaozhi supervisor task");
    }
    build_main();
    build_plus_main();
    build_xiaozhi();
    build_settings();
    build_control();
    build_power();
    s_current = page_screen(0);
    s_last_activity = lv_tick_get();
    lv_obj_invalidate(s_current);
    /* Push one complete first frame while the caller still owns the BSP LVGL
       lock.  Subsequent timer updates can then use partial refresh safely. */
    lv_refr_now(NULL);
    /* One-second updates are sufficient for the clock and avoid redrawing the
       full AMOLED frame twice for the same displayed second. */
    lv_timer_create(ui_timer, 1000, NULL);
    /* 50 FPS keeps the eye motion fluid. The 90-degree transform is a
       pixel-exact copy, so the per-frame cost stays low. */
    /* 30 FPS leaves headroom for audio decode/playback on the other core. */
    lv_timer_create(xiaozhi_face_timer, 33, NULL);
}

void state_ui_button_a_short(void)
{
    if (s_screen_saver_active) {
        exit_screen_saver();
        return;
    }
    if (!s_screen_on) {
        screen_wake();
        return;
    }
    if (s_current == s_power) return;
    if (s_current == s_control) {
        load_screen_with_transition(enabled_return_screen(s_before_control), TRANSITION_CONTROL_UP);
        return;
    }
    if (s_current == s_settings) {
        load_screen(first_enabled_screen(), false);
        return;
    }
    int current_page = (s_current == s_main || s_current == s_plus_main) ? 0 :
        s_current == s_xiaozhi_page ? 1 : -1;
    if (current_page < 0) {
        load_screen(first_enabled_screen(), true);
        return;
    }
    for (uint8_t offset = 1; offset <= PAGE_COUNT; ++offset) {
        uint8_t next = (current_page + offset) % PAGE_COUNT;
        if (page_enabled(next)) {
            load_screen(page_screen(next), true);
            return;
        }
    }
}

void state_ui_button_a_long(void)
{
    if (s_screen_saver_active) {
        exit_screen_saver();
        return;
    }
    if (!s_screen_on) {
        screen_wake();
        return;
    }
    if (s_current == s_power) return;
    if (s_current == s_settings) {
        load_screen(first_enabled_screen(), false);
    } else {
        load_screen(s_settings, true);
    }
}

void state_ui_button_b_short(void)
{
    if (s_screen_saver_active) {
        exit_screen_saver();
        return;
    }
    if (!s_screen_on) {
        screen_wake();
        return;
    }
    if (enter_screen_saver()) return;
    s_last_activity = lv_tick_get();
    screen_off();
}

void state_ui_button_b_long(void)
{
    if (s_screen_saver_active) {
        exit_screen_saver();
        return;
    }
    if (!s_screen_on) {
        screen_wake();
        return;
    }
    if (s_current == s_power) return;
    s_before_power = s_current;
    load_screen_with_transition(s_power, TRANSITION_DIRECT);
}

void state_ui_set_power(uint8_t percent, bool external_power, bool charging)
{
    if (s_external_power != external_power) {
        s_external_power = external_power;
        s_last_activity = lv_tick_get();
        /* A transient battery classification can blank the panel before
           VBUS is re-detected. Restoring external power must also light
           the panel again, otherwise a docked device stays black and
           looks like it went to sleep. */
        if (external_power && !s_screen_on) {
            screen_wake();
        }
    }
    s_battery = percent;
    if (percent <= 100) {
        set_label_text_fmt_cached(s_battery_label,
                              charging ? "电量 %u%% · 充电中" :
                              external_power ? "电量 %u%% · 充电中" : "电量 %u%%",
                              percent);
    } else {
        set_label_text_cached(s_battery_label,
                          charging ? "电量 -- · 充电中" :
                          external_power ? "电量 -- · 充电中" : "电量 --");
    }
}
