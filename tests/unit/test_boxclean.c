/* Free space: what goes out of set-aside/ and what stays, one case per row
 * of the table in the design */
#include <stdlib.h>
#include "unit.h"
#include "core/archive.h"
#include "core/bagit.h"
#include "core/boxclean.h"
#include "core/fsu8.h"
#include "core/session.h"
#include "core/sha256.h"

#define CROOT "tmp_unit/clean"

static void put(const char *path, int n)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) { while (n--) fputc('x', f); fclose(f); }
}

/* an entry of set-aside/: a file of n bytes, or with "dir/file" a folder */
static void aside(const char *bag, const char *rel, int n)
{
    char p[700], d[700], *s;
    snprintf(p, sizeof(p), "%s/" SESSION_ASIDE_DIR "/%s", bag, rel);
    str_copy(d, sizeof(d), p);
    if ((s = strrchr(d, '/')) != NULL) { *s = 0; u8_mkdirs(d); }
    put(p, n);
}

static int there(const char *bag, const char *rel)
{
    char p[700];
    snprintf(p, sizeof(p), "%s/" SESSION_ASIDE_DIR "/%s", bag, rel);
    return u8_exists(p);
}

/* medium n with that status; a master of its own unless it has none */
static void medium(meta_box *b, const char *bag, int n, const char *status)
{
    meta_disk *d = meta_disk_get(b, n); char base[96], p[700];
    str_copy(d->status, sizeof(d->status), status);
    if (!strcmp(status, ST_DUPLICATE) || !strcmp(status, ST_MISSING)) return;
    session_disk_base(b, NULL, d->kind, d->num, base, sizeof(base));
    if (d->kind == MEDIA_FLOPPY) {
        snprintf(d->master, sizeof(d->master), "%s.dsk", base);
        snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
    } else {
        str_copy(d->master, sizeof(d->master), base);
        snprintf(p, sizeof(p), "%s/data/%s", bag, base);
        u8_mkdirs(p);
        snprintf(p, sizeof(p), "%s/data/%s/%s.bin", bag, base, base);
    }
    put(p, 40 + n);
    d->nreads = 1;
    str_copy(d->reads[0].file, sizeof(d->reads[0].file), d->master);
    str_copy(d->reads[0].at, sizeof(d->reads[0].at), "2026-11-01T10:00:00");
    snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
    sha256_path(p, d->reads[0].sha256, &d->reads[0].bytes);
}

/* a box of nf floppies and nc CDs with those statuses, closed unless open */
static void make_box(const char *bag, const char *title, int nf, int nc, const char *const *status, int open)
{
    meta_box *b = meta_new(); strbuf err; char data[700]; int n, bad;
    sb_init(&err);
    u8_mkdirs(CROOT);
    CHECK(bag_create(bag, &err) == 0);
    str_copy(b->title, sizeof(b->title), title);
    str_copy(b->platform, sizeof(b->platform), "X");
    str_copy(b->started, sizeof(b->started), "2026-11-01T09:00:00");
    meta_set_counts(b, nf, nc, 0);
    for (n = 1; n <= nf + nc; n++) medium(b, bag, n, status[n - 1]);
    snprintf(data, sizeof(data), "%s/data", bag);
    if (!open) str_copy(b->closed, sizeof(b->closed), "2026-11-01T11:00:00");
    CHECK(meta_save(b, data, NULL, NULL, &err) == 0);
    if (!open) CHECK(bag_close(bag, b, NULL, &err, &bad) == 0);
    sb_free(&err);
    meta_delete(b);
}

static const clean_item *item(const clean_plan *p, const char *name)
{
    int i;
    for (i = 0; i < p->n; i++) if (!strcmp(p->item[i].name, name)) return &p->item[i];
    return NULL;
}

static int no_identical(void *ctx, const char *path, int isdir)
{
    if (strstr(path, ".identical")) { (*(int *)ctx)++; return -1; }
    return boxclean_rm_delete(NULL, path, isdir);
}

#define BOX CROOT "/Misto"

