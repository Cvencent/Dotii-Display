#include "animation.h"

#include <math.h>

/* Animation primitives ported from aora-bot (emotion-ball/js/emotions.js).
   All primitives are pure functions of time and return a bounded scalar so
   callers can clamp safely. Non-positive period or amplitude yields 0. */

#define ANIM_TWO_PI 6.2831853f

static float fractf(float value)
{
    return value - floorf(value);
}

float animation_eval(anim_type_t type, float t_seconds, float amp, float period, float phase)
{
    if (period <= 0.0f || amp == 0.0f) {
        return 0.0f;
    }

    const float x = (t_seconds / period) + phase;

    switch (type) {
    case ANIM_SINE:
        return amp * sinf(ANIM_TWO_PI * x);
    case ANIM_GLANCE: {
        /* Smooth square wave: dwells at both extremes like glance left/right. */
        const float s = sinf(ANIM_TWO_PI * x);
        return amp * (s / sqrtf(0.2f + s * s));
    }
    case ANIM_PULSE:
        return amp * (0.5f - 0.5f * cosf(ANIM_TWO_PI * x));
    case ANIM_JITTER: {
        const float noise = ANIM_TWO_PI * fractf(x);
        const float j = sinf(noise) * 0.5f + sinf(2.3f * noise) * 0.5f;
        return amp * j * 0.5f;
    }
    case ANIM_SCAN: {
        /* Phase-neutral triangle: 0 -> +1 at quarter -> 0 -> -1 at 3/4 -> 0. */
        const float a = fractf(x);
        float tri;
        if (a < 0.25f) {
            tri = a * 4.0f;
        } else if (a < 0.75f) {
            tri = 1.0f - (a - 0.25f) * 4.0f;
        } else {
            tri = -1.0f + (a - 0.75f) * 4.0f;
        }
        return amp * tri;
    }
    case ANIM_BLINK:
        return amp * (0.5f + 0.5f * sinf(ANIM_TWO_PI * x));
    default:
        return 0.0f;
    }
}
