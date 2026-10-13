#include "emotion_engine.h"

#include <math.h>
#include <string.h>

static float clampf(float value, float low, float high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static const emotion_def_t *resolve(const char *id_or_name)
{
    if (id_or_name == NULL || id_or_name[0] == '\0') {
        return NULL;
    }
    const emotion_def_t *def = emotion_catalog_by_id(id_or_name);
    if (def == NULL) {
        def = emotion_catalog_by_name(id_or_name);
    }
    return def;
}

static void apply_def(emotion_engine_t *engine, const emotion_def_t *def, uint32_t now_ms)
{
    engine->current = def;
    engine->entered_at_ms = now_ms;
    engine->openness = def->openness;
    engine->snapshot.id[0] = def->id[0];
    engine->snapshot.id[1] = def->id[1];
    engine->snapshot.id[2] = '\0';
    engine->snapshot.visual.id = def->id;
    engine->snapshot.visual.group = def->group;
    engine->snapshot.visual.gaze = def->gaze;
}

void emotion_engine_init(emotion_engine_t *engine, uint32_t now_ms)
{
    if (engine == NULL) {
        return;
    }
    memset(engine, 0, sizeof(*engine));
    engine->base = emotion_catalog_fallback();
    engine->now_ms = now_ms;
    apply_def(engine, engine->base, now_ms);
    engine->snapshot.active = true;
    engine->snapshot.temporary = false;
    engine->snapshot.hold_ms = 0;
    engine->snapshot.visual.openness = engine->base->openness;
    engine->snapshot.visual.openness = engine->base->openness;
}

bool emotion_engine_set(emotion_engine_t *engine, const char *id_or_name, uint32_t now_ms)
{
    if (engine == NULL) {
        return false;
    }
    const emotion_def_t *def = resolve(id_or_name);
    if (def == NULL) {
        return false;
    }
    engine->base = def;
    engine->temporary = false;
    engine->hold_ms = 0;
    engine->now_ms = now_ms;
    apply_def(engine, def, now_ms);
    engine->snapshot.active = true;
    engine->snapshot.temporary = false;
    engine->snapshot.hold_ms = 0;
    engine->snapshot.visual.openness = def->openness;
    engine->snapshot.visual.openness = def->openness;
    return true;
}

bool emotion_engine_set_hold(emotion_engine_t *engine, const char *id_or_name,
                             uint32_t hold_ms, uint32_t now_ms)
{
    if (engine == NULL) {
        return false;
    }
    const emotion_def_t *def = resolve(id_or_name);
    if (def == NULL) {
        return false;
    }
    engine->temporary = true;
    engine->hold_ms = hold_ms;
    engine->now_ms = now_ms;
    apply_def(engine, def, now_ms);
    engine->snapshot.active = true;
    engine->snapshot.temporary = true;
    engine->snapshot.hold_ms = hold_ms;
    engine->snapshot.visual.openness = def->openness;
    engine->snapshot.visual.openness = def->openness;
    return true;
}

void emotion_engine_set_gaze(emotion_engine_t *engine, float nx, float ny)
{
    if (engine == NULL) {
        return;
    }
    engine->gaze_x = clampf(nx, -1.0f, 1.0f);
    engine->gaze_y = clampf(ny, -1.0f, 1.0f);
    engine->snapshot.gaze_x = engine->gaze_x;
    engine->snapshot.gaze_y = engine->gaze_y;
}

bool emotion_engine_tick(emotion_engine_t *engine, uint32_t now_ms)
{
    if (engine == NULL) {
        return false;
    }
    engine->now_ms = now_ms;
    engine->snapshot.elapsed_seconds = (float)(now_ms - engine->entered_at_ms) / 1000.0f;
    engine->snapshot.gaze_x = engine->gaze_x;
    engine->snapshot.gaze_y = engine->gaze_y;

    if (engine->temporary && engine->current != engine->base) {
        const uint32_t elapsed = now_ms - engine->entered_at_ms;
        if (elapsed >= engine->hold_ms) {
            engine->temporary = false;
            apply_def(engine, engine->base, now_ms);
            engine->snapshot.temporary = false;
            engine->snapshot.hold_ms = 0;
            engine->snapshot.visual.openness = engine->base->openness;
            engine->snapshot.visual.openness = engine->base->openness;
            return true;
        }
    }
    return false;
}

const emotion_engine_state_t *emotion_engine_state(const emotion_engine_t *engine)
{
    if (engine == NULL) {
        return NULL;
    }
    return &engine->snapshot;
}

const char *emotion_engine_current_id(const emotion_engine_t *engine)
{
    if (engine == NULL || engine->current == NULL) {
        return "02";
    }
    return engine->current->id;
}
