#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#define CODEX_BRIDGE_LINE_MAX 768
#define CODEX_BRIDGE_BODY_MAX 4096
#define CODEX_BRIDGE_ID_MAX 80
#define CODEX_BRIDGE_CAPABILITY_MAX 32
#define CODEX_BRIDGE_CAPABILITY_LEN 24

typedef struct {
    char type[24];
    char message_id[CODEX_BRIDGE_ID_MAX];
    char thread_id[CODEX_BRIDGE_ID_MAX];
    char kind[16];
    char status[24];
    char title[161];
    char *body;
    int seq;
    int total;
    bool ignored;
    cJSON *data;
    char capabilities[CODEX_BRIDGE_CAPABILITY_MAX][CODEX_BRIDGE_CAPABILITY_LEN];
    size_t capability_count;
} codex_bridge_message_t;

typedef void (*codex_bridge_message_handler_t)(
    const codex_bridge_message_t *message,
    void *context
);

esp_err_t codex_bridge_protocol_feed(
    const char *line,
    size_t length,
    codex_bridge_message_t *message
);
void codex_bridge_protocol_reset_message(codex_bridge_message_t *message);
bool codex_bridge_protocol_has_capability(
    const codex_bridge_message_t *message,
    const char *capability
);
const char *codex_bridge_protocol_body(
    const codex_bridge_message_t *message,
    const char *message_id
);
size_t codex_bridge_protocol_body_length(const codex_bridge_message_t *message);

/* ---- CBDP v1 binary codec ---- */

#define CBDP_VERSION_MAJOR 1
#define CBDP_FIXED_HEADER_SIZE 28
#define CBDP_MAGIC_0 0xCB
#define CBDP_MAGIC_1 0x01

#define CBDP_MSG_HELLO 0x01
#define CBDP_MSG_WELCOME 0x02
#define CBDP_MSG_AUTH 0x03
#define CBDP_MSG_AUTH_OK 0x04
#define CBDP_MSG_PING 0x05
#define CBDP_MSG_PONG 0x06
#define CBDP_MSG_SUBSCRIBE 0x07
#define CBDP_MSG_UNSUBSCRIBE 0x08
#define CBDP_MSG_SNAPSHOT 0x09
#define CBDP_MSG_DELTA 0x0A
#define CBDP_MSG_ACK 0x0B
#define CBDP_MSG_RESYNC 0x0C
#define CBDP_MSG_COMMAND 0x0D
#define CBDP_MSG_RESULT 0x0E
#define CBDP_MSG_ERROR 0x0F
#define CBDP_MSG_SEND_BEGIN 0x10
#define CBDP_MSG_SEND_CHUNK 0x11
#define CBDP_MSG_SEND_END 0x12
#define CBDP_MSG_AUDIO_BEGIN 0x13
#define CBDP_MSG_AUDIO_CHUNK 0x14
#define CBDP_MSG_AUDIO_END 0x15
#define CBDP_MSG_APPROVAL_REQUEST 0x16
#define CBDP_MSG_APPROVAL_RESOLVE 0x17
#define CBDP_MSG_DEVICE_STATUS 0x18
#define CBDP_MSG_CAPABILITY_UPDATE 0x19
#define CBDP_MSG_CLOSE 0x1A
#define CBDP_MSG_FRAGMENT 0x1B
#define CBDP_MSG_DISCOVERY 0x1C
#define CBDP_MSG_DISCOVERY_REPLY 0x1D
#define CBDP_MSG_HOME_COUNTERS 0x1E
/* Bridge -> device: wireless pairing token offer, sent right after WELCOME
   when the device asked for pairing in its HELLO. Bridge-to-device only. */
#define CBDP_MSG_PAIRING 0x1F

