#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dotii_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sources are ordered by priority: lower index wins when several are active. */
typedef enum {
    EMOTION_SRC_TOUCH = 0,
    EMOTION_SRC_XIAOZHI,
    EMOTION_SRC_BUSINESS,
    EMOTION_SRC_COUNT,
} emotion_source_t;

typedef struct {
    char id[3];
    bool active;
    uint32_t started_at_ms;
    uint32_t hold_ms; /* 0 = sticky until cleared or replaced */
} emotion_router_slot_t;

typedef struct {
    dotii_engine_t *engine;
    emotion_router_slot_t slots[EMOTION_SRC_COUNT];
    char active_id[3];
    emotion_source_t active_source;
    bool has_active;
} emotion_router_t;

void emotion_router_init(emotion_router_t *router, dotii_engine_t *engine);

/* Raises a source. A valid emotion id/name/legacy-alias replaces that source's
   slot. An unknown id is ignored and returns false without disturbing others. */
bool emotion_router_set_source(emotion_router_t *router, emotion_source_t src,
                               const char *emotion, uint16_t hold_ms, uint32_t now_ms);

void emotion_router_clear_source(emotion_router_t *router, emotion_source_t src,
                                 uint32_t now_ms);

/* Applies the highest-priority active source to the engine, then ages slots. */
void emotion_router_tick(emotion_router_t *router, uint32_t now_ms);

const char *emotion_router_active_id(const emotion_router_t *router);
bool emotion_router_active(const emotion_router_t *router, emotion_source_t src,
                           const char **out_id);

#ifdef __cplusplus
}
#endif