static void the_table(void)
{
    /* floppy 1 clean, 2 marginal, 3 duplicate, 4 fragile; CD 1 verified by redump, CD 2 read once */
    static const char *const st[] = { ST_CLEAN, ST_MARGINAL, ST_DUPLICATE, ST_FRAGILE, ST_REDUMP, ST_READ_ONCE };
    static const char *const stay[] = {
        "FLOPPY03.dsk.duplicate", "FLOPPY03_b.dsk.duplicate2", "FLOPPY02.dsk.failed", "FLOPPY04.dsk.failed",
        "CD01_c.failed2/CD01_c_disc.txt", "CD02.failed/CD02.bin",
        "FLOPPY01.dsk.damaged", "FLOPPY01.dsk.replaced", "FLOPPY01.dsk.rejected", "FLOPPY01.dsk.removed",
        "FLOPPY01.dsk.unused", "previous-2026-11-01T110000/metadata.json", "notes.txt",
        "CD10.failed", "FLOPPY01x.dsk.failed", "OTHER.dsk.failed", NULL };
    clean_plan p; const clean_item *it; arc_check c; arc_box *a; strbuf err; int i, refused = 0;

    make_box(BOX, "Misto", 4, 2, st, 0);
    /* identical to the read kept: always */
    aside(BOX, "FLOPPY01.dsk.identical", 10);
    aside(BOX, "FLOPPY01_b.dsk.identical2", 11);
    aside(BOX, "CD02_b.identical/CD02_b.bin", 20);
    /* duplicate: of a medium that ended well, not of one still a duplicate */
    aside(BOX, "FLOPPY01.dsk.duplicate", 12);
    aside(BOX, "CD01.duplicate/CD01.bin", 50);
    aside(BOX, "CD01.duplicate/CD01_disc.txt", 3);
    aside(BOX, "FLOPPY03.dsk.duplicate", 13);
    aside(BOX, "FLOPPY03_b.dsk.duplicate2", 13);
    /* failed: a file goes, of a folder the .txt logs stay */
    aside(BOX, "FLOPPY01_c.dsk.failed", 14);
    aside(BOX, "CD01_b.failed/CD01_b.bin", 100);
    aside(BOX, "CD01_b.failed/CD01_b_disc.txt", 5);
    aside(BOX, "CD01_b.failed/dic-console.TXT", 7);
    aside(BOX, "CD01_c.failed2/CD01_c_disc.txt", 5);       /* only logs: nothing to take */
    aside(BOX, "FLOPPY02.dsk.failed", 15);                  /* marginal */
    aside(BOX, "FLOPPY04.dsk.failed", 15);                  /* fragile */
    aside(BOX, "CD02.failed/CD02.bin", 16);                 /* read once */
    /* never */
    aside(BOX, "FLOPPY01.dsk.damaged", 9);
    aside(BOX, "FLOPPY01.dsk.replaced", 9);
    aside(BOX, "FLOPPY01.dsk.rejected", 9);
    aside(BOX, "FLOPPY01.dsk.removed", 9);
    aside(BOX, "FLOPPY01.dsk.unused", 9);
    aside(BOX, "previous-2026-11-01T110000/metadata.json", 9);
    aside(BOX, "notes.txt", 9);
    /* a stem that is no medium of this box */
    aside(BOX, "CD10.failed", 9);
    aside(BOX, "FLOPPY01x.dsk.failed", 9);
    aside(BOX, "OTHER.dsk.failed", 9);

    CHECK(boxclean_plan(BOX, &p) == 0);
    CHECK(p.n == 7);
    CHECK(p.bytes == 10 + 11 + 20 + 12 + 53 + 14 + 100);
    CHECK(p.kept == 16);
    CHECK(p.kept_bytes == 13 + 13 + 5 + 15 + 15 + 16 + 9 * 10);
    CHECK((it = item(&p, "FLOPPY01.dsk.identical")) != NULL && it->bytes == 10 && !it->partial);
    CHECK(item(&p, "FLOPPY01_b.dsk.identical2") != NULL);
    CHECK((it = item(&p, "CD02_b.identical")) != NULL && it->bytes == 20 && !it->partial);
    CHECK(item(&p, "FLOPPY01.dsk.duplicate") != NULL);
    CHECK((it = item(&p, "CD01.duplicate")) != NULL && it->bytes == 53 && !it->partial);
    CHECK(item(&p, "FLOPPY01_c.dsk.failed") != NULL);
    CHECK((it = item(&p, "CD01_b.failed")) != NULL && it->bytes == 100 && it->partial);
    /* what the Archive list shows is the same number */
    a = (arc_box *)calloc(1, sizeof(arc_box));
    CHECK(a && arc_load(BOX, a) && a->reclaim == p.bytes);
    free(a);

    /* one entry cannot be removed: the others go, and the caller knows how many did not */
    CHECK(boxclean_run(BOX, &p, no_identical, &refused) == 3 && refused == 3);
    boxclean_free(&p);
    CHECK(there(BOX, "FLOPPY01.dsk.identical") && there(BOX, "CD02_b.identical/CD02_b.bin"));
    CHECK(!there(BOX, "FLOPPY01.dsk.duplicate") && !there(BOX, "CD01.duplicate") && !there(BOX, "FLOPPY01_c.dsk.failed"));
    CHECK(boxclean_plan(BOX, &p) == 0 && p.n == 3 && p.bytes == 41);
    CHECK(boxclean_run(BOX, &p, boxclean_rm_delete, NULL) == 0);
    boxclean_free(&p);
    CHECK(!there(BOX, "FLOPPY01.dsk.identical") && !there(BOX, "FLOPPY01_b.dsk.identical2") && !there(BOX, "CD02_b.identical"));
    /* of the failed CD read only the logs are left */
    CHECK(!there(BOX, "CD01_b.failed/CD01_b.bin"));
    CHECK(there(BOX, "CD01_b.failed/CD01_b_disc.txt") && there(BOX, "CD01_b.failed/dic-console.TXT"));
    for (i = 0; stay[i]; i++) CHECK(there(BOX, stay[i]));
    /* nothing left to take, and the box is as valid as before */
    CHECK(boxclean_plan(BOX, &p) == 0 && p.n == 0 && p.bytes == 0 && p.kept == 17);
    boxclean_free(&p);
    sb_init(&err);
    CHECK(arc_verify(BOX, &c, &err) == 0 && c.changed == 0 && c.lost == 0 && c.extra == 0 && c.files > 6);
    sb_free(&err);
}

