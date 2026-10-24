#include "qqmusic_client.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "qqmusic";

/* The management bridge owns the QQ Music session, so the device asks IT for
   songs instead of calling Tencent directly. The raw Tencent search reply is
   ~48 KB for three results (full metadata per hit), while the bridge reduces it
   to the four fields the device actually uses: about 350 bytes. */
#define QQMUSIC_RESPONSE_SIZE 4096u

typedef struct {
    char *body;
    size_t capacity;
    size_t length;
    bool truncated;
} http_body_t;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_body_t *ctx = (http_body_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && ctx != NULL && event->data != NULL) {
        size_t room = ctx->capacity > ctx->length + 1 ? ctx->capacity - ctx->length - 1 : 0;
        size_t copy = (size_t)event->data_len < room ? (size_t)event->data_len : room;
        if (copy > 0) {
            memcpy(ctx->body + ctx->length, event->data, copy);
            ctx->length += copy;
            ctx->body[ctx->length] = '\0';
        }
        if (copy < (size_t)event->data_len) {
            ctx->truncated = true;
            ESP_LOGE(TAG, "response truncated at %u bytes", (unsigned)ctx->capacity);
        }
    }
    return ESP_OK;
}

/* bridge_url is stored as ".../api/v1/snapshot"; reuse its origin + prefix. */
static void bridge_endpoint(const char *bridge_url, const char *path,
                            char *out, size_t out_size)
{
    out[0] = '\0';
    if (bridge_url == NULL || bridge_url[0] == '\0') return;
    const char *marker = strstr(bridge_url, "/api/v1/");
    size_t base = marker != NULL ? (size_t)(marker - bridge_url) : strlen(bridge_url);
    if (base + strlen(path) + 1 > out_size) return;
    memcpy(out, bridge_url, base);
    out[base] = '\0';
    strlcat(out, path, out_size);
}

static bool post_json(const char *bridge_url, const char *bridge_token,
                      const char *path, cJSON *request,
                      char *response, size_t response_size)
{
    char url[288];
    bridge_endpoint(bridge_url, path, url, sizeof(url));
    if (url[0] == '\0') {
        ESP_LOGW(TAG, "bridge url unavailable");
        return false;
    }
    char *body_text = cJSON_PrintUnformatted(request);
    if (body_text == NULL) return false;

    ESP_LOGI(TAG, "bridge %s: internal free=%u", path,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    http_body_t body = {.body = response, .capacity = response_size, .length = 0};
    response[0] = '\0';
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event,
        .user_data = &body,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (http == NULL) {
        cJSON_free(body_text);
        return false;
    }
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_header(http, "Accept", "application/json");
    if (bridge_token != NULL && bridge_token[0] != '\0') {
        esp_http_client_set_header(http, "X-Bridge-Token", bridge_token);
    }
    esp_http_client_set_post_field(http, body_text, (int)strlen(body_text));
    esp_err_t error = esp_http_client_perform(http);
    int status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    cJSON_free(body_text);

    if (error != ESP_OK || status != 200 || body.truncated) {
        ESP_LOGW(TAG, "bridge %s failed: %s HTTP %d", path, esp_err_to_name(error), status);
        return false;
    }
    return body.length > 0;
}

bool qqmusic_search(const char *bridge_url, const char *bridge_token,
                    const char *query, qqmusic_song_t *out,
                    int max, int *out_count)
{
    if (out_count != NULL) *out_count = 0;
    if (query == NULL || query[0] == '\0' || out == NULL || max <= 0) return false;
    if (max > QQMUSIC_MAX_RESULTS) max = QQMUSIC_MAX_RESULTS;

    cJSON *request = cJSON_CreateObject();
    if (request == NULL) return false;
    cJSON_AddStringToObject(request, "q", query);
    cJSON_AddNumberToObject(request, "limit", max);

    char *response = heap_caps_calloc(1, QQMUSIC_RESPONSE_SIZE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) {
        cJSON_Delete(request);
        return false;
    }
    const bool posted = post_json(bridge_url, bridge_token,
                                  "/api/v1/device/music/search",
                                  request, response, QQMUSIC_RESPONSE_SIZE);
    cJSON_Delete(request);
    if (!posted) {
        heap_caps_free(response);
        return false;
    }

    cJSON *root = cJSON_Parse(response);
    heap_caps_free(response);
    if (root == NULL) return false;
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    int count = 0;
    if (cJSON_IsArray(results)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, results) {
            if (count >= max) break;
            const cJSON *mid = cJSON_GetObjectItemCaseSensitive(item, "mid");
            const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (!cJSON_IsString(mid) || !cJSON_IsString(title)) continue;
            qqmusic_song_t *song = &out[count];
            memset(song, 0, sizeof(*song));
            strlcpy(song->mid, mid->valuestring, sizeof(song->mid));
            strlcpy(song->title, title->valuestring, sizeof(song->title));
            const cJSON *artist = cJSON_GetObjectItemCaseSensitive(item, "artist");
            if (cJSON_IsString(artist)) strlcpy(song->artist, artist->valuestring, sizeof(song->artist));
            const cJSON *album = cJSON_GetObjectItemCaseSensitive(item, "album");
            if (cJSON_IsString(album)) strlcpy(song->album, album->valuestring, sizeof(song->album));
            count++;
        }
    }
    cJSON_Delete(root);
    if (out_count != NULL) *out_count = count;
    return count > 0;
}

