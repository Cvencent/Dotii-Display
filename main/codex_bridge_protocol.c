#include "codex_bridge_protocol.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "mbedtls/md.h"
#include "esp_random.h"

#define CODEX_BRIDGE_CHUNK_MAX 32
#define CODEX_BRIDGE_CHUNK_BODY_MAX 480
#define CODEX_BRIDGE_TRUNCATION_SUFFIX "\n...[已截断]"

static const char *const SUPPORTED_CAPABILITIES[] = {
    "status", "text", "tasks", "results", "read_ack", "quota",
};

static char s_chunk_message_id[CODEX_BRIDGE_ID_MAX];
static char *s_chunks[CODEX_BRIDGE_CHUNK_MAX];
static int s_chunk_total;

static bool supported_capability(const char *value)
{
    for (size_t index = 0;
         index < sizeof(SUPPORTED_CAPABILITIES) / sizeof(SUPPORTED_CAPABILITIES[0]);
         ++index) {
        if (strcmp(value, SUPPORTED_CAPABILITIES[index]) == 0) return true;
    }
    return false;
}

static void *bridge_malloc(size_t size)
{
    void *buffer = heap_caps_malloc(
        size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (buffer == NULL) {
        buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

static void clear_chunks(void)
{
    for (size_t index = 0; index < CODEX_BRIDGE_CHUNK_MAX; ++index) {
        heap_caps_free(s_chunks[index]);
        s_chunks[index] = NULL;
    }
    s_chunk_message_id[0] = '\0';
    s_chunk_total = 0;
}

static void clear_message_fields(codex_bridge_message_t *message)
{
    if (message == NULL) return;
    heap_caps_free(message->body);
    cJSON_Delete(message->data);
    memset(message, 0, sizeof(*message));
}

static void copy_string(cJSON *root, const char *name, char *target, size_t size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        strlcpy(target, value->valuestring, size);
    }
}

static void copy_capabilities(cJSON *root, codex_bridge_message_t *message)
{
    /* bridge_ready carries "capabilities"; capabilities_update carries the
       recomputed effective set in "enabled". */
    cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    if (!cJSON_IsArray(capabilities)) {
        capabilities = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    }
    if (!cJSON_IsArray(capabilities)) return;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, capabilities) {
        if (!cJSON_IsString(item) || item->valuestring == NULL) continue;
        if (!supported_capability(item->valuestring)) continue;
        if (message->capability_count >= CODEX_BRIDGE_CAPABILITY_MAX) break;
        strlcpy(
            message->capabilities[message->capability_count],
            item->valuestring,
            CODEX_BRIDGE_CAPABILITY_LEN
        );
        message->capability_count++;
    }
}

static size_t utf8_prefix_bytes(const char *text, size_t limit)
{
    size_t offset = 0;
    while (offset < limit) {
        unsigned char lead = (unsigned char)text[offset];
        size_t character_length = lead < 0x80 ? 1 :
                                  (lead & 0xE0) == 0xC0 ? 2 :
                                  (lead & 0xF0) == 0xE0 ? 3 :
                                  (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (character_length > limit - offset) break;
        bool valid = true;
        for (size_t index = 1; index < character_length; ++index) {
            if (((unsigned char)text[offset + index] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        offset += character_length;
    }
    return offset;
}

static bool compose_body(codex_bridge_message_t *message)
{
    size_t length = 0;
    for (int index = 0; index < s_chunk_total; ++index) {
        size_t chunk_length = s_chunks[index] == NULL ? 0 : strlen(s_chunks[index]);
        if (chunk_length > SIZE_MAX - length) return false;
        length += chunk_length;
    }

    const size_t body_limit = CODEX_BRIDGE_BODY_MAX - 1;
    const size_t suffix_length = sizeof(CODEX_BRIDGE_TRUNCATION_SUFFIX) - 1;
    const bool truncated = length > body_limit;
    const size_t content_limit = truncated ? body_limit - suffix_length : body_limit;

    char *body = bridge_malloc(body_limit + 1);
    if (body == NULL) return false;

    size_t written = 0;
    for (int index = 0; index < s_chunk_total && written < content_limit; ++index) {
        if (s_chunks[index] == NULL) continue;
        size_t chunk_length = strlen(s_chunks[index]);
        size_t available = content_limit - written;
        if (chunk_length > available) {
            chunk_length = utf8_prefix_bytes(s_chunks[index], available);
        }
        memcpy(body + written, s_chunks[index], chunk_length);
        written += chunk_length;
        if (chunk_length < strlen(s_chunks[index])) break;
    }
    if (truncated) {
        memcpy(body + written, CODEX_BRIDGE_TRUNCATION_SUFFIX, suffix_length);
        written += suffix_length;
    }
    body[written] = '\0';
    heap_caps_free(message->body);
    message->body = body;
    return true;
}

esp_err_t codex_bridge_protocol_feed(
    const char *line,
    size_t length,
    codex_bridge_message_t *message
)
{
    if (line == NULL || message == NULL) return ESP_ERR_INVALID_ARG;
    if (length == 0 || length > CODEX_BRIDGE_LINE_MAX) return ESP_ERR_INVALID_SIZE;
    cJSON *root = cJSON_ParseWithLength(line, length);
    if (root == NULL) return ESP_ERR_INVALID_ARG;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(type->valuestring, "message") != 0) {
        clear_message_fields(message);
        strlcpy(message->type, type->valuestring, sizeof(message->type));
        copy_capabilities(root, message);
        if (strcmp(type->valuestring, "read_state") == 0) {
            copy_string(root, "message_id", message->message_id, sizeof(message->message_id));
            message->data = cJSON_Duplicate(root, true);
        } else if (strcmp(type->valuestring, "codex_state") == 0) {
            message->data = cJSON_Duplicate(root, true);
        }
        if (strcmp(type->valuestring, "bridge_ready") != 0 &&
            strcmp(type->valuestring, "capabilities_update") != 0 &&
            strcmp(type->valuestring, "read_state") != 0 &&
            strcmp(type->valuestring, "codex_state") != 0 &&
            strcmp(type->valuestring, "error") != 0) {
            message->ignored = true;
        }
        cJSON_Delete(root);
        return ESP_OK;
    }

    char message_id[CODEX_BRIDGE_ID_MAX] = {0};
    copy_string(root, "message_id", message_id, sizeof(message_id));
    cJSON *seq_value = cJSON_GetObjectItemCaseSensitive(root, "seq");
    cJSON *total_value = cJSON_GetObjectItemCaseSensitive(root, "total");
    int seq = cJSON_IsNumber(seq_value) ? seq_value->valueint : 0;
    int total = cJSON_IsNumber(total_value) ? total_value->valueint : 1;
    if (message_id[0] == '\0' || seq < 0 || total <= 0 || seq >= total ||
        seq >= CODEX_BRIDGE_CHUNK_MAX || total > CODEX_BRIDGE_CHUNK_MAX) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(s_chunk_message_id, message_id) != 0 || s_chunk_total != total) {
        clear_chunks();
        strlcpy(s_chunk_message_id, message_id, sizeof(s_chunk_message_id));
        s_chunk_total = total;
    }
    cJSON *body_value = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (!cJSON_IsString(body_value) || body_value->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    size_t chunk_length = strlen(body_value->valuestring);
    if (chunk_length > CODEX_BRIDGE_CHUNK_BODY_MAX) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    heap_caps_free(s_chunks[seq]);
    s_chunks[seq] = bridge_malloc(chunk_length + 1);
    if (s_chunks[seq] == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    strcpy(s_chunks[seq], body_value->valuestring);

    clear_message_fields(message);
    message->ignored = false;
    strlcpy(message->type, type->valuestring, sizeof(message->type));
    strlcpy(message->message_id, message_id, sizeof(message->message_id));
    copy_string(root, "thread_id", message->thread_id, sizeof(message->thread_id));
    copy_string(root, "kind", message->kind, sizeof(message->kind));
    copy_string(root, "status", message->status, sizeof(message->status));
    copy_string(root, "title", message->title, sizeof(message->title));
    message->seq = seq;
    message->total = total;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) message->data = cJSON_Duplicate(data, true);
    if (!compose_body(message)) {
        clear_message_fields(message);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

void codex_bridge_protocol_reset_message(codex_bridge_message_t *message)
{
    if (message == NULL) return;
    clear_message_fields(message);
    clear_chunks();
}

bool codex_bridge_protocol_has_capability(
    const codex_bridge_message_t *message,
    const char *capability
)
{
    if (message == NULL || capability == NULL) return false;
    for (size_t index = 0; index < message->capability_count; ++index) {
        if (strcmp(message->capabilities[index], capability) == 0) return true;
    }
    return false;
}

const char *codex_bridge_protocol_body(
    const codex_bridge_message_t *message,
    const char *message_id
)
{
    if (message == NULL || message->body == NULL) return "";
    if (message_id != NULL && message_id[0] != '\0' &&
        strcmp(message->message_id, message_id) != 0) return "";
    return message->body;
}

size_t codex_bridge_protocol_body_length(const codex_bridge_message_t *message)
{
    return message != NULL && message->body != NULL ? strlen(message->body) : 0;
}

/* ---- CBDP v1 binary codec ---- */

static uint16_t cbdp_read_u16(const uint8_t *value)
{
    return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
}

static uint32_t cbdp_read_u32(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24) |
           ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) |
           (uint32_t)value[3];
}

static uint64_t cbdp_read_u64(const uint8_t *value)
{
    return ((uint64_t)cbdp_read_u32(value) << 32) |
           cbdp_read_u32(value + 4);
}

static void cbdp_write_u16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8);
    target[1] = (uint8_t)value;
}

static void cbdp_write_u32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24);
    target[1] = (uint8_t)(value >> 16);
    target[2] = (uint8_t)(value >> 8);
    target[3] = (uint8_t)value;
}

