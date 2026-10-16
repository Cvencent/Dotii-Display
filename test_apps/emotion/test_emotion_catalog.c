#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "emotion_catalog.h"

static void test_lookup_by_id(void)
{
    const emotion_def_t *happy = emotion_catalog_by_id("10");
    assert(happy != NULL);
    assert(strcmp(happy->id, "10") == 0);
    assert(strcmp(happy->name, "happy") == 0);
    assert(emotion_catalog_by_id("02") != NULL);
    assert(strcmp(emotion_catalog_by_id("02")->id, "02") == 0);
}

static void test_lookup_by_name(void)
{
    const emotion_def_t *happy = emotion_catalog_by_name("happy");
    assert(happy != NULL);
    assert(strcmp(happy->id, "10") == 0);
    assert(emotion_catalog_by_name("thinking")->id[0] == '3');
    const emotion_def_t *music = emotion_catalog_by_name("music");
    assert(music != NULL);
    assert(strcmp(music->id, "42") == 0);
    assert(emotion_catalog_by_name("does_not_exist") == NULL);
}

static void test_unknown_and_fallback(void)
{
    assert(emotion_catalog_by_id("99") == NULL);
    assert(emotion_catalog_by_id(NULL) == NULL);
    assert(emotion_catalog_by_name(NULL) == NULL);
    const emotion_def_t *fallback = emotion_catalog_fallback();
    assert(fallback != NULL);
    assert(strcmp(fallback->id, "02") == 0);
}

static void test_table_integrity(void)
{
    size_t count = emotion_catalog_count();
    assert(count >= 32);
    for (size_t i = 0; i < count; ++i) {
        const emotion_def_t *def = emotion_catalog_at(i);
        assert(def != NULL);
        assert(strlen(def->id) == 2);
        assert(def->anim_count <= 3);
        assert(def->openness >= 0.0f && def->openness <= 1.5f);
    }
    assert(emotion_catalog_at(count) == NULL);
    assert(strcmp(emotion_catalog_at(0)->id, "00") == 0);
}

int main(void)
{
    test_lookup_by_id();
    test_lookup_by_name();
    test_unknown_and_fallback();
    test_table_integrity();
    printf("emotion_catalog tests passed\n");
    return 0;
}
