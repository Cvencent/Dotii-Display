#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

#include "xiaozhi_client.h"
#include "dotii_data.h"

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
        XIAOZHI_STATE_LISTENING, NULL, NULL), "03") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_THINKING, NULL, "正在检索资料"), "28") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "任务完成了，太好了"), "33") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "这个操作失败了"), "34") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_MUSIC, NULL, NULL), "36") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "我好开心呀"), "10") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "我有点不明白"), "19") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "别生气"), "23") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_SPEAKING, NULL, "我有点累了"), "24") == 0);
}

static void test_emotion_policy_state_overrides_stale_text(void)
{
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_MUSIC, "happy", "任务完成了"), "36") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_LISTENING, "happy", "任务完成了"), "03") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_ERROR, "happy", "任务完成了"), "34") == 0);
}

static void test_emotion_policy_rejects_unknown_emotion_and_falls_back(void)
{
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_LISTENING, "definitely-not-an-emotion", NULL), "03") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_IDLE, NULL, NULL), "00") == 0);
    assert(strcmp(xiaozhi_client_emotion_policy(
        XIAOZHI_STATE_DISABLED, NULL, NULL), "00") == 0);
}

static void test_emotion_policy_covers_every_catalog_id(void)
{
    const size_t count = dotii_catalog_count();
    assert(count > 0);
    for (size_t i = 0; i < count; ++i) {
        const dotii_expr_t *def = dotii_catalog_at(i);
        assert(def != NULL);
        const char *mapped = xiaozhi_client_emotion_policy(
            XIAOZHI_STATE_SPEAKING, def->name, NULL);
        assert(mapped != NULL);
        assert(dotii_catalog_by_id(mapped) != NULL);
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
    /* The service pushes its own catalogue URL here. It cannot honour the
       user's QQ Music account or the confirmed-search gate, so playback is
       refused and music must arrive through the MCP tools instead. */
    assert(xiaozhi_client_handle_text(
        &client,
        "{\"type\":\"notify\",\"audio_url\":\"https://music.example/song.ogg\"}"
    ));
    assert(xiaozhi_client_state(&client) != XIAOZHI_STATE_MUSIC);
    assert(xiaozhi_client_pending_audio_url(&client) == NULL);

    assert(xiaozhi_client_handle_text(
        &client,
        "{\"type\":\"llm\",\"emotion\":\"playing_music\",\"text\":\"music\"}"
    ));
    assert(strcmp(xiaozhi_client_last_emotion(&client), "36") == 0);

}
static void test_play_music_requires_confirmed_index(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    client.connected = true;
    client.running = true;
    /* Even with a query, play_music must not start anything without a
       confirmed song_index from a prior search_music call. */
    assert(xiaozhi_client_handle_text(&client,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"play_music\",\"arguments\":"
        "{\"query\":\"陈奕迅 单车\"}}}"));
    assert(xiaozhi_client_state(&client) != XIAOZHI_STATE_MUSIC);
    assert(client.pending_audio_url[0] == '\0');
}

/* The service wraps MCP in an envelope: type=mcp with a payload object.
   If the firmware only looks at the top level, tools never run. */
static void test_mcp_envelope_is_understood(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);

    assert(xiaozhi_client_handle_text(&client,
        "{\"session_id\":\"s1\",\"type\":\"mcp\",\"payload\":"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{}}}"));

    /* tools/list must also be accepted in envelope form */
    assert(xiaozhi_client_handle_text(&client,
        "{\"session_id\":\"s1\",\"type\":\"mcp\",\"payload\":"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\","
        "\"params\":{}}}"));

    /* play_music without a confirmed index must be refused, not played */
    assert(xiaozhi_client_handle_text(&client,
        "{\"session_id\":\"s1\",\"type\":\"mcp\",\"payload\":"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"play_music\",\"arguments\":"
        "{\"query\":\"单车\"}}}}"));
    assert(xiaozhi_client_state(&client) != XIAOZHI_STATE_MUSIC);
    assert(client.pending_audio_url[0] == '\0');
}