static bool cbdp_payload_allowed(uint8_t message_type)
{
    return message_type == CBDP_MSG_SEND_CHUNK ||
           message_type == CBDP_MSG_AUDIO_CHUNK ||
           message_type == CBDP_MSG_FRAGMENT ||
           message_type == CBDP_MSG_RESULT;
}

esp_err_t codex_bridge_tlv_next(
    const uint8_t **cursor,
    const uint8_t *end,
    codex_bridge_tlv_t *out
)
{
    if (cursor == NULL || *cursor == NULL || end == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*cursor == end) return ESP_ERR_NOT_FOUND;
    if (*cursor > end || (size_t)(end - *cursor) < 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *current = *cursor;
    const uint16_t length = cbdp_read_u16(current + 2);
    if ((size_t)(end - current - 4) < length) return ESP_ERR_INVALID_SIZE;
    out->tag = cbdp_read_u16(current);
    out->length = length;
    out->value = current + 4;
    *cursor = current + 4 + length;
    return ESP_OK;
}

esp_err_t codex_bridge_tlv_find(
    const uint8_t *tlv,
    size_t tlv_len,
    uint16_t tag,
    codex_bridge_tlv_t *out
)
{
    if (tlv == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    const uint8_t *cursor = tlv;
    const uint8_t *end = tlv + tlv_len;
    codex_bridge_tlv_t found = {0};
    bool matched = false;
    while (cursor < end) {
        codex_bridge_tlv_t item = {0};
        esp_err_t error = codex_bridge_tlv_next(&cursor, end, &item);
        if (error != ESP_OK) return error;
        if (item.tag != tag) continue;
        if (matched) return ESP_ERR_INVALID_ARG;
        matched = true;
        found = item;
    }
    if (!matched) return ESP_ERR_NOT_FOUND;
    *out = found;
    return ESP_OK;
}

static esp_err_t cbdp_tlv_exact(const codex_bridge_tlv_t *tlv, size_t length)
{
    if (tlv == NULL || tlv->value == NULL) return ESP_ERR_INVALID_ARG;
    return tlv->length == length ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t codex_bridge_tlv_u8(const codex_bridge_tlv_t *tlv, uint8_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t error = cbdp_tlv_exact(tlv, 1);
    if (error != ESP_OK) return error;
    *out = tlv->value[0];
    return ESP_OK;
}

esp_err_t codex_bridge_tlv_u16(const codex_bridge_tlv_t *tlv, uint16_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t error = cbdp_tlv_exact(tlv, 2);
    if (error != ESP_OK) return error;
    *out = cbdp_read_u16(tlv->value);
    return ESP_OK;
}

esp_err_t codex_bridge_tlv_u32(const codex_bridge_tlv_t *tlv, uint32_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t error = cbdp_tlv_exact(tlv, 4);
    if (error != ESP_OK) return error;
    *out = cbdp_read_u32(tlv->value);
    return ESP_OK;
}

esp_err_t codex_bridge_tlv_u64(const codex_bridge_tlv_t *tlv, uint64_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t error = cbdp_tlv_exact(tlv, 8);
    if (error != ESP_OK) return error;
    *out = cbdp_read_u64(tlv->value);
    return ESP_OK;
}

esp_err_t codex_bridge_tlv_utf8(
    const codex_bridge_tlv_t *tlv,
    char *out,
    size_t capacity
)
{
    if (tlv == NULL || out == NULL || capacity == 0) return ESP_ERR_INVALID_ARG;
    if (tlv->length >= capacity) return ESP_ERR_INVALID_SIZE;
    if (tlv->length > 0) memcpy(out, tlv->value, tlv->length);
    out[tlv->length] = '\0';
    return ESP_OK;
}

esp_err_t codex_bridge_parse_pairing_offer(
    const uint8_t *tlv,
    size_t tlv_len,
    char *token_out,
    size_t capacity
)
{
    if (tlv == NULL || token_out == NULL || capacity == 0) return ESP_ERR_INVALID_ARG;
    codex_bridge_tlv_t state = {0};
    esp_err_t error = codex_bridge_tlv_find(
        tlv, tlv_len, CBDP_TLV_PAIRING_STATE, &state);
    if (error != ESP_OK) return error;
    uint8_t state_value = 0;
    error = codex_bridge_tlv_u8(&state, &state_value);
    if (error != ESP_OK) return error;
    if (state_value != CBDP_PAIRING_STATE_OFFER) return ESP_ERR_INVALID_ARG;

    codex_bridge_tlv_t token = {0};
    error = codex_bridge_tlv_find(
        tlv, tlv_len, CBDP_TLV_PAIRING_TOKEN, &token);
    if (error != ESP_OK) return error;
    if (token.value == NULL ||
        token.length < CBDP_PAIRING_TOKEN_MIN_BYTES ||
        token.length > CBDP_PAIRING_TOKEN_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (token.length >= capacity) return ESP_ERR_INVALID_SIZE;
    /* The consumer stores this as a NUL-terminated C string and feeds
       strlen() into the HMAC, so an embedded NUL would be silently truncated
       and break AUTH on every reconnect. Refuse it instead of storing a
       corrupt token. */
    if (memchr(token.value, '\0', token.length) != NULL) return ESP_ERR_INVALID_ARG;
    memcpy(token_out, token.value, token.length);
    token_out[token.length] = '\0';
    return ESP_OK;
}

esp_err_t codex_bridge_frame_parse(
    const uint8_t *bytes,
    size_t length,
    codex_bridge_frame_t *out
)
{
    if (bytes == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (length < CBDP_FIXED_HEADER_SIZE) return ESP_ERR_INVALID_SIZE;
    if (bytes[0] != CBDP_MAGIC_0 || bytes[1] != CBDP_MAGIC_1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes[2] != CBDP_VERSION_MAJOR) return ESP_ERR_INVALID_VERSION;
    const uint16_t header_len = cbdp_read_u16(bytes + 6);
    const uint32_t payload_len = cbdp_read_u32(bytes + 8);
    if (header_len < CBDP_FIXED_HEADER_SIZE ||
        (size_t)header_len > length ||
        payload_len > length - header_len ||
        (size_t)header_len + (size_t)payload_len != length) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (bytes[27] != 0) return ESP_ERR_INVALID_ARG;
    const uint8_t message_type = bytes[3];
    if (payload_len > 0 && !cbdp_payload_allowed(message_type)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *cursor = bytes + CBDP_FIXED_HEADER_SIZE;
    const uint8_t *end = bytes + header_len;
    while (cursor < end) {
        codex_bridge_tlv_t item = {0};
        esp_err_t error = codex_bridge_tlv_next(&cursor, end, &item);
        if (error != ESP_OK) return error;
    }

    memset(out, 0, sizeof(*out));
    out->version_major = bytes[2];
    out->message_type = message_type;
    out->flags = cbdp_read_u16(bytes + 4);
    out->header_len = header_len;
    out->payload_len = payload_len;
    out->topic_id = cbdp_read_u32(bytes + 12);
    out->seq = cbdp_read_u32(bytes + 16);
    out->ack_seq = cbdp_read_u32(bytes + 20);
    out->code = cbdp_read_u16(bytes + 24);
    out->qos = bytes[26];
    out->header_tlv = bytes + CBDP_FIXED_HEADER_SIZE;
    out->header_tlv_len = header_len - CBDP_FIXED_HEADER_SIZE;
    out->payload = payload_len > 0 ? bytes + header_len : NULL;
    out->frame_len = length;
    return ESP_OK;
}

void codex_bridge_tlv_builder_init(
    codex_bridge_tlv_builder_t *builder,
    uint8_t *data,
    size_t capacity
)
{
    if (builder == NULL) return;
    builder->data = data;
    builder->capacity = data == NULL ? 0 : capacity;
    builder->length = 0;
    builder->failed = data == NULL;
}

esp_err_t codex_bridge_tlv_builder_add(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    const void *value,
    size_t length
)
{
    if (builder == NULL || builder->failed) return ESP_ERR_INVALID_STATE;
    if (length > UINT16_MAX || (length > 0 && value == NULL)) {
        builder->failed = true;
        return ESP_ERR_INVALID_ARG;
    }
    const size_t needed = 4 + length;
    if (builder->length > builder->capacity ||
        needed > builder->capacity - builder->length) {
        builder->failed = true;
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *target = builder->data + builder->length;
    cbdp_write_u16(target, tag);
    cbdp_write_u16(target + 2, (uint16_t)length);
    if (length > 0) memcpy(target + 4, value, length);
    builder->length += needed;
    return ESP_OK;
}

esp_err_t codex_bridge_tlv_builder_u8(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    uint8_t value
)
{
    return codex_bridge_tlv_builder_add(builder, tag, &value, 1);
}

esp_err_t codex_bridge_tlv_builder_u16(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    uint16_t value
)
{
    uint8_t bytes[2];
    cbdp_write_u16(bytes, value);
    return codex_bridge_tlv_builder_add(builder, tag, bytes, sizeof(bytes));
}

esp_err_t codex_bridge_tlv_builder_u32(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    uint32_t value
)
{
    uint8_t bytes[4];
    cbdp_write_u32(bytes, value);
    return codex_bridge_tlv_builder_add(builder, tag, bytes, sizeof(bytes));
}

esp_err_t codex_bridge_tlv_builder_u64(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    uint64_t value
)
{
    uint8_t bytes[8];
    cbdp_write_u32(bytes, (uint32_t)(value >> 32));
    cbdp_write_u32(bytes + 4, (uint32_t)value);
    return codex_bridge_tlv_builder_add(builder, tag, bytes, sizeof(bytes));
}

esp_err_t codex_bridge_tlv_builder_utf8(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    const char *value
)
{
    if (value == NULL) return ESP_ERR_INVALID_ARG;
    return codex_bridge_tlv_builder_add(builder, tag, value, strlen(value));
}

esp_err_t codex_bridge_tlv_builder_bytes(
    codex_bridge_tlv_builder_t *builder,
    uint16_t tag,
    const void *value,
    size_t length
)
{
    return codex_bridge_tlv_builder_add(builder, tag, value, length);
}

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
)
{
    if (out == NULL || out_len == NULL || header == NULL || header->failed) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > 0 && payload == NULL) return ESP_ERR_INVALID_ARG;
    if (payload_len > 0 && !cbdp_payload_allowed(message_type)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (header->length > UINT16_MAX - CBDP_FIXED_HEADER_SIZE ||
        payload_len > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t header_len = CBDP_FIXED_HEADER_SIZE + header->length;
    if (header_len > capacity || payload_len > capacity - header_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    out[0] = CBDP_MAGIC_0;
    out[1] = CBDP_MAGIC_1;
    out[2] = CBDP_VERSION_MAJOR;
    out[3] = message_type;
    cbdp_write_u16(out + 4, flags);
    cbdp_write_u16(out + 6, (uint16_t)header_len);
    cbdp_write_u32(out + 8, (uint32_t)payload_len);
    cbdp_write_u32(out + 12, topic_id);
    cbdp_write_u32(out + 16, seq);
    cbdp_write_u32(out + 20, ack_seq);
    cbdp_write_u16(out + 24, code);
    out[26] = qos;
    out[27] = 0;
    if (header->length > 0) memcpy(out + CBDP_FIXED_HEADER_SIZE, header->data, header->length);
    if (payload_len > 0) memcpy(out + header_len, payload, payload_len);
    *out_len = header_len + payload_len;
    return ESP_OK;
}

esp_err_t codex_bridge_auth_proof(
    const char *token,
    const uint8_t session_id[16],
    const uint8_t client_nonce[16],
    const uint8_t server_nonce[16],
    const char *device_id,
    uint8_t proof[32]
)
{
    if (token == NULL || session_id == NULL || client_nonce == NULL ||
        server_nonce == NULL || device_id == NULL || proof == NULL ||
        strlen(token) < 16 || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    static const char PREFIX[] = "CBDP1-AUTH";
    const size_t device_length = strlen(device_id);
    const size_t data_length = sizeof(PREFIX) - 1 + 16 + 16 + 16 + device_length;
    uint8_t *data = heap_caps_malloc(data_length, MALLOC_CAP_8BIT);
    if (data == NULL) return ESP_ERR_NO_MEM;
    size_t offset = 0;
    memcpy(data + offset, PREFIX, sizeof(PREFIX) - 1);
    offset += sizeof(PREFIX) - 1;
    memcpy(data + offset, session_id, 16);
    offset += 16;
    memcpy(data + offset, client_nonce, 16);
    offset += 16;
    memcpy(data + offset, server_nonce, 16);
    offset += 16;
    memcpy(data + offset, device_id, device_length);

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    esp_err_t result = ESP_OK;
    if (info == NULL ||
        mbedtls_md_hmac(info, (const unsigned char *)token, strlen(token),
                        data, data_length, proof) != 0) {
        result = ESP_ERR_INVALID_STATE;
    }
    heap_caps_free(data);
    return result;
}


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
)
{
    if (request == NULL || device_id == NULL || model == NULL || firmware == NULL ||
        out == NULL || out_len == NULL ||
        request->message_type != CBDP_MSG_DISCOVERY) {
        return ESP_ERR_INVALID_ARG;
    }
    codex_bridge_tlv_t nonce = {0};
    esp_err_t error = codex_bridge_tlv_find(
        request->header_tlv,
        request->header_tlv_len,
        CBDP_TLV_DISCOVERY_NONCE,
        &nonce
    );
    if (error != ESP_OK || nonce.length != 16) return ESP_ERR_INVALID_ARG;
    if (strlen(device_id) > 64 || strlen(model) > 64 || strlen(firmware) > 64) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (transport_security > 1) return ESP_ERR_INVALID_ARG;

    uint8_t header_bytes[320];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_DEVICE_ID, device_id);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_MODEL, model);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_FIRMWARE, firmware);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_PROFILE, profile);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_SUPPORTED_CAPS, supported_caps);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_TRANSPORT_SECURITY, transport_security);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_PORT, port);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_DISCOVERY_NONCE, nonce.value, nonce.length);
    if (error != ESP_OK) return error;

    return codex_bridge_frame_build(
        out,
        capacity,
        CBDP_MSG_DISCOVERY_REPLY,
        CBDP_FLAG_RESPONSE,
        0,
        0,
        0,
        0,
        CBDP_QOS_TELEMETRY,
        &header,
        NULL,
        0,
        out_len
    );
}


