#include "unit.h"
#include "core/session.h"
#include "core/fsu8.h"
#include "core/runner.h"

/* fake medium: the first read succeeds, the second leaves a half-written file */
static int fake_calls;
static void put(const char *p, int n)
{
    FILE *f = u8_fopen(p, "wb");
    if (f) { while (n--) fputc('x', f); fclose(f); }
}
static int f_scan(const char *o, runner_line_fn fn, void *c) { (void)o; (void)fn; (void)c; return -1; }
static int f_read(const char *o, runner_line_fn fn, void *c)
{
    (void)fn; (void)c;
    put(o, fake_calls++ ? 100 : 1000);
    return fake_calls == 1 ? 0 : -1;
}
static int f_cmp(const char *const *i, int n, int f, compare_result *r) { (void)i; (void)n; (void)f; (void)r; return 2; }
static void f_tech(void *c, const char *i, json_writer *w) { (void)c; (void)i; (void)w; }
static void h_event(void *c, event_kind k, int d, const char *t) { (void)c; (void)k; (void)d; (void)t; }
static int h_wait(void *c, const disk_turn *t, char *l, size_t ll, char *no, size_t nl)
{
    (void)c; (void)l; (void)ll; (void)no; (void)nl;
    return t->n == 1 ? CMD_READ : CMD_FINISH;
}

static void test_partial(void)
{
    media_ops m = { "floppy", ".dsk", NULL, f_scan, f_read, f_cmp, f_tech };
    session_host h; session_cfg c; meta_box *box = meta_new();
    char p[256];

    str_copy(box->title, sizeof(box->title), "T");
    str_copy(box->platform, sizeof(box->platform), "X");
    box->media_expected = 2;
    memset(&h, 0, sizeof(h));
    h.event = h_event; h.wait_disk = h_wait; h.ctx = NULL;
    memset(&c, 0, sizeof(c));
    c.box = box; c.outroot = "tmp_unit/sess"; c.names[MEDIA_FLOPPY] = "d{nn}";
    c.maxread = 3; c.start = 1; c.media[MEDIA_FLOPPY] = &m;
    fake_calls = 0;
    CHECK(session_run(&c, &h) == SESSION_CLOSED);
    session_bagdir(p, sizeof(p), "tmp_unit/sess", "T");
    strcat(p, U8_SEP "data" U8_SEP "d01_b.dsk");
    CHECK(!u8_exists(p));
    meta_delete(box);
}

/* Cancel pressed during the comparison, which launches no programs:
 * the disk closes cleanly but the session does not ask for the next one */
static int asked;
/* like floppy.c: with cancellation on, the read counts as interrupted */
static int g_read(const char *o, runner_line_fn fn, void *c)
{
    (void)fn; (void)c;
    if (runner_stopped()) return 1;
    put(o, 500);
    return 0;
}
static int g_cmp(const char *const *i, int n, int f, compare_result *r)
{
    (void)i; (void)n; (void)f; (void)r;
    runner_cancel();
    return 0;
}
static int g_wait(void *c, const disk_turn *t, char *l, size_t ll, char *no, size_t nl)
{
    (void)c; (void)t; (void)l; (void)ll; (void)no; (void)nl;
    asked++;
    return CMD_READ;
}

static void cancel_in_compare(int ask_again, const char *title)
{
    media_ops m = { "floppy", ".dsk", NULL, f_scan, g_read, g_cmp, f_tech };
    session_host h; session_cfg c; meta_box *box = meta_new();
    int rc;

    str_copy(box->title, sizeof(box->title), title);
    str_copy(box->platform, sizeof(box->platform), "X");
    box->media_expected = 3;
    memset(&h, 0, sizeof(h));
    h.event = h_event; h.wait_disk = g_wait; h.ask_again = ask_again;
    memset(&c, 0, sizeof(c));
    c.box = box; c.outroot = "tmp_unit/sess"; c.names[MEDIA_FLOPPY] = "d{nn}";
    c.maxread = 3; c.start = 1; c.media[MEDIA_FLOPPY] = &m;
    asked = 0;
    rc = session_run(&c, &h);
    if (ask_again) {
        CHECK(rc == SESSION_CLOSED && asked == 3);
        CHECK_STR(box->disk[1].status, "pulito");
        CHECK_STR(box->disk[2].status, "pulito");
    } else {
        CHECK(rc == SESSION_OPEN && asked == 1);
        CHECK_STR(box->disk[0].status, "pulito");
    }
    runner_clear();
    meta_delete(box);
}