static void test_state_mapping(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"chat\",\"state\":\"thinking\"}"));
    assert(xiaozhi_client_state(&client) == XIAOZHI_STATE_THINKING);
    assert(strcmp(xiaozhi_client_state_emotion(xiaozhi_client_state(&client)), "04") == 0);

    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"chat\",\"state\":\"speaking\"}"));
    assert(xiaozhi_client_state(&client) == XIAOZHI_STATE_SPEAKING);
    assert(strcmp(xiaozhi_client_state_emotion(xiaozhi_client_state(&client)), "05") == 0);
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
    assert(xiaozhi_client_wake_detection_allowed(&client));
    client.connected = true;
    assert(xiaozhi_client_wake_detection_allowed(&client));
    client.state = XIAOZHI_STATE_MUSIC;
    client.state = XIAOZHI_STATE_THINKING;
    assert(!xiaozhi_client_wake_detection_allowed(&client));
    client.state = XIAOZHI_STATE_MUSIC;
    assert(xiaozhi_client_wake_detection_allowed(&client));
    client.music_task = NULL;
    client.running = true;
    client.wake_ready = true;
    client.connected = false;
    client.wake_resume_pending = true;
    assert(xiaozhi_client_take_wake_resume(&client));
    assert(!xiaozhi_client_take_wake_resume(&client));
}

/* Regression: "播放陈奕迅的单车" used to end with the device back in
   LISTENING while the song played. The cause was an ordering bug in the
   tts-stop handler: it tested "is music already playing?" *before* releasing
   the queued song, so the answer was stale, the microphone was re-opened, and
   the song was uploaded back to the cloud (mic and speaker fighting).
   These tests pin the ownership rule: while music owns the session, no server
   message may switch the device back to listening. */
static void test_tts_stop_keeps_music_ownership(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    client.running = true;
    client.connected = true;
    client.state = XIAOZHI_STATE_MUSIC;
    client.listening = false;

    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"tts\",\"state\":\"stop\"}"));
    assert(client.state == XIAOZHI_STATE_MUSIC);
    assert(!client.listening);
}

static void test_server_listening_state_ignored_during_music(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    client.running = true;
    client.connected = true;
    client.state = XIAOZHI_STATE_MUSIC;
    client.music_task = (void *)1; /* a live playback worker */
    client.listening = false;

    /* The service flips the session back to listening once its own reply has
       finished. That must not re-open the microphone mid-song. */
    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"state\",\"state\":\"listening\"}"));
    assert(client.state == XIAOZHI_STATE_MUSIC);
    assert(!client.listening);

    /* TTS starting mid-song must not steal the speaker either. */
    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"tts\",\"state\":\"start\"}"));
    assert(client.state == XIAOZHI_STATE_MUSIC);
    assert(!client.listening);
}

static void test_server_state_applies_again_after_music_stops(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    client.running = true;
    client.connected = true;
    client.state = XIAOZHI_STATE_MUSIC;
    client.music_task = (void *)1;
    client.listening = false;

    /* An explicit stop (MCP music stop, wake interrupt, or track end) hands
       the session back, so normal server state transitions resume. */
    client.music_stop_requested = true;
    client.music_task = NULL;
    assert(xiaozhi_client_handle_text(&client, "{\"type\":\"state\",\"state\":\"idle\"}"));
    assert(client.state == XIAOZHI_STATE_IDLE);
    assert(!client.listening);
}

/* Regression: the service closes idle sockets, and the reconnect used to
   reset the face to IDLE. A reconnecting "hello" (and the socket lifecycle
   events around it) must not interrupt a song that is still playing. */
