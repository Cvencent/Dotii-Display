#include "codex_bridge_protocol.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"

#define CODEX_BRIDGE_CHUNK_MAX 32
#define CODEX_BRIDGE_CHUNK_BODY_MAX 480
#define CODEX_BRIDGE_TRUNCATION_SUFFIX "\n...[已截断]"

static const char *const SUPPORTED_CAPABILITIES[] = {
    "status", "text", "tasks", "results", "read_ack",
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
    cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
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
        if (strcmp(type->valuestring, "bridge_ready") != 0 &&
            strcmp(type->valuestring, "capabilities_update") != 0 &&
            strcmp(type->valuestring, "read_state") != 0 &&
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
