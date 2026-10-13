#include "emotion_pose.h"

#include <math.h>

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

void emotion_pose_eval(const emotion_engine_state_t *state,
                       const emotion_def_t *def,
                       const character_def_t *character,
                       emotion_pose_t *out)
{
    if (out == NULL) {
        return;
    }
    out->body_x = 0.0f;
    out->body_y = 0.0f;
    out->body_rotate_deg = 0.0f;
    out->body_scale = 1.0f;
    out->eye_openness = 1.0f;
    out->eye_scale = 1.0f;
    out->eye_look_x = 0.0f;
    out->eye_look_y = 0.0f;
    out->mouth_open = 0.0f;
    out->blush = 0.0f;

    const character_def_t *pack = character != NULL ? character : character_pack_fallback();
    out->eye_w = 56;
    out->eye_h = 70;

    if (state == NULL || def == NULL) {
        return;
    }

    const float t = state->elapsed_seconds;
    float open = def->openness;

    for (uint8_t i = 0; i < def->anim_count && i < 3; ++i) {
        const emotion_anim_t *anim = &def->anims[i];
        const float value = animation_eval(anim->type, t, anim->amp, anim->period, anim->phase);
        switch (anim->target) {
        case ANIM_TARGET_EYES:
            switch (anim->prop) {
            case ANIM_PROP_OPEN:   open += value; break;
            case ANIM_PROP_SCALE:  out->eye_scale += value; break;
            case ANIM_PROP_LOOK_X: out->eye_look_x += value; break;
            case ANIM_PROP_LOOK_Y: out->eye_look_y += value; break;
            case ANIM_PROP_X:      out->eye_look_x += value * 0.01f; break;
            case ANIM_PROP_Y:      out->eye_look_y += value * 0.05f; break;
            default: break;
            }
            break;
        case ANIM_TARGET_BODY:
            switch (anim->prop) {
            case ANIM_PROP_X:      out->body_x += value; break;
            case ANIM_PROP_Y:      out->body_y += value; break;
            case ANIM_PROP_ROTATE: out->body_rotate_deg += value; break;
            case ANIM_PROP_SCALE:  out->body_scale += value; break;
            default: break;
            }
            break;
        case ANIM_TARGET_LEFT:
        case ANIM_TARGET_RIGHT:
        default:
            break;
        }
    }

    /* Gaze combines the emotion's own drift with the external touch/gaze input. */
    out->eye_look_x = clampf(out->eye_look_x + state->gaze_x, -1.0f, 1.0f);
    out->eye_look_y = clampf(out->eye_look_y + state->gaze_y, -1.0f, 1.0f);
    out->eye_openness = clampf(open, 0.0f, 1.4f);
    out->eye_scale = clampf(out->eye_scale, 0.3f, 2.0f);
    out->body_scale = clampf(out->body_scale, 0.5f, 1.5f);
    out->body_rotate_deg = clampf(out->body_rotate_deg, -20.0f, 20.0f);
    out->body_x = clampf(out->body_x, -40.0f, 40.0f);
    out->body_y = clampf(out->body_y, -40.0f, 40.0f);
    out->mouth_open = clampf(fabsf(out->eye_look_y) * 0.25f, 0.0f, 1.0f);

    /* The character's eye anchors set the base eye size. */
    out->eye_w = (int)lroundf((float)pack->face.eye_dx * 0.85f);
    out->eye_h = (int)lroundf((float)pack->face.eye_dx * 1.05f);
    if (out->eye_w < 24) {
        out->eye_w = 24;
    }
    if (out->eye_h < 24) {
        out->eye_h = 24;
    }
}
