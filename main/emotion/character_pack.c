#include "character_pack.h"

#include <string.h>

/* Body outlines are 48-point closed rings in a 240x240 canvas (centre 120).
   dotii is the project's own rounded face; nimbo/twinkle are original
   characters from aora-bot/mood-mates; blob is aora-bot's ball. The dotii
   ring is generated from an ellipse (a = 1.0, b = 0.82); the others from
   the aora-bot geometry descriptors described in mood-mates character docs. */

static const int16_t k_dotii_ring[48 * 2] = {
    224, 120, 223, 134, 220, 147, 215, 159, 208, 171, 200, 181, 190, 190, 180, 198, 168, 204, 157, 208, 145, 212, 132, 214, 120, 214, 108, 214, 95, 212, 83, 208, 72, 204, 60, 198, 50, 190, 40, 181, 32, 171, 25, 159, 20, 147, 17, 134, 16, 120, 17, 106, 20, 93, 25, 81, 32, 69, 40, 59, 50, 50, 60, 42, 72, 36, 83, 32, 95, 28, 108, 26, 120, 26, 132, 26, 145, 28, 157, 32, 168, 36, 180, 42, 190, 50, 200, 59, 208, 69, 215, 81, 220, 93, 223, 106,
};

static const int16_t k_nimbo_ring[48 * 2] = {
    230, 120, 229, 134, 223, 147, 215, 159, 207, 170, 201, 182, 195, 195, 187, 207, 175, 215, 161, 219, 146, 218, 133, 216, 120, 215, 107, 216, 94, 218, 79, 219, 65, 215, 53, 207, 45, 195, 39, 182, 33, 170, 25, 159, 17, 147, 11, 134, 10, 120, 14, 106, 22, 94, 31, 83, 38, 73, 43, 61, 48, 48, 55, 35, 65, 25, 78, 19, 93, 17, 107, 18, 120, 19, 133, 18, 147, 17, 162, 19, 175, 25, 185, 35, 192, 48, 197, 61, 202, 73, 209, 83, 218, 94, 226, 106,
};

static const int16_t k_twinkle_ring[48 * 2] = {
    224, 120, 222, 133, 215, 146, 207, 156, 198, 165, 191, 174, 185, 185, 178, 196, 170, 207, 160, 216, 147, 220, 133, 220, 120, 217, 108, 211, 97, 207, 86, 203, 73, 201, 61, 197, 48, 192, 38, 183, 31, 172, 28, 158, 28, 145, 30, 132, 31, 120, 30, 108, 28, 95, 28, 82, 31, 68, 38, 57, 48, 48, 61, 43, 73, 39, 86, 37, 97, 33, 108, 29, 120, 23, 133, 20, 147, 20, 160, 24, 170, 33, 178, 44, 185, 55, 191, 66, 198, 75, 207, 84, 215, 94, 222, 107,
};

static const int16_t k_blob_ring[48 * 2] = {
    229, 120, 227, 134, 222, 147, 216, 160, 208, 171, 200, 181, 190, 190, 180, 198, 170, 206, 158, 213, 146, 219, 134, 223, 120, 226, 106, 227, 92, 225, 78, 221, 66, 214, 54, 205, 45, 195, 38, 183, 32, 171, 27, 158, 24, 146, 22, 133, 21, 120, 21, 107, 21, 94, 24, 80, 28, 67, 35, 54, 43, 43, 54, 33, 66, 26, 79, 21, 93, 18, 106, 17, 120, 18, 133, 21, 146, 24, 158, 29, 170, 34, 181, 40, 192, 48, 203, 57, 212, 67, 220, 79, 225, 92, 228, 106,
};



static const character_def_t k_characters[] = {
    {
        "dotii", "Dotii", "dotii",
        0x36446E, 0xF1F8F5, 0xE5705F, 0xF1F8F5,
        { 46, 100, 34, 44, 22 },
        48, k_dotii_ring,
    },
    {
        "nimbo", "云宝", "aora-bot",
        0x8AD6C8, 0x25333B, 0xF0A8A8, 0x31424B,
        { 44, 100, 36, 46, 24 },
        48, k_nimbo_ring,
    },
    {
        "twinkle", "亮亮", "aora-bot",
        0xF0C24B, 0x3B3325, 0xF0A8A8, 0x5A4A2A,
        { 42, 98, 34, 42, 22 },
        48, k_twinkle_ring,
    },
    {
        "blob", "球球", "aora-bot",
        0xF6EFE4, 0x25333B, 0xF0A8A8, 0x31424B,
        { 44, 100, 34, 46, 24 },
        48, k_blob_ring,
    },
};

static const character_def_t *k_fallback = &k_characters[0]; /* dotii */

const character_def_t *character_pack_lookup(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_characters) / sizeof(k_characters[0]); ++i) {
        if (strcmp(k_characters[i].id, id) == 0) {
            return &k_characters[i];
        }
    }
    return NULL;
}

const character_def_t *character_pack_fallback(void)
{
    return k_fallback;
}

size_t character_pack_count(void)
{
    return sizeof(k_characters) / sizeof(k_characters[0]);
}

const character_def_t *character_pack_at(size_t index)
{
    if (index >= character_pack_count()) {
        return NULL;
    }
    return &k_characters[index];
}

const char *character_pack_id_at(size_t index)
{
    if (index >= character_pack_count()) {
        return NULL;
    }
    return k_characters[index].id;
}

size_t character_pack_next_index(size_t index)
{
    const size_t count = character_pack_count();
    return count == 0 ? 0 : (index + 1) % count;
}
