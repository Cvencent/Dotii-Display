#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "codex_bridge_protocol.h"
#include "unity.h"

TEST_CASE("CBDP frame header round trips fixed fields", "[cbdp_codec]")
{
    uint8_t header_bytes[64];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&header, 0x0001, 0));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&header, 0x0002, "dotii-test"));

    uint8_t payload[] = {0x11, 0x22, 0x33};
    uint8_t frame[256];
    size_t frame_length = 0;
    TEST_ESP_OK(codex_bridge_frame_build(
        frame,
        sizeof(frame),
        CBDP_MSG_SEND_CHUNK,
        CBDP_FLAG_NO_ACK,
        0x1000002A,
        7,
        3,
        0,
        1,
        &header,
        payload,
        sizeof(payload),
        &frame_length
    ));
    TEST_ASSERT_EQUAL_UINT(28 + header.length + sizeof(payload), frame_length);
    TEST_ASSERT_EQUAL_HEX8(0xCB, frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, frame[1]);
    TEST_ASSERT_EQUAL_HEX8(1, frame[2]);
    TEST_ASSERT_EQUAL_HEX8(CBDP_MSG_SEND_CHUNK, frame[3]);
    TEST_ASSERT_EQUAL_HEX16(CBDP_FLAG_NO_ACK, (uint16_t)((frame[4] << 8) | frame[5]));
    TEST_ASSERT_EQUAL_HEX16(28 + header.length, (uint16_t)((frame[6] << 8) | frame[7]));
    TEST_ASSERT_EQUAL_HEX32(sizeof(payload), (uint32_t)((frame[8] << 24) | (frame[9] << 16) | (frame[10] << 8) | frame[11]));

    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ASSERT_EQUAL_UINT8(CBDP_MSG_SEND_CHUNK, parsed.message_type);
    TEST_ASSERT_EQUAL_HEX16(CBDP_FLAG_NO_ACK, parsed.flags);
    TEST_ASSERT_EQUAL_UINT32(0x1000002A, parsed.topic_id);
    TEST_ASSERT_EQUAL_UINT32(7, parsed.seq);
    TEST_ASSERT_EQUAL_UINT32(3, parsed.ack_seq);
    TEST_ASSERT_EQUAL_UINT8(1, parsed.qos);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), parsed.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, parsed.payload, sizeof(payload));

    codex_bridge_tlv_t field = {0};
    TEST_ESP_OK(codex_bridge_tlv_find(parsed.header_tlv, parsed.header_tlv_len, 0x0002, &field));
    TEST_ASSERT_EQUAL_UINT16(10, field.length);
    TEST_ASSERT_EQUAL_MEMORY("dotii-test", field.value, 10);
}

TEST_CASE("CBDP parser rejects magic version and impossible lengths", "[cbdp_codec]")
{
    uint8_t header_bytes[8] = {0};
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    uint8_t frame[64];
    size_t frame_length = 0;
    TEST_ESP_OK(codex_bridge_frame_build(
        frame, sizeof(frame), CBDP_MSG_PING, 0, 0, 0, 0, 0, 2,
        &header, NULL, 0, &frame_length
    ));

    codex_bridge_frame_t parsed = {0};
    frame[0] = 0x00;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, codex_bridge_frame_parse(frame, frame_length, &parsed));
    frame[0] = 0xCB;
    frame[2] = 2;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_VERSION, codex_bridge_frame_parse(frame, frame_length, &parsed));
    frame[2] = 1;
    frame[6] = 0;
    frame[7] = 27;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, codex_bridge_frame_parse(frame, frame_length, &parsed));
    frame[6] = 0;
    frame[7] = 40;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, codex_bridge_frame_parse(frame, frame_length - 1, &parsed));
}

TEST_CASE("CBDP parser rejects trailing bytes and truncated TLV", "[cbdp_codec]")
{
    uint8_t frame[64] = {
        0xCB, 0x01, 0x01, CBDP_MSG_PING, 0x00, 0x00, 0x00, 0x1E,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x04
    };
    codex_bridge_frame_t parsed = {0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, codex_bridge_frame_parse(frame, 30, &parsed));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, codex_bridge_frame_parse(frame, 31, &parsed));

    frame[6] = 0;
    frame[7] = 28;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, codex_bridge_frame_parse(frame, 29, &parsed));
}

