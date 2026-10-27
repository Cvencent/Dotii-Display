#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* Up to three user-defined wake words, each as space-separated pinyin. The
   list itself is tiny because MultiNet keeps the vocabulary in RAM: only the
   model binary is large, and that ships inside the firmware. */
#define XIAOZHI_WAKE_WORD_MAX 3
#define XIAOZHI_WAKE_WORD_EACH_BYTES 64
#define XIAOZHI_WAKE_WORD_LIST_BYTES \
    ((XIAOZHI_WAKE_WORD_EACH_BYTES * XIAOZHI_WAKE_WORD_MAX) + XIAOZHI_WAKE_WORD_MAX)

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
    /* Fixed WakeNet fallback word; kept for the case where the custom list
       below is empty or the MultiNet model cannot start. */
    char wake_word[32];
    /* Up to XIAOZHI_WAKE_WORD_MAX user-defined wake words, pinyin separated by
       ';'. MultiNet recognises command words, so these are injected at runtime
       instead of needing one WakeNet model per word. */
    char wake_words[XIAOZHI_WAKE_WORD_LIST_BYTES];
    char voice[32];
    bool mcp_enabled;
    /* Dedicated MCP access point published by the xiaozhi console
       (wss://api.xiaozhi.me/mcp/?token=...). Tools are registered here, not on
       the conversation socket. */
    char mcp_url[512];
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
    uint32_t server_emotion_revision;
    char last_user_text[192];
    uint32_t user_text_revision;
    char last_assistant_text[192];
    uint32_t assistant_text_revision;
    char last_text[192];
    char pending_audio_url[256];
    /* A confirmed song waits here until the assistant stops speaking, so the
       spoken confirmation and the music never fight over the speaker. */
    char pending_music_url[384];
    char pending_music_title[192];
    uint32_t pending_music_ms;
    bool pending_music_ready;
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
    /* MultiNet recogniser (user-defined words). */
    void *wake_data;
    void *wake_iface;
    /* WakeNet recogniser (built-in phrase). Runs alongside MultiNet so the
       device keeps a dependable wake word even when the custom list is empty
       or the model cannot match. */
    void *wake_wn_data;
    void *wake_wn_iface;
    void *audio_encode;
    void *audio_decode;
    void *input_resampler;
    void *output_resampler;
    void *speaker;
    void *microphone;
    uint8_t output_volume;
    atomic_bool wake_event_pending;
    atomic_bool wake_reply_pending;
    atomic_bool wake_reply_playing;
    atomic_bool music_task_started;
    atomic_bool task_chime_pending;
    atomic_bool task_chime_failed;
    bool codec_ready;
    bool speaker_ready;
    int server_sample_rate;
    int server_frame_duration;
    volatile bool listening;
    volatile bool wake_resume_pending;
    volatile bool music_stop_requested;
    atomic_bool music_fade_requested;
    bool wake_ready;
    /* True when the active recogniser is MultiNet (user-defined words) rather
       than the built-in WakeNet phrase; the detect API differs. */
    bool wake_multinet;
    bool wake_wn_ready;
    bool wake_music_fallback;
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

/* Chooses the local expression for the current Xiaozhi turn. Transport and
   playback states force their own catalogs; while speaking, a valid
   server emotion wins over simple text intent cues. The result is always
   a catalog id. */
const char *xiaozhi_client_emotion_policy(xiaozhi_session_state_t state,
                                          const char *server_emotion,
                                          const char *text);

typedef enum {
    XIAOZHI_MUSIC_FORMAT_UNKNOWN = 0,
    XIAOZHI_MUSIC_FORMAT_OGG,
    XIAOZHI_MUSIC_FORMAT_MP3,
    XIAOZHI_MUSIC_FORMAT_WAV,
    XIAOZHI_MUSIC_FORMAT_M4A,
} xiaozhi_music_format_t;

xiaozhi_music_format_t xiaozhi_music_format_from_bytes(const uint8_t *data, size_t size);
xiaozhi_music_format_t xiaozhi_music_format_from_url(const char *url);

/* Push-to-talk control used by the round-screen touch UI. */
bool xiaozhi_client_send_listen(xiaozhi_client_t *client, bool start);

/* Play a resolved stream URL (used by the local QQ Music path). */
bool xiaozhi_client_play_url(xiaozhi_client_t *client, const char *url, const char *title);

/* Queue a resolved song so it starts only after the assistant finishes its
   spoken confirmation. A timeout starts it anyway if no speech arrives. */
void xiaozhi_client_queue_music(xiaozhi_client_t *client, const char *url, const char *title);
void xiaozhi_client_tick_pending_music(xiaozhi_client_t *client);
/* Drops a parked song. Called when a newer request arrives or when the
   queued entry has outlived its usefulness, so an old request can never
   start playing after a later one failed. */
void xiaozhi_client_cancel_pending_music(xiaozhi_client_t *client);
bool xiaozhi_client_send_abort(xiaozhi_client_t *client);

/* Speaker volume used by both the control center and the codec layer. */
uint8_t xiaozhi_client_get_volume(const xiaozhi_client_t *client);
uint8_t xiaozhi_client_set_volume(xiaozhi_client_t *client, int volume, bool persist);

/* Voice wake is produced by the audio task and consumed by the LVGL thread. */
void xiaozhi_client_notify_wake_word(xiaozhi_client_t *client);
/* Queue a short completion chime (failed=true plays a lower tone). */
void xiaozhi_client_notify_task_completed(xiaozhi_client_t *client, bool failed);
bool xiaozhi_client_take_wake_event(xiaozhi_client_t *client);
bool xiaozhi_client_take_wake_reply(xiaozhi_client_t *client);

/* Wake-word detection remains available while the websocket is reconnecting.
   In that window the event is acknowledged and replayed locally. This take
   fires when the next hello arrives so the transport can start listening. */
bool xiaozhi_client_wake_detection_allowed(const xiaozhi_client_t *client);
bool xiaozhi_client_take_wake_resume(xiaozhi_client_t *client);

/* Apply a fixed linear gain in place. Full-scale samples are clipped to the
   signed 16-bit range so the same block can feed wake word and uplink Opus. */
void xiaozhi_pcm_amplify(int16_t *samples, size_t sample_count, uint16_t multiplier);

/* Fade a mono PCM block to silence for an interruptible music stop. */
void xiaozhi_pcm_fade_out(int16_t *samples, size_t sample_count);

/* Average interleaved left/right microphone samples in place. */
void xiaozhi_pcm_downmix_stereo(int16_t *samples, size_t frame_count);

/* Parses a server text JSON frame. Returns true when the state changed. */
bool xiaozhi_client_handle_text(xiaozhi_client_t *client, const char *json);

#ifdef __cplusplus
}
#endif