#define CBDP_FLAG_MORE_FRAGMENTS 0x0001
#define CBDP_FLAG_COMPRESSED 0x0002
#define CBDP_FLAG_CRC32_PRESENT 0x0004
#define CBDP_FLAG_URGENT 0x0008
#define CBDP_FLAG_NO_ACK 0x0010
#define CBDP_FLAG_TEXT_TRUNCATED 0x0020
#define CBDP_FLAG_SNAPSHOT_PAGE 0x0040
#define CBDP_FLAG_RESUMED 0x0080
#define CBDP_FLAG_REQUEST 0x0100
#define CBDP_FLAG_RESPONSE 0x0200
#define CBDP_FLAG_EVENT 0x0400
#define CBDP_FLAG_COMMAND 0x0800
#define CBDP_FLAG_VENDOR_EXTENSION 0x8000

#define CBDP_QOS_CRITICAL 0
#define CBDP_QOS_STATE 1
#define CBDP_QOS_TELEMETRY 2

#define CBDP_PROFILE_PANEL 1

/* pairing_state values. NONE is the implicit default when the TLV is absent,
   so a device that already holds a token keeps its previous HELLO shape. */
#define CBDP_PAIRING_STATE_NONE 0
#define CBDP_PAIRING_STATE_REQUEST 1
#define CBDP_PAIRING_STATE_OFFER 2

/* The pairing token is UTF-8 and MUST be 16..64 bytes on the wire. */
#define CBDP_PAIRING_TOKEN_MIN_BYTES 16
#define CBDP_PAIRING_TOKEN_MAX_BYTES 64

/* Error codes that mean the stored token itself is unusable. AUTH_EXPIRED is
   deliberately not one of them: it only asks for a fresh AUTH. */
#define CBDP_ERR_UNAUTHORIZED 0x0003
#define CBDP_ERR_AUTH_EXPIRED 0x0017
#define CBDP_ERR_TOKEN_ROTATED 0x0018

#define CBDP_SCOPE_SUMMARY 0
#define CBDP_SCOPE_THREAD 1
#define CBDP_SCOPE_LIVE 2

