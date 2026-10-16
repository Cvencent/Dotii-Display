#include <assert.h>
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

int main(void)
{
    test_emotion_event();
    test_notify_music();
    test_state_mapping();
    test_hello_and_bad_input();
    printf("xiaozhi_client tests passed\n");
    return 0;
}
