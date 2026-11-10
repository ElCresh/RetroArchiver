#include <stdlib.h>
#include "unit.h"
#include "core/archive.h"
#include "core/boxedit.h"
#include "core/boximg.h"
#include "core/boxtxn.h"
#include "core/fsu8.h"
#include "core/messages.h"
#include "core/sha256.h"

#define IROOT "tmp_unit/img"
#define ISRC  "tmp_unit/img_src"

static void put(const char *path, const char *text)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static int contains(const char *path, const char *needle)
{
    static char buf[131072]; FILE *f = u8_fopen(path, "rb"); size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, needle) != NULL;
}

/* a closed box in IROOT/<folder> with nf floppies and nc CDs; returns its path */
static const char *make_box(const char *folder, int nf, int nc)
{
    static char bag[300];
    meta_box *b = meta_new(); strbuf err; int i, bad; char p[600];
    sb_init(&err);
    snprintf(bag, sizeof(bag), IROOT "/%s", folder);
    u8_mkdirs(IROOT);
    CHECK(bag_create(bag, &err) == 0);
    str_copy(b->title, sizeof(b->title), folder);
    str_copy(b->platform, sizeof(b->platform), "MS-DOS");
    str_copy(b->started, sizeof(b->started), "2026-10-10T10:00:00");
    str_copy(b->closed, sizeof(b->closed), "2026-10-10T11:00:00");
    meta_set_counts(b, nf, nc, 0);
    str_copy(b->names[MEDIA_FLOPPY], sizeof(b->names[0]), "FLOPPY{nn}");
    str_copy(b->names[MEDIA_CD], sizeof(b->names[0]), "CD{nn}");
    for (i = 1; i <= nf + nc; i++) {
        meta_disk *d = meta_disk_get(b, i);
        str_copy(d->status, sizeof(d->status), "clean");
        if (d->kind == MEDIA_FLOPPY) snprintf(d->master, sizeof(d->master), "FLOPPY%02d.dsk", d->num);
        else {
            snprintf(d->master, sizeof(d->master), "CD%02d", d->num);
            snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
            u8_mkdirs(p);
        }
        d->nreads = 1;
        str_copy(d->reads[0].file, sizeof(d->reads[0].file), d->master);
        snprintf(p, sizeof(p), d->kind == MEDIA_FLOPPY ? "%s/data/%s" : "%s/data/%s/x.bin", bag, d->master);
        put(p, d->master);
        snprintf(p, sizeof(p), "%s/data/%s", bag, d->master);
        sha256_path(p, d->reads[0].sha256, &d->reads[0].bytes);
    }
    snprintf(p, sizeof(p), "%s/data", bag);
    CHECK(meta_save(b, p, NULL, NULL, &err) == 0);
    CHECK(bag_close(bag, b, NULL, &err, &bad) == 0);
    sb_free(&err);
    meta_delete(b);
    return bag;
}

static int verified(const char *bag)
{
    arc_check c; strbuf err; int ok;
    sb_init(&err);
    ok = arc_verify(bag, &c, &err) == 0 && !c.changed && !c.lost && !c.extra && !bag_is_open(bag);
    sb_free(&err);
    return ok;
}

static int no_editing(const char *bag)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/editing", bag);
    return !u8_exists(p);
}

static meta_box *load(const char *bag)
{
    meta_box *b = meta_new(); strbuf err; char p[400];
    sb_init(&err);
    snprintf(p, sizeof(p), "%s/data", bag);
    CHECK(meta_load(b, p, &err) == 0);
    sb_free(&err);
    return b;
}

static meta_image what(const char *w)
{
    meta_image im;
    CHECK(boximg_parse_what(w, &im) == 0);
    return im;
}

/* a source file with that content; its path */
static const char *src(const char *name, const char *content)
{
    static char p[8][300]; static int k;
    char *out = p[k++ % 8];
    u8_mkdirs(ISRC);
    snprintf(out, 300, ISRC "/%s", name);
    put(out, content);
    return out;
}