#define CBDP_TLV_VERSION_MINOR 0x0001
#define CBDP_TLV_DEVICE_ID 0x0002
#define CBDP_TLV_MODEL 0x0003
#define CBDP_TLV_FIRMWARE 0x0004
#define CBDP_TLV_NAME 0x0005
#define CBDP_TLV_PROFILE 0x0006
#define CBDP_TLV_SUPPORTED_CAPS 0x0007
#define CBDP_TLV_ENABLED_CAPS 0x0008
#define CBDP_TLV_EFFECTIVE_CAPS 0x0009
#define CBDP_TLV_TRANSPORT_SECURITY 0x000A
#define CBDP_TLV_CLIENT_NONCE 0x000B
#define CBDP_TLV_SESSION_ID 0x000C
#define CBDP_TLV_SERVER_NONCE 0x000D
#define CBDP_TLV_AUTH_PROOF 0x000E
#define CBDP_TLV_AUTH_MODE 0x000F
#define CBDP_TLV_MAX_FRAME_BYTES 0x0010
#define CBDP_TLV_MAX_TEXT_CHARS 0x0011
#define CBDP_TLV_QUEUE_DEPTH 0x0012
#define CBDP_TLV_HEARTBEAT_SECONDS 0x0013
#define CBDP_TLV_HISTORY_DEPTH 0x0014
#define CBDP_TLV_MAX_SUBSCRIPTIONS 0x0015
#define CBDP_TLV_MAX_AUDIO_BYTES 0x0016
#define CBDP_TLV_POLICY_VERSION 0x0017
#define CBDP_TLV_TRANSPORT_ALLOWED 0x0018
#define CBDP_TLV_SNAPSHOT_PAGE_OBJECTS 0x0019
#define CBDP_TLV_MAX_CHUNK_BYTES 0x001A
#define CBDP_TLV_COALESCE_WINDOW_MS 0x001B
#define CBDP_TLV_REQUEST_TTL_SECONDS 0x001D
#define CBDP_TLV_WRITER_LEASE_SECONDS 0x001E
#define CBDP_TLV_MAX_TEXT_BYTES 0x001F
#define CBDP_TLV_DEVICE_REF 0x0020
#define CBDP_TLV_THREAD_REF 0x0021
#define CBDP_TLV_THREAD_ID 0x0022
#define CBDP_TLV_TURN_REF 0x0023
#define CBDP_TLV_TURN_ID 0x0024
#define CBDP_TLV_MESSAGE_REF 0x0025
#define CBDP_TLV_MESSAGE_ID 0x0026
#define CBDP_TLV_APPROVAL_REF 0x0027
#define CBDP_TLV_APPROVAL_ID 0x0028
#define CBDP_TLV_EVENT_ID 0x0029
#define CBDP_TLV_REQUEST_ID 0x002A
#define CBDP_TLV_IDEM 0x002B
#define CBDP_TLV_METHOD_ID 0x002C
#define CBDP_TLV_TX_ID 0x002D
#define CBDP_TLV_SNAPSHOT_ID 0x002E
#define CBDP_TLV_OWNER_DEVICE_REF 0x002F
#define CBDP_TLV_TOPIC_ID 0x0030
#define CBDP_TLV_SCOPE 0x0031
#define CBDP_TLV_QOS 0x0032
#define CBDP_TLV_CURSOR 0x0033
#define CBDP_TLV_TOPIC_SEQ 0x0034
#define CBDP_TLV_GLOBAL_SEQ 0x0035
#define CBDP_TLV_REVISION 0x0036
#define CBDP_TLV_EVENT_TYPE 0x0037
#define CBDP_TLV_EVENT_PAYLOAD 0x0038
#define CBDP_TLV_SNAPSHOT_REVISION 0x003C
#define CBDP_TLV_PAGE_INDEX 0x003D
#define CBDP_TLV_PAGE_COUNT 0x003E
#define CBDP_TLV_OBJECT_TYPE 0x003F
#define CBDP_TLV_TITLE 0x0040
#define CBDP_TLV_SUMMARY 0x0041
#define CBDP_TLV_BODY 0x0042
#define CBDP_TLV_BODY_REF 0x0043
#define CBDP_TLV_STATUS 0x0044
#define CBDP_TLV_KIND 0x0045
#define CBDP_TLV_TOTAL_BYTES 0x0047
#define CBDP_TLV_OFFSET 0x0048
#define CBDP_TLV_DATA 0x0049
#define CBDP_TLV_CREATED_AT_MS 0x004B
#define CBDP_TLV_UPDATED_AT_MS 0x004C
#define CBDP_TLV_STARTED_AT_MS 0x004D
#define CBDP_TLV_COMPLETED_AT_MS 0x004E
#define CBDP_TLV_CURRENT_ACTION 0x004F
#define CBDP_TLV_MODEL_NAME 0x0051
#define CBDP_TLV_CWD 0x0052
#define CBDP_TLV_READ_AT_MS 0x0054
#define CBDP_TLV_SENDER 0x0055
#define CBDP_TLV_PARAMS 0x0060
#define CBDP_TLV_RESULT 0x0061
#define CBDP_TLV_ERROR_MESSAGE 0x0062
#define CBDP_TLV_STATE 0x0064
#define CBDP_TLV_MODE 0x0070
#define CBDP_TLV_BASE_REVISION 0x0071
#define CBDP_TLV_CLIENT_TIMESTAMP_MS 0x0072
#define CBDP_TLV_USE_ACTIVE_THREAD 0x007B
#define CBDP_TLV_PROMPT 0x0080
#define CBDP_TLV_OPTIONS 0x0081
#define CBDP_TLV_DECISION 0x0082
#define CBDP_TLV_RISK 0x0084
#define CBDP_TLV_EXPIRES_AT_MS 0x0085
#define CBDP_TLV_DEFAULT_DECISION 0x0086
#define CBDP_TLV_APPROVAL_KIND 0x0087
#define CBDP_TLV_ONLINE 0x0090
#define CBDP_TLV_DEVICE_STATE 0x0091
#define CBDP_TLV_QUEUE_DEPTH_CURRENT 0x0092
#define CBDP_TLV_REASON 0x0096
#define CBDP_TLV_PING_ID 0x0097
#define CBDP_TLV_SERVER_TIME_MS 0x0098
#define CBDP_TLV_ECHO 0x0099
#define CBDP_TLV_OBJECT 0x00A0
#define CBDP_TLV_OBJECTS 0x00A1
#define CBDP_TLV_OBJECT_COUNT 0x00A2
#define CBDP_TLV_COMPLETE 0x00A3
#define CBDP_TLV_EVENT 0x00A4
#define CBDP_TLV_NEXT_CURSOR 0x00A5
#define CBDP_TLV_SNAPSHOT_KIND 0x00A6
#define CBDP_TLV_THREAD_STATE 0x00A7
#define CBDP_TLV_OWNER_STATE 0x00A8
#define CBDP_TLV_RESYNC_REASON 0x00A9
#define CBDP_TLV_CAPABILITY_CATALOG 0x00AA
#define CBDP_TLV_CAPABILITY_SOURCE 0x00AB
#define CBDP_TLV_CAPABILITY_BITS 0x00AC
#define CBDP_TLV_PORT 0x00B0
#define CBDP_TLV_DISCOVERY_NONCE 0x00B1
#define CBDP_TLV_DISCOVERY_SCOPE 0x00B2
#define CBDP_TLV_BRIDGE_INSTANCE_ID 0x00B3
#define CBDP_TLV_PRIORITY_CLASS 0x00B4
#define CBDP_TLV_WINDOW_FRAMES 0x00B6
#define CBDP_TLV_WINDOW_BYTES 0x00B7
#define CBDP_TLV_RETRANSMIT_TIMEOUT_MS 0x00B8
#define CBDP_TLV_MAX_RETRIES 0x00B9
#define CBDP_TLV_FRAGMENT_TIMEOUT_MS 0x00BA
#define CBDP_TLV_ACK_DELAY_MS 0x00BB
/* system.codex_state (0x0704) counters. The Bridge is the authoritative
   source for the running / unread counters shown on the panel. */
