# CBDP v1 ESP32-S3 Panel Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Dotii firmware's legacy JSON-line Codex transport with a CBDP v1 `panel` client that interoperates with the frozen protocol document.

**Architecture:** Keep the existing `codex_bridge_client_start(handler)` boundary so the UI and BLE status code need minimal changes. Under that boundary, use a pure CBDP frame/TLV codec plus a bounded projection translator that turns CBDP snapshots and deltas into the existing `codex_state`, `message`, and `read_state` handler records. The device remains a TCP listener on 8765 and UDP discovery responder on 8764, as required by Device Listener Binding.

**Tech Stack:** ESP-IDF 5.5/6.x, C, FreeRTOS sockets, mbedTLS HMAC-SHA256, cJSON only for the existing UI boundary, Unity tests.

**Spec:** `D:/dotii/CodexBridge-fix/docs/CBDP-V1-PROTOCOL.md`

## Global Constraints

- Modify only Dotii firmware sources/tests/docs. Do not modify `CodexBridge-fix` or mobile APK sources.
- Device profile is `panel` (`profile=1`).
- CBDP fixed header is 28 bytes, big-endian, magic `CB 01`, major version `1`.
- The device listens on TCP `8765`; Bridge initiates TCP and the device immediately sends `HELLO`.
- Discovery is binary CBDP on UDP `8764`; no JSON discovery fallback.
- Supported message types, tags, flags, error codes, and capabilities come only from the frozen protocol document.
- Unknown TLVs are skipped. A repeated known scalar TLV is `INVALID_FRAME`.
- `compression`, `aead_reserved`, `vendor`, `batch`, and all other `[本轮不实现]` features must never be advertised or sent.
- Auth must use `HMAC-SHA256` mode `1` and the exact `CBDP1-AUTH` construction.
- Token values, auth proofs, and nonces must never be logged.
- The device must use budgets returned by `WELCOME`, not inferred panel defaults.
- Partial multi-page snapshots must not replace committed UI state.
- Unknown events must safely advance cursor after being applied as a no-op.
- No completion claim without fresh unit-test and firmware-build output.

## Review Focus

- Any frame with truncation, trailing bytes, an oversized length, duplicate known TLV, or invalid reserved fields must fail closed without memory corruption.
- Any session-id/nonce/HMAC mismatch or replayed auth must fail closed and must not mark the client online.
- Duplicate transport sequence frames must be ACKed but not applied twice; gaps must not advance the cumulative ACK.
- Multi-page snapshots must be atomic at the projection boundary.
- An unknown event type or an unsupported optional TLV must not disconnect a valid session.

---

### Task 1: CBDP Frame and TLV Codec

**Files:**
- Replace: `main/codex_bridge_protocol.h`
- Replace: `main/codex_bridge_protocol.c`
- Create: `test_apps/codex_bridge_protocol/main/test_cbdp_codec.c`
- Modify: `test_apps/codex_bridge_protocol/main/CMakeLists.txt`

**Interfaces:**
- Consumes: raw byte buffers and protocol constants from the spec.
- Produces:
  - `typedef struct { uint16_t tag; uint16_t length; const uint8_t *value; } codex_bridge_tlv_t;`
  - `typedef struct { ... uint16_t message_type; uint16_t flags; uint16_t header_len; uint32_t payload_len; uint32_t topic_id; uint32_t seq; uint32_t ack_seq; uint16_t code; uint8_t qos; const uint8_t *header_tlv; size_t header_tlv_len; const uint8_t *payload; size_t payload_len; size_t frame_len; } codex_bridge_frame_t;`
  - `esp_err_t codex_bridge_frame_parse(const uint8_t *bytes, size_t length, codex_bridge_frame_t *out);`
  - `esp_err_t codex_bridge_tlv_next(const uint8_t **cursor, const uint8_t *end, codex_bridge_tlv_t *out);`
  - `esp_err_t codex_bridge_tlv_find(const uint8_t *tlv, size_t tlv_len, uint16_t tag, codex_bridge_tlv_t *out);`
  - typed readers `codex_bridge_tlv_u8/u16/u32/u64`, `codex_bridge_tlv_utf8`.
  - `typedef struct { uint8_t *data; size_t capacity; size_t length; bool failed; } codex_bridge_tlv_builder_t;`
  - `void codex_bridge_tlv_builder_init(...)`, `..._add(...)`, `..._u8/u16/u32/u64/utf8/bytes(...)`.
  - `esp_err_t codex_bridge_frame_build(uint8_t *out, size_t capacity, uint8_t message_type, uint16_t flags, uint32_t topic_id, uint32_t seq, uint32_t ack_seq, uint16_t code, uint8_t qos, const codex_bridge_tlv_builder_t *header, const uint8_t *payload, size_t payload_len, size_t *out_len);`