/* adds source as w to the box and saves; the save's result */
static int attach(const char *bag, const char *source, const char *w, img_result *res, strbuf *err)
{
    meta_box *b = load(bag); img_list l; meta_image im = what(w); int rc;
    boximg_list_load(b, &l);
    rc = boximg_list_add(&l, b, source, &im, err);
    if (rc == 0) rc = boximg_save(bag, &l, "scanned today", NULL, res, err);
    boximg_list_free(&l);
    meta_delete(b);
    return rc;
}

static int exists_in(const char *bag, const char *rel)
{
    char p[500];
    snprintf(p, sizeof(p), "%s/%s", bag, rel);
    return u8_exists(p);
}

#define TAKEN(t) ((const char (*)[64])(t))

static void names(void)
{
    char taken[4][64], out[64]; meta_image im;
    memset(taken, 0, sizeof(taken));
    im = what("box-front");
    boximg_name(&im, "Fronte Scatola.JPG", TAKEN(taken), 0, out, sizeof(out));
    CHECK_STR(out, "box-front.jpg");
    strcpy(taken[0], "box-front.jpg");
    boximg_name(&im, "x.TIF", TAKEN(taken), 1, out, sizeof(out));
    CHECK_STR(out, "box-front-2.tif");
    strcpy(taken[1], "BOX-FRONT-2.tif");
    boximg_name(&im, "y.jpeg", TAKEN(taken), 2, out, sizeof(out));
    CHECK_STR(out, "box-front-3.jpeg");
    im = what("cd2-front");
    boximg_name(&im, "a.png", TAKEN(taken), 2, out, sizeof(out));
    CHECK_STR(out, "cd02-front.png");
    im = what("floppy3-front");
    boximg_name(&im, "a.jpg", TAKEN(taken), 2, out, sizeof(out));
    CHECK_STR(out, "floppy03-front.jpg");
    im = what("insert");
    boximg_name(&im, "manual.tif", TAKEN(taken), 2, out, sizeof(out));
    CHECK_STR(out, "insert-01.tif");
    strcpy(taken[2], "insert-01.tif");
    boximg_name(&im, "manual.pdf", TAKEN(taken), 3, out, sizeof(out));
    CHECK_STR(out, "insert-02.pdf");

    im = what("box-front");
    strcpy(im.file, "box-front-2.jpg");
    CHECK(boximg_same_base(&im));
    im.side = IMG_BACK;
    CHECK(!boximg_same_base(&im));
    im = what("insert");
    strcpy(im.file, "insert-03.tif");
    CHECK(boximg_same_base(&im));
    strcpy(im.file, "insert.tif");
    CHECK(!boximg_same_base(&im));

    CHECK(boximg_file_type("a.pdf") == 2 && boximg_file_type("a.exe") == 0 && boximg_file_type("A.JPEG") == 1);
    CHECK(boximg_file_type("noext") == 0 && boximg_file_type("x.y/scan.TIFF") == 1);
}

