/* Dotii v8 host frame renderer + visual invariants.

   Produces BMP frames under output/emotion-preview/_fw_frames so the rendered
   result can be inspected, and checks two hard visual rules:
     1. the outer disc must not move when the head animates;
     2. talking/singing must open a mouth cavity. */
#include "dotii_data.h"
#include "dotii_engine.h"
#include "dotii_data.h"
#include "dotii_engine.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_SIZE 466
#define MOUTH_Y 300.57f
#define CENTER 233.0f

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void write_bmp(const char *path, const uint16_t *fb, int32_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    const int32_t row = (size * 3 + 3) / 4 * 4;
    const int32_t data = row * size;
    const int32_t off = 54;
    const int32_t file = off + data;
    uint8_t header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B'; header[1] = 'M';
    uint32_t u = (uint32_t)file; memcpy(header + 2, &u, 4);
    u = (uint32_t)off; memcpy(header + 10, &u, 4);
    u = 40; memcpy(header + 14, &u, 4);
    int32_t s32 = size; memcpy(header + 18, &s32, 4);
    memcpy(header + 22, &s32, 4);
    uint16_t planes = 1; memcpy(header + 26, &planes, 2);
    uint16_t bpp = 24; memcpy(header + 28, &bpp, 2);
    u = (uint32_t)data; memcpy(header + 34, &u, 4);
    fwrite(header, 1, 54, f);
    uint8_t *rowbuf = (uint8_t *)malloc((size_t)row);
    for (int32_t y = size - 1; y >= 0; --y) {
        memset(rowbuf, 0, (size_t)row);
        for (int32_t x = 0; x < size; ++x) {
            uint16_t px = fb[(size_t)y * size + x];
            uint8_t r = (uint8_t)(((px >> 11) & 0x1f) << 3);
            uint8_t g = (uint8_t)(((px >> 5) & 0x3f) << 2);
            uint8_t b = (uint8_t)((px & 0x1f) << 3);
            rowbuf[x * 3 + 0] = b;
            rowbuf[x * 3 + 1] = g;
            rowbuf[x * 3 + 2] = r;
        }
        fwrite(rowbuf, 1, (size_t)row, f);
    }
    free(rowbuf);
    fclose(f);
}

static void render_at(dotii_engine_t *engine, const char *id, uint32_t ms,
                      uint16_t *fb, const dotii_expr_t **out_expr, dotii_pose_t *out_pose)
{
    dotii_engine_init(engine, 0);
    dotii_engine_set(engine, id, 0);
    dotii_engine_tick(engine, ms);
    const dotii_expr_t *e = dotii_catalog_by_id(id);
    if (e == NULL) e = dotii_catalog_idle();
    dotii_draw_render(fb, FRAME_SIZE, e, &engine->state.pose, ms, NULL, NULL, NULL);
    if (out_expr) *out_expr = e;
    if (out_pose) *out_pose = engine->state.pose;
}

static int is_cavity(uint16_t px)
{
    /* Expand RGB565 to 8-bit before comparing, exactly like the probe. */
    const int r = ((px >> 11) & 0x1f) << 3;
    const int g = ((px >> 5) & 0x3f) << 2;
    const int b = (px & 0x1f) << 3;
    /* rose-plum mouth cavity: red clearly above green and blue */
    return (r > 60) && (r >= g + 24) && (r > b + 16);
}

static int count_mouth_pixels(const uint16_t *fb)
{
    int n = 0;
    for (int y = (int)MOUTH_Y - 60; y <= (int)MOUTH_Y + 60; ++y) {
        if (y < 0 || y >= FRAME_SIZE) continue;
        for (int x = (int)CENTER - 90; x <= (int)CENTER + 90; ++x) {
            if (x < 0 || x >= FRAME_SIZE) continue;
            if (is_cavity(fb[(size_t)y * FRAME_SIZE + x])) ++n;
        }
    }
    return n;
}