static void test_reconnect_hello_keeps_music_face(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    client.running = true;
    client.connected = true;
    client.state = XIAOZHI_STATE_MUSIC;
    client.music_task = (void *)1;
    client.listening = false;

    assert(xiaozhi_client_handle_text(
        &client, "{\"type\":\"hello\",\"session_id\":\"abc\","
                 "\"audio_params\":{\"sample_rate\":24000,\"frame_duration\":60}}"));
    assert(client.state == XIAOZHI_STATE_MUSIC);
    assert(strcmp(client.session_id, "abc") == 0);
    assert(!client.listening);

    /* Once the song is over, a hello behaves normally again. */
    client.music_stop_requested = true;
    client.music_task = NULL;
    client.state = XIAOZHI_STATE_MUSIC;
    assert(xiaozhi_client_handle_text(
        &client, "{\"type\":\"hello\",\"session_id\":\"def\"}"));
    assert(client.state == XIAOZHI_STATE_IDLE);
}

/* Regression: a newer song request must drop any parked one, otherwise an old
   request starts playing after a newer one already failed. */
static void test_new_request_cancels_parked_song(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);

    xiaozhi_client_queue_music(&client, "https://example.invalid/old.m4a", "danche");
    assert(client.pending_music_ready);

    xiaozhi_client_cancel_pending_music(&client);
    assert(!client.pending_music_ready);
    assert(client.pending_music_url[0] == 0);
}

static void test_expired_parked_song_is_dropped_not_played(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    xiaozhi_client_queue_music(&client, "https://example.invalid/old.m4a", "danche");
    client.pending_music_ms = (uint32_t)(esp_timer_get_time() / 1000) - 60000u;
    client.state = XIAOZHI_STATE_IDLE;
    xiaozhi_client_tick_pending_music(&client);
    assert(!client.pending_music_ready);
}

static void test_stt_and_llm_text_are_kept_separate(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);
    assert(xiaozhi_client_handle_text(
        &client, "{\"type\":\"stt\",\"text\":\"我今天有点难过\"}"));
    assert(strcmp(client.last_user_text, "我今天有点难过") == 0);
    assert(xiaozhi_client_handle_text(
        &client, "{\"type\":\"llm\",\"emotion\":\"down\",\"text\":\"我懂你，会陪着你\"}"));
    assert(strcmp(client.last_assistant_text, "我懂你，会陪着你") == 0);
    assert(strcmp(client.last_user_text, "我今天有点难过") == 0);
}

static void test_repeated_turns_advance_revisions(void)
{
    xiaozhi_client_t client;
    xiaozhi_client_init(&client);

    assert(xiaozhi_client_handle_text(
        &client, "{\"type\":\"stt\",\"text\":\"你好小智\"}"));
    const uint32_t first_user = client.user_text_revision;
    assert(xiaozhi_client_handle_text(
        &client, "{\"type\":\"stt\",\"text\":\"你好小智\"}"));
    assert(client.user_text_revision == first_user + 1u);

    assert(xiaozhi_client_handle_text(
        &client, "{\"type\":\"llm\",\"emotion\":\"happy\",\"text\":\"我在呢\"}"));
    const uint32_t first_assistant = client.assistant_text_revision;
    const uint32_t first_server = client.server_emotion_revision;
    assert(xiaozhi_client_handle_text(
        &client, "{\"type\":\"llm\",\"emotion\":\"happy\",\"text\":\"我在呢\"}"));
    assert(client.assistant_text_revision == first_assistant + 1u);
    assert(client.server_emotion_revision == first_server + 1u);
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
    test_mcp_envelope_is_understood();
    test_play_music_requires_confirmed_index();
    test_state_mapping();
    test_hello_and_bad_input();
    test_output_volume_clamps_to_supported_range();
    test_pcm_amplifier_scales_and_saturates();
    test_pcm_stereo_downmix_keeps_in_phase_speech();
    test_wake_event_is_consumed_once();
    test_wake_reply_is_consumed_once();
    test_wake_detection_continues_during_reconnect();
    test_tts_stop_keeps_music_ownership();
    test_server_listening_state_ignored_during_music();
    test_server_state_applies_again_after_music_stops();
    test_reconnect_hello_keeps_music_face();
    test_new_request_cancels_parked_song();
    test_expired_parked_song_is_dropped_not_played();
    test_stt_and_llm_text_are_kept_separate();
    test_repeated_turns_advance_revisions();
    printf("xiaozhi_client tests passed\n");
    return 0;
}
