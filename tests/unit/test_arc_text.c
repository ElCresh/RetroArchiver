/* The archive's texts in the window (src/gui/arc_text.c): in English they
 * are what core/archive.c writes, for every kind of box; in Italian a few
 * samples. */
#include "unit.h"
#include "gui/arc_text.h"
#include "gui/lang.h"
#include <stdlib.h>
#include <wchar.h>

/* a box from its media's characters (p m f e -) and kinds (f c d) */
static void make(arc_box *b, int state, const char *disks, const char *kinds)
{
    int i, k;
    memset(b, 0, sizeof(*b));
    b->state = state;
    strcpy(b->disks, disks);
    strcpy(b->kinds, kinds);
    b->expected = (int)strlen(disks);
    for (i = 0; disks[i]; i++) {
        k = kinds[i] == 'c' ? MEDIA_CD : kinds[i] == 'd' ? MEDIA_DVD : MEDIA_FLOPPY;
        b->count[k]++;
        if (disks[i] != '-') { b->archived++; b->kind_archived[k]++; }
        if (disks[i] == 'p') b->clean++;
        else if (disks[i] == 'm') b->marginal++;
        else if (disks[i] == 'f') b->fragile++;
        else if (disks[i] == 'e') b->errors++;
        else if (!b->first_todo) b->first_todo = i + 1;
    }
}

static int same(const arc_box *b, int open_cd)
{
    char core[1200]; wchar_t want[1200], got[1200]; int ok = 1;
    arc_advice(b, core, sizeof(core));
    mbstowcs(want, core, 1200);
    arc_advice_w(b, got, 1200);
    /* the core still thinks CDs cannot be read: the window says to resume */
    if (!open_cd && wcscmp(want, got)) { ok = 0; printf("  advice: %s\n     vs: %ls\n", core, got); }
    arc_outcome(b, core, sizeof(core));
    mbstowcs(want, core, 1200);
    arc_outcome_w(b, got, 1200);
    if (wcscmp(want, got)) { ok = 0; printf("  outcome: %s\n      vs: %ls\n", core, got); }
    arc_media(b, core, sizeof(core));
    mbstowcs(want, core, 1200);
    arc_media_w(b, got, 1200);
    if (wcscmp(want, got)) { ok = 0; printf("  media: %s\n    vs: %ls\n", core, got); }
    return ok;
}

void test_arc_text(void)
{
    static const struct { int state; const char *disks, *kinds; int open_cd; } boxes[] = {
        { ARC_COMPLETE, "pp", "ff", 0 },
        { ARC_COMPLETE, "p", "f", 0 },
        { ARC_COMPLETE, "ppp", "ffc", 0 },
        { ARC_COMPLETE, "pmfe", "ffff", 0 },
        { ARC_COMPLETE, "mm", "fc", 0 },
        { ARC_COMPLETE, "pf", "fc", 0 },
        { ARC_COMPLETE, "ff", "cc", 0 },
        { ARC_INCOMPLETE, "p-", "ff", 0 },
        { ARC_INCOMPLETE, "p--", "ffc", 0 },
        { ARC_INCOMPLETE, "e-p-", "ffff", 0 },
        { ARC_INCOMPLETE, "pp", "ff", 0 },
        { ARC_OPEN, "pp", "ff", 0 },
        { ARC_OPEN, "p", "f", 0 },
        { ARC_OPEN, "p-", "ff", 0 },
        { ARC_OPEN, "--", "ff", 0 },
        { ARC_OPEN, "pp-", "ffc", 1 },
        { ARC_OPEN, "---", "fcd", 0 },
    };
    arc_box b; wchar_t w[1200]; size_t i;
    lang_reset();
    for (i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
        make(&b, boxes[i].state, boxes[i].disks, boxes[i].kinds);
        CHECK(same(&b, boxes[i].open_cd));
    }
    make(&b, ARC_OPEN, "pp-", "ffc");
    arc_advice_w(&b, w, 1200);
    CHECK(!wcscmp(w, L"Open: 2 floppies out of 2 archived. Resume from CD 1."));
    b.redump = 1; b.clean = 2;
    arc_outcome_w(&b, w, 1200);
    CHECK(!wcscmp(w, L"2 clean (1 verified with redump)"));
    make(&b, ARC_BROKEN, "", "");
    str_copy(b.error, sizeof(b.error), "cannot find x/metadata.json: if the box is damaged, move its folder away and start over");
    arc_advice_w(&b, w, 1200);
    CHECK(!wcscmp(w, L"Cannot read this box: cannot find x/metadata.json: if the box is damaged, move its folder away and start over."));

    /* in Italian, with the real file */
    CHECK(lang_load_file("../lang/it.lng", NULL) == 0);
    make(&b, ARC_INCOMPLETE, "pm-", "ffc");
    arc_advice_w(&b, w, 1200);
    CHECK(!wcscmp(w, L"Chiuso incompleto: manca CD 1. Pacchetto chiuso e verificato. Floppy 2 è marginale: "
                     L"le letture non coincidono del tutto."));
    arc_outcome_w(&b, w, 1200);
    CHECK(!wcscmp(w, L"1 pulito, 1 marginale"));
    make(&b, ARC_COMPLETE, "pp", "ff");
    arc_advice_w(&b, w, 1200);
    CHECK(!wcscmp(w, L"Chiuso completo: tutti i floppy letti e verificati."));
    lang_reset();
}
