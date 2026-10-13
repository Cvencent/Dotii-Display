#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Onboard xiaozhi AI client.

   Connects to a xiaozhi WebSocket server, performs the documented hello
   handshake, and turns the server's text/emotion events into local state the
   emotion router can consume. Audio (Opus) and wake-word are wired through
   the BSP codec but are gated behind CONFIG_XIAOZHI_ENABLE so the default
   build does not pull in the speech stack. */

typedef enum {
    XIAOZHI_STATE_DISABLED = 0,
    XIAOZHI_STATE_IDLE,
    XIAOZHI_STATE_CONNECTING,
    XIAOZHI_STATE_LISTENING,
    XIAOZHI_STATE_THINKING,
    XIAOZHI_STATE_SPEAKING,
    XIAOZHI_STATE_ERROR,
} xiaozhi_session_state_t;

typedef struct {
    char server_url[192];
    char token[80];
    char device_id[40];
    bool mcp_enabled;
} xiaozhi_config_t;

typedef struct {
    bool running;
    bool connected;
    xiaozhi_session_state_t state;
    char last_emotion[24];
    char last_text[192];
    char session_id[64];
    void *transport;
    xiaozhi_config_t config;
} xiaozhi_client_t;

/* Returns true when the firmware was built with xiaozhi support enabled. */
bool xiaozhi_client_available(void);

void xiaozhi_client_init(xiaozhi_client_t *client);
void xiaozhi_client_configure(xiaozhi_client_t *client, const xiaozhi_config_t *config);
bool xiaozhi_client_start(xiaozhi_client_t *client);
void xiaozhi_client_stop(xiaozhi_client_t *client);

xiaozhi_session_state_t xiaozhi_client_state(const xiaozhi_client_t *client);

/* Latest emotion name reported by the server ("happy", "sad", ...). Empty when
   none has been reported since the last session. */
const char *xiaozhi_client_last_emotion(const xiaozhi_client_t *client);

/* Maps a session state to a local emotion id, or NULL when no override is
   wanted (e.g. disabled/idle). Never returns an unknown id. */
const char *xiaozhi_client_state_emotion(xiaozhi_session_state_t state);

/* Parses a server text JSON frame. Returns true when the state changed.
   Exposed for host testing; the transport calls it on every text frame. */
bool xiaozhi_client_handle_text(xiaozhi_client_t *client, const char *json);

#ifdef __cplusplus
}
#endif
