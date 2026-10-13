#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "character_pack.h"

static void test_lookup(void)
{
    const character_def_t *dotii = character_pack_lookup("dotii");
    assert(dotii != NULL);
    assert(strcmp(dotii->id, "dotii") == 0);
    assert(strcmp(dotii->name, "Dotii") == 0);

    const character_def_t *nimbo = character_pack_lookup("nimbo");
    assert(nimbo != NULL);
    assert(strcmp(nimbo->id, "nimbo") == 0);

    assert(character_pack_lookup("nope") == NULL);
    assert(character_pack_lookup(NULL) == NULL);
    assert(strcmp(character_pack_fallback()->id, "dotii") == 0);
}

static void test_ring_integrity(void)
{
    size_t count = character_pack_count();
    assert(count >= 4);
    for (size_t i = 0; i < count; ++i) {
        const character_def_t *pack = character_pack_at(i);
        assert(pack != NULL);
        assert(pack->ring != NULL);
        assert(pack->ring_points >= 24);
        assert(strlen(pack->id) > 0);
        /* Every point must be inside the 240x240 canvas. */
        for (uint16_t p = 0; p < pack->ring_points; ++p) {
            int16_t x = pack->ring[p * 2];
            int16_t y = pack->ring[p * 2 + 1];
            assert(x >= 0 && x < 240);
            assert(y >= 0 && y < 240);
        }
        assert(pack->eye_color != 0);
        assert(pack->body_color != 0);
    }
    assert(character_pack_at(count) == NULL);
    assert(character_pack_id_at(0) != NULL);
    assert(character_pack_id_at(count) == NULL);
}

int main(void)
{
    test_lookup();
    test_ring_integrity();
    printf("character_pack tests passed\n");
    return 0;
}
