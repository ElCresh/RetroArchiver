/* the session in edit mode: a closed box, some media read again, each one
 * its own transaction */
#include <stdlib.h>
#include "unit.h"
#include "core/archive.h"
#include "core/bagit.h"
#include "core/boxedit.h"
#include "core/boxtxn.h"
#include "core/fsu8.h"
#include "core/runner.h"
#include "core/session.h"

#define SROOT "tmp_unit/sedit"

static int e_fill, e_cmp_rc, e_disk, e_nwait, e_nchoose, e_choose_ans;
static int e_script[8];
static disk_turn e_turns[8];
static edit_choice e_choice;

static int e_scan(const char *o, runner_line_fn fn, void *c) { (void)o; (void)fn; (void)c; return -1; }
static int e_read(const char *o, runner_line_fn fn, void *c)
{
    FILE *f; int n = 400 + e_disk;
    (void)fn; (void)c;
    if (runner_stopped()) return 1;
    if ((f = u8_fopen(o, "wb")) != NULL) { while (n--) fputc(e_fill, f); fclose(f); }
    return 0;
}
static int e_cmp(const char *const *i, int n, int f, compare_result *r) { (void)i; (void)n; (void)f; (void)r; return e_cmp_rc; }
static void e_tech(void *c, const char *i, json_writer *w) { (void)c; (void)i; (void)w; }
static void e_event(void *c, event_kind k, int d, const char *t) { (void)c; (void)k; (void)d; (void)t; }
static int e_wait(void *c, const disk_turn *t, char *l, size_t ll, char *no, size_t nl)
{
    (void)c; (void)l; (void)ll; (void)no; (void)nl;
    e_disk = t->n;
    if (e_nwait < 8) e_turns[e_nwait] = *t;
    return e_nwait < 8 ? e_script[e_nwait++] : CMD_QUIT;
}
static int e_choose(void *c, const edit_choice *ch)
{
    (void)c;
    e_choice = *ch;
    e_nchoose++;
    return e_choose_ans;
}

static media_ops e_media = { "floppy", ".dsk", NULL, e_scan, e_read, e_cmp, e_tech };

static void setup(session_cfg *c, session_host *h, meta_box *box, const char *title, int ask_again)
{
    str_copy(box->title, sizeof(box->title), title);
    str_copy(box->platform, sizeof(box->platform), "X");
    meta_set_counts(box, 2, 0, 0);
    memset(h, 0, sizeof(*h));
    h->event = e_event; h->wait_disk = e_wait; h->choose = e_choose; h->ask_again = ask_again;
    memset(c, 0, sizeof(*c));
    c->box = box; c->outroot = SROOT; c->names[MEDIA_FLOPPY] = "e{nn}";
    c->maxread = 2; c->start = 1; c->media[MEDIA_FLOPPY] = &e_media;
    memset(e_script, 0, sizeof(e_script));          /* CMD_READ */
    e_nwait = e_nchoose = 0;
}

/* a closed box of 2 floppies read with fill and the outcome of rc */
static void closed_box(const char *title, int fill, int rc)
{
    session_cfg c; session_host h; meta_box *box = meta_new();
    setup(&c, &h, box, title, 0);
    e_fill = fill; e_cmp_rc = rc;
    CHECK(session_run(&c, &h) == SESSION_CLOSED);
    meta_delete(box);
}

/* the edit of title: media in reread, read with fill and rc */
static int edit(const char *title, const char *reread, int fill, int rc, int ask_again)
{
    session_cfg c; session_host h; meta_box *box = meta_new(); int r, i;
    setup(&c, &h, box, title, ask_again);
    for (i = 0; reread[i]; i++) c.reread[reread[i] - '0'] = 1;
    c.edit = 1;
    c.reason = "new drive";
    e_fill = fill; e_cmp_rc = rc;
    r = session_run(&c, &h);
    runner_clear();
    meta_delete(box);
    return r;
}

