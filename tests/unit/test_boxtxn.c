#include <stdlib.h>
#include "unit.h"
#include "core/archive.h"
#include "core/boxtxn.h"
#include "core/fsu8.h"
#include "core/sha256.h"

#define TROOT "tmp_unit/txn"

static void put(const char *path, const char *text)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static char *slurp(const char *path)
{
    FILE *f = u8_fopen(path, "rb"); long n; char *s;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    s = (char *)malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static void read_of(meta_disk *d, const char *bag, const char *file, const char *text)
{
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", bag, file);
    put(p, text);
    d->nreads = 1;
    str_copy(d->status, sizeof(d->status), "clean");
    str_copy(d->master, sizeof(d->master), strrchr(file, '/') + 1);
    str_copy(d->reads[0].file, sizeof(d->reads[0].file), d->master);
    str_copy(d->reads[0].at, sizeof(d->reads[0].at), "2026-10-12T10:00:00");
    sha256_file(p, d->reads[0].sha256, &d->reads[0].bytes);
}

/* a closed bag with A.dsk and B.dsk; the new read of A waits in editing/data.
 * old: the manifest as it was; *nb: the box after the change */
static void make_bag(const char *bag, char **old, meta_box **nb, txn_moves *m)
{
    meta_box *b = meta_new(); strbuf err; int bad; char p[600];
    sb_init(&err);
    u8_mkdirs(TROOT);
    CHECK(bag_create(bag, &err) == 0);
    str_copy(b->title, sizeof(b->title), "T");
    str_copy(b->platform, sizeof(b->platform), "X");
    b->media_expected = 2;
    read_of(meta_disk_get(b, 1), bag, "data/A.dsk", "old A");
    read_of(meta_disk_get(b, 2), bag, "data/B.dsk", "B stays");
    snprintf(p, sizeof(p), "%s/data", bag);
    CHECK(meta_save(b, p, NULL, NULL, &err) == 0);
    CHECK(bag_close(bag, b, NULL, &err, &bad) == 0);
    snprintf(p, sizeof(p), "%s/manifest-sha256.txt", bag);
    *old = slurp(p);
    snprintf(p, sizeof(p), "%s/" BOXTXN_STAGE, bag);
    u8_mkdirs(p);
    read_of(meta_disk_get(b, 1), bag, BOXTXN_STAGE "/A.dsk", "new A, better");
    memset(m, 0, sizeof(*m));
    txn_add(m, "data/A.dsk", "set-aside/A.dsk.replaced");
    txn_add(m, BOXTXN_STAGE "/A.dsk", "data/A.dsk");
    *nb = b;
    sb_free(&err);
}

/* the bag is either the old one or the new one, whole and valid */
static int which(const char *bag, const char *old)
{
    char p[600]; char *now; arc_check c; strbuf err; int r = 0;
    sb_init(&err);
    snprintf(p, sizeof(p), "%s/manifest-sha256.txt", bag);
    now = slurp(p);
    if (arc_verify(bag, &c, &err) == 0 && !c.changed && !c.lost && !c.extra && !bag_is_open(bag)) {
        if (now && !strcmp(now, old)) r = 1;            /* old */
        else if (now) r = 2;                              /* new */
    }
    snprintf(p, sizeof(p), "%s/" BOXTXN_DIR, bag);
    if (u8_exists(p)) r = 0;
    free(now);
    sb_free(&err);
    return r;
}

static void txn_commit_ok(void)
{
    char *old; meta_box *b; txn_moves m; strbuf err; char p[600];
    sb_init(&err);
    make_bag(TROOT "/ok", &old, &b, &m);
    CHECK(boxtxn_commit(TROOT "/ok", &m, b, NULL, NULL, NULL, NULL, &err) == 0);
    CHECK(which(TROOT "/ok", old) == 2);
    CHECK(u8_exists(TROOT "/ok/set-aside/A.dsk.replaced"));
    CHECK(!u8_exists(TROOT "/ok/" BAG_OPEN_MARK));
    {
        /* the four old tag files, kept */
        int found = 0;
        snprintf(p, sizeof(p), "%s", TROOT "/ok/set-aside");
        found = u8_isdir(p);
        CHECK(found);
    }
    txn_free(&m); meta_delete(b); free(old); sb_free(&err);
}

typedef struct { int n; char name[128]; } first_ctx;
static void first_prev(void *c, const char *name, int isdir)
{
    first_ctx *f = (first_ctx *)c;
    if (isdir && !strncmp(name, "previous-", 9)) { f->n++; str_copy(f->name, sizeof(f->name), name); }
}

static void txn_previous_kept(void)
{
    char *old; meta_box *b; txn_moves m; strbuf err; char p[600]; first_ctx f = { 0, "" };
    sb_init(&err);
    make_bag(TROOT "/prev", &old, &b, &m);
    CHECK(boxtxn_commit(TROOT "/prev", &m, b, NULL, NULL, NULL, NULL, &err) == 0);
    u8_listdir(TROOT "/prev/set-aside", first_prev, &f);
    CHECK(f.n == 1);
    snprintf(p, sizeof(p), TROOT "/prev/set-aside/%s/manifest-sha256.txt", f.name);
    {
        char *kept = slurp(p);
        CHECK(kept && !strcmp(kept, old));
        free(kept);
    }
    snprintf(p, sizeof(p), TROOT "/prev/set-aside/%s/metadata.json", f.name);
    CHECK(u8_exists(p));
    txn_free(&m); meta_delete(b); free(old); sb_free(&err);
}

static void txn_crash_each_step(void)
{
    int step; char bag[100], fin[600]; strbuf err, log;
    for (step = 1; step <= 9; step++) {
        char *old; meta_box *b; txn_moves m; int w, rc;
        sb_init(&err); sb_init(&log);
        snprintf(bag, sizeof(bag), TROOT "/crash%d", step);
        make_bag(bag, &old, &b, &m);
        boxtxn_test_crash(step);
        CHECK(boxtxn_commit(bag, &m, b, NULL, NULL, NULL, NULL, &err) == -2);
        boxtxn_test_crash(0);
        rc = boxtxn_recover(bag, NULL, NULL, fin, sizeof(fin), &log);
        CHECK(rc == BOXTXN_FORWARD || rc == BOXTXN_BACK || rc == BOXTXN_CLEANED);
        w = which(fin, old);
        if (!w) printf("  crash at step %d: recovered %d, %s\n", step, rc, log.s ? log.s : "");
        CHECK(w != 0);
        if (step >= 5) CHECK(w == 2);
        txn_free(&m); meta_delete(b); free(old); sb_free(&err); sb_free(&log);
    }
}

static void txn_fail_each_step(void)
{
    int step; char bag[100], p[600];
    for (step = 5; step <= 8; step++) {
        char *old; meta_box *b; txn_moves m; strbuf err;
        sb_init(&err);
        snprintf(bag, sizeof(bag), TROOT "/fail%d", step);
        make_bag(bag, &old, &b, &m);
        boxtxn_test_fail(step);
        CHECK(boxtxn_commit(bag, &m, b, NULL, NULL, NULL, step == 8 ? TROOT "/fail8-new" : NULL, &err) == -1);
        boxtxn_test_fail(0);
        CHECK(err.len > 0);
        /* the new read is still waiting: the caller decides what to do with it */
        snprintf(p, sizeof(p), "%s/" BOXTXN_STAGE "/A.dsk", bag);
        CHECK(u8_exists(p));
        snprintf(p, sizeof(p), "%s/" BOXTXN_DIR, bag);
        u8_rmtree(p);
        CHECK(which(bag, old) == 1);
        CHECK(!u8_exists(TROOT "/fail8-new"));
        txn_free(&m); meta_delete(b); free(old); sb_free(&err);
    }
}

/* a failure after the new metadata.json is written: the old one comes back, byte for byte */
static void txn_fail_restores_metadata(void)
{
    char *old, *meta_before, *meta_after, p[600]; meta_box *b; txn_moves m; strbuf err;
    sb_init(&err);
    make_bag(TROOT "/failmeta", &old, &b, &m);
    meta_before = slurp(TROOT "/failmeta/data/metadata.json");
    boxtxn_test_fail(6);
    CHECK(boxtxn_commit(TROOT "/failmeta", &m, b, NULL, NULL, NULL, NULL, &err) == -1);
    boxtxn_test_fail(0);
    meta_after = slurp(TROOT "/failmeta/data/metadata.json");
    CHECK(meta_before && meta_after && !strcmp(meta_before, meta_after));
    snprintf(p, sizeof(p), "%s/" BOXTXN_DIR, TROOT "/failmeta");
    u8_rmtree(p);
    CHECK(which(TROOT "/failmeta", old) == 1);
    free(meta_before); free(meta_after);
    txn_free(&m); meta_delete(b); free(old); sb_free(&err);
}

/* undoing a move fails: the journal stays, a later recovery finishes the job */
static void txn_undo_fails(void)
{
    char *old, fin[600]; meta_box *b; txn_moves m; strbuf err, log;
    sb_init(&err); sb_init(&log);
    make_bag(TROOT "/undo", &old, &b, &m);
    boxtxn_test_fail(7);
    boxtxn_test_fail_undo(1);
    CHECK(boxtxn_commit(TROOT "/undo", &m, b, NULL, NULL, NULL, NULL, &err) == -1);
    boxtxn_test_fail(0);
    boxtxn_test_fail_undo(0);
    CHECK(u8_exists(TROOT "/undo/" BOXTXN_DIR "/journal.txt"));
    CHECK(boxtxn_recover(TROOT "/undo", NULL, NULL, fin, sizeof(fin), &log) > 0);
    CHECK(which(fin, old) != 0);
    txn_free(&m); meta_delete(b); free(old); sb_free(&err); sb_free(&log);
}

/* power cut inside the cleanup, after the commit: never rolled back */
static void txn_crash_cleanup(void)
{
    int step; char bag[100], fin[600];
    for (step = 10; step <= 11; step++) {
        char *old; meta_box *b; txn_moves m; strbuf err, log;
        sb_init(&err); sb_init(&log);
        snprintf(bag, sizeof(bag), TROOT "/crclean%d", step);
        make_bag(bag, &old, &b, &m);
        boxtxn_test_crash(step);
        CHECK(boxtxn_commit(bag, &m, b, NULL, NULL, NULL, NULL, &err) == -2);
        boxtxn_test_crash(0);
        boxtxn_recover(bag, NULL, NULL, fin, sizeof(fin), &log);
        CHECK(which(fin, old) == 2);
        txn_free(&m); meta_delete(b); free(old); sb_free(&err); sb_free(&log);
    }
}

static void txn_rename(void)
{
    char *old; meta_box *b; txn_moves m; strbuf err;
    sb_init(&err);
    make_bag(TROOT "/ren", &old, &b, &m);
    CHECK(boxtxn_commit(TROOT "/ren", &m, b, NULL, NULL, NULL, TROOT "/ren2", &err) == 0);
    CHECK(!u8_exists(TROOT "/ren"));
    CHECK(which(TROOT "/ren2", old) == 2);
    txn_free(&m); meta_delete(b); free(old);

    /* the destination is another folder: nothing is touched */
    make_bag(TROOT "/ren3", &old, &b, &m);
    u8_mkdirs(TROOT "/ren4");
    sb_clear(&err);
    CHECK(boxtxn_commit(TROOT "/ren3", &m, b, NULL, NULL, NULL, TROOT "/ren4", &err) == -1);
    CHECK(strstr(err.s, "ren4") != NULL);
    CHECK(which(TROOT "/ren3", old) == 0);   /* editing/ still holds the new read */
    CHECK(u8_exists(TROOT "/ren3/" BOXTXN_STAGE "/A.dsk"));
    txn_free(&m); meta_delete(b); free(old); sb_free(&err);
}

/* power cut around the folder's new name: the journal travels with the bag */
static void txn_crash_rename(void)
{
    int step; char bag[100], nd[100], fin[600];
    for (step = 8; step <= 9; step++) {
        char *old; meta_box *b; txn_moves m; strbuf err, log;
        sb_init(&err); sb_init(&log);
        snprintf(bag, sizeof(bag), TROOT "/crren%d", step);
        snprintf(nd, sizeof(nd), TROOT "/crren%d new", step);
        make_bag(bag, &old, &b, &m);
        boxtxn_test_crash(step);
        CHECK(boxtxn_commit(bag, &m, b, NULL, NULL, NULL, nd, &err) == -2);
        boxtxn_test_crash(0);
        /* the archive finds the bag where it is now */
        CHECK(boxtxn_recover(step == 8 ? bag : nd, NULL, NULL, fin, sizeof(fin), &log) == BOXTXN_FORWARD);
        CHECK_STR(fin, nd);
        CHECK(!u8_exists(bag));
        CHECK(which(nd, old) == 2);
        txn_free(&m); meta_delete(b); free(old); sb_free(&err); sb_free(&log);
    }
}

static void aside_name_taken(void)
{
    txn_moves m; char out[320];
    memset(&m, 0, sizeof(m));
    u8_mkdirs(TROOT "/aside/set-aside");
    put(TROOT "/aside/set-aside/X.replaced", "x");
    txn_aside_name(TROOT "/aside", &m, "X", ".replaced", out, sizeof(out));
    CHECK_STR(out, "set-aside/X.replaced2");
    txn_add(&m, "data/X", out);
    txn_aside_name(TROOT "/aside", &m, "X", ".replaced", out, sizeof(out));
    CHECK_STR(out, "set-aside/X.replaced3");
    txn_aside_name(TROOT "/aside", &m, "Y", ".removed", out, sizeof(out));
    CHECK_STR(out, "set-aside/Y.removed");
    txn_free(&m);
}

static void recover_no_journal(void)
{
    char *old; meta_box *b; txn_moves m; strbuf err, log; char fin[600];
    sb_init(&err); sb_init(&log);
    make_bag(TROOT "/nojournal", &old, &b, &m);
    /* the stage's metadata.json lists the finished read, as the session leaves it */
    CHECK(meta_save(b, TROOT "/nojournal/" BOXTXN_STAGE, NULL, NULL, &err) == 0);
    put(TROOT "/nojournal/" BOXTXN_STAGE "/half.dsk", "half");
    CHECK(boxtxn_recover(TROOT "/nojournal", NULL, NULL, fin, sizeof(fin), &log) == BOXTXN_CLEANED);
    CHECK(which(TROOT "/nojournal", old) == 1);
    CHECK(u8_exists(TROOT "/nojournal/set-aside/A.dsk.unused"));
    CHECK(!u8_exists(TROOT "/nojournal/set-aside/half.dsk.unused"));
    CHECK(boxtxn_recover(TROOT "/nojournal", NULL, NULL, fin, sizeof(fin), &log) == BOXTXN_NONE);
    txn_free(&m); meta_delete(b); free(old); sb_free(&err); sb_free(&log);
}

void test_boxtxn(void)
{
    u8_mkdir("tmp_unit");
    txn_commit_ok();
    txn_previous_kept();
    txn_crash_each_step();
    txn_fail_each_step();
    txn_fail_restores_metadata();
    txn_undo_fails();
    txn_crash_cleanup();
    txn_rename();
    txn_crash_rename();
    aside_name_taken();
    recover_no_journal();
}