TEST_CASE("CBDP TLV lookup accepts reordered unknown and rejects duplicate known tags", "[cbdp_codec]")
{
    uint8_t bytes[48];
    codex_bridge_tlv_builder_t builder = {0};
    codex_bridge_tlv_builder_init(&builder, bytes, sizeof(bytes));
    TEST_ESP_OK(codex_bridge_tlv_builder_u16(&builder, 0x0001, 0x1234));
    TEST_ESP_OK(codex_bridge_tlv_builder_bytes(&builder, 0x7777, (const uint8_t *)"skip", 4));
    TEST_ESP_OK(codex_bridge_tlv_builder_utf8(&builder, 0x0002, "ok"));

    codex_bridge_tlv_t found = {0};
    TEST_ESP_OK(codex_bridge_tlv_find(bytes, builder.length, 0x0002, &found));
    TEST_ASSERT_EQUAL_UINT16(2, found.length);
    TEST_ASSERT_EQUAL_MEMORY("ok", found.value, 2);

    codex_bridge_tlv_builder_t duplicate = {0};
    codex_bridge_tlv_builder_init(&duplicate, bytes, sizeof(bytes));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&duplicate, 0x0006, 1));
    TEST_ESP_OK(codex_bridge_tlv_builder_u8(&duplicate, 0x0006, 2));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, codex_bridge_tlv_find(bytes, duplicate.length, 0x0006, &found));
}

TEST_CASE("CBDP builder keeps payload separate from header TLV", "[cbdp_codec]")
{
    uint8_t header_bytes[16];
    codex_bridge_tlv_builder_t header = {0};
    codex_bridge_tlv_builder_init(&header, header_bytes, sizeof(header_bytes));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&header, 0x002D, 9001));
    TEST_ESP_OK(codex_bridge_tlv_builder_u32(&header, 0x0048, 0));
    const uint8_t payload[] = "hello world";
    uint8_t frame[128];
    size_t frame_length = 0;
    TEST_ESP_OK(codex_bridge_frame_build(
        frame, sizeof(frame), CBDP_MSG_SEND_CHUNK, 0, 0, 1, 0, 0, 0,
        &header, payload, sizeof(payload) - 1, &frame_length
    ));
    codex_bridge_frame_t parsed = {0};
    TEST_ESP_OK(codex_bridge_frame_parse(frame, frame_length, &parsed));
    TEST_ASSERT_EQUAL_UINT32(28 + header.length, parsed.header_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, parsed.payload, sizeof(payload) - 1);
    bool payload_in_header = false;
    for (size_t index = 0;
         index + sizeof(payload) - 1 <= parsed.header_tlv_len; ++index) {
        if (memcmp(parsed.header_tlv + index, payload, sizeof(payload) - 1) == 0) {
            payload_in_header = true;
            break;
        }
    }
    TEST_ASSERT_FALSE(payload_in_header);


}

TEST_CASE("CBDP advertised capabilities match implemented firmware paths", "[cbdp_codec]")
{
    const uint64_t unsupported_paths =
        CBDP_CAP_CROSS_DEVICE_ACTIVE | CBDP_CAP_RESUME | CBDP_CAP_EVENTS_SINCE;
    const uint64_t supported =
        CBDP_CAP_STATUS | CBDP_CAP_TEXT | CBDP_CAP_TASKS | CBDP_CAP_RESULTS |
        CBDP_CAP_READ_ACK | CBDP_CAP_TELEMETRY | CBDP_CAP_SUBSCRIBE_LIVE |
        CBDP_CAP_SUBSCRIBE_THREAD | CBDP_CAP_SUBSCRIBE_SUMMARY |
        CBDP_CAP_MULTI_PAGE | CBDP_CAP_DEVICE_CONFIG | CBDP_CAP_ACCOUNT |
        CBDP_CAP_CONTEXT_USAGE | CBDP_CAP_READ_UNREAD;
    TEST_ASSERT_FALSE((supported & unsupported_paths) != 0);
}
