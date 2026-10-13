#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "animation.h"

static void check_bounded(anim_type_t type, float amp)
{
    const float samples[] = {0.0f, 0.001f, 0.5f, 1.0f, 12.34f, 1.0e6f};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        float v = animation_eval(type, samples[i], amp, 1.7f, 0.3f);
        assert(!isnan(v));
        /* Output is bounded by |amp| * 1.1 for every primitive. */
        assert(fabsf(v) <= fabsf(amp) * 1.1f);
    }
}

static void test_bounds_all_primitives(void)
{
    for (int t = 0; t < ANIM_TYPE_COUNT; ++t) {
        check_bounded((anim_type_t)t, 1.0f);
        check_bounded((anim_type_t)t, 2.0f);
        check_bounded((anim_type_t)t, 0.25f);
    }
}

static void test_zero_period_and_amp(void)
{
    assert(animation_eval(ANIM_SINE, 1.0f, 1.0f, 0.0f, 0.0f) == 0.0f);
    assert(animation_eval(ANIM_SINE, 1.0f, 1.0f, -3.0f, 0.0f) == 0.0f);
    assert(animation_eval(ANIM_SINE, 1.0f, 0.0f, 2.0f, 0.0f) == 0.0f);
}

static void test_phase_origin(void)
{
    /* sine at phase 0 and t=0 is 0; a quarter period later it is +amp. */
    assert(fabsf(animation_eval(ANIM_SINE, 0.0f, 1.0f, 4.0f, 0.0f)) < 1e-4f);
    assert(fabsf(animation_eval(ANIM_SINE, 1.0f, 1.0f, 4.0f, 0.0f) - 1.0f) < 1e-4f);
    /* scan sweeps to its maximum at a quarter period. */
    assert(fabsf(animation_eval(ANIM_SCAN, 1.0f, 1.0f, 4.0f, 0.0f) - 1.0f) < 1e-4f);
}

int main(void)
{
    test_bounds_all_primitives();
    test_zero_period_and_amp();
    test_phase_origin();
    printf("animation tests passed\n");
    return 0;
}