static void parse_guess(void)
{
    static const char *const good[] = { "box-front", "box-back", "box-spine", "box-top", "box-bottom", "box-other",
                                        "cd2-front", "floppy3-back", "dvd1-front", "insert", "case-front", "case-back",
                                        "case-spine", "case-other" };
    static const char *const bad[] = { "cd0-front", "box", "case", "case-", "case-left", "insert-front", "cd2", "cd2-spine", "tape1-front", "" };
    meta_box *b = meta_new(); meta_image im; strbuf err; size_t i; char t[64];
    sb_init(&err);
    meta_set_counts(b, 3, 2, 0);
    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK(boximg_parse_what(good[i], &im) == 0);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(boximg_parse_what(bad[i], &im) == -1);
    im = what("floppy3-back");
    CHECK(im.kind == IMG_MEDIUM && im.mkind == MEDIA_FLOPPY && im.mnum == 3 && im.side == IMG_BACK);
    boximg_what_text(&im, t, sizeof(t));
    CHECK_STR(t, "floppy 3 back");

    CHECK(boximg_guess("front.jpg", b, &im) == 1 && im.kind == IMG_BOX && im.side == IMG_FRONT);
    CHECK(boximg_guess("Retro.png", b, &im) == 1 && im.kind == IMG_BOX && im.side == IMG_BACK);
    CHECK(boximg_guess("C:\\scan\\fronte scatola.jpg", b, &im) == 1 && im.kind == IMG_BOX && im.side == IMG_FRONT);
    CHECK(boximg_guess("cd2.tif", b, &im) == 1 && im.kind == IMG_MEDIUM && im.mkind == MEDIA_CD && im.mnum == 2
          && im.side == IMG_FRONT);
    CHECK(boximg_guess("CD 1 retro.jpg", b, &im) == 1 && im.mnum == 1 && im.side == IMG_BACK);
    CHECK(boximg_guess("floppy_03.jpg", b, &im) == 1 && im.mkind == MEDIA_FLOPPY && im.mnum == 3);
    CHECK(boximg_guess("cd3.tif", b, &im) == 0 && im.kind == IMG_INSERT);      /* the box has 2 CDs */
    CHECK(boximg_guess("scan0001.jpg", b, &im) == 0 && im.kind == IMG_INSERT);
    CHECK(boximg_guess("manuale.pdf", b, &im) == 1 && im.kind == IMG_INSERT);
    CHECK(boximg_guess("abcd1.jpg", b, &im) == 0);
    /* the jewel case is not the big box, and its words win over "cd1" */
    CHECK(boximg_guess("jewel case front.jpg", b, &im) == 1 && im.kind == IMG_CASE && im.side == IMG_FRONT);
    CHECK(boximg_guess("custodia cd1 retro.jpg", b, &im) == 1 && im.kind == IMG_CASE && im.side == IMG_BACK);
    CHECK(boximg_guess("inlay.png", b, &im) == 1 && im.kind == IMG_CASE && im.side == IMG_BACK);
    CHECK(boximg_guess("Custodia.tif", b, &im) == 1 && im.kind == IMG_CASE && im.side == IMG_FRONT);
    im = what("case-spine");
    CHECK(im.kind == IMG_CASE && im.side == IMG_SPINE);
    boximg_what_text(&im, t, sizeof(t));
    CHECK_STR(t, "case spine");
    boximg_base(&im, t, sizeof(t));
    CHECK_STR(t, "case-spine");
    CHECK(boximg_valid(b, &im, "x.jpg", &err) == 0);
    CHECK(strcmp(meta_img_kind_key(IMG_CASE), "case") == 0 && meta_img_kind_parse("case") == IMG_CASE);

    im = what("box-front");
    CHECK(boximg_valid(b, &im, "a.pdf", &err) == -1 && strstr(err.s, "PDF"));
    sb_clear(&err);
    CHECK(boximg_valid(b, &im, "a.exe", &err) == -1 && strstr(err.s, "JPEG"));
    sb_clear(&err);
    im = what("cd3-front");
    CHECK(boximg_valid(b, &im, "a.jpg", &err) == -1 && strstr(err.s, "CD 3"));
    im = what("cd2-front");
    CHECK(boximg_valid(b, &im, "a.jpg", &err) == 0);
    im = what("insert");
    CHECK(boximg_valid(b, &im, "a.pdf", &err) == 0);
    sb_free(&err);
    meta_delete(b);
}

static void order(void)
{
    static const char *const w[] = { "insert", "cd1-back", "box-back", "floppy1-front", "box-front", "box-front" };
    meta_box *b = meta_new(); int idx[META_MAXIMG], i;
    meta_set_counts(b, 1, 1, 0);
    for (i = 0; i < 6; i++) b->image[b->nimage++] = what(w[i]);
    boximg_order(b, idx);
    CHECK(idx[0] == 4 && idx[1] == 5 && idx[2] == 2 && idx[3] == 3 && idx[4] == 1 && idx[5] == 0);
    meta_delete(b);
}

