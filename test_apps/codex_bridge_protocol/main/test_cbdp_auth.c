#include <string.h>

#include "codex_bridge_protocol.h"
#include "unity.h"

static const uint8_t SESSION_ID[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
};
static const uint8_t CLIENT_NONCE[16] = {0};
static const uint8_t SERVER_NONCE[16] = {
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
};
static const uint8_t EXPECTED[32] = {
    0x7D, 0xF2, 0xDE, 0xC2, 0xFA, 0xE6, 0x1A, 0x43,
    0xD6, 0x91, 0x00, 0xEA, 0x21, 0x7F, 0x30, 0x06,
    0x7E, 0x05, 0x93, 0x7B, 0xF1, 0x87, 0xBC, 0xE0,
    0x2A, 0xAF, 0x35, 0x5F, 0x7D, 0xC7, 0x4A, 0x92,
};

TEST_CASE("CBDP HMAC matches frozen authentication vector", "[cbdp_auth]")
{
    uint8_t proof[32] = {0};
    TEST_ESP_OK(codex_bridge_auth_proof(
        "0123456789abcdef0123456789abcdef",
        SESSION_ID,
        CLIENT_NONCE,
        SERVER_NONCE,
        "esp32-lab",
        proof
    ));
    TEST_ASSERT_EQUAL_MEMORY(EXPECTED, proof, sizeof(EXPECTED));
}

/* Build a PAIRING header TLV block with the given state and token bytes. */
static size_t build_pairing_offer(uint8_t *out, size_t capacity, uint8_t state,
                                  const uint8_t *token, size_t token_length)
{
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, out, capacity);
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_PAIRING_STATE, state));
    TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&builder, CBDP_TLV_PAIRING_TOKEN,
                                               token, token_length));
    return builder.length;
}

TEST_CASE("CBDP PAIRING offer accepts boundary token lengths", "[cbdp_pairing]")
{
    uint8_t tlv[160];
    char token[CBDP_PAIRING_TOKEN_MAX_BYTES + 1];
    /* 16 and 64 bytes are the inclusive wire limits. */
    const char *min_token = "0123456789abcdef";
    const char *max_token =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    size_t length = build_pairing_offer(tlv, sizeof(tlv), CBDP_PAIRING_STATE_OFFER,
                                        (const uint8_t *)min_token, strlen(min_token));
    TEST_ESP_OK(codex_bridge_parse_pairing_offer(tlv, length, token, sizeof(token)));
    TEST_ASSERT_EQUAL_STRING(min_token, token);

    length = build_pairing_offer(tlv, sizeof(tlv), CBDP_PAIRING_STATE_OFFER,
                                 (const uint8_t *)max_token, strlen(max_token));
    TEST_ESP_OK(codex_bridge_parse_pairing_offer(tlv, length, token, sizeof(token)));
    TEST_ASSERT_EQUAL_STRING(max_token, token);
}

TEST_CASE("CBDP PAIRING offer rejects out-of-range and malformed tokens", "[cbdp_pairing]")
{
    uint8_t tlv[160];
    char token[CBDP_PAIRING_TOKEN_MAX_BYTES + 1];
    const char *too_short = "0123456789abcde";
    const char *too_long =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0";

    size_t length = build_pairing_offer(tlv, sizeof(tlv), CBDP_PAIRING_STATE_OFFER,
                                        (const uint8_t *)too_short, strlen(too_short));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE,
        codex_bridge_parse_pairing_offer(tlv, length, token, sizeof(token)));

    length = build_pairing_offer(tlv, sizeof(tlv), CBDP_PAIRING_STATE_OFFER,
                                 (const uint8_t *)too_long, strlen(too_long));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE,
        codex_bridge_parse_pairing_offer(tlv, length, token, sizeof(token)));

    /* state must be OFFER(2); a device-side REQUEST(1) is not an offer. */
    length = build_pairing_offer(tlv, sizeof(tlv), CBDP_PAIRING_STATE_REQUEST,
                                 (const uint8_t *)"0123456789abcdef", 16);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
        codex_bridge_parse_pairing_offer(tlv, length, token, sizeof(token)));

    /* An embedded NUL would be silently truncated by the C-string consumer
       and break every later AUTH, so it must be refused. */
    const uint8_t nul_token[16] = {0x30, 0x31, 0x00, 0x33, 0x34, 0x35, 0x36,
                                   0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64,
                                   0x65, 0x66};
    length = build_pairing_offer(tlv, sizeof(tlv), CBDP_PAIRING_STATE_OFFER,
                                 nul_token, sizeof(nul_token));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
        codex_bridge_parse_pairing_offer(tlv, length, token, sizeof(token)));

    /* A destination that cannot hold the token plus its terminator. */
    char tight[16];
    length = build_pairing_offer(tlv, sizeof(tlv), CBDP_PAIRING_STATE_OFFER,
                                 (const uint8_t *)"0123456789abcdef", 16);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE,
        codex_bridge_parse_pairing_offer(tlv, length, tight, sizeof(tight)));
}

TEST_CASE("CBDP HELLO carries pairing_state only when unpaired", "[cbdp_pairing]")
{
    /* Mirrors the field rule from 11.8: a device with a stored token omits the
       TLV so its HELLO stays byte-identical to older builds. */
    uint8_t tlv[160];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, tlv, sizeof(tlv));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&builder, CBDP_TLV_PAIRING_STATE,
                                            CBDP_PAIRING_STATE_REQUEST));
    const size_t length = builder.length;
    codex_bridge_tlv_t field = {0};
    TEST_ESP_OK(codex_bridge_tlv_find(tlv, length, CBDP_TLV_PAIRING_STATE, &field));
    uint8_t value = 0;
    TEST_ESP_OK(codex_bridge_tlv_u8(&field, &value));
    TEST_ASSERT_EQUAL_UINT8(CBDP_PAIRING_STATE_REQUEST, value);
}

TEST_CASE("CBDP HMAC rejects missing inputs", "[cbdp_auth]")
{
    uint8_t proof[32] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, codex_bridge_auth_proof(
        NULL, SESSION_ID, CLIENT_NONCE, SERVER_NONCE, "esp32-lab", proof));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, codex_bridge_auth_proof(
        "short", SESSION_ID, CLIENT_NONCE, SERVER_NONCE, "esp32-lab", proof));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, codex_bridge_auth_proof(
        "0123456789abcdef", SESSION_ID, CLIENT_NONCE, SERVER_NONCE, "", proof));
}