static void bagpath(const char *title, const char *rel, char *out, size_t n)
{
    char bag[400];
    session_bagdir(bag, sizeof(bag), SROOT, title, "");
    snprintf(out, n, "%s/%s", bag, rel);
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

static int has(const char *title, const char *rel, const char *needle)
{
    char p[600]; char *s; int r;
    bagpath(title, rel, p, sizeof(p));
    s = slurp(p);
    r = s && strstr(s, needle) != NULL;
    free(s);
    return r;
}

static int exists(const char *title, const char *rel)
{
    char p[600];
    bagpath(title, rel, p, sizeof(p));
    return u8_exists(p);
}

static int verified(const char *title)
{
    char bag[400]; arc_check c; strbuf err; int ok;
    sb_init(&err);
    session_bagdir(bag, sizeof(bag), SROOT, title, "");
    ok = arc_verify(bag, &c, &err) == 0 && !c.changed && !c.lost && !c.extra && !bag_is_open(bag)
         && !exists(title, BAG_EDIT_DIR);
    sb_free(&err);
    return ok;
}

static void edit_better_kept(void)
{
    char *before, p[600];
    closed_box("Better", 'a', 2);              /* error: the reads differ */
    bagpath("Better", "manifest-sha256.txt", p, sizeof(p));
    before = slurp(p);
    CHECK(edit("Better", "1", 'b', 0, 1) == SESSION_CLOSED);
    CHECK(verified("Better"));
    CHECK(has("Better", "data/metadata.json", "\"kept\": \"new\""));
    CHECK(has("Better", "data/metadata.json", "\"change\": \"reread\""));
    CHECK(has("Better", "data/metadata.json", "\"reason\": \"new drive\""));
    CHECK(exists("Better", "set-aside/e01.dsk.replaced"));
    CHECK(exists("Better", "set-aside/e01_b.dsk.replaced"));
    {
        char *after = slurp(p);
        CHECK(before && after && strcmp(before, after));
        free(after);
    }
    CHECK(has("Better", "../archive.csv", ";clean;"));
    free(before);
}

static void edit_worse_rejected(void)
{
    char *before, *after, p[600];
    closed_box("Worse", 'a', 0);
    bagpath("Worse", "manifest-sha256.txt", p, sizeof(p));
    before = slurp(p);
    {
        session_cfg c; session_host h; meta_box *box = meta_new();
        setup(&c, &h, box, "Worse", 1);
        c.reread[1] = 1; c.edit = 1;
        e_script[1] = CMD_SKIP;                 /* told it was worse: keep the old one */
        e_fill = 'b'; e_cmp_rc = 2;
        CHECK(session_run(&c, &h) == SESSION_CLOSED);
        CHECK(e_nwait == 2 && !e_turns[0].edit_worse && e_turns[1].edit_worse);
        runner_clear();
        meta_delete(box);
    }
    after = slurp(p);
    CHECK(before && after && !strcmp(before, after));
    CHECK(exists("Worse", "set-aside/e01.dsk.rejected"));
    CHECK(verified("Worse"));
    free(before); free(after);
}

static void edit_same_grade_asks(void)
{
    closed_box("Same1", 'a', 0);
    e_choose_ans = 1;
    CHECK(edit("Same1", "1", 'b', 0, 1) == SESSION_CLOSED);
    CHECK(e_nchoose == 1);
    CHECK_STR(e_choice.old_status, "clean");
    CHECK_STR(e_choice.new_status, "clean");
    CHECK(e_choice.n == 1);
    CHECK(exists("Same1", "set-aside/e01.dsk.replaced"));
    CHECK(verified("Same1"));

    closed_box("Same0", 'a', 0);
    e_choose_ans = 0;
    CHECK(edit("Same0", "1", 'b', 0, 1) == SESSION_CLOSED);
    CHECK(e_nchoose == 1);
    CHECK(exists("Same0", "set-aside/e01.dsk.rejected"));
    CHECK(!has("Same0", "data/metadata.json", "history"));
    CHECK(verified("Same0"));
}

static void edit_identical(void)
{
    char *before, *after, p[600];
    closed_box("Ident", 'a', 0);
    bagpath("Ident", "manifest-sha256.txt", p, sizeof(p));
    before = slurp(p);
    CHECK(edit("Ident", "1", 'a', 0, 1) == SESSION_CLOSED);
    after = slurp(p);
    /* the history says it was confirmed; the new read is kept aside, not deleted */
    CHECK(before && after && strcmp(before, after));
    CHECK(e_nchoose == 0);
    CHECK(exists("Ident", "set-aside/e01.dsk.identical"));
    CHECK(has("Ident", "data/metadata.json", "\"kept\": \"old-identical\""));
    CHECK(verified("Ident"));
    free(before); free(after);
}

static void edit_per_disk(void)
{
    session_cfg c; session_host h; meta_box *box = meta_new();
    closed_box("PerDisk", 'a', 2);
    setup(&c, &h, box, "PerDisk", 1);
    c.reread[1] = c.reread[2] = 1; c.edit = 1;
    e_script[1] = CMD_QUIT;                     /* stopped at the second */
    e_fill = 'b'; e_cmp_rc = 0;
    CHECK(session_run(&c, &h) == SESSION_CLOSED);
    runner_clear();
    meta_delete(box);
    CHECK(verified("PerDisk"));
    CHECK(exists("PerDisk", "set-aside/e01.dsk.replaced"));
    CHECK(!exists("PerDisk", "set-aside/e02.dsk.replaced"));
    CHECK(has("PerDisk", "data/metadata.json", "\"medium\": \"floppy 1\""));
    CHECK(!has("PerDisk", "data/metadata.json", "\"medium\": \"floppy 2\""));
}

static void edit_skip_unchanged(void)
{
    char *before, *after, p[600];
    session_cfg c; session_host h; meta_box *box = meta_new();
    closed_box("Skip", 'a', 0);
    bagpath("Skip", "manifest-sha256.txt", p, sizeof(p));
    before = slurp(p);
    setup(&c, &h, box, "Skip", 1);
    c.reread[2] = 1; c.edit = 1;
    e_script[0] = CMD_SKIP;
    CHECK(session_run(&c, &h) == SESSION_CLOSED);
    meta_delete(box);
    after = slurp(p);
    CHECK(before && after && !strcmp(before, after));
    CHECK(!has("Skip", "data/metadata.json", "history"));
    CHECK(verified("Skip"));
    free(before); free(after);
}

static void edit_damaged_repair(void)
{
    char p[600]; FILE *f;
    closed_box("Damaged", 'a', 0);
    bagpath("Damaged", "data/e01.dsk", p, sizeof(p));
    if ((f = u8_fopen(p, "ab")) != NULL) { fputs("rot", f); fclose(f); }
    /* reading the other one is not enough */
    CHECK(edit("Damaged", "2", 'b', 0, 1) == SESSION_ERROR);
    CHECK(!exists("Damaged", BAG_EDIT_DIR));
    CHECK(edit("Damaged", "1", 'b', 0, 1) == SESSION_CLOSED);
    CHECK(exists("Damaged", "set-aside/e01.dsk.damaged"));
    CHECK(has("Damaged", "data/metadata.json", "\"old_damaged\": true"));
    CHECK(verified("Damaged"));
}

/* set-aside cannot be written (here a file has its name): the new read is
 * never deleted, it waits in editing/ for the next recovery */
static void edit_aside_blocked(void)
{
    char bag[400], p[600], fin[600]; FILE *f; strbuf log;
    closed_box("Blocked", 'a', 0);
    session_bagdir(bag, sizeof(bag), SROOT, "Blocked", "");
    snprintf(p, sizeof(p), "%s/set-aside", bag);
    if ((f = u8_fopen(p, "wb")) != NULL) fclose(f);
    CHECK(edit("Blocked", "1", 'b', 2, 0) == SESSION_CLOSED);      /* worse: to be set aside */
    CHECK(exists("Blocked", BAG_EDIT_DIR "/data/e01.dsk"));
    u8_remove(p);
    sb_init(&log);
    CHECK(boxtxn_recover(bag, NULL, NULL, fin, sizeof(fin), &log) == BOXTXN_CLEANED);
    CHECK(exists("Blocked", "set-aside/e01.dsk.unused"));
    sb_free(&log);
}

/* a damaged medium that is not read again in the end stays damaged: the
 * transaction of another medium does not approve its new bytes */
static void edit_damage_kept(void)
{
    session_cfg c; session_host h; meta_box *box = meta_new();
    char p[600], bag[400]; FILE *f; arc_check ck; strbuf err;
    closed_box("KeptDamage", 'a', 0);
    bagpath("KeptDamage", "data/e02.dsk", p, sizeof(p));
    if ((f = u8_fopen(p, "ab")) != NULL) { fputs("rot", f); fclose(f); }
    setup(&c, &h, box, "KeptDamage", 1);
    c.reread[1] = c.reread[2] = 1; c.edit = 1;
    e_script[1] = CMD_QUIT;
    e_fill = 'b'; e_cmp_rc = 0;
    e_choose_ans = 1;                           /* as good as the old one: keep the new */
    CHECK(session_run(&c, &h) == SESSION_CLOSED);
    runner_clear();
    meta_delete(box);
    CHECK(exists("KeptDamage", "set-aside/e01.dsk.replaced"));
    sb_init(&err);
    session_bagdir(bag, sizeof(bag), SROOT, "KeptDamage", "");
    CHECK(arc_verify(bag, &ck, &err) == 0 && ck.changed == 1);
    sb_free(&err);
}

/* a box of 261017 left open: closing it moves messi-da-parte/ to set-aside/ */
static void close_converts_aside(void)
{
    session_cfg c; session_host h; meta_box *box = meta_new(); char bag[400], p[600]; FILE *f;
    setup(&c, &h, box, "Aperto Vecchio", 0);
    e_script[1] = CMD_QUIT;
    e_fill = 'a'; e_cmp_rc = 0;
    CHECK(session_run(&c, &h) == SESSION_OPEN);
    session_bagdir(bag, sizeof(bag), SROOT, "Aperto Vecchio", "");
    snprintf(p, sizeof(p), "%s/messi-da-parte", bag);
    u8_mkdir(p);
    snprintf(p, sizeof(p), "%s/messi-da-parte/e01_b.dsk.duplicato", bag);
    if ((f = u8_fopen(p, "wb")) != NULL) { fputs("old", f); fclose(f); }
    setup(&c, &h, box, "Aperto Vecchio", 0);
    e_fill = 'c';
    CHECK(session_run(&c, &h) == SESSION_CLOSED);
    meta_delete(box);
    CHECK(!exists("Aperto Vecchio", "messi-da-parte"));
    CHECK(exists("Aperto Vecchio", "set-aside/e01_b.dsk.duplicate"));
}

static void edit_refuses_open(void)
{
    session_cfg c; session_host h; meta_box *box = meta_new();
    setup(&c, &h, box, "Open", 0);
    e_script[0] = CMD_QUIT;
    e_fill = 'a'; e_cmp_rc = 0;
    CHECK(session_run(&c, &h) == SESSION_OPEN);
    meta_delete(box);
    CHECK(edit("Open", "1", 'b', 0, 1) == SESSION_ERROR);
}

void test_session_edit(void)
{
    u8_mkdirs(SROOT);
    edit_better_kept();
    edit_worse_rejected();
    edit_same_grade_asks();
    edit_identical();
    edit_per_disk();
    edit_skip_unchanged();
    edit_damaged_repair();
    edit_refuses_open();
    edit_aside_blocked();
    edit_damage_kept();
    close_converts_aside();
}