static void meta_roundtrip(void)
{
    meta_box *b = meta_new(), *c = meta_new(); strbuf err; meta_image *im;
    sb_init(&err);
    u8_mkdirs("tmp_unit/img_meta");
    str_copy(b->title, sizeof(b->title), "T");
    str_copy(b->platform, sizeof(b->platform), "MS-DOS");
    meta_set_counts(b, 1, 2, 0);
    CHECK(meta_save(b, "tmp_unit/img_meta", NULL, NULL, &err) == 0);
    CHECK(!contains("tmp_unit/img_meta/metadata.json", "\"images\""));
    CHECK(contains("tmp_unit/img_meta/metadata.json", "\"schema\": \"retroarchiver-3\""));
    CHECK(meta_load(c, "tmp_unit/img_meta", &err) == 0 && c->nimage == 0);

    im = &b->image[b->nimage++];
    *im = what("box-front");
    strcpy(im->file, "box-front.jpg"); strcpy(im->sha256, "aa"); im->bytes = 7;
    strcpy(im->original, "Fronte \xc3\xa8.JPG"); strcpy(im->at, "2026-11-10T10:00:00");
    im = &b->image[b->nimage++];
    *im = what("cd2-back");
    strcpy(im->file, "cd02-back.png"); strcpy(im->note, "ring code visible");
    im = &b->image[b->nimage++];
    *im = what("insert");
    strcpy(im->file, "insert-01.pdf");
    CHECK(meta_save(b, "tmp_unit/img_meta", NULL, NULL, &err) == 0);
    CHECK(contains("tmp_unit/img_meta/metadata.json", "\"file\": \"images/box-front.jpg\""));
    CHECK(contains("tmp_unit/img_meta/metadata.json", "\"schema\": \"retroarchiver-4\""));
    CHECK(meta_load(c, "tmp_unit/img_meta", &err) == 0 && c->nimage == 3);
    CHECK_STR(c->schema, "retroarchiver-4");
    CHECK(meta_schema_current("retroarchiver-4") && meta_schema_current("retroarchiver-3")
          && !meta_schema_current("retroarchiver-2") && !meta_schema_current("retroarchiver-5"));
    CHECK_STR(c->image[0].file, "box-front.jpg");
    CHECK(c->image[0].kind == IMG_BOX && c->image[0].side == IMG_FRONT && c->image[0].bytes == 7);
    CHECK_STR(c->image[0].original, "Fronte \xc3\xa8.JPG");
    CHECK_STR(c->image[0].at, "2026-11-10T10:00:00");
    CHECK_STR(c->image[0].sha256, "aa");
    CHECK(c->image[1].kind == IMG_MEDIUM && c->image[1].mkind == MEDIA_CD && c->image[1].mnum == 2
          && c->image[1].side == IMG_BACK);
    CHECK_STR(c->image[1].note, "ring code visible");
    CHECK(c->image[2].kind == IMG_INSERT);

    /* an entry of a kind this version does not know, or outside images/, is skipped */
    put("tmp_unit/img_meta/metadata.json",
        "{\"schema\": \"retroarchiver-3\", \"box\": {\"title\": \"T\", \"platform\": \"P\", \"media_expected\": 1,"
        " \"media\": [{\"type\": \"floppy\", \"count\": 1}]}, \"media\": [],"
        " \"images\": [{\"file\": \"images/a.jpg\", \"kind\": \"poster\"},"
        " {\"file\": \"../a.jpg\", \"kind\": \"insert\"},"
        " {\"file\": \"images/insert-01.jpg\", \"kind\": \"insert\"}]}");
    CHECK(meta_load(c, "tmp_unit/img_meta", &err) == 0 && c->nimage == 1);
    CHECK_STR(c->image[0].file, "insert-01.jpg");
    sb_free(&err);
    meta_delete(b); meta_delete(c);
}

