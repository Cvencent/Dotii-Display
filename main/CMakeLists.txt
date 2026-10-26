idf_component_register(
    SRCS
        "main.c"
        "app_state.c"
        "connectivity.c"
        "codex_bridge_client.c"
        "bridge_url.c"
        "codex_bridge_protocol.c"
        "codex_bridge_projection.c"
        "codex_read_tracker.c"
        "codex_snapshot_merge.c"
        "management_snapshot.c"
        "device_identity.c"
        "device_config.c"
        "ble_bridge.c"
        "board_input.c"
        "state_ui.c"
        "xiaozhi_client.c"
        "emotion/dotii_data.c"
        "emotion/dotii_draw.c"
        "emotion/dotii_engine.c"
        "emotion/dotii_render.c"
        "emotion/emotion_router.c"
        "emotion/emotion_mood.c"
        "emotion/emotion_mood_store.c"
        "qqmusic_client.c"
        "generated/ui_font_cjk_20.c"
        "generated/ui_font_fixed_20.c"
        "generated/ui_font_cjk_24.c"
        "generated/ui_font_cjk_28.c"
        "generated/ui_font_digits_64.c"
        "generated/ui_icons.c"
        "generated/wake_reply_audio.c"
    INCLUDE_DIRS "." "generated" "emotion"
    REQUIRES
        esp_event
        esp_app_format
        esp_http_client
        esp_netif
        esp_timer
        esp_wifi
        esp_websocket_client
        espressif__esp_audio_codec
        espressif__esp_audio_effects
        espressif__esp-sr
        bt
        espressif__cjson
        mbedtls
        nvs_flash
        waveshare__esp32_s3_touch_amoled_1_75
        esp_driver_usb_serial_jtag
)

# LVGL 9.4 + GCC 15 can emit a benign warning when IRAM declarations and
# definitions receive different unique section suffixes. Limit the workaround
# to LVGL so warnings remain strict for our application and other components.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-error=attributes)