#define CBDP_TLV_RUNNING_COUNT 0x00BC
#define CBDP_TLV_COMPLETED_UNREAD 0x00BD
#define CBDP_TLV_FAILED_UNREAD 0x00BE
/* Wireless pairing. pairing_state is valid in HELLO (device request) and in
   PAIRING (bridge offer); pairing_token is PAIRING-only. */
#define CBDP_TLV_PAIRING_STATE 0x00C0
#define CBDP_TLV_PAIRING_TOKEN 0x00C1
#define CBDP_TLV_FRAGMENT_STREAM_ID 0x00F0
#define CBDP_TLV_FRAGMENT_INDEX 0x00F1
#define CBDP_TLV_FRAGMENT_TOTAL 0x00F2
#define CBDP_TLV_ORIGINAL_TYPE 0x00F3
#define CBDP_TLV_ORIGINAL_FLAGS 0x00F4
#define CBDP_TLV_ORIGINAL_PAYLOAD_LEN 0x00F5
#define CBDP_TLV_ORIGINAL_HEADER_TLV 0x00F6
#define CBDP_TLV_PAYLOAD_CRC32 0x00FF

/* Account / rate-limit / token-usage fields (account events and reads).
   remaining_percent is the REMAINING percentage: the Bridge already inverted
   usedPercent, so the firmware must never invert it again. reset_at_ms is
   epoch milliseconds; the Bridge already scaled the upstream epoch seconds. */