static void add_three(void)
{
    const char *bag = make_box("Add", 1, 2); img_result res; strbuf err; meta_box *b; arc_box *a;
    char hex[65]; long long n;
    sb_init(&err);
    CHECK(attach(bag, src("Fronte Scatola.JPG", "jpeg bytes"), "box-front", &res, &err) == 0);
    CHECK(res.added == 1 && res.skipped == 0);
    CHECK(attach(bag, src("cd2.png", "png bytes"), "cd2-front", &res, &err) == 0);
    CHECK(attach(bag, src("manual.tif", "tiff bytes"), "insert", &res, &err) == 0);
    CHECK(verified(bag) && no_editing(bag));
    CHECK(contains(IROOT "/Add/manifest-sha256.txt", "data/images/box-front.jpg"));
    CHECK(contains(IROOT "/Add/manifest-sha256.txt", "data/images/cd02-front.png"));
    CHECK(contains(IROOT "/Add/manifest-sha256.txt", "data/images/insert-01.tif"));
    CHECK(contains(IROOT "/Add/data/metadata.json", "\"action\": \"images\""));
    CHECK(contains(IROOT "/Add/data/metadata.json", "\"reason\": \"scanned today\""));
    CHECK(contains(IROOT "/Add/data/metadata.json", "\"change\": \"added\""));
    CHECK(sha256_file(IROOT "/Add/data/images/box-front.jpg", hex, &n) == 0 && n == 10);
    b = load(bag);
    CHECK(b->nimage == 3);
    CHECK_STR(b->image[0].sha256, hex);
    CHECK_STR(b->image[0].original, "Fronte Scatola.JPG");
    CHECK(b->image[0].bytes == 10 && b->image[0].at[0]);
    meta_delete(b);
    a = (arc_box *)calloc(1, sizeof(arc_box));
    CHECK(arc_load(bag, a) && a->images == 3 && a->state == ARC_COMPLETE);
    /* retroarchiver-4 is this version's: not old, not to convert, Edit works on it */
    CHECK(!a->old_format && !boxedit_is_old(bag) && boxedit_editable(bag, &err));
    CHECK(contains(IROOT "/Add/data/metadata.json", "\"schema\": \"retroarchiver-4\""));
    CHECK(boxedit_convert(bag, NULL, &err) == 1);
    sb_clear(&err);
    free(a);

    /* the same bytes again: not attached twice, and it says which one it is */
    CHECK(attach(bag, src("copy.jpg", "jpeg bytes"), "box-back", &res, &err) == 0);
    CHECK(res.added == 0 && res.skipped == 1);
    CHECK_STR(res.skipped_names, "copy.jpg = box-front.jpg");
    CHECK(!exists_in(bag, "data/images/box-back.jpg") && no_editing(bag) && verified(bag));
    /* a second front takes -2 */
    CHECK(attach(bag, src("front2.tif", "other front"), "box-front", &res, &err) == 0);
    CHECK(exists_in(bag, "data/images/box-front-2.tif") && verified(bag));
    sb_free(&err);
}

