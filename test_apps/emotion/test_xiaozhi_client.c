#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xiaozhi_client.h"
#include "emotion_catalog.h"

static void test_emotion_event(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    assert(xiaozhi_client_last_emotion(&client) == NULL);
    /* Server names are normalized onto the local catalog ids the renderer
       consumes, so "happy" has to come back as the catalog entry "10". */
    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"llm\",\"emotion\":\"happy\",\"text\":\"hi\"}"));
    assert(strcmp(xiaozhi_client_last_emotion(&client), "10") == 0);
    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"llm\",\"emotion\":\"33\",\"text\":\"hi\"}"));
    assert(strcmp(xiaozhi_client_last_emotion(&client), "33") == 0);
}

static void test_emotion_policy_prefers_server_then_state_then_text(void)
{
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, "happy", "anything"), "10") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_LISTENING, NULL, NULL), "35") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_THINKING, NULL, "正在检索资料"), "40") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "任务完成了，太好了"), "33") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "这个操作失败了"), "34") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_MUSIC, NULL, NULL), "42") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "我好开心呀"), "10") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "我有点不明白"), "20") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "别生气"), "21") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "我有点累了"), "15") == 0);
}

static void test_emotion_policy_state_overrides_stale_text(void)
{
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_MUSIC, "happy", "任务完成了"), "42") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_LISTENING, "happy", "任务完成了"), "35") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_ERROR, "happy", "任务完成了"), "34") == 0);
}

static void test_emotion_policy_rejects_unknown_emotion_and_falls_back(void)
{
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_LISTENING, "definitely-not-an-emotion", NULL), "35") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_IDLE, NULL, NULL), "02") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_DISABLED, NULL, NULL), "02") == 0);
}

static void test_emotion_policy_covers_every_catalog_id(void)
{
    const size_t count = emotion_catalog_count();
    assert(count > 0);
    for (size_t i = 0; i < count; ++i) {
        const emotion_def_t *def = emotion_catalog_at(i);
        assert(def != NULL);
        const char *mapped = xiaozhi_client_emotion_policy(
            XIAOZHI_STATE_SPEAKING, def->name, NULL);
        assert(mapped != NULL);
        assert(emotion_catalog_by_id(mapped) != NULL);
    }
}

static void test_music_format_sniffing(void)
{
    const uint8_t mp3[] = { 'I', 'D', '3', 4, 0, 0 };
    const uint8_t wav[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' };
    const uint8_t ogg[] = { 'O', 'g', 'g', 'S', 0, 0 };
    const uint8_t m4a[] = { 0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ' };
    const uint8_t unknown[] = { 0x11, 0x22, 0x33, 0x44 };

    assert(xiaozhi_music_format_from_bytes(mp3, sizeof(mp3)) == XIAOZHI_MUSIC_FORMAT_MP3);
    assert(xiaozhi_music_format_from_bytes(wav, sizeof(wav)) == XIAOZHI_MUSIC_FORMAT_WAV);
    assert(xiaozhi_music_format_from_bytes(ogg, sizeof(ogg)) == XIAOZHI_MUSIC_FORMAT_OGG);
    assert(xiaozhi_music_format_from_bytes(m4a, sizeof(m4a)) == XIAOZHI_MUSIC_FORMAT_M4A);
    assert(xiaozhi_music_format_from_bytes(unknown, sizeof(unknown)) == XIAOZHI_MUSIC_FORMAT_UNKNOWN);
}

static void test_music_format_sniff_handles_short_and_sparse_streams(void)
{
    const uint8_t short_mp3[] = { 'I', 'D', '3' };
    const uint8_t short_ogg[] = { 'O', 'g', 'g' };
    const uint8_t empty[] = { 0 };

    assert(xiaozhi_music_format_from_bytes(short_mp3, sizeof(short_mp3)) == XIAOZHI_MUSIC_FORMAT_UNKNOWN);
    assert(xiaozhi_music_format_from_bytes(short_ogg, sizeof(short_ogg)) == XIAOZHI_MUSIC_FORMAT_UNKNOWN);
    assert(xiaozhi_music_format_from_bytes(empty, 0) == XIAOZHI_MUSIC_FORMAT_UNKNOWN);
}
static void test_music_format_from_url_ignores_query_and_defaults_unknown(void)
{
    assert(xiaozhi_music_format_from_url("https://example/song.mp3?token=1") == XIAOZHI_MUSIC_FORMAT_MP3);
    assert(xiaozhi_music_format_from_url("https://example/song.WAV") == XIAOZHI_MUSIC_FORMAT_WAV);
    assert(xiaozhi_music_format_from_url("https://example/song.m4a") == XIAOZHI_MUSIC_FORMAT_M4A);
    assert(xiaozhi_music_format_from_url("https://example/song.ogg") == XIAOZHI_MUSIC_FORMAT_OGG);
    assert(xiaozhi_music_format_from_url("https://example/song") == XIAOZHI_MUSIC_FORMAT_UNKNOWN);
}
static void test_notify_music(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    assert(xiaozhi_client_handle_text(
        &client,
        "{\"type\":\"notify\",\"audio_url\":\"https://music.example/song.ogg\"}"
    ));
    assert(xiaozhi_client_state(&client) == XIAOZHI_STATE_MUSIC);
    assert(strcmp(xiaozhi_client_state_emotion(xiaozhi_client_state(&client)), "42") == 0);
    assert(strcmp(xiaozhi_client_pending_audio_url(&client),
                  "https://music.example/song.ogg") == 0);

    assert(xiaozhi_client_handle_text(
        &client,
        "{\"type\":\"llm\",\"emotion\":\"playing_music\",\"text\":\"music\"}"
    ));
    assert(strcmp(xiaozhi_client_last_emotion(&client), "42") == 0);
}
static void test_state_mapping(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"chat\",\"state\":\"thinking\"}"));
    assert(xiaozhi_client_state(&client) == XIAOZHI_STATE_THINKING);
    assert(strcmp(xiaozhi_client_state_emotion(xiaozhi_client_state(&client)), "30") == 0);

    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"chat\",\"state\":\"speaking\"}"));
    assert(xiaozhi_client_state(&client) == XIAOZHI_STATE_SPEAKING);
    assert(strcmp(xiaozhi_client_state_emotion(xiaozhi_client_state(&client)), "39") == 0);
}