- [ ] **Step 1: Write failing codec tests**

Create `test_cbdp_codec.c` with tests named:
- `TEST_CASE("CBDP frame header round trips fixed fields", "[cbdp_codec]")`
- `TEST_CASE("CBDP parser rejects magic version and impossible lengths", "[cbdp_codec]")`
- `TEST_CASE("CBDP parser rejects trailing bytes and truncated TLV", "[cbdp_codec]")`
- `TEST_CASE("CBDP TLV lookup accepts reordered unknown and rejects duplicate known tags", "[cbdp_codec]")`
- `TEST_CASE("CBDP builder keeps payload separate from header TLV", "[cbdp_codec]")`

Assertions must include the exact 28-byte size, magic bytes `CB 01`, big-endian `header_len`/`payload_len`, unknown-TLV skip behavior, and duplicate detection.

- [ ] **Step 2: Run tests and confirm RED**

Run the codec test target with the ESP-IDF Unity runner. Expected: compile failure because the new codec types/functions do not exist.

- [ ] **Step 3: Implement minimal codec**

Implement bounds-first frame parsing and TLV iteration/builders. Do not allocate. Reject payload on any message type other than `SEND_CHUNK`, `AUDIO_CHUNK`, `FRAGMENT`, and `RESULT` (`content.read` only).

- [ ] **Step 4: Run codec tests and confirm GREEN**

Run the codec suite. Expected: all `[cbdp_codec]` tests pass.

### Task 2: Authentication and Discovery

**Files:**
- Modify: `main/codex_bridge_protocol.h`
- Modify: `main/codex_bridge_protocol.c`
- Modify: `main/codex_bridge_client.c`
- Create: `test_apps/codex_bridge_protocol/main/test_cbdp_discovery.c`
- Modify: `test_apps/codex_bridge_protocol/main/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 codec, `device_config_get()->codex_bridge_token`, `device_identity_get()`.
- Produces:
  - `esp_err_t codex_bridge_auth_proof(const char *token, const uint8_t session_id[16], const uint8_t client_nonce[16], const uint8_t server_nonce[16], const char *device_id, uint8_t proof[32]);`
  - `esp_err_t codex_bridge_build_discovery_reply(...)` internal to client.

- [ ] **Step 1: Write failing tests**

Tests:
- `TEST_CASE("CBDP HMAC matches frozen authentication vector", "[cbdp_auth]")`
- `TEST_CASE("CBDP discovery request receives binary reply with echoed nonce", "[cbdp_discovery]")`
- `TEST_CASE("CBDP discovery ignores malformed non-discovery datagrams", "[cbdp_discovery]")`

The frozen HMAC test must use session `0102030405060708090A0B0C0D0E0F10`, zero client nonce, server nonce `1112131415161718191A1B1C1D1E1F20`, device `esp32-lab`, token `0123456789abcdef0123456789abcdef`, and expected proof `7DF2DEC2FAE61A43D69100EA217F30067E05937BF187BCE02AAF355F7DC74A92`.

- [ ] **Step 2: Run tests and confirm RED**

Expected: auth helper and binary discovery are absent.

- [ ] **Step 3: Implement HMAC and binary discovery**

Use `mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), ...)` over exactly `CBDP1-AUTH || session_id || client_nonce || server_nonce || device_id`. Replace the ASCII discovery branch with CBDP `DISCOVERY`/`DISCOVERY_REPLY` frames.

- [ ] **Step 4: Run tests and confirm GREEN**

Expected: `[cbdp_auth]` and `[cbdp_discovery]` pass.

### Task 3: Device Listener Session State Machine

**Files:**
- Replace transport/session portions of: `main/codex_bridge_client.c`
- Modify: `main/codex_bridge_client.h`
- Create: `test_apps/codex_bridge_protocol/main/test_cbdp_session.c`
- Modify: `test_apps/codex_bridge_protocol/main/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 codec, Task 2 auth/discovery.
- Produces:
  - `codex_bridge_client_start(handler, context)` unchanged.
  - `codex_bridge_client_stop()`, `is_online()`, capability getters unchanged.
  - New `uint64_t codex_bridge_client_next_request_id(void);`
  - Internal `send_frame_locked(...)`, `read_frame(...)`, `handle_session_frame(...)`.