static void remove_relabel(void)
{
    const char *bag = make_box("Rem", 1, 1); img_result res; strbuf err; meta_box *b; img_list l;
    img_change ch[8]; int n;
    sb_init(&err);
    CHECK(attach(bag, src("a.jpg", "front"), "box-front", &res, &err) == 0);
    CHECK(attach(bag, src("b.jpg", "back"), "box-back", &res, &err) == 0);
    CHECK(attach(bag, src("c.jpg", "cd"), "cd1-front", &res, &err) == 0);

    /* front and back swap what they show: the two files swap their names */
    b = load(bag);
    boximg_list_load(b, &l);
    l.item[0].im.side = IMG_BACK;
    l.item[1].im.side = IMG_FRONT;
    n = boximg_diff(b, &l, ch, 8);
    CHECK(n == 2 && ch[0].what == IMGCH_RELABELED && ch[1].what == IMGCH_RELABELED);
    CHECK(boximg_save(bag, &l, NULL, NULL, &res, &err) == 0 && res.relabeled == 2);
    boximg_list_free(&l); meta_delete(b);
    CHECK(verified(bag) && no_editing(bag));
    CHECK(contains(IROOT "/Rem/data/images/box-back.jpg", "front"));
    CHECK(contains(IROOT "/Rem/data/images/box-front.jpg", "back"));
    CHECK(contains(IROOT "/Rem/data/metadata.json", "\"change\": \"relabeled\""));
    CHECK(contains(IROOT "/Rem/data/metadata.json", "\"from\": \"images/box-front.jpg\""));
    b = load(bag);
    CHECK_STR(b->image[0].file, "box-back.jpg");
    CHECK(b->image[0].side == IMG_BACK);

    /* remove the CD's image: it goes to set-aside, nothing is deleted */
    boximg_list_load(b, &l);
    boximg_list_remove(&l, 2);
    n = boximg_diff(b, &l, ch, 8);
    CHECK(n == 1 && ch[0].what == IMGCH_REMOVED);
    CHECK_STR(ch[0].from.file, "cd01-front.jpg");
    CHECK(boximg_save(bag, &l, "wrong scan", NULL, &res, &err) == 0 && res.removed == 1);
    boximg_list_free(&l); meta_delete(b);
    CHECK(verified(bag) && no_editing(bag));
    CHECK(!exists_in(bag, "data/images/cd01-front.jpg"));
    CHECK(contains(IROOT "/Rem/set-aside/cd01-front.jpg.removed", "cd"));
    CHECK(!contains(IROOT "/Rem/manifest-sha256.txt", "cd01-front"));
    CHECK(contains(IROOT "/Rem/data/metadata.json", "\"set_aside\": \"set-aside/cd01-front.jpg.removed\""));

    /* only the order, then only a description */
    b = load(bag);
    boximg_list_load(b, &l);
    CHECK(boximg_list_move(&l, 0, 1) == 1 && boximg_list_move(&l, 1, 1) == 1);
    n = boximg_diff(b, &l, ch, 8);
    CHECK(n == 1 && ch[0].what == IMGCH_REORDERED);
    CHECK(boximg_save(bag, &l, NULL, NULL, &res, &err) == 0);
    boximg_list_free(&l); meta_delete(b);
    CHECK(contains(IROOT "/Rem/data/metadata.json", "\"reordered\": true"));
    b = load(bag);
    CHECK_STR(b->image[0].file, "box-front.jpg");
    boximg_list_load(b, &l);
    str_copy(l.item[0].im.note, sizeof(l.item[0].im.note), "with the price tag");
    CHECK(boximg_save(bag, &l, NULL, NULL, &res, &err) == 0 && res.relabeled == 1);
    boximg_list_free(&l); meta_delete(b);
    b = load(bag);
    CHECK_STR(b->image[0].note, "with the price tag");
    CHECK_STR(b->image[0].file, "box-front.jpg");
    /* nothing to change: nothing is written */
    boximg_list_load(b, &l);
    CHECK(boximg_diff(b, &l, ch, 8) == 0);
    CHECK(boximg_save(bag, &l, NULL, NULL, &res, &err) == 0);
    boximg_list_free(&l); meta_delete(b);

    /* the last images go: data/images goes with them */
    b = load(bag);
    boximg_list_load(b, &l);
    boximg_list_remove(&l, 0); boximg_list_remove(&l, 0);
    CHECK(boximg_save(bag, &l, NULL, NULL, &res, &err) == 0 && res.removed == 2);
    boximg_list_free(&l); meta_delete(b);
    CHECK(verified(bag) && !exists_in(bag, "data/images"));
    b = load(bag);
    CHECK(b->nimage == 0);
    CHECK_STR(b->schema, "retroarchiver-3");
    meta_delete(b);
    CHECK(!contains(IROOT "/Rem/data/metadata.json", "\"original_name\": \"a.jpg\",\n      \"added\""));
    sb_free(&err);
}