/* mixed box: the floppies are read, the CD is asked for but stays to be read */
static disk_turn turns[8];
static int nturns, cd_cmd;
static int m_read(const char *o, runner_line_fn fn, void *c) { (void)fn; (void)c; put(o, 700); return 0; }
static int m_cmp(const char *const *i, int n, int f, compare_result *r) { (void)i; (void)n; (void)f; (void)r; return 0; }
static int m_wait(void *c, const disk_turn *t, char *l, size_t ll, char *no, size_t nl)
{
    (void)c; (void)no; (void)nl;
    if (nturns < 8) turns[nturns++] = *t;
    if (t->kind == MEDIA_CD) { str_copy(l, ll, "NTWKS40_IT"); return cd_cmd; }
    return CMD_READ;
}

static void mixed_box(void)
{
    media_ops m = { "floppy", ".dsk", NULL, f_scan, m_read, m_cmp, f_tech };
    session_host h; session_cfg c; meta_box *box = meta_new();
    char p[256], q[300]; int rc;

    str_copy(box->title, sizeof(box->title), "NT misto");
    str_copy(box->platform, sizeof(box->platform), "Windows NT");
    meta_set_counts(box, 2, 1, 0);
    memset(&h, 0, sizeof(h));
    h.event = h_event; h.wait_disk = m_wait;
    memset(&c, 0, sizeof(c));
    c.box = box; c.outroot = "tmp_unit/sess"; c.maxread = 2; c.start = 1;
    c.media[MEDIA_FLOPPY] = &m;             /* no media_ops for CDs */
    nturns = 0; cd_cmd = CMD_READ;
    rc = session_run(&c, &h);
    CHECK(rc == SESSION_OPEN);
    CHECK(nturns == 3);
    CHECK(turns[0].kind == MEDIA_FLOPPY && turns[0].num == 1 && turns[0].count == 2 && turns[0].readable);
    CHECK(turns[2].kind == MEDIA_CD && turns[2].num == 1 && turns[2].count == 1 && !turns[2].readable);
    CHECK(turns[2].n == 3 && turns[2].total == 3);
    CHECK_STR(box->disk[0].status, "pulito");
    CHECK_STR(box->disk[0].master, "FLOPPY01.dsk");
    CHECK_STR(box->disk[2].status, "");     /* to be read, not missing */
    CHECK_STR(box->disk[2].label, "NTWKS40_IT");
    CHECK_STR(box->names[MEDIA_CD], "CD{nn}");
    session_bagdir(p, sizeof(p), "tmp_unit/sess", "NT misto");
    snprintf(q, sizeof(q), "%s" U8_SEP ".in-corso", p);
    CHECK(u8_exists(q));

    /* resumed: only the CD is asked for; Salta leaves it still to be read */
    nturns = 0; cd_cmd = CMD_SKIP;
    meta_set_counts(box, 2, 1, 0);
    CHECK(session_run(&c, &h) == SESSION_OPEN && nturns == 1 && turns[0].kind == MEDIA_CD);
    CHECK_STR(box->disk[2].status, "");
    /* End of box: the CD counts as missing and the package closes */
    nturns = 0; cd_cmd = CMD_FINISH;
    CHECK(session_run(&c, &h) == SESSION_CLOSED);
    CHECK_STR(box->disk[2].status, "mancante");
    CHECK(!u8_exists(q));
    meta_delete(box);
}

void test_session(void)
{
    char out[64], longt[501], dir[256];


    session_expand(out, sizeof(out), "disco{nn}", 7, "X");
    CHECK_STR(out, "disco07");
    session_expand(out, sizeof(out), "{set} - {nnn}", 3, "Monkey");
    CHECK_STR(out, "Monkey - 003");
    session_expand(out, sizeof(out), "D{n}-{x}", 12, "T");
    CHECK_STR(out, "D12-{x}");
    session_expand(out, sizeof(out), "{data}", 1, "T");
    CHECK(strlen(out) == 10 && out[4] == '-' && out[7] == '-');
    memset(longt, 'A', 500); longt[500] = 0;
    session_expand(out, sizeof(out), "{set}{nn}", 1, longt);
    CHECK(strlen(out) == 63);

    session_bagdir(dir, sizeof(dir), "arch", "A/B");
    CHECK_STR(dir, "arch" U8_SEP "A_B");
    test_partial();
    cancel_in_compare(0, "Annullato");
    cancel_in_compare(1, "Annullato con riprova");
    mixed_box();
}
