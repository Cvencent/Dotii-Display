#include <stdint.h>
#include <string.h>

#include "codex_bridge_protocol.h"
#include "unity.h"

static size_t build_discovery_request(
    uint8_t *frame,
    size_t capacity,
    const uint8_t nonce[16]
)
{
    uint8_t header_bytes[64];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&header, CBDP_TLV_PROFILE, CBDP_PROFILE_PANEL));
    TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&header, CBDP_TLV_DISCOVERY_NONCE, nonce, 16));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&header, CBDP_TLV_DISCOVERY_SCOPE, 1));
    size_t length = 0;
    TEST_ESP_OK(codex_bridge_frame_build(
        frame, capacity, CBDP_MSG_DISCOVERY, CBDP_FLAG_REQUEST, 0, 0, 0, 0, 2,
        &header, NULL, 0, &length
    ));
    return length;
}

TEST_CASE("CBDP discovery reply echoes nonce and device identity", "[cbdp_discovery]")
{
    const uint8_t nonce[16] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };
    uint8_t request_frame[256];
    const size_t request_length = build_discovery_request(
        request_frame, sizeof(request_frame), nonce);
    codex_bridge_frame_t request = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(request_frame, request_length, &request));

    uint8_t reply_frame[512];
    size_t reply_length = 0;
    TEST_ESP_OK(codex_bridge_build_discovery_reply(
        &request,
        "dotii-001122334455",
        "dotii",
        "1.2.3",
        CBDP_PROFILE_PANEL,
        CBDP_CAP_STATUS | CBDP_CAP_TEXT | CBDP_CAP_TASKS,
        8765,
        0,
        reply_frame,
        sizeof(reply_frame),
        &reply_length
    ));

    codex_bridge_frame_t reply = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(reply_frame, reply_length, &reply));
    TEST_ASSERT_EQUAL_UINT8(CBDP_MSG_DISCOVERY_REPLY, reply.message_type);
    TEST_ASSERT_BITS_HIGH(CBDP_FLAG_RESPONSE, reply.flags);

    codex_bridge_tlv_t field = {0};
    TEST_ESP_OK(codex_bridge_tlv_find(reply.header_tlv, reply.header_tlv_len, CBDP_TLV_DISCOVERY_NONCE, &field));
    TEST_ASSERT_EQUAL_UINT16(16, field.length);
    TEST_ASSERT_EQUAL_MEMORY(nonce, field.value, 16);
    TEST_ESP_OK(codex_bridge_tlv_find(reply.header_tlv, reply.header_tlv_len, CBDP_TLV_DEVICE_ID, &field));
    TEST_ASSERT_EQUAL_UINT16(strlen("dotii-001122334455"), field.length);
    TEST_ASSERT_EQUAL_MEMORY("dotii-001122334455", field.value, field.length);
    TEST_ESP_OK(codex_bridge_tlv_find(reply.header_tlv, reply.header_tlv_len, CBDP_TLV_PORT, &field));
    uint16_t port = 0;
    TEST_ESP_OK(codex_bridge_tlv_u16(&field, &port));
    TEST_ASSERT_EQUAL_UINT16(8765, port);
    TEST_ESP_OK(codex_bridge_tlv_find(reply.header_tlv, reply.header_tlv_len, CBDP_TLV_PROFILE, &field));
    uint8_t profile = 0;
    TEST_ESP_OK(codex_bridge_tlv_u8(&field, &profile));
    TEST_ASSERT_EQUAL_UINT8(CBDP_PROFILE_PANEL, profile);
}

TEST_CASE("CBDP discovery rejects malformed non-discovery frames", "[cbdp_discovery]")
{
    const uint8_t nonce[16] = {0};
    uint8_t request_frame[256];
    const size_t request_length = build_discovery_request(
        request_frame, sizeof(request_frame), nonce);
    codex_bridge_frame_t request = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(request_frame, request_length, &request));
    request.message_type = CBDP_MSG_PING;

    uint8_t reply_frame[128];
    size_t reply_length = 0;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, codex_bridge_build_discovery_reply(
        &request, "dotii", "dotii", "1.2.3", CBDP_PROFILE_PANEL, 0, 8765, 0,
        reply_frame, sizeof(reply_frame), &reply_length));
}
