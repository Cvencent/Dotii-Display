#include "emotion_render.h"

#include <math.h>
#include <string.h>

#include "emotion_pose.h"

#define DESIGN 240

static lv_obj_t *make_layer(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 10, 10);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

bool emotion_render_attach(emotion_render_t *render, lv_obj_t *parent, int canvas)
{
    if (render == NULL || parent == NULL) {
        return false;
    }
    if (render->attached) {
        return true;
    }
    memset(render, 0, sizeof(*render));
    render->root = parent;

    render->body = make_layer(parent);
    render->blush_left = make_layer(parent);
    render->blush_right = make_layer(parent);
    render->eye_left = make_layer(parent);
    render->eye_right = make_layer(parent);
    render->mouth = make_layer(parent);

    /* Store the scale factor so logical 240x240 geometry maps to the panel. */
    const int scale = canvas > 0 ? canvas : 466;
    lv_obj_set_user_data(render->root, (void *)(intptr_t)scale);
    render->attached = true;
    return true;
}

void emotion_render_detach(emotion_render_t *render)
{
    if (render == NULL || !render->attached) {
        return;
    }
    memset(render, 0, sizeof(*render));
}

static void apply_rect(lv_obj_t *obj, float cx, float cy, float w, float h,
                       uint32_t rgb, lv_opa_t opa, int scale)
{
    if (obj == NULL) {
        return;
    }
    const int sw = (int)lroundf(w * scale / (float)DESIGN);
    const int sh = (int)lroundf(h * scale / (float)DESIGN);
    const int sx = (int)lroundf((cx - DESIGN / 2.0f) * scale / (float)DESIGN);
    const int sy = (int)lroundf((cy - DESIGN / 2.0f) * scale / (float)DESIGN);
    lv_obj_set_size(obj, sw < 1 ? 1 : sw, sh < 1 ? 1 : sh);
    lv_obj_align(obj, LV_ALIGN_CENTER, sx, sy);
    lv_obj_set_style_bg_color(obj, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    if (opa == LV_OPA_TRANSP) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void emotion_render_update(emotion_render_t *render,
                           const emotion_engine_state_t *state,
                           const emotion_def_t *def,
                           const character_def_t *character)
{
    if (render == NULL || !render->attached) {
        return;
    }
    const int scale = (int)(intptr_t)lv_obj_get_user_data(render->root);
    const character_def_t *pack = character != NULL ? character : character_pack_fallback();

    emotion_pose_t pose;
    emotion_pose_eval(state, def, pack, &pose);

    /* Body sway/scale is expressed by nudging eye/mouth anchors together. */
    const float dx = pose.body_x;
    const float dy = pose.body_y;
    const float scale_mul = pose.body_scale;

    const float eye_dx = (float)pack->face.eye_dx * scale_mul;
    const float eye_cy = 120.0f + pack->face.eye_cy - 120.0f + dy;
    const float eye_w = (float)pose.eye_w * pose.eye_scale;
    const float eye_h = (float)pose.eye_h * pose.eye_scale * (0.15f + 0.85f * pose.eye_openness);
    const float look_x = pose.eye_look_x * 10.0f;
    const float look_y = pose.eye_look_y * 6.0f;

    apply_rect(render->eye_left, 120.0f - eye_dx + dx + look_x, eye_cy + look_y,
               eye_w, eye_h, pack->eye_color, LV_OPA_COVER, scale);
    apply_rect(render->eye_right, 120.0f + eye_dx + dx + look_x, eye_cy + look_y,
               eye_w, eye_h, pack->eye_color, LV_OPA_COVER, scale);

    const float blush_opa = pose.blush * 200.0f;
    apply_rect(render->blush_left, 120.0f - pack->face.blush_dx + dx,
               120.0f + pack->face.blush_dy + dy, 26.0f, 14.0f,
               pack->blush_color, (lv_opa_t)lroundf(blush_opa), scale);
    apply_rect(render->blush_right, 120.0f + pack->face.blush_dx + dx,
               120.0f + pack->face.blush_dy + dy, 26.0f, 14.0f,
               pack->blush_color, (lv_opa_t)lroundf(blush_opa), scale);

    const float mouth_w = 20.0f + pose.mouth_open * 18.0f;
    const float mouth_h = 6.0f + pose.mouth_open * 14.0f;
    apply_rect(render->mouth, 120.0f + dx, 120.0f + pack->face.mouth_dy + dy,
               mouth_w, mouth_h, pack->mouth_color, LV_OPA_COVER, scale);
}
