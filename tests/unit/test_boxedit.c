#include <stdlib.h>
#include "unit.h"
#include "core/archive.h"
#include "core/boxedit.h"
#include "core/fsu8.h"
#include "core/session.h"
#include "core/sha256.h"

#define EROOT "tmp_unit/edit"

static void put(const char *path, const char *text)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static int contains(const char *path, const char *needle)
{
    static char buf[65536]; FILE *f = u8_fopen(path, "rb"); size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, needle) != NULL;
}

/* a closed box in EROOT/<folder>: nf floppies FLOPPY01.dsk..., then nc CDs */
static meta_box *make_box(const char *folder, const char *title, const char *edition, int nf, int nc)
{
    meta_box *b = meta_new(); strbuf err; int i, bad; char bag[300], p[600];
    sb_init(&err);
    snprintf(bag, sizeof(bag), EROOT "/%s", folder);
    u8_mkdirs(EROOT);
    CHECK(bag_create(bag, &err) == 0);
    str_copy(b->title, sizeof(b->title), title);
    str_copy(b->edition, sizeof(b->edition), edition);
    str_copy(b->platform, sizeof(b->platform), "MS-DOS");
    str_copy(b->started, sizeof(b->started), "2026-10-10T10:00:00");
    str_copy(b->closed, sizeof(b->closed), "2026-10-10T11:00:00");
    meta_set_counts(b, nf, nc, 0);
    str_copy(b->names[MEDIA_FLOPPY], sizeof(b->names[0]), "FLOPPY{nn}");
    str_copy(b->names[MEDIA_CD], sizeof(b->names[0]), "CD{nn}");
    for (i = 1; i <= nf + nc; i++) {
        meta_disk *d = meta_disk_get(b, i);
        int kind, num;
        meta_slot(b, i, &kind, &num);
        d->kind = kind; d->num = num;
        str_copy(d->status, sizeof(d->status), "clean");
        if (kind == MEDIA_FLOPPY) snprintf(d->master, sizeof(d->master), "FLOPPY%02d.dsk", num);
        else {
            snprintf(d->master, sizeof(d->master), "CD%02d", num);
            snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
            u8_mkdirs(p);
        }
        d->nreads = 1;
        str_copy(d->reads[0].file, sizeof(d->reads[0].file), d->master);
        snprintf(p, sizeof(p), kind == MEDIA_FLOPPY ? "%s/data/%s" : "%s/data/%s/x.bin", bag, d->master);
        put(p, d->master);
        snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
        sha256_path(p, d->reads[0].sha256, &d->reads[0].bytes);
    }
    snprintf(p, sizeof(p), "%s/data", bag);
    CHECK(meta_save(b, p, NULL, NULL, &err) == 0);
    CHECK(bag_close(bag, b, NULL, &err, &bad) == 0);
    sb_free(&err);
    return b;
}

static int verified(const char *bag)
{
    arc_check c; strbuf err; int ok;
    sb_init(&err);
    ok = arc_verify(bag, &c, &err) == 0 && !c.changed && !c.lost && !c.extra && !bag_is_open(bag);
    sb_free(&err);
    return ok;
}

static void grade_order(void)
{
    CHECK(boxedit_grade("redump-verified", "dic-cd") == 6);
    CHECK(boxedit_grade("clean", "dic-cd") == 5);
    CHECK(boxedit_grade("clean", "") == 5);
    CHECK(boxedit_grade("clean", "dic-data") == 4);
    CHECK(boxedit_grade("marginal", "") == 3 && boxedit_grade("unstable", "dic-cd") == 3);
    CHECK(boxedit_grade("fragile", "") == 2 && boxedit_grade("read-once", "dic-cd") == 2);
    CHECK(boxedit_grade("error", "") == 1 && boxedit_grade("errors", "dic-cd") == 1);
    CHECK(boxedit_grade("read-failed", "") == 0 && boxedit_grade("", "") == 0);
    CHECK(boxedit_grade("missing", "") == 0 && boxedit_grade("interrupted", "") == 0);
}

static void decide_cases(void)
{
    CHECK(boxedit_decide(0, "", 2, "b") == KEEP_NEW);
    CHECK(boxedit_decide(0, "", 0, "b") == KEEP_OLD);
    CHECK(boxedit_decide(4, "a", 5, "b") == KEEP_NEW);
    CHECK(boxedit_decide(5, "a", 1, "b") == KEEP_OLD);
    CHECK(boxedit_decide(5, "a", 5, "a") == KEEP_OLD_IDENTICAL);
    CHECK(boxedit_decide(5, "a", 5, "b") == KEEP_ASK);
}