/* The Tencent CDN throttles device traffic badly (the song plays slowly with
   gaps). The LAN bridge proxies the same stream, so the device fetches audio
   from a machine on the same network and no longer depends on CDN throughput.
   Build the proxy URL directly instead of paying for a resolve round trip;
   the bridge resolves the mid and answers 404 if the song is not playable. */
bool qqmusic_resolve_mid(const char *bridge_url, const char *bridge_token,
                         const char *mid, char *out_url, size_t url_size)
{
    if (bridge_url == NULL || bridge_url[0] == '\0' ||
        mid == NULL || mid[0] == '\0' || out_url == NULL || url_size == 0) {
        return false;
    }
    for (const char *check = mid; *check != '\0'; ++check) {
        const char c = *check;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    (void)bridge_token; /* The proxy URL is token-gated by the firmware's
                           dedicated HTTP header in music_task. */
    char stream_path[80];
    snprintf(stream_path, sizeof(stream_path), "/api/v1/device/music/stream?mid=%.32s", mid);
    bridge_endpoint(bridge_url, stream_path, out_url, url_size);
    return out_url[0] != '\0';
}

bool qqmusic_resolve_song(const char *bridge_url, const char *bridge_token,
                          const char *cookie, const char *query,
                          char *out_title, size_t title_size,
                          char *out_url, size_t url_size)
{
    (void)cookie;
    qqmusic_song_t songs[1];
    int count = 0;
    if (!qqmusic_search(bridge_url, bridge_token, query, songs, 1, &count) || count < 1) {
        return false;
    }
    if (out_title != NULL && title_size > 0) {
        snprintf(out_title, title_size, "%s%s%s", songs[0].title,
                 songs[0].artist[0] ? " - " : "", songs[0].artist);
    }
    return qqmusic_resolve_mid(bridge_url, bridge_token, songs[0].mid, out_url, url_size);
}

bool qqmusic_search_songs(const char *bridge_url, const char *bridge_token,
                          const char *query, int limit,
                          char *out_text, size_t out_size)
{
    if (out_text == NULL || out_size == 0) return false;
    out_text[0] = '\0';
    qqmusic_song_t songs[QQMUSIC_MAX_RESULTS];
    int count = 0;
    if (!qqmusic_search(bridge_url, bridge_token, query, songs, limit, &count)) return false;

    size_t used = 0;
    for (int i = 0; i < count && used + 1 < out_size; ++i) {
        char line[360];
        int written;
        if (songs[i].album[0] != '\0') {
            written = snprintf(line, sizeof(line), "%d. %s - %s（%s）\n",
                               i + 1, songs[i].title,
                               songs[i].artist[0] ? songs[i].artist : "未知歌手",
                               songs[i].album);
        } else {
            written = snprintf(line, sizeof(line), "%d. %s - %s\n",
                               i + 1, songs[i].title,
                               songs[i].artist[0] ? songs[i].artist : "未知歌手");
        }
        if (written <= 0) continue;
        size_t length = (size_t)written;
        const size_t room = out_size - used - 1;
        if (length > room) length = room;
        memcpy(out_text + used, line, length);
        used += length;
        out_text[used] = '\0';
    }
    return used > 0;
}