**Required session order:** device sends `HELLO`; Bridge sends `WELCOME`; device sends `AUTH`; Bridge sends `AUTH_OK`; only then may business frames flow.

- [ ] **Step 1: Write failing session tests**

Fixture server tests:
- `TEST_CASE("CBDP device sends HELLO before WELCOME and authenticates with HMAC", "[cbdp_session]")`
- `TEST_CASE("CBDP rejects wrong server nonce without going online", "[cbdp_session]")`
- `TEST_CASE("CBDP promotes online only after matching AUTH_OK", "[cbdp_session]")`
- `TEST_CASE("CBDP heartbeat sends PING and accepts PONG", "[cbdp_session]")`
- `TEST_CASE("CBDP duplicate inbound seq is ACKed but delivered once", "[cbdp_session]")`

Use a real loopback TCP listener fixture in the test app. Assert `supported_caps` is the panel subset implemented by this firmware and `profile == 1`.

- [ ] **Step 2: Run tests and confirm RED**

Expected: legacy JSON handshake fails against the CBDP fixture.

- [ ] **Step 3: Implement HELLO/WELCOME/AUTH/AUTH_OK and heartbeat**

Parse WELCOME budgets into session state. Use returned `max_frame_bytes`, `max_chunk_bytes`, and `heartbeat_seconds`. Keep one active device-listener session. Maintain independent inbound/outbound transport sequence numbers starting at 1 after reconnect. Reject unknown auth mode, wrong request response, and stale replay.

- [ ] **Step 4: Run tests and confirm GREEN**

Expected: all `[cbdp_session]` tests pass.

### Task 4: Subscriptions, Snapshot/DELTA Projection, and Cursor ACK

**Files:**
- Create: `main/codex_bridge_projection.h`
- Create: `main/codex_bridge_projection.c`
- Modify: `main/codex_bridge_client.c`
- Modify: `main/codex_bridge_client.h`
- Modify: `main/CMakeLists.txt`
- Create: `test_apps/codex_bridge_protocol/main/test_cbdp_projection.c`
- Modify: `test_apps/codex_bridge_protocol/main/CMakeLists.txt`

**Interfaces:**
- Consumes: parsed `SNAPSHOT`, `DELTA`, `RESULT`, `CAPABILITY_UPDATE` frames.
- Produces internal projection functions that emit the existing `codex_bridge_message_t` handler shape:
  - `type="codex_state"` for counters and account/quota state.
  - `type="message", kind="task"` for thread/task arrays.
  - `type="message", kind="status"` for active thread/current action.
  - `type="read_state"` for `message.read`.
- `codex_bridge_projection_commit_snapshot(...)` must be atomic across pages.
- `codex_bridge_projection_apply_delta(...)` must be idempotent on topic/revision.
- `codex_bridge_projection_build_ack(...)` sets cursor only after successful apply.

- [ ] **Step 1: Write failing projection tests**

Tests:
- `TEST_CASE("CBDP snapshot commits only after final page", "[cbdp_projection]")`
- `TEST_CASE("CBDP thread and message objects emit existing task JSON", "[cbdp_projection]")`
- `TEST_CASE("CBDP message appended delta updates task and advances cursor", "[cbdp_projection]")`
- `TEST_CASE("CBDP unknown event is a no-op but advances cursor", "[cbdp_projection]")`
- `TEST_CASE("CBDP message read delta matches external bridge fields", "[cbdp_projection]")`

