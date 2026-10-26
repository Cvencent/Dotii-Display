#include <string.h>

#include "bridge_url.h"
#include "unity.h"

TEST_CASE("Bridge URL rewrite changes only the IPv4 host", "[bridge_url]")
{
    char rewritten[256];

    TEST_ASSERT_TRUE(bridge_url_replace_ipv4_host(
        "http://192.168.0.47:8787/api/v1/snapshot?source=device",
        "192.168.0.49",
        rewritten,
        sizeof(rewritten)));
    TEST_ASSERT_EQUAL_STRING(
        "http://192.168.0.49:8787/api/v1/snapshot?source=device",
        rewritten);
}

TEST_CASE("Bridge URL rewrite preserves HTTPS without an explicit port", "[bridge_url]")
{
    char rewritten[256];

    TEST_ASSERT_TRUE(bridge_url_replace_ipv4_host(
        "https://10.0.0.7/api/v1/snapshot",
        "192.168.0.49",
        rewritten,
        sizeof(rewritten)));
    TEST_ASSERT_EQUAL_STRING(
        "https://192.168.0.49/api/v1/snapshot",
        rewritten);
}

TEST_CASE("Bridge URL rewrite leaves hostnames untouched", "[bridge_url]")
{
    char rewritten[256] = "unchanged";

    TEST_ASSERT_FALSE(bridge_url_replace_ipv4_host(
        "http://dotii-bridge.local:8787/api/v1/snapshot",
        "192.168.0.49",
        rewritten,
        sizeof(rewritten)));
    TEST_ASSERT_EQUAL_STRING("unchanged", rewritten);
}

TEST_CASE("Bridge URL rewrite rejects invalid input and short output", "[bridge_url]")
{
    char rewritten[32];

    TEST_ASSERT_FALSE(bridge_url_replace_ipv4_host(
        "http://192.168.0.47:8787/api/v1/snapshot",
        "192.168.0.999",
        rewritten,
        sizeof(rewritten)));
    TEST_ASSERT_FALSE(bridge_url_replace_ipv4_host(
        "http://192.168.0.47:8787/api/v1/snapshot",
        "192.168.0.49",
        rewritten,
        sizeof(rewritten)));
    TEST_ASSERT_FALSE(bridge_url_replace_ipv4_host(NULL, "192.168.0.49",
                                                   rewritten, sizeof(rewritten)));
}

TEST_CASE("Bridge URL rewrite accepts an already current IPv4 host", "[bridge_url]")
{
    char rewritten[256];

    TEST_ASSERT_TRUE(bridge_url_replace_ipv4_host(
        "http://192.168.0.49:8787/api/v1/snapshot",
        "192.168.0.49",
        rewritten,
        sizeof(rewritten)));
    TEST_ASSERT_EQUAL_STRING(
        "http://192.168.0.49:8787/api/v1/snapshot",
        rewritten);
}
