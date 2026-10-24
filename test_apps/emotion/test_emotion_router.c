#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "emotion_router.h"

static void test_priority_and_expiry(void)
{
    dotii_engine_t engine;
    dotii_engine_init(&engine, 0);
    emotion_router_t router;
    emotion_router_init(&router, &engine);

    /* Xiaozhi thinking under a temporary touch surprise. */
    assert(emotion_router_set_source(&router, EMOTION_SRC_XIAOZHI, "04", 0, 0));
    assert(emotion_router_set_source(&router, EMOTION_SRC_TOUCH, "20", 1000, 0));
    emotion_router_tick(&router, 0);
    assert(strcmp(emotion_router_active_id(&router), "20") == 0);
    assert(strcmp(dotii_engine_current_id(&engine), "20") == 0);

    /* Touch still active at t=500. */
    emotion_router_tick(&router, 500);
    assert(strcmp(emotion_router_active_id(&router), "20") == 0);

    /* Touch expires at t=1100 -> xiaozhi wins. */
    emotion_router_tick(&router, 1100);
    assert(strcmp(emotion_router_active_id(&router), "04") == 0);
    assert(strcmp(dotii_engine_current_id(&engine), "04") == 0);

    /* Clearing xiaozhi -> idle fallback. */
    emotion_router_clear_source(&router, EMOTION_SRC_XIAOZHI, 1100);
    emotion_router_tick(&router, 1200);
    assert(strcmp(emotion_router_active_id(&router), "00") == 0);
}

static void test_unknown_and_alias(void)
{
    dotii_engine_t engine;
    dotii_engine_init(&engine, 0);
    emotion_router_t router;
    emotion_router_init(&router, &engine);

    assert(!emotion_router_set_source(&router, EMOTION_SRC_TOUCH, "99", 0, 0));
    assert(!emotion_router_set_source(&router, EMOTION_SRC_TOUCH, "", 0, 0));
    assert(!emotion_router_set_source(&router, EMOTION_SRC_TOUCH, NULL, 0, 0));

    /* New vocabulary name resolves to its stable id. */
    assert(emotion_router_set_source(&router, EMOTION_SRC_TOUCH, "happy", 0, 0));
    emotion_router_tick(&router, 0);
    assert(strcmp(emotion_router_active_id(&router), "10") == 0);

    /* Legacy aora names still route; touch outranks business so clear it first. */
    emotion_router_clear_source(&router, EMOTION_SRC_TOUCH, 5);
    assert(emotion_router_set_source(&router, EMOTION_SRC_BUSINESS, "panicked", 0, 0));
    emotion_router_tick(&router, 10);
    assert(strcmp(emotion_router_active_id(&router), "20") == 0);
}

static void test_business_lowest(void)
{
    dotii_engine_t engine;
    dotii_engine_init(&engine, 0);
    emotion_router_t router;
    emotion_router_init(&router, &engine);

    emotion_router_set_source(&router, EMOTION_SRC_BUSINESS, "36", 0, 0);
    emotion_router_set_source(&router, EMOTION_SRC_XIAOZHI, "03", 0, 0);
    emotion_router_tick(&router, 10);
    assert(strcmp(emotion_router_active_id(&router), "03") == 0);

    emotion_router_clear_source(&router, EMOTION_SRC_XIAOZHI, 20);
    emotion_router_tick(&router, 30);
    assert(strcmp(emotion_router_active_id(&router), "36") == 0);

    emotion_router_clear_source(&router, EMOTION_SRC_BUSINESS, 40);
    emotion_router_tick(&router, 50);
    assert(strcmp(emotion_router_active_id(&router), "00") == 0);
}

int main(void)
{
    test_priority_and_expiry();
    test_unknown_and_alias();
    test_business_lowest();
    printf("emotion router: all checks passed\n");
    return 0;
}

