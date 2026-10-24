/* Deterministic checks for the Dotii v8 expression engine. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dotii_engine.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static int pose_finite(const dotii_pose_t *p)
{
    return isfinite(p->pupil_x) && isfinite(p->pupil_y) &&
           isfinite(p->eye_open) && isfinite(p->eye_open_l) &&
           isfinite(p->eye_w) && isfinite(p->eye_h) &&
           isfinite(p->lid_top) && isfinite(p->lid_bot) &&
           isfinite(p->brow_y) && isfinite(p->brow_w) && isfinite(p->brow_px) &&
           isfinite(p->mouth_open) && isfinite(p->mouth_w) && isfinite(p->mouth_y) &&
           isfinite(p->blush) && isfinite(p->dim) &&
           isfinite(p->head_x) && isfinite(p->head_y) && isfinite(p->head_rot) &&
           isfinite(p->head_scale) && isfinite(p->blink_k);
}

static void test_set_and_hold(void)
{
    dotii_engine_t e;
    dotii_engine_init(&e, 1000);
    CHECK(strcmp(dotii_engine_current_id(&e), "00") == 0, "init is idle 00");
    CHECK(dotii_engine_set(&e, "happy", 2000), "set happy");
    CHECK(strcmp(dotii_engine_current_id(&e), "10") == 0, "happy is 10");

    CHECK(dotii_engine_set(&e, "idle", 3000), "set idle");
    CHECK(dotii_engine_set_hold(&e, "surprised", 500, 3100), "set hold surprised");
    CHECK(strcmp(dotii_engine_current_id(&e), "20") == 0, "hold active");
    dotii_engine_tick(&e, 3400);
    CHECK(strcmp(dotii_engine_current_id(&e), "20") == 0, "hold still active");
    CHECK(dotii_engine_tick(&e, 3700), "hold expiry reports change");
    CHECK(strcmp(dotii_engine_current_id(&e), "00") == 0, "hold reverts to idle");
}

static void test_alias_and_gaze(void)
{
    dotii_engine_t e;
    dotii_engine_init(&e, 0);
    CHECK(dotii_engine_set(&e, "panicked", 0), "alias panicked resolves");
    CHECK(strcmp(dotii_engine_current_id(&e), "20") == 0, "panicked -> surprised");
    CHECK(!dotii_engine_set(&e, "no_such_thing", 0), "unknown rejected");

    dotii_engine_set(&e, "idle", 0);
    dotii_engine_tick(&e, 100);
    const float before = dotii_engine_state(&e)->pose.pupil_x;
    dotii_engine_set_gaze(&e, 0.9f, -0.6f, 100);
    dotii_engine_tick(&e, 150);
    const float after = dotii_engine_state(&e)->pose.pupil_x;
    CHECK(after > before + 0.2f, "touch gaze steers the pupils");
}

static void test_all_expressions_stable(void)
{
    for (size_t i = 0; i < dotii_catalog_count(); ++i) {
        const dotii_expr_t *expr = dotii_catalog_at(i);
        dotii_engine_t e;
        dotii_engine_init(&e, 0);
        CHECK(dotii_engine_set(&e, expr->id, 0), "set every expression");
        for (uint32_t t = 0; t <= 20000; t += 250) {
            dotii_engine_tick(&e, t);
            const dotii_pose_t *p = &dotii_engine_state(&e)->pose;
            if (!pose_finite(p)) {
                printf("FAIL: %s produced non-finite pose at t=%u\n", expr->id, (unsigned)t);
                ++failures;
                break;
            }
            if (p->eye_open < -0.01f || p->eye_open > 2.45f) {
                printf("FAIL: %s eye_open out of range at t=%u (%.3f)\n", expr->id, (unsigned)t, (double)p->eye_open);
                ++failures;
                break;
            }
            if (p->mouth_open < -0.01f || p->mouth_open > 1.01f) {
                printf("FAIL: %s mouth_open out of range at t=%u (%.3f)\n", expr->id, (unsigned)t, (double)p->mouth_open);
                ++failures;
                break;
            }
            if (p->dim < -0.01f || p->dim > 1.01f) {
                printf("FAIL: %s dim out of range at t=%u (%.3f)\n", expr->id, (unsigned)t, (double)p->dim);
                ++failures;
                break;
            }
            if (fabsf(p->head_rot) > 0.35f) {
                printf("FAIL: %s head rotation unbounded at t=%u (%.3f rad)\n", expr->id, (unsigned)t, (double)p->head_rot);
                ++failures;
                break;
            }
            if (p->head_scale < 0.85f || p->head_scale > 1.2f) {
                printf("FAIL: %s head scale out of range at t=%u (%.3f)\n", expr->id, (unsigned)t, (double)p->head_scale);
                ++failures;
                break;
            }
        }
    }
}

static void test_sleep_closed_idle_open(void)
{
    dotii_engine_t e;
    dotii_engine_init(&e, 0);
    CHECK(dotii_engine_set(&e, "sleep", 0), "set sleep");
    for (uint32_t t = 0; t <= 20000; t += 250) {
        dotii_engine_tick(&e, t);
        const dotii_pose_t *p = &dotii_engine_state(&e)->pose;
        CHECK(p->eye_open < 0.10f, "sleep keeps eyes nearly shut");
        CHECK(p->blink_k == 1.0f, "sleep uses its own closed arc, not a blink");
    }

    dotii_engine_init(&e, 0);
    int saw_blink = 0;
    for (uint32_t t = 0; t <= 40000; t += 20) {
        dotii_engine_tick(&e, t);
        const dotii_pose_t *p = &dotii_engine_state(&e)->pose;
        if (p->blink_k < 0.6f) saw_blink = 1;
    }
    CHECK(saw_blink, "idle blinks within 40 s");
}

static void test_talk_and_sing_open(void)
{
    const char *ids[] = { "05", "36" };
    for (int k = 0; k < 2; ++k) {
        dotii_engine_t e;
        dotii_engine_init(&e, 0);
        CHECK(dotii_engine_set(&e, ids[k], 0), "set talk/sing");
        float lo = 10.0f, hi = -1.0f;
        for (uint32_t t = 0; t <= 8000; t += 16) {
            dotii_engine_tick(&e, t);
            const float v = dotii_engine_state(&e)->pose.mouth_open;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        /* continuity is measured at 1 ms resolution: the 85 ms talk envelope is
           intentionally fast, but it must never jump discontinuously. */
        float prev = 0.0f, max_step = 0.0f;
        for (uint32_t t = 0; t <= 8000; ++t) {
            dotii_engine_tick(&e, t);
            const float v = dotii_engine_state(&e)->pose.mouth_open;
            if (t > 0) {
                const float step = fabsf(v - prev);
                if (step > max_step) max_step = step;
            }
            prev = v;
        }
        printf("  %s mouth_open range %.3f..%.3f, 1ms max step %.4f\n", ids[k], (double)lo, (double)hi, (double)max_step);
        if (strcmp(ids[k], "05") == 0) CHECK(lo < 0.12f, "talking closes the mouth between syllables");
        else CHECK(lo < 0.45f, "singing keeps a relaxed open mouth");
        CHECK(hi > 0.60f, "mouth opens during speech");
        CHECK(max_step < 0.08f, "mouth opening is continuous (no jumps)");
    }
}

