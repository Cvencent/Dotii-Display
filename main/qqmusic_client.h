#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QQMUSIC_MAX_RESULTS 5

typedef struct {
    char mid[48];
    char title[128];
    char artist[96];
    char album[96];
} qqmusic_song_t;

/* The management bridge holds the QQ Music session and returns a compact
   result set (mid/title/artist/album only), so the device never downloads the
   ~48 KB raw Tencent payload. The bridge also proxies the audio stream: the
   Tencent CDN throttles device traffic badly and causes stutter. */

bool qqmusic_search(const char *bridge_url, const char *bridge_token,
                    const char *query, qqmusic_song_t *out,
                    int max, int *out_count);

bool qqmusic_resolve_mid(const char *bridge_url, const char *bridge_token,
                         const char *mid, char *out_url, size_t url_size);

bool qqmusic_resolve_song(const char *bridge_url, const char *bridge_token,
                          const char *cookie, const char *query,
                          char *out_title, size_t title_size,
                          char *out_url, size_t url_size);

bool qqmusic_search_songs(const char *bridge_url, const char *bridge_token,
                          const char *query, int limit,
                          char *out_text, size_t out_size);

#ifdef __cplusplus
}
#endif