static void test_hello_and_bad_input(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"hello\",\"session_id\":\"abc\"}"));
    assert(strcmp(client.session_id, "abc") == 0);
    assert(!xiaozhi_client_handle_text(&client, "not json"));
    assert(!xiaozhi_client_handle_text(&client, NULL));
    assert(xiaozhi_client_state_emotion(XIAOZHI_STATE_IDLE) == NULL);
    assert(xiaozhi_client_state_emotion(XIAOZHI_STATE_DISABLED) == NULL);
}

static void test_output_volume_clamps_to_supported_range(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);

    assert(xiaozhi_client_get_volume(&client) == 80);
    assert(xiaozhi_client_set_volume(&client, 125, false) == 100);
    assert(xiaozhi_client_get_volume(&client) == 100);
    assert(xiaozhi_client_set_volume(&client, -20, false) == 0);
    assert(xiaozhi_client_get_volume(&client) == 0);
    assert(xiaozhi_client_set_volume(&client, 42, false) == 42);
    assert(xiaozhi_client_get_volume(&client) == 42);
}

static void test_pcm_amplifier_scales_and_saturates(void)
{
    int16_t samples[] = {
        0,
        100,
        -100,
        10000,
        -10000,
        INT16_MAX,
        INT16_MIN,
    };

    xiaozhi_pcm_amplify(samples, sizeof(samples) / sizeof(samples[0]), 8);
    assert(samples[0] == 0);
    assert(samples[1] == 800);
    assert(samples[2] == -800);
    assert(samples[3] == INT16_MAX);
    assert(samples[4] == -INT16_MAX);
    assert(samples[5] == INT16_MAX);
    assert(samples[6] == -INT16_MAX);
}

static void test_pcm_stereo_downmix_keeps_in_phase_speech(void)
{
    int16_t samples[] = {
        100, -100,
        300, 100,
        INT16_MAX, INT16_MAX,
        INT16_MIN, INT16_MIN,
    };

    xiaozhi_pcm_downmix_stereo(samples, 4);
    assert(samples[0] == 0);
    assert(samples[1] == 200);
    assert(samples[2] == INT16_MAX);
    assert(samples[3] == INT16_MIN);
}

static void test_wake_event_is_consumed_once(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);

    assert(!xiaozhi_client_take_wake_event(&client));
    xiaozhi_client_notify_wake_word(&client);
    assert(xiaozhi_client_take_wake_event(&client));
    assert(!xiaozhi_client_take_wake_event(&client));

    xiaozhi_client_notify_wake_word(&client);
    xiaozhi_client_notify_wake_word(&client);
    assert(xiaozhi_client_take_wake_event(&client));
    assert(!xiaozhi_client_take_wake_event(&client));
}

static void test_wake_reply_is_consumed_once(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);

    assert(!xiaozhi_client_take_wake_reply(&client));
    xiaozhi_client_notify_wake_word(&client);
    assert(xiaozhi_client_take_wake_reply(&client));
    assert(!xiaozhi_client_take_wake_reply(&client));
}

static void test_wake_detection_continues_during_reconnect(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);

    assert(!xiaozhi_client_wake_detection_allowed(&client));
    client.running = true;
    client.wake_ready = true;
    client.connected = false;
    client.state = XIAOZHI_STATE_CONNECTING;
    assert(xiaozhi_client_wake_detection_allowed(&client));

    client.listening = true;
    assert(!xiaozhi_client_wake_detection_allowed(&client));
    client.listening = false;

    client.music_task = (void *)1;
    assert(!xiaozhi_client_wake_detection_allowed(&client));
    client.music_task = NULL;

    client.state = XIAOZHI_STATE_THINKING;
    assert(!xiaozhi_client_wake_detection_allowed(&client));

    client.state = XIAOZHI_STATE_CONNECTING;
    client.running = true;
    client.wake_ready = true;
    client.connected = false;
    client.wake_resume_pending = true;
    assert(xiaozhi_client_take_wake_resume(&client));
    assert(!xiaozhi_client_take_wake_resume(&client));
}

int main(void)
{
    test_emotion_event();
    test_emotion_policy_prefers_server_then_state_then_text();
    test_emotion_policy_state_overrides_stale_text();
    test_emotion_policy_rejects_unknown_emotion_and_falls_back();
    test_emotion_policy_covers_every_catalog_id();
    test_music_format_sniffing();
    test_music_format_sniff_handles_short_and_sparse_streams();
    test_music_format_from_url_ignores_query_and_defaults_unknown();
    test_notify_music();
    test_state_mapping();
    test_hello_and_bad_input();
    test_output_volume_clamps_to_supported_range();
    test_pcm_amplifier_scales_and_saturates();
    test_pcm_stereo_downmix_keeps_in_phase_speech();
    test_wake_event_is_consumed_once();
    test_wake_reply_is_consumed_once();
    test_wake_detection_continues_during_reconnect();
    printf("xiaozhi_client tests passed\n");
    return 0;
}