static void test_head_motion_present(void)
{
    dotii_engine_t e;
    dotii_engine_init(&e, 0);
    CHECK(dotii_engine_set(&e, "idle", 0), "set idle");
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (uint32_t t = 0; t <= 30000; t += 16) {
        dotii_engine_tick(&e, t);
        const dotii_pose_t *p = &dotii_engine_state(&e)->pose;
        if (p->head_x < minx) minx = p->head_x;
        if (p->head_x > maxx) maxx = p->head_x;
        if (p->head_y < miny) miny = p->head_y;
        if (p->head_y > maxy) maxy = p->head_y;
    }
    printf("  idle head travel x=%.2fpx y=%.2fpx\n", (double)(maxx - minx), (double)(maxy - miny));
    CHECK(maxx - minx > 1.5f, "idle head drifts horizontally (alive, not frozen)");
    CHECK(maxy - miny > 1.5f, "idle head drifts vertically (alive, not frozen)");
}

int main(void)
{
    test_set_and_hold();
    test_alias_and_gaze();
    test_all_expressions_stable();
    test_sleep_closed_idle_open();
    test_talk_and_sing_open();
    test_head_motion_present();
    if (failures == 0) { printf("dotii engine: all checks passed\n"); return 0; }
    printf("%d failures\n", failures);
    return 1;
}

