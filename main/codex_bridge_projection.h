#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "codex_bridge_protocol.h"

/* Number of recently seen global sequence numbers kept for exact duplicate
   detection. The Bridge replays a window of events after a reconnect, so a
   small ring avoids applying the same frame twice. */
#define CBDP_PROJECTION_SEEN_RING 64

/* The Bridge pushes its authoritative counters over the UDP home-counter
   channel every two seconds. A gap of eight seconds means that channel is
   dead and the event channel may take the numbers over. */
#define CBDP_HOME_COUNTERS_FRESH_MS 8000

typedef void (*codex_bridge_projection_emit_t)(
    const codex_bridge_message_t *message,
    void *context
);

typedef struct {
    cJSON *threads;
    cJSON *staged_threads;
    uint32_t snapshot_id;
    uint32_t snapshot_revision;
    uint16_t snapshot_page_count;
    uint32_t snapshot_page_mask;
    uint32_t active_thread_ref;
    uint32_t active_turn_ref;
    uint32_t last_topic_id;
    uint64_t last_topic_seq;
    uint64_t last_global_seq;
    uint64_t last_revision;
    bool have_last_event;
    /* Authoritative counters received from the Bridge summary event
       (system.codex_state, event type 0x0704). */
    bool have_summary;
    uint16_t summary_running;
    uint16_t summary_completed_unread;
    uint16_t summary_failed_unread;
    /* Global sequence number of the newest applied system.codex_state frame.
       The event carries an aggregated snapshot, so a frame with an older
       global sequence number is a replay of a stale value and has to be
       dropped instead of being published as a new counter change. */
    uint64_t summary_global_seq;
    bool have_summary_global_seq;
    /* Counters received over the UDP home-counter channel (0x1E). They are
       published by the Bridge every two seconds and are authoritative for
       the lifetime of a session, so they replace the counters of the
       event channel instead of competing with them. */
    bool have_home_counters;
    uint16_t home_running;
    uint16_t home_completed_unread;
    uint16_t home_failed_unread;
    int64_t home_counters_ms;
    bool home_counters_guard_logged;
    /* Bitmask of the counter fields the last home-counter datagram carried
       (1 = running, 2 = completed, 4 = failed). */
    uint8_t home_fields;
    /* True when the Bridge session was replaced by a new one (restart or
       crash). The next home-counter datagram re-establishes the reference
       values and is published as a baseline, so the panel returns to the
       numbers of the new Bridge even when they are identical to the ones of
       the dead instance. */
    bool home_counters_resync;
    /* Wall clock of the last published home-counter frame. Used for the
       periodic heartbeat that keeps the panel numbers alive. */
    int64_t home_counters_emit_ms;
    uint64_t seen_global_seqs[CBDP_PROJECTION_SEEN_RING];
    uint8_t seen_count;
    uint8_t seen_next;
    /* True while the Bridge is still replaying a subscription window. Every
       frame that arrives inside the window belongs to the catch-up, not to a
       live change, so the panel must not treat it as a freshly finished
       task. */
    bool catchup;
    /* True while a replay must not reach the panel at all. The Bridge replays
       an arbitrary history window when it cannot honour our cursor, and each
       of those frames carries an old counter value, so publishing them one by
       one would make the counters flicker. The window is flushed once, after
       the replay ended. */
    bool quiet;
    codex_bridge_projection_emit_t emit;
    void *emit_context;
} codex_bridge_projection_t;

/* Route cJSON allocations to PSRAM. Call once during app startup before
   any cJSON tree is created, so every module shares the same hooks. */
void codex_bridge_cjson_hooks_install(void);

esp_err_t codex_bridge_projection_init(
    codex_bridge_projection_t *projection,
    codex_bridge_projection_emit_t emit,
    void *context
);
void codex_bridge_projection_reset(codex_bridge_projection_t *projection);
esp_err_t codex_bridge_projection_apply_snapshot(
    codex_bridge_projection_t *projection,
    const codex_bridge_frame_t *frame
);
/* Resolve the Bridge-assigned topic_id for a thread previously seen in a
   snapshot or delta. Returns false when the thread is unknown or has no
   topic yet. */
bool codex_bridge_projection_thread_topic(
    const codex_bridge_projection_t *projection,
    const char *thread_id,
    uint32_t *topic_id
);

/* Decode an account.changed / rate_limits.changed / token_usage.changed
   payload (also used for the account.* read RESULT payloads, which carry the
   same field set) and publish it to the emit callback. */
/* Decode an account.* read RESULT (0x0090..0x0092) into the same published
   shape as the account.*.changed events. */
esp_err_t codex_bridge_projection_apply_account_result(
    codex_bridge_projection_t *projection,
    uint16_t method_id,
    const uint8_t *header_tlv,
    size_t header_tlv_length
);

esp_err_t codex_bridge_projection_apply_account(
    codex_bridge_projection_t *projection,
    uint16_t event_type,
    const uint8_t *payload,
    size_t payload_length
);

esp_err_t codex_bridge_projection_apply_delta(
    codex_bridge_projection_t *projection,
    const codex_bridge_frame_t *frame,
    uint64_t *cursor
);

/* The Bridge stores cursors per (topic_id, scope). Most thread events belong
   to the thread scope, while message.read is delivered at summary scope. */
uint8_t codex_bridge_projection_delta_scope(const codex_bridge_frame_t *frame);

/* Mark the projection as being inside a subscription catch-up window. While
   the flag is set the published counters carry a "baseline" marker so the
   panel treats them as a new reference point instead of a new completion. */
void codex_bridge_projection_set_catchup(codex_bridge_projection_t *projection, bool catchup);
/* Suppress every publication until the flag is cleared again. */
void codex_bridge_projection_set_quiet(codex_bridge_projection_t *projection, bool quiet);
/* Forget the system.codex_state sequence watermark. The Bridge restarts its
   event history after a crash, so its global sequence numbers start over at
   a lower value and the stale-frame guard has to be released. */
void codex_bridge_projection_forget_summary_seq(codex_bridge_projection_t *projection);

/* Forget everything that belongs to a single Bridge session: the state
   sequence watermark and the duplicate detection ring. A new authenticated
   session can be served by a different Bridge process, and the Bridge
   restarts its global sequence numbers when it rebuilds its event store.
   Keeping the old watermark would mark every snapshot of the new instance
   as stale and would freeze the panel counters on the dead instance. */
void codex_bridge_projection_forget_session_state(codex_bridge_projection_t *projection);

/* Publish the current state once. Used when a replay window closes so the
   panel receives the authoritative values instead of the whole history. */
void codex_bridge_projection_flush(codex_bridge_projection_t *projection);

/* Apply the counters received over the UDP home-counter channel.
   Returns true when the panel was updated. */
bool codex_bridge_projection_apply_home_counters(
    codex_bridge_projection_t *projection,
    bool have_running,
    uint16_t running,
    bool have_completed,
    uint16_t completed,
    bool have_failed,
    uint16_t failed
);

/* True while the UDP home-counter channel still delivers fresh numbers. */
bool codex_bridge_projection_home_counters_fresh(
    const codex_bridge_projection_t *projection);
