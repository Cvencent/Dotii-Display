#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xiaozhi_client.h"

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
int main(void)
{
    test_emotion_event();
    test_notify_music();
    test_state_mapping();
    test_hello_and_bad_input();
    test_output_volume_clamps_to_supported_range();
    test_pcm_amplifier_scales_and_saturates();
    test_pcm_stereo_downmix_keeps_in_phase_speech();
    test_wake_event_is_consumed_once();
    test_wake_reply_is_consumed_once();
    printf("xiaozhi_client tests passed\n");
    return 0;
}