#define CBDP_TLV_TOKEN_USAGE 0x0050
#define CBDP_TLV_ACCOUNT_REF 0x0180
#define CBDP_TLV_ACCOUNT_ID 0x0181
#define CBDP_TLV_PLAN_TYPE 0x0182
#define CBDP_TLV_RATE_LIMITS 0x0183
#define CBDP_TLV_RATE_LIMIT_WINDOW 0x0184
#define CBDP_TLV_REMAINING_PERCENT 0x0185
#define CBDP_TLV_RESET_AT_MS 0x0186
#define CBDP_TLV_SEVEN_DAY_TOKENS 0x018D
#define CBDP_TLV_SEVEN_DAY_AS_OF 0x018E
#define CBDP_TLV_SEVEN_DAY_WINDOW_DAYS 0x018F
#define CBDP_TLV_TOKEN_TOTAL 0x01AE
#define CBDP_TLV_TOKEN_INPUT 0x01AF
#define CBDP_TLV_TOKEN_OUTPUT 0x01B0
#define CBDP_TLV_TOKEN_CACHED 0x01B1
#define CBDP_TLV_TOKEN_REASONING 0x01B2

#define CBDP_METHOD_MESSAGE_READ_UPDATE 0x00A0
#define CBDP_METHOD_ACTIVE_STATE_READ 0x00B1

/* Account, rate-limit and token-usage reads (method_id). The Bridge answers
   these from the same committed event store it pushes account.changed,
   rate_limits.changed and token_usage.changed from, so a pull and a push can
   never disagree. */
#define CBDP_METHOD_ACCOUNT_READ 0x0090
#define CBDP_METHOD_ACCOUNT_RATE_LIMITS_READ 0x0091
#define CBDP_METHOD_ACCOUNT_TOKEN_USAGE_READ 0x0092

/* Account-scoped state events published on system.thread_summaries. */
#define CBDP_EVENT_ACCOUNT_CHANGED 0x0E01
#define CBDP_EVENT_RATE_LIMITS_CHANGED 0x0E02
#define CBDP_EVENT_TOKEN_USAGE_CHANGED 0x0E03

#define CBDP_CAP_STATUS (1ULL << 0)
#define CBDP_CAP_TEXT (1ULL << 1)
#define CBDP_CAP_TASKS (1ULL << 2)
#define CBDP_CAP_RESULTS (1ULL << 3)
#define CBDP_CAP_APPROVAL (1ULL << 4)
#define CBDP_CAP_AUDIO (1ULL << 5)
#define CBDP_CAP_VOICE (1ULL << 6)
#define CBDP_CAP_READ_ACK (1ULL << 7)
#define CBDP_CAP_CONTROL (1ULL << 8)
#define CBDP_CAP_HISTORY (1ULL << 9)
#define CBDP_CAP_TELEMETRY (1ULL << 10)
#define CBDP_CAP_SUBSCRIBE_LIVE (1ULL << 11)
#define CBDP_CAP_SUBSCRIBE_THREAD (1ULL << 12)
#define CBDP_CAP_SUBSCRIBE_SUMMARY (1ULL << 13)
#define CBDP_CAP_WRITER_LEASE (1ULL << 14)
#define CBDP_CAP_MULTI_PAGE (1ULL << 15)
#define CBDP_CAP_TLS (1ULL << 17)
#define CBDP_CAP_DEVICE_CONFIG (1ULL << 19)
#define CBDP_CAP_COMMAND_STATUS (1ULL << 20)
#define CBDP_CAP_ACCOUNT (1ULL << 42)
#define CBDP_CAP_CONTEXT_USAGE (1ULL << 43)
#define CBDP_CAP_READ_UNREAD (1ULL << 45)
#define CBDP_CAP_NOTICE (1ULL << 47)
#define CBDP_CAP_MODEL_SWITCH (1ULL << 52)
#define CBDP_CAP_CROSS_DEVICE_ACTIVE (1ULL << 54)
#define CBDP_CAP_RESUME (1ULL << 55)
#define CBDP_CAP_EVENTS_SINCE (1ULL << 61)

typedef struct {
    uint16_t tag;
    uint16_t length;
    const uint8_t *value;
} codex_bridge_tlv_t;

