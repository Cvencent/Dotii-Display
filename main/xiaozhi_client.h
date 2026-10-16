#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Onboard xiaozhi client.

   The device follows the official activation and websocket protocol. The
   server owns conversational emotion decisions through the llm.emotion field;
   this module only normalizes that field and exposes the current session
   state to the local emotion router. */

typedef enum {
    XIAOZHI_STATE_DISABLED = 0,
    XIAOZHI_STATE_IDLE,
    XIAOZHI_STATE_ACTIVATING,
    XIAOZHI_STATE_CONNECTING,
    XIAOZHI_STATE_LISTENING,
    XIAOZHI_STATE_THINKING,
    XIAOZHI_STATE_SPEAKING,
    XIAOZHI_STATE_MUSIC,
    XIAOZHI_STATE_ERROR,
} xiaozhi_session_state_t;

typedef struct {
    char server_url[192];
    char wake_word[32];
    char voice[32];
    bool mcp_enabled;
} xiaozhi_config_t;

typedef struct {
    bool running;
    bool connected;
    bool activated;
    xiaozhi_session_state_t state;
    char activation_code[24];
    char activation_message[160];
    char last_emotion[24];
    uint32_t emotion_until_ms;
    char last_text[192];
    char pending_audio_url[256];
    char session_id[64];
    char websocket_url[192];
    char token[160];
    void *transport;
    void *activation_task;
    void *playback_task;
    void *microphone_task;
    void *music_task;
    void *music_http;
    void *wake_task;
    void *wake_models;
    void *wake_data;
    void *wake_iface;
    void *audio_encode;
    void *audio_decode;
    void *input_resampler;
    void *output_resampler;
    void *speaker;
    void *microphone;
    bool codec_ready;
    int server_sample_rate;
    int server_frame_duration;
    volatile bool listening;
    volatile bool music_stop_requested;
    bool wake_ready;
    xiaozhi_config_t config;
} xiaozhi_client_t;

bool xiaozhi_client_available(void);

void xiaozhi_client_init(xiaozhi_client_t *client);
void xiaozhi_client_configure(xiaozhi_client_t *client, const xiaozhi_config_t *config);
bool xiaozhi_client_start(xiaozhi_client_t *client);
void xiaozhi_client_stop(xiaozhi_client_t *client);

xiaozhi_session_state_t xiaozhi_client_state(const xiaozhi_client_t *client);
const char *xiaozhi_client_state_name(xiaozhi_session_state_t state);
bool xiaozhi_client_activation_required(const xiaozhi_client_t *client);
const char *xiaozhi_client_activation_code(const xiaozhi_client_t *client);
const char *xiaozhi_client_activation_message(const xiaozhi_client_t *client);
const char *xiaozhi_client_websocket_url(const xiaozhi_client_t *client);

/* Latest server emotion, normalized to a local catalog id/name. */
const char *xiaozhi_client_last_emotion(const xiaozhi_client_t *client);
const char *xiaozhi_client_pending_audio_url(const xiaozhi_client_t *client);

/* Maps a session state to a local emotion id, or NULL when no override is
   wanted (e.g. disabled/idle). */
const char *xiaozhi_client_state_emotion(xiaozhi_session_state_t state);

/* Push-to-talk control used by the round-screen touch UI. */
bool xiaozhi_client_send_listen(xiaozhi_client_t *client, bool start);
bool xiaozhi_client_send_abort(xiaozhi_client_t *client);

/* Parses a server text JSON frame. Returns true when the state changed. */
bool xiaozhi_client_handle_text(xiaozhi_client_t *client, const char *json);

#ifdef __cplusplus
}
#endif