static void the_rest(void)
{
    static const char *const good[] = { ST_CLEAN, ST_REDUMP };
    static const char *const once[] = { ST_CLEAN, ST_READ_ONCE };
    clean_plan p; meta_box *m; strbuf err;

    /* the only entry goes: so does the empty folder */
    make_box(CROOT "/Pulito", "Pulito", 1, 1, good, 0);
    aside(CROOT "/Pulito", "CD01.failed/CD01.bin", 30);
    CHECK(boxclean_plan(CROOT "/Pulito", &p) == 0 && p.n == 1 && !p.item[0].partial && p.kept == 0);
    CHECK(boxclean_run(CROOT "/Pulito", &p, boxclean_rm_delete, NULL) == 0);
    boxclean_free(&p);
    CHECK(!u8_exists(CROOT "/Pulito/" SESSION_ASIDE_DIR));
    /* no set-aside at all: an empty plan, not an error */
    CHECK(boxclean_plan(CROOT "/Pulito", &p) == 0 && p.n == 0 && p.kept == 0);
    boxclean_free(&p);

    /* "ended well" is clean or verified by redump, nothing else */
    m = meta_new(); sb_init(&err);
    CHECK(meta_load(m, CROOT "/Pulito/data", &err) == 0 && boxclean_all_well(m));
    make_box(CROOT "/Una volta", "Una volta", 1, 1, once, 0);
    CHECK(meta_load(m, CROOT "/Una volta/data", &err) == 0 && !boxclean_all_well(m));
    CHECK(meta_load(m, BOX "/data", &err) == 0 && !boxclean_all_well(m));
    sb_free(&err); meta_delete(m);

    /* not a box Free space works on: open, being edited, of 261017, not a box */
    make_box(CROOT "/Aperto", "Aperto", 1, 1, good, 1);
    aside(CROOT "/Aperto", "CD01.failed/CD01.bin", 30);
    CHECK(boxclean_plan(CROOT "/Aperto", &p) == -1 && p.n == 0);
    make_box(CROOT "/In modifica", "In modifica", 1, 1, good, 0);
    aside(CROOT "/In modifica", "CD01.failed/CD01.bin", 30);
    u8_mkdir(CROOT "/In modifica/" BAG_EDIT_DIR);
    CHECK(boxclean_plan(CROOT "/In modifica", &p) == -1);
    make_box(CROOT "/Vecchio", "Vecchio", 1, 1, good, 0);
    aside(CROOT "/Vecchio", "CD01.failed/CD01.bin", 30);
    u8_mkdir(CROOT "/Vecchio/" SESSION_ASIDE_DIR_OLD);
    CHECK(boxclean_plan(CROOT "/Vecchio", &p) == -1);
    CHECK(boxclean_plan(CROOT "/non-esiste", &p) == -1);
    CHECK(there(CROOT "/Aperto", "CD01.failed/CD01.bin") && there(CROOT "/Vecchio", "CD01.failed/CD01.bin"));
}

void test_boxclean(void)
{
    the_table();
    the_rest();
}