static int stop_at;
static int progress_stop(void *ctx, int k, int n, const char *name, long long done, long long total)
{
    int *calls = (int *)ctx;
    (void)n; (void)name; (void)done; (void)total;
    ++*calls;
    return k >= stop_at;
}

static void failures(void)
{
    const char *bag = make_box("Fail", 1, 0); img_result res; strbuf err; meta_box *b; img_list l;
    meta_image im; img_host host; int calls = 0; char p[400];
    sb_init(&err);
    CHECK(attach(bag, src("a.jpg", "front"), "box-front", &res, &err) == 0);

    /* the source is gone when saving (the NAS went offline) */
    b = load(bag);
    boximg_list_load(b, &l);
    im = what("box-back");
    CHECK(boximg_list_add(&l, b, src("gone.jpg", "x"), &im, &err) == 0);
    u8_remove(ISRC "/gone.jpg");
    CHECK(boximg_save(bag, &l, NULL, NULL, &res, &err) == -1);
    CHECK(err.s && strstr(err.s, "gone.jpg") && strstr(err.s, "not changed"));
    CHECK(verified(bag) && no_editing(bag) && res.added == 0);
    boximg_list_free(&l);
    sb_clear(&err);

    /* stopped while the second file is copied */
    boximg_list_load(b, &l);
    CHECK(boximg_list_add(&l, b, src("b.jpg", "back"), &im, &err) == 0);
    im = what("insert");
    CHECK(boximg_list_add(&l, b, src("c.jpg", "insert"), &im, &err) == 0);
    host.ctx = &calls; host.progress = progress_stop;
    stop_at = 2;
    CHECK(boximg_save(bag, &l, NULL, &host, &res, &err) == 1);
    CHECK(calls > 1 && verified(bag) && no_editing(bag));
    CHECK(!exists_in(bag, "data/images/box-back.jpg"));
    stop_at = 99;
    CHECK(boximg_save(bag, &l, NULL, &host, &res, &err) == 0 && res.added == 2);
    CHECK(verified(bag));
    boximg_list_free(&l);
    meta_delete(b);

    /* a PDF only as an insert; a type that is not accepted */
    CHECK(attach(bag, src("m.pdf", "pdf"), "box-other", &res, &err) == -1);
    sb_clear(&err);
    CHECK(attach(bag, src("m.pdf", "pdf"), "insert", &res, &err) == 0);
    CHECK(exists_in(bag, "data/images/insert-02.pdf"));
    CHECK(attach(bag, src("m.exe", "exe"), "insert", &res, &err) == -1);
    sb_clear(&err);

    /* an open box has no images */
    snprintf(p, sizeof(p), "%s/" BAG_OPEN_MARK, bag);
    put(p, "open");
    b = meta_new();
    meta_set_counts(b, 1, 0, 0);
    boximg_list_load(b, &l);
    im = what("box-top");
    CHECK(boximg_list_add(&l, b, src("t.jpg", "top"), &im, &err) == 0);
    CHECK(boximg_save(bag, &l, NULL, NULL, &res, &err) == -1 && strstr(err.s, "still open"));
    u8_remove(p);
    boximg_list_free(&l);
    meta_delete(b);
    CHECK(verified(bag));
    sb_free(&err);
}

