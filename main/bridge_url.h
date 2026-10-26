#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Replace the IPv4 literal host inside a bridge URL with `ipv4`, keeping the
   scheme, port, path and query intact. Returns false and leaves `out`
   untouched when the URL has no IPv4 host (for example a hostname or a
   malformed URL), when `ipv4` is not a dotted-decimal address, or when `out`
   cannot hold the result. */
bool bridge_url_replace_ipv4_host(const char *url, const char *ipv4,
                                  char *out, size_t out_size);
