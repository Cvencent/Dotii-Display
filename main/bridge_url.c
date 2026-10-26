#include "bridge_url.h"

#include <string.h>

/* Parse one dotted-decimal octet. Rejects empty parts, non-digits, values
   above 255 and ambiguous leading zeros such as "010". */
static bool parse_octet(const char **cursor, unsigned *out)
{
    const char *text = *cursor;
    if (*text < '0' || *text > '9') return false;

    unsigned value = 0;
    size_t digits = 0;
    while (text[digits] >= '0' && text[digits] <= '9') {
        if (digits == 3) return false;
        value = value * 10u + (unsigned)(text[digits] - '0');
        ++digits;
    }
    if (digits > 1 && text[0] == '0') return false;
    if (value > 255u) return false;

    *cursor = text + digits;
    *out = value;
    return true;
}

static bool is_dotted_decimal_ipv4(const char *text, size_t length)
{
    if (text == NULL || length == 0) return false;
    char copy[16];
    if (length >= sizeof(copy)) return false;
    memcpy(copy, text, length);
    copy[length] = '\0';

    const char *cursor = copy;
    for (int index = 0; index < 4; ++index) {
        unsigned octet = 0;
        if (!parse_octet(&cursor, &octet)) return false;
        if (index < 3) {
            if (*cursor != '.') return false;
            ++cursor;
        }
    }
    return *cursor == '\0';
}

bool bridge_url_replace_ipv4_host(const char *url, const char *ipv4,
                                  char *out, size_t out_size)
{
    if (url == NULL || ipv4 == NULL || out == NULL || out_size == 0) return false;

    const char *scheme_end = strstr(url, "://");
    if (scheme_end == NULL) return false;

    const size_t scheme_length = (size_t)(scheme_end - url);
    if (scheme_length == 0) return false;

    const char *authority = scheme_end + 3;
    const char *host_end = authority;
    while (*host_end != '\0' && *host_end != ':' && *host_end != '/' &&
           *host_end != '?' && *host_end != '#') {
        ++host_end;
    }

    const size_t host_length = (size_t)(host_end - authority);
    if (!is_dotted_decimal_ipv4(authority, host_length)) return false;
    if (!is_dotted_decimal_ipv4(ipv4, strlen(ipv4))) return false;

    const size_t ipv4_length = strlen(ipv4);
    const size_t tail_length = strlen(host_end);
    if (scheme_length + 3 > out_size || ipv4_length > out_size - scheme_length - 3 ||
        tail_length > out_size - scheme_length - 3 - ipv4_length - 1) {
        return false;
    }

    size_t written = 0;
    memcpy(out, url, scheme_length);
    written += scheme_length;
    memcpy(out + written, "://", 3);
    written += 3;
    memcpy(out + written, ipv4, ipv4_length);
    written += ipv4_length;
    memcpy(out + written, host_end, tail_length);
    written += tail_length;
    out[written] = '\0';
    return true;
}