/* the power goes during the transaction: the recovery finishes it */
static void crash_recover(void)
{
    static const int steps[] = { 5, 7, 2 };
    int k;
    for (k = 0; k < 3; k++) {
        char folder[16], fin[400], p[400]; const char *bag; img_result res; strbuf err, log; int rc;
        snprintf(folder, sizeof(folder), "Crash%d", steps[k]);
        bag = make_box(folder, 1, 0);
        sb_init(&err); sb_init(&log);
        CHECK(attach(bag, src("a.jpg", "front"), "box-front", &res, &err) == 0);
        boxtxn_test_crash(steps[k]);
        CHECK(attach(bag, src("b.jpg", "back"), "box-back", &res, &err) == -1);
        boxtxn_test_crash(0);
        rc = boxtxn_recover(bag, boxedit_tech, NULL, fin, sizeof(fin), &log);
        snprintf(p, sizeof(p), "%s/data/images/box-back.jpg", bag);
        if (steps[k] == 2) CHECK(rc == BOXTXN_CLEANED && !u8_exists(p));   /* no journal yet: as it was */
        else CHECK(rc == BOXTXN_FORWARD && u8_exists(p));
        CHECK(verified(bag) && no_editing(bag));
        sb_free(&err); sb_free(&log);
    }
}

/* Edit removes CD 2: its images go to set-aside with it */
static void medium_removed(void)
{
    const char *bag = make_box("Med", 1, 2); img_result res; strbuf err; meta_box *old, *ed; edit_change ch[8];
    char nd[600]; int n;
    sb_init(&err);
    CHECK(attach(bag, src("a.jpg", "cd1"), "cd1-front", &res, &err) == 0);
    CHECK(attach(bag, src("b.jpg", "cd2 front"), "cd2-front", &res, &err) == 0);
    CHECK(attach(bag, src("c.jpg", "cd2 back"), "cd2-back", &res, &err) == 0);
    old = load(bag); ed = load(bag);
    meta_set_counts(ed, 1, 1, 0);
    n = boxedit_diff(bag, old, ed, ch, 8);
    CHECK(n == 1 && ch[0].what == CH_REMOVED && ch[0].kind == MEDIA_CD && ch[0].num == 2 && ch[0].images == 2);
    CHECK(boxedit_save_data(bag, ed, "sold", nd, sizeof(nd), &err) == 0);
    meta_delete(old); meta_delete(ed);
    CHECK(verified(bag) && no_editing(bag));
    CHECK(exists_in(bag, "set-aside/cd02-front.jpg.removed") && exists_in(bag, "set-aside/cd02-back.jpg.removed"));
    CHECK(exists_in(bag, "data/images/cd01-front.jpg") && !exists_in(bag, "data/images/cd02-front.jpg"));
    CHECK(contains(IROOT "/Med/data/metadata.json", "\"images_set_aside\""));
    old = load(bag);
    CHECK(old->nimage == 1);
    CHECK_STR(old->image[0].file, "cd01-front.jpg");
    meta_delete(old);
    sb_free(&err);
}

/* bag_close checks the images against the fingerprint taken when attached */
static void close_checks(void)
{
    const char *bag = make_box("Chk", 1, 0); img_result res; strbuf err; meta_box *b; int bad;
    sb_init(&err);
    CHECK(attach(bag, src("a.jpg", "front"), "box-front", &res, &err) == 0);
    b = load(bag);
    put(IROOT "/Chk/data/images/box-front.jpg", "tampered");
    CHECK(bag_close(bag, b, NULL, &err, &bad) == -1 && bad == 0 && strstr(err.s, "changed after it was attached"));
    sb_clear(&err);
    u8_remove(IROOT "/Chk/data/images/box-front.jpg");
    CHECK(bag_close(bag, b, NULL, &err, &bad) == -1 && strstr(err.s, "is missing"));
    meta_delete(b);
    sb_free(&err);
}

void test_boximg(void)
{
    names();
    parse_guess();
    order();
    meta_roundtrip();
    add_three();
    remove_relabel();
    failures();
    crash_recover();
    medium_removed();
    close_checks();
}
