#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A character pack is pure data: a closed body outline, a palette, and the
   anchors used to place the eyes, mouth and blush. New characters only add a
   row in character_pack.c; the engine and renderer never change. */

typedef struct {
    int16_t eye_dx;    /* eye centre distance from the face centre line */
    int16_t eye_cy;    /* eye centre y relative to canvas centre (120) */
    int16_t mouth_dy;  /* mouth y relative to canvas centre */
    int16_t blush_dx;  /* blush centre x offset */
    int16_t blush_dy;  /* blush centre y offset */
} character_face_t;

typedef struct {
    const char *id;
    const char *name;      /* Chinese display label */
    const char *origin;    /* short credit, e.g. "aora-bot" or "dotii" */
    uint32_t body_color;   /* 0xRRGGBB */
    uint32_t eye_color;
    uint32_t blush_color;
    uint32_t mouth_color;
    character_face_t face;
    uint16_t ring_points;  /* number of points in ring */
    const int16_t *ring;   /* 2 * ring_points ints: x0,y0,x1,y1,... */
} character_def_t;

const character_def_t *character_pack_lookup(const char *id);
const character_def_t *character_pack_fallback(void);
size_t character_pack_count(void);
const character_def_t *character_pack_at(size_t index);
const char *character_pack_id_at(size_t index);
size_t character_pack_next_index(size_t index);

#ifdef __cplusplus
}
#endif