/* Build an unsolicited identity announcement for the discovery port.

   The Bridge listens on that port for devices that announce themselves and
   re-dials the announced address. The device is the TCP server in this
   protocol, so without an announcement the Bridge cannot find the device
   again after its own process was restarted. The payload is identical to the
   reply of a discovery request, which the Bridge already accepts, except that
   the nonce is generated here and therefore not checked by the Bridge. */
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
)
{
    if (device_id == NULL || model == NULL || firmware == NULL ||
        out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(device_id) == 0 || strlen(device_id) > 64 ||
        strlen(model) == 0 || strlen(model) > 64 ||
        strlen(firmware) == 0 || strlen(firmware) > 64) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (profile > 2) return ESP_ERR_INVALID_ARG;
    if (transport_security > 1) return ESP_ERR_INVALID_ARG;
    if (port == 0) return ESP_ERR_INVALID_ARG;

    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));

    uint8_t header_bytes[320];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    esp_err_t error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_DEVICE_ID, device_id);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_MODEL, model);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_utf8(&header, CBDP_TLV_FIRMWARE, firmware);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_PROFILE, profile);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u64(&header, CBDP_TLV_SUPPORTED_CAPS, supported_caps);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u8(&header, CBDP_TLV_TRANSPORT_SECURITY, transport_security);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_u16(&header, CBDP_TLV_PORT, port);
    if (error == ESP_OK) error = codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_DISCOVERY_NONCE, nonce, sizeof(nonce));
    if (error != ESP_OK) return error;

    return codex_bridge_frame_build(
        out, capacity,
        CBDP_MSG_DISCOVERY_REPLY, CBDP_FLAG_RESPONSE,
        0, 0, 0, 0, CBDP_QOS_TELEMETRY,
        &header, NULL, 0, out_len
    );
}