typedef struct {
    uint8_t version_major;
    uint8_t message_type;
    uint16_t flags;
    uint16_t header_len;
    uint32_t payload_len;
    uint32_t topic_id;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t code;
    uint8_t qos;
    const uint8_t *header_tlv;
    size_t header_tlv_len;
    const uint8_t *payload;
    size_t frame_len;
} codex_bridge_frame_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t length;
    bool failed;
} codex_bridge_tlv_builder_t;

esp_err_t codex_bridge_build_discovery_reply(
    const codex_bridge_frame_t *request,
    const char *device_id,
    const char *model,
    const char *firmware,
    uint8_t profile,
    uint64_t supported_caps,
    uint16_t port,
    uint8_t transport_security,
    uint8_t *out,
    size_t capacity,
    size_t *out_len
);
esp_err_t codex_bridge_build_presence_reply(
    const char *device_id,
    const char *model,
    const char *firmware,
    uint8_t profile,
    uint64_t supported_caps,
    uint16_t port,
    uint8_t transport_security,
    uint8_t *out,
    size_t capacity,
    size_t *out_len
);
esp_err_t codex_bridge_auth_proof(
    const char *token,
    const uint8_t session_id[16],
    const uint8_t client_nonce[16],
    const uint8_t server_nonce[16],
    const char *device_id,
    uint8_t proof[32]
);
esp_err_t codex_bridge_frame_parse(
    const uint8_t *bytes,
    size_t length,
    codex_bridge_frame_t *out
);
esp_err_t codex_bridge_frame_build(
    uint8_t *out,
    size_t capacity,
    uint8_t message_type,
    uint16_t flags,
    uint32_t topic_id,
    uint32_t seq,
    uint32_t ack_seq,
    uint16_t code,
    uint8_t qos,
    const codex_bridge_tlv_builder_t *header,
    const uint8_t *payload,
    size_t payload_len,
    size_t *out_len
);
esp_err_t codex_bridge_tlv_next(
    const uint8_t **cursor,
    const uint8_t *end,
    codex_bridge_tlv_t *out
);
esp_err_t codex_bridge_tlv_find(
    const uint8_t *tlv,
    size_t tlv_len,
    uint16_t tag,
    codex_bridge_tlv_t *out
);
esp_err_t codex_bridge_tlv_u8(const codex_bridge_tlv_t *tlv, uint8_t *out);
esp_err_t codex_bridge_tlv_u16(const codex_bridge_tlv_t *tlv, uint16_t *out);
esp_err_t codex_bridge_tlv_u32(const codex_bridge_tlv_t *tlv, uint32_t *out);
esp_err_t codex_bridge_tlv_u64(const codex_bridge_tlv_t *tlv, uint64_t *out);
/* 11.8: parse a PAIRING(0x1F) offer. Requires pairing_state=OFFER and a
   pairing_token of 16..64 UTF-8 bytes with no embedded NUL, copying the token
   as a NUL-terminated string. Returns ESP_ERR_INVALID_ARG for a malformed or
   non-offer frame and ESP_ERR_INVALID_SIZE for an out-of-range token. */
esp_err_t codex_bridge_parse_pairing_offer(
    const uint8_t *tlv,
    size_t tlv_len,
    char *token_out,
    size_t capacity
);

esp_err_t codex_bridge_tlv_utf8(
    const codex_bridge_tlv_t *tlv,
    char *out,
    size_t capacity
);
void codex_bridge_tlv_builder_init(
    codex_bridge_tlv_builder_t *builder,
    uint8_t *data,
    size_t capacity
);
esp_err_t codex_bridge_tlv_builder_add(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    const void *value,
    size_t length
);
esp_err_t codex_bridge_tlv_builder_u8(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    uint8_t value
);
esp_err_t codex_bridge_tlv_builder_u16(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    uint16_t value
);
esp_err_t codex_bridge_tlv_builder_u32(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    uint32_t value
);
esp_err_t codex_bridge_tlv_builder_u64(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    uint64_t value
);
esp_err_t codex_bridge_tlv_builder_utf8(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    const char *value
);
esp_err_t codex_bridge_tlv_builder_bytes(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    const void *value,
    size_t length
);
