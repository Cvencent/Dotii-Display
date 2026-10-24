/* Host validation of the generated Dotii v8 catalog. */
#include "dotii_data.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)
#define CHECKF(cond, fmt, ...) do { if (!(cond)) { printf("FAIL: " fmt "\n", __VA_ARGS__); ++failures; } } while (0)

int main(void)
{
    CHECK(dotii_catalog_count() == DOTII_EXPR_COUNT, "catalog has 48 entries");
    CHECK(strcmp(dotii_catalog_idle()->name, "IDLE") == 0, "idle is IDLE");
    CHECK(strcmp(dotii_catalog_idle()->id, "00") == 0, "idle id is 00");

    size_t total_tracks = 0;
    size_t total_sig = 0;

    for (size_t i = 0; i < dotii_catalog_count(); ++i) {
        const dotii_expr_t *e = dotii_catalog_at(i);
        CHECK(e != NULL, "catalog entry present");
        if (e == NULL) continue;

        CHECKF(strlen(e->id) == 2, "expr %u id length", (unsigned)i);
        CHECKF(e->id[0] == (char)('0' + (int)(i / 10)) && e->id[1] == (char)('0' + (int)(i % 10)),
               "expr %u id matches index (%s)", (unsigned)i, e->id);
        CHECK(e->name != NULL && e->name[0] != '\0', "expr has EN name");
        CHECK(e->label != NULL && e->label[0] != '\0', "expr has label");
        CHECK(e->accent != NULL && e->accent[0] == '#', "accent is hex");
        CHECK(e->track_count > 0, "expr has motion tracks");
        CHECK(e->base.eye_arc < DOTII_EYE_ARC_COUNT, "eye arc enum in range");
        CHECK(e->base.brow < DOTII_BROW_COUNT, "brow enum in range");
        CHECK(e->base.mouth < DOTII_MOUTH_COUNT, "mouth enum in range");
        CHECK(e->base.eye_extra_l < DOTII_EYE_EXTRA_COUNT, "left eye extra in range");
        CHECK(e->base.eye_extra_r < DOTII_EYE_EXTRA_COUNT, "right eye extra in range");
        CHECK(e->base.fx_count <= 3, "effect count <= 3");
        for (int k = 0; k < e->base.fx_count; ++k)
            CHECK(e->base.fx[k] < DOTII_FX_COUNT, "effect enum in range");
        for (int c = 0; c < DOTII_CH_COUNT; ++c)
            CHECK(isfinite(e->base.value[c]), "base channel finite");

        total_tracks += e->track_count;
        for (int k = 0; k < e->track_count; ++k) {
            const dotii_track_t *t = &e->tracks[k];
            CHECK(t->channel < DOTII_CH_COUNT, "track channel in range");
            CHECK(t->wave < DOTII_WAVE_COUNT, "track wave in range");
            CHECK(isfinite(t->amp) && isfinite(t->period_ms) && isfinite(t->phase),
                  "track numbers finite");
            CHECK(t->period_ms > 100.0f, "track period positive");
        }

        total_sig += e->sig_count;
        for (int k = 0; k < e->sig_count; ++k) {
            const dotii_sig_layer_t *s = &e->sig[k];
            CHECK(s->wave < 3, "sig wave in range");
            CHECK(isfinite(s->f) && s->f > 0.0f, "sig frequency positive");
            for (int a = 0; a < DOTII_SIG_COUNT; ++a)
                CHECK(isfinite(s->a[a]), "sig amplitude finite");
        }
    }

    CHECKF(total_tracks == 1063, "total motion tracks = 1063 (got %u)", (unsigned)total_tracks);
    CHECK(total_sig >= 48 * 2, "every expression has signature layers");
    /* Layout contract: the face must be spread out across the round screen.
       These bounds are asserted (rather than one exact value) so the spacing can
       be tuned for readability without silently collapsing back to a tight,
       clustered face. */
    CHECKF(DOTII_LAYOUT_EYE_SEP >= 0.40f && DOTII_LAYOUT_EYE_SEP <= 0.48f,
           "eyeSep must stay in the widened 0.40..0.48R band (got %.4f)",
           (double)DOTII_LAYOUT_EYE_SEP);
    CHECKF(DOTII_LAYOUT_EYE_W >= 0.45f && DOTII_LAYOUT_EYE_W <= 0.51f,
           "eyeW must stay in the 0.45..0.51R band (got %.4f)",
           (double)DOTII_LAYOUT_EYE_W);
    CHECKF(DOTII_LAYOUT_MOUTH_Y >= 0.33f,
           "mouthY must clear the eyes (>= 0.33R, got %.4f)",
           (double)DOTII_LAYOUT_MOUTH_Y);
    /* Eyes must not spill past the disc: outer edge = eyeSep + eyeW/2. */
    CHECKF(DOTII_LAYOUT_EYE_SEP + DOTII_LAYOUT_EYE_W * 0.5f <= 0.78f,
           "eye outer edge must stay inside the disc (got %.4fR)",
           (double)(DOTII_LAYOUT_EYE_SEP + DOTII_LAYOUT_EYE_W * 0.5f));

    /* lookup + alias coverage */
    CHECK(dotii_catalog_by_id("00") == dotii_catalog_idle(), "by id 00");
    CHECK(dotii_catalog_by_id("99") == NULL, "unknown id rejected");
    CHECK(dotii_catalog_by_id("000") == NULL, "over-long id rejected");
    CHECK(dotii_catalog_by_name("idle") != NULL, "case-insensitive name");
    CHECK(dotii_catalog_by_name("HAPPY") != NULL, "case-insensitive happy");
    CHECK(dotii_catalog_by_name("nope") == NULL, "unknown name rejected");

    const char *legacy[] = { "sleeping", "waking", "idle", "curious", "spacing_out", "booting",
        "dormant", "shake_awake", "happy", "puzzled", "down", "surprised", "shy", "tired",
        "focused", "panicked", "resigned", "satisfied", "confused", "angry", "thinking",
        "receiving", "busy", "done", "error", "listening", "loading", "recalling", "refusing",
        "replying", "searching", "powering_off", "music" };
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); ++i) {
        const char *mapped = dotii_alias_resolve(legacy[i]);
        CHECKF(mapped != NULL, "alias %s resolves", legacy[i]);
        if (mapped != NULL) CHECKF(dotii_catalog_by_name(mapped) != NULL, "alias %s -> %s exists", legacy[i], mapped);
    }
    CHECK(dotii_alias_resolve("nonexistent") == NULL, "unknown alias rejected");

    if (failures == 0) { printf("dotii catalog: all checks passed (%u tracks)\n", (unsigned)total_tracks); return 0; }
    printf("%d failures\n", failures);
    return 1;
}


