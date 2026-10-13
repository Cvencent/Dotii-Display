#include "emotion_router.h"

#include <string.h>

static const emotion_def_t *resolve(const char *emotion)
{
    if (emotion == NULL || emotion[0] == '\0') {
        return NULL;
    }
    const emotion_def_t *def = emotion_catalog_by_id(emotion);
    if (def == NULL) {
        def = emotion_catalog_by_name(emotion);
    }
    return def;
}

void emotion_router_init(emotion_router_t *router, emotion_engine_t *engine)
{
    if (router == NULL) {
        return;
    }
    memset(router, 0, sizeof(*router));
    router->engine = engine;
    router->active_id[0] = '0';
    router->active_id[1] = '2';
    router->active_id[2] = '\0';
    router->active_source = EMOTION_SRC_COUNT;
}

bool emotion_router_set_source(emotion_router_t *router, emotion_source_t src,
                               const char *emotion, uint16_t hold_ms, uint32_t now_ms)
{
    if (router == NULL || src < 0 || src >= EMOTION_SRC_COUNT) {
        return false;
    }
    const emotion_def_t *def = resolve(emotion);
    if (def == NULL) {
        return false;
    }
    emotion_router_slot_t *slot = &router->slots[src];
    slot->id[0] = def->id[0];
    slot->id[1] = def->id[1];
    slot->id[2] = '\0';
    slot->active = true;
    slot->started_at_ms = now_ms;
    slot->hold_ms = hold_ms;
    return true;
}

void emotion_router_clear_source(emotion_router_t *router, emotion_source_t src,
                                 uint32_t now_ms)
{
    (void)now_ms;
    if (router == NULL || src < 0 || src >= EMOTION_SRC_COUNT) {
        return;
    }
    router->slots[src].active = false;
}

static void expire_slots(emotion_router_t *router, uint32_t now_ms)
{
    for (int i = 0; i < EMOTION_SRC_COUNT; ++i) {
        emotion_router_slot_t *slot = &router->slots[i];
        if (!slot->active || slot->hold_ms == 0) {
            continue;
        }
        if (now_ms - slot->started_at_ms >= slot->hold_ms) {
            slot->active = false;
        }
    }
}

void emotion_router_tick(emotion_router_t *router, uint32_t now_ms)
{
    if (router == NULL || router->engine == NULL) {
        return;
    }
    expire_slots(router, now_ms);

    for (int i = 0; i < EMOTION_SRC_COUNT; ++i) {
        const emotion_router_slot_t *slot = &router->slots[i];
        if (!slot->active) {
            continue;
        }
        const bool changed = (router->active_source != (emotion_source_t)i) ||
                             (strcmp(router->active_id, slot->id) != 0) ||
                             !router->has_active;
        if (changed) {
            emotion_engine_set(router->engine, slot->id, now_ms);
            memcpy(router->active_id, slot->id, sizeof(router->active_id));
            router->active_source = (emotion_source_t)i;
            router->has_active = true;
        }
        emotion_engine_tick(router->engine, now_ms);
        return;
    }

    /* No source active: fall back to the engine's own base emotion. */
    if (router->has_active) {
        emotion_engine_set(router->engine, emotion_catalog_fallback()->id, now_ms);
        router->has_active = false;
        router->active_source = EMOTION_SRC_COUNT;
        memcpy(router->active_id, emotion_catalog_fallback()->id, 3);
    }
    emotion_engine_tick(router->engine, now_ms);
}

const char *emotion_router_active_id(const emotion_router_t *router)
{
    if (router == NULL) {
        return "02";
    }
    return router->active_id;
}

bool emotion_router_active(const emotion_router_t *router, emotion_source_t src,
                           const char **out_id)
{
    if (router == NULL || src < 0 || src >= EMOTION_SRC_COUNT) {
        return false;
    }
    const emotion_router_slot_t *slot = &router->slots[src];
    if (!slot->active) {
        return false;
    }
    if (out_id != NULL) {
        *out_id = slot->id;
    }
    return true;
}