static void edit_title_and_edition(void)
{
    meta_box *b = make_box("Gioco", "Gioco", "", 2, 0); strbuf err; char nb[600];
    edit_change ch[16]; int n;
    sb_init(&err);
    str_copy(b->title, sizeof(b->title), "Gioco nuovo");
    str_copy(b->edition, sizeof(b->edition), "IT");
    str_copy(b->publisher, sizeof(b->publisher), "LucasArts");
    {
        meta_box *old = meta_new();
        CHECK(meta_load(old, EROOT "/Gioco/data", &err) == 0);
        n = boxedit_diff(EROOT "/Gioco", old, b, ch, 16);
        CHECK(n == 4);              /* title, edition, publisher, folder */
        CHECK(ch[n - 1].what == CH_FOLDER && strstr(ch[n - 1].to, "Gioco nuovo (IT)") != NULL);
        meta_delete(old);
    }
    CHECK(boxedit_save_data(EROOT "/Gioco", b, "typo", nb, sizeof(nb), &err) == 0);
    CHECK(strstr(nb, "Gioco nuovo (IT)") != NULL);
    CHECK(!u8_exists(EROOT "/Gioco"));
    CHECK(verified(EROOT "/Gioco nuovo (IT)"));
    CHECK(contains(EROOT "/Gioco nuovo (IT)/bag-info.txt", "Title: Gioco nuovo\n"));
    CHECK(contains(EROOT "/Gioco nuovo (IT)/bag-info.txt", "Publisher: LucasArts\n"));
    CHECK(contains(EROOT "/Gioco nuovo (IT)/data/metadata.json", "\"action\": \"edit\""));
    CHECK(contains(EROOT "/Gioco nuovo (IT)/data/metadata.json", "\"reason\": \"typo\""));
    CHECK(contains(EROOT "/Gioco nuovo (IT)/data/metadata.json", "\"to\": \"Gioco nuovo (IT)\""));
    meta_delete(b); sb_free(&err);
}

static void edit_case_only_title(void)
{
    meta_box *b = make_box("caso", "caso", "", 1, 0); strbuf err; char nb[600];
    sb_init(&err);
    str_copy(b->title, sizeof(b->title), "Caso");
    CHECK(boxedit_save_data(EROOT "/caso", b, "", nb, sizeof(nb), &err) == 0);
    if (err.len) printf("  case only: %s\n", err.s);
    CHECK(!strcmp(nb + strlen(nb) - 5, U8_SEP "Caso"));
    CHECK(verified(nb));
    meta_delete(b); sb_free(&err);
}

static void edit_target_exists(void)
{
    meta_box *b = make_box("Uno", "Uno", "", 1, 0), *o = make_box("Due", "Due", "", 1, 0);
    strbuf err; char nb[600];
    sb_init(&err);
    str_copy(b->title, sizeof(b->title), "Due");
    CHECK(boxedit_save_data(EROOT "/Uno", b, "", nb, sizeof(nb), &err) == -1);
    CHECK(strstr(err.s, "Due") != NULL);
    CHECK(verified(EROOT "/Uno") && verified(EROOT "/Due"));
    CHECK(contains(EROOT "/Uno/bag-info.txt", "Title: Uno\n"));
    meta_delete(b); meta_delete(o); sb_free(&err);
}

static void edit_remove_last(void)
{
    meta_box *b = make_box("Tre", "Tre", "", 3, 1), *c = meta_new(); strbuf err; char nb[600];
    sb_init(&err);
    meta_set_counts(b, 2, 1, 0);
    CHECK(boxedit_save_data(EROOT "/Tre", b, "", nb, sizeof(nb), &err) == 0);
    CHECK(verified(EROOT "/Tre"));
    CHECK(u8_exists(EROOT "/Tre/set-aside/FLOPPY03.dsk.removed"));
    CHECK(!u8_exists(EROOT "/Tre/data/FLOPPY03.dsk"));
    CHECK(meta_load(c, EROOT "/Tre/data", &err) == 0);
    CHECK(c->media_expected == 3 && meta_count(c, MEDIA_FLOPPY) == 2);
    /* the CD moved up: now position 3 */
    CHECK(meta_disk_get(c, 3) && meta_disk_get(c, 3)->kind == MEDIA_CD && !strcmp(meta_disk_get(c, 3)->master, "CD01"));
    CHECK(contains(EROOT "/Tre/data/metadata.json", "\"change\": \"removed\""));
    CHECK(contains(EROOT "/" SESSION_CSV, "3;FLOPPY03.dsk;1;removed;"));
    meta_delete(b); meta_delete(c); sb_free(&err);
}