int main(void)
{
    uint16_t *fb = (uint16_t *)malloc(sizeof(uint16_t) * FRAME_SIZE * FRAME_SIZE);
    uint16_t *fb2 = (uint16_t *)malloc(sizeof(uint16_t) * FRAME_SIZE * FRAME_SIZE);
    if (!fb || !fb2) return 1;
    system("if not exist output\\emotion-preview\\_fw_frames mkdir output\\emotion-preview\\_fw_frames >nul 2>&1");

    char path[256];
    dotii_engine_t engine;
    const uint32_t sample_ms = 2500u;

    /* every expression, one representative frame */
    for (size_t i = 0; i < dotii_catalog_count(); ++i) {
        const dotii_expr_t *e = dotii_catalog_at(i);
        if (e == NULL) continue;
        render_at(&engine, e->id, sample_ms, fb, NULL, NULL);
        snprintf(path, sizeof(path), "output\\emotion-preview\\_fw_frames\\%02d_%s.bmp", (int)i, e->name);
        write_bmp(path, fb, FRAME_SIZE);
    }

    /* animation strips: idle / listening / speaking / music */
    const char *anim_ids[4] = { "00", "03", "05", "36" };
    const char *anim_names[4] = { "IDLE", "LISTENING", "SPEAKING", "MUSIC" };
    for (int n = 0; n < 4; ++n) {
        for (int k = 0; k < 6; ++k) {
            const uint32_t ms = 400u + (uint32_t)k * 530u;
            render_at(&engine, anim_ids[n], ms, fb, NULL, NULL);
            snprintf(path, sizeof(path), "output\\emotion-preview\\_fw_frames\\anim_%s_%d.bmp", anim_names[n], k);
            write_bmp(path, fb, FRAME_SIZE);
        }
    }

    /* --- invariant 1: static outer disc --- */
    render_at(&engine, "00", 0, fb, NULL, NULL);
    for (uint32_t t = 500; t <= 12000; t += 500) {
        render_at(&engine, "00", t, fb2, NULL, NULL);
        int diff = 0;
        for (int y = 0; y < FRAME_SIZE; ++y) {
            for (int x = 0; x < FRAME_SIZE; ++x) {
                const float dx = (float)x + 0.5f - CENTER, dy = (float)y + 0.5f - CENTER;
                const float q = sqrtf(dx * dx + dy * dy);
                /* edge ring: 5..12 px inside the disc boundary */
                if (q > 224.0f && q < 231.0f) {
                    if (fb[(size_t)y * FRAME_SIZE + x] != fb2[(size_t)y * FRAME_SIZE + x]) ++diff;
                }
            }
        }
        if (diff != 0) {
            printf("FAIL: outer ring moved at t=%u (%d pixels differ)\n", (unsigned)t, diff);
            ++failures;
            break;
        }
    }
    printf("  static outer ring: OK\n");

    /* --- invariant 2: talk / sing open a cavity, closed mouths do not --- */
    render_at(&engine, "02", 2500, fb, NULL, NULL);   /* sleep, flat mouth */
    const int closed_px = count_mouth_pixels(fb);
    render_at(&engine, "05", 2500, fb, NULL, NULL);   /* talking */
    const int talk_px = count_mouth_pixels(fb);
    render_at(&engine, "36", 2500, fb, NULL, NULL);   /* singing */
    const int sing_px = count_mouth_pixels(fb);
    printf("  mouth cavity pixels: closed=%d talk=%d sing=%d\n", closed_px, talk_px, sing_px);
    CHECK(talk_px > closed_px + 250, "talking opens a visible mouth cavity");
    CHECK(sing_px > closed_px + 250, "singing opens a visible mouth cavity");

    /* --- invariant 3: every frame draw call returns a sane dirty rect --- */
    dotii_rect_t dirty, content;
    render_at(&engine, "00", 1000, fb, NULL, NULL);
    dotii_draw_render(fb, FRAME_SIZE, dotii_catalog_idle(), &engine.state.pose, 1000, NULL, &dirty, &content);
    CHECK(dirty.x1 == 0 && dirty.y1 == 0 && dirty.x2 == FRAME_SIZE - 1 && dirty.y2 == FRAME_SIZE - 1,
          "NULL clip forces a full repaint");
    CHECK(content.x1 >= 0 && content.y1 >= 0 && content.x2 < FRAME_SIZE && content.y2 < FRAME_SIZE,
          "content rect inside the canvas");
    CHECK(content.x2 > content.x1 && content.y2 > content.y1, "content rect is non-empty");

    free(fb);
    free(fb2);
    if (failures == 0) { printf("frames: all checks passed\n"); return 0; }
    printf("%d failures\n", failures);
    return 1;
}