Expected JSON keys for task items: `thread_id`, `title`, `status`, `message_id`, `result_message_id`, `last_user_message`, `conversation_text`, `last_updated_epoch`, `message_count`. Use stable textual forms `thread-<ref>` and `msg-<ref>` only at the cJSON boundary.

- [ ] **Step 2: Run tests and confirm RED**

Expected: CBDP data is not delivered to the existing handler.

- [ ] **Step 3: Implement bounded projection and subscriptions**

After `AUTH_OK`, subscribe to:
- `system.thread_summaries` (`0x50000003`), `summary` scope `0`, `state` QoS `1`.
- `system.resync` (`0x50000002`), `live` scope `2`, `critical` QoS `0`.

Store returned/known cursors per `(topic_id, scope)` and send `ACK` with the new cursor after applying a `DELTA`. Do not commit a snapshot until `complete=true` and all pages are present.

- [ ] **Step 4: Run tests and confirm GREEN**

Expected: all `[cbdp_projection]` tests pass.

### Task 5: Read ACK, Device Status, and BLE Status Compatibility

**Files:**
- Modify: `main/codex_bridge_client.c`
- Modify: `main/codex_bridge_client.h`
- Modify: `main/state_ui.c` only if a reference selector is required
- Modify: `test_apps/codex_bridge_protocol/main/test_cbdp_projection.c`

**Interfaces:**
- Consumes: `codex_bridge_client_send_read_ack(const char *message_id)` existing UI call.
- Produces: `COMMAND(message.read.update, method_id=0x00A0)` with 16-byte idem, `message_id` or `message_ref`, `read_at_ms`, `request_id`.
- Capability getters continue returning user-facing names derived from `effective_caps`.

- [ ] **Step 1: Write failing test**

`TEST_CASE("CBDP read ack uses message.read.update and stable idem", "[cbdp_command]")`
Assert flags include `REQUEST|COMMAND`, method `0x00A0`, exact message id/ref, and identical idem on retry.

- [ ] **Step 2: Run tests and confirm RED**

Expected: legacy `read_ack` JSON is still sent.

- [ ] **Step 3: Implement command encoding and capability-name mapping**

Map every supported capability bit to its protocol name for BLE and UI introspection. Keep a bounded message-ref-to-thread-ref entry when a message event is applied so read ACK can include the optional thread selector.

- [ ] **Step 4: Run tests and confirm GREEN**

Expected: `[cbdp_command]` passes.

### Task 6: Firmware Integration and Verification

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `test_apps/codex_bridge_protocol/main/CMakeLists.txt`
- Modify: `开发指南.md` only if protocol architecture text is now stale.

- [ ] **Step 1: Wire projection into firmware build**

Add `codex_bridge_projection.c` to both component and test app source lists. Remove cJSON dependency from the codec itself, but keep it in the client/projection boundary.

- [ ] **Step 2: Run focused tests**

Run the Unity test app. Expected: `0 failures` for all `[cbdp_*]` tests.

- [ ] **Step 3: Run the full firmware build**

Use the configured ESP-IDF environment and run `idf.py build`. Expected: exit code `0`, no warnings from modified firmware files.

- [ ] **Step 4: Inspect the final diff**

Run `git diff --check` and `git status --short -- main test_apps/codex_bridge_protocol`. Confirm no `CodexBridge-fix` or mobile files changed.

- [ ] **Step 5: Commit only firmware changes**

Commit the modified `main/` files and CBDP tests with a conventional message such as `feat(firmware): migrate Codex bridge to CBDP v1`.

## Self-Review

- Spec sections 3-15, 17, 18, 20-23, 25, 28.1-28.6, 29.3-29.4 are covered by codec, auth, session, subscription, projection, and command tasks.
- Full-payload `SEND_*`, `AUDIO_*`, and approval UI are not wired because the current Dotii UI has no text/audio/approval controls. Their frame constants and parser support remain available, and their capability bits must stay clear until the corresponding UI path exists.
- Unknown TLV/event handling, multi-page snapshot atomicity, and duplicate sequence handling each have an owning test.
- All later tasks consume exact types and function names introduced in Task 1; no parallel shape is defined.
