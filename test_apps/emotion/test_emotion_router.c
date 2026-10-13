#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "emotion_router.h"

static void test_priority_and_expiry(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    emotion_router_t router;
    emotion_router_init(&router, &engine);

    /* Touch on top of an active xiaozhi thinking state. */
    assert(emotion_router_set_source(&router, EMOTION_SRC_XIAOZHI, "30", 0, 0));
    assert(emotion_router_set_source(&router, EMOTION_SRC_TOUCH, "13", 1000, 0));
    emotion_router_tick(&router, 0);
    assert(strcmp(emotion_router_active_id(&router), "13") == 0);
    assert(strcmp(emotion_engine_current_id(&engine), "13") == 0);

    /* Touch still active at t=500. */
    emotion_router_tick(&router, 500);
    assert(strcmp(emotion_router_active_id(&router), "13") == 0);

    /* Touch expires at t=1100 -> xiaozhi wins. */
    emotion_router_tick(&router, 1100);
    assert(strcmp(emotion_router_active_id(&router), "30") == 0);
    assert(strcmp(emotion_engine_current_id(&engine), "30") == 0);

    /* Clearing xiaozhi -> idle fallback. */
    emotion_router_clear_source(&router, EMOTION_SRC_XIAOZHI, 1100);
    emotion_router_tick(&router, 1200);
    assert(strcmp(emotion_router_active_id(&router), "02") == 0);
}

static void test_unknown_and_bounds(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    emotion_router_t router;
    emotion_router_init(&router, &engine);

    assert(!emotion_router_set_source(&router, EMOTION_SRC_TOUCH, "99", 0, 0));
    assert(!emotion_router_set_source(&router, EMOTION_SRC_TOUCH, "", 0, 0));
    assert(!emotion_router_set_source(&router, EMOTION_SRC_TOUCH, NULL, 0, 0));
    assert(emotion_router_set_source(&router, EMOTION_SRC_TOUCH, "happy", 0, 0));
    emotion_router_tick(&router, 0);
    assert(strcmp(emotion_router_active_id(&router), "10") == 0);
}

static void test_business_lowest(void)
{
    emotion_engine_t engine;
    emotion_engine_init(&engine, 0);
    emotion_router_t router;
    emotion_router_init(&router, &engine);

    emotion_router_set_source(&router, EMOTION_SRC_BUSINESS, "33", 0, 0);
    emotion_router_set_source(&router, EMOTION_SRC_XIAOZHI, "35", 0, 0);
    emotion_router_tick(&router, 10);
    assert(strcmp(emotion_router_active_id(&router), "35") == 0);

    emotion_router_clear_source(&router, EMOTION_SRC_XIAOZHI, 10);
    emotion_router_tick(&router, 20);
    assert(strcmp(emotion_router_active_id(&router), "33") == 0);
}

int main(void)
{
    test_priority_and_expiry();
    test_unknown_and_bounds();
    test_business_lowest();
    printf("emotion_router tests passed\n");
    return 0;
}