static void edit_add(void)
{
    meta_box *b = make_box("Add", "Add", "", 1, 1), *c = meta_new(); strbuf err; char nb[600];
    sb_init(&err);
    meta_set_counts(b, 1, 2, 0);
    CHECK(boxedit_save_data(EROOT "/Add", b, "", nb, sizeof(nb), &err) == 0);
    CHECK(verified(EROOT "/Add"));
    CHECK(contains(EROOT "/Add/bag-info.txt", "Box-Status: incomplete\n"));
    CHECK(meta_load(c, EROOT "/Add/data", &err) == 0);
    CHECK(meta_disk_get(c, 3) && !strcmp(meta_disk_get(c, 3)->status, ST_MISSING));
    CHECK(contains(EROOT "/Add/data/metadata.json", "\"change\": \"added\""));
    meta_delete(b); meta_delete(c); sb_free(&err);
}

static void edit_set_frozen(void)
{
    meta_box *b = make_box("Set", "Set", "", 1, 0); strbuf err; char nb[600];
    sb_init(&err);
    /* the box was named with the title: its new reads keep that name */
    str_copy(b->names[MEDIA_FLOPPY], sizeof(b->names[0]), "{set} {nn}");
    {
        char p[300];
        snprintf(p, sizeof(p), EROOT "/Set/data");
        CHECK(meta_save(b, p, NULL, NULL, &err) == 0);
        {
            int bad; CHECK(bag_close(EROOT "/Set", b, NULL, &err, &bad) == 0);
        }
    }
    str_copy(b->title, sizeof(b->title), "Set nuovo");
    CHECK(boxedit_save_data(EROOT "/Set", b, "", nb, sizeof(nb), &err) == 0);
    CHECK(contains(EROOT "/Set nuovo/data/metadata.json", "\"floppy\": \"Set {nn}\""));
    meta_delete(b); sb_free(&err);
}

/* technical data as the read wrote it: an edit of the data does not
 * compute it again (the redump DATs may not be there any more) */
static void probe_tech(void *ctx, const meta_disk *d, const char *img, json_writer *w)
{
    (void)ctx; (void)d; (void)img;
    jw_str(w, "probe", "as read");
}

static void edit_keeps_tech(void)
{
    meta_box *b = make_box("Tech", "Tech", "", 1, 0); strbuf err; char nb[600]; int bad;
    sb_init(&err);
    CHECK(meta_save(b, EROOT "/Tech/data", probe_tech, NULL, &err) == 0);
    CHECK(bag_close(EROOT "/Tech", b, NULL, &err, &bad) == 0);
    str_copy(b->publisher, sizeof(b->publisher), "Altro");
    CHECK(boxedit_save_data(EROOT "/Tech", b, "", nb, sizeof(nb), &err) == 0);
    CHECK(contains(EROOT "/Tech/data/metadata.json", "\"probe\": \"as read\""));
    CHECK(!contains(EROOT "/Tech/data/metadata.json", "filesystem_note"));
    CHECK(verified(EROOT "/Tech"));
    meta_delete(b); sb_free(&err);
}

static void edit_nothing(void)
{
    meta_box *b = make_box("Same", "Same", "", 1, 0); strbuf err; char nb[600];
    sb_init(&err);
    CHECK(boxedit_save_data(EROOT "/Same", b, "", nb, sizeof(nb), &err) == 0);
    CHECK(!u8_exists(EROOT "/Same/set-aside"));
    CHECK(!contains(EROOT "/Same/data/metadata.json", "history"));
    meta_delete(b); sb_free(&err);
}

static void editable_rules(void)
{
    meta_box *b = make_box("Ed", "Ed", "", 1, 0); strbuf why;
    sb_init(&why);
    CHECK(boxedit_editable(EROOT "/Ed", &why) == 1);
    u8_mkdir(EROOT "/Ed/editing");
    CHECK(boxedit_editable(EROOT "/Ed", &why) == 0 && why.len > 0);
    u8_rmtree(EROOT "/Ed/editing");
    u8_mkdir(EROOT "/Ed/messi-da-parte");
    CHECK(boxedit_editable(EROOT "/Ed", &why) == 0);
    u8_rmtree(EROOT "/Ed/messi-da-parte");
    put(EROOT "/Ed/" BAG_OPEN_MARK, "x");
    CHECK(boxedit_editable(EROOT "/Ed", &why) == 0);
    meta_delete(b); sb_free(&why);
}

void test_boxedit(void)
{
    u8_mkdir("tmp_unit");
    grade_order();
    decide_cases();
    edit_title_and_edition();
    edit_case_only_title();
    edit_target_exists();
    edit_remove_last();
    edit_add();
    edit_set_frozen();
    edit_nothing();
    edit_keeps_tech();
    editable_rules();
}
