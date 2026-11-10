/* dskrun - batch archiving of floppies, from the console.
 *
 * For each disk: beep, wait, then scan, two reads, comparison and, if
 * needed, a third read, all automatically. The result is one BagIt
 * package per box (images, scans, metadata.json) plus one row per disk
 * in archive.csv. The logic lives in core/session.c, shared with the
 * GUI. A box can also hold CDs and DVDs (-C, -D): they are asked for
 * after the floppies and read with DiscImageCreator (-X, -d).
 *
 * On-screen text has no accents: the Windows XP console does not render
 * them well.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/fsu8.h"
#include "core/session.h"
#include "core/boxedit.h"
#include "core/boxtxn.h"
#include "core/archive.h"
#include "core/boxclean.h"
#include "core/boximg.h"

#ifdef _WIN32
#include <windows.h>
/* only on a console: when output goes to a file or pipe (WinRM, tests) stay silent */
static void beep2(void)
{
    DWORD mode;
    if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) return;
    Beep(880, 200); Sleep(90); Beep(1320, 260);
}
/* argv and the console are not UTF-8 on Windows: convert from the code page */
static void to_utf8(const char *in, char *out, size_t n, UINT cp)
{
    wchar_t w[1024]; char buf[4096];
    /* wide buffer first, then str_copy cuts on a UTF-8 boundary */
    if (!MultiByteToWideChar(cp, 0, in, -1, w, 1024) ||
        !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), NULL, NULL))
        str_copy(out, n, in);
    else
        str_copy(out, n, buf);
}
#define ARG_CP     CP_ACP
#define CONSOLE_CP GetConsoleCP()
#else
#include <unistd.h>
static void beep2(void) { if (isatty(1)) { printf("\a"); fflush(stdout); } }
static void to_utf8(const char *in, char *out, size_t n, unsigned cp) { (void)cp; str_copy(out, n, in); }
#define ARG_CP     0
#define CONSOLE_CP 0
#endif

/* Ctrl+C: the first one stops SAMdisk (started without a console, it
 * would not receive it) and the session saves and closes; the second one
 * in the same phase really exits. Reset at every phase. */
static volatile int ctrlc;
#ifdef _WIN32
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (ctrlc++) return FALSE;
    runner_cancel();
    return TRUE;
}
static void catch_ctrlc(void) { SetConsoleCtrlHandler(on_ctrl, TRUE); }
#else
#include <signal.h>
static void on_sigint(int sig)
{
    if (ctrlc++) { signal(sig, SIG_DFL); raise(sig); return; }
    runner_cancel();
}
static void catch_ctrlc(void) { signal(SIGINT, on_sigint); }
#endif

/* The program text is UTF-8 (titles, labels, SAMdisk and DiscImageCreator
 * lines, file names). On a real Windows console it goes out in its code
 * page (850 on an Italian XP), otherwise accents come out as two garbage
 * characters; on a file or pipe it stays UTF-8. */
#ifdef _WIN32
static void put_text(const char *s)
{
    static int console = -1; DWORD mode; char *c;
    if (console < 0) console = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
    if (!console || !(c = u8_to_cp(s, GetConsoleOutputCP()))) { fputs(s, stdout); return; }
    fputs(c, stdout);
    free(c);
}
#else
static void put_text(const char *s) { fputs(s, stdout); }
#endif

static void say(const char *fmt, ...)
{
    strbuf b; va_list ap;
    sb_init(&b);
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    put_text(b.s);
    sb_free(&b);
}

static const meta_box *g_box;           /* used to name the media */
static int nclean, nmarginal, nerror, nother;
/* last outcome of each disk: with -r a retried disk is counted once */
static char outcome[META_MAXDISK + 1];

static void count_outcome(int disk, const char *text)
{
    int i; char o = meta_status_class(text);
    if (o == 'f') o = 'm';                  /* fragile, read once: needs a look */
    else if (o == '-') o = 'a';
    if (disk >= 1 && disk <= META_MAXDISK) outcome[disk] = o;
    nclean = nmarginal = nerror = nother = 0;
    for (i = 1; i <= META_MAXDISK; i++) {
        if (outcome[i] == 'p') nclean++;
        else if (outcome[i] == 'm') nmarginal++;
        else if (outcome[i] == 'e') nerror++;
        else if (outcome[i] == 'a') nother++;
    }
}

/* "floppy 2", "CD 1" */
static const char *who(int n)
{
    static char out[32]; int kind, num;
    meta_slot(g_box, n, &kind, &num);
    meta_disk_name(kind, num, out, sizeof(out));
    return out;
}

static void on_event(void *ctx, event_kind k, int disk, const char *text)
{
    (void)ctx;
    switch (k) {
    case EV_PHASE:
        ctrlc = 0;
        say("\n  --- %s: %s ---\n", who(disk), text);
        break;
    case EV_LINE:
        say("  %s\n", text);
        break;
    case EV_DISK_DONE:
        count_outcome(disk, text);
        say("\n  >>> %s: %s\n", who(disk), text);
        say("  >>> so far: %d clean, %d marginal, %d with errors, %d skipped or to redo\n",
               nclean, nmarginal, nerror, nother);
        break;
    case EV_BOX_DONE:
        say("\n  >>> box %s: package closed and verified.\n", text);
        break;
    case EV_ERROR:
        say("\n  ERROR: %s\n", text);
        break;
    }
    fflush(stdout);
}

static int wait_disk(void *ctx, const disk_turn *t, char *label, size_t llen, char *note, size_t nlen)
{
    char line[160]; size_t k; const char *kind = meta_kind_label(t->kind);
    (void)ctx; (void)note; (void)nlen;
    beep2();
    say("\n");
    say("  ============================================================\n");
    if (t->readable && t->edit_worse) {
        say("    The new read of %s %d was worse than the old one, which is kept.\n", kind, t->num);
        say("    Clean the disc and press ENTER to read it again,\n");
        say("    or S to keep the old one   -   Q stop here\n");
    } else if (t->readable && t->protection_retry) {
        if (t->protection_retry == 1)
            say("    Read %s %d again, looking for copy protections (/sf)\n", kind, t->num);
        else
            say("    Read %s %d again, without looking for copy protections\n", kind, t->num);
        say("    ENTER to read it again, S to keep what was read\n");
        say("    F finish the box   -   Q quit and resume later\n");
    } else if (t->readable) {
        say("    Insert %s %d of %d\n", kind, t->num, t->count);
        say("    ENTER to read it (or type the label and ENTER)\n");
        say("    S skip   -   F finish the box   -   Q quit and resume later\n");
    } else {
        say("    %s %d of %d: reading %s is not supported yet\n", kind, t->num, t->count, kind);
        say("    The %s stays unread and the box stays open: resume it\n", kind);
        say("    when dskrun can read it. ENTER or S to go on\n");
        say("    (you can type the label and ENTER)\n");
        say("    F finish the box (the %s will be missing)   -   Q quit\n", kind);
    }
    say("  ============================================================\n");
    say("  > ");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return CMD_QUIT;
    k = strcspn(line, "\r\n");
    line[k] = 0;
    if (k == 1 && (line[0] == 'q' || line[0] == 'Q')) return CMD_QUIT;
    if (k == 1 && (line[0] == 's' || line[0] == 'S')) return CMD_SKIP;
    if (k == 1 && (line[0] == 'f' || line[0] == 'F')) return CMD_FINISH;
    if (k) to_utf8(line, label, llen, CONSOLE_CP);
    return CMD_READ;
}

/* --reread: two reads as good as each other but different */
static int choose(void *ctx, const edit_choice *c)
{
    char line[40]; const char *kind = meta_kind_label(c->kind);
    (void)ctx;
    beep2();
    say("\n  %s %d: the new read is as good as the old one (%s), but different.\n", kind, c->num, c->new_status);
    say("    old: %s, %s, %s, %s\n", c->old_status, c->old_method[0] ? c->old_method : "-",
        c->old_drive[0] ? c->old_drive : "-", c->old_at);
    say("    new: %s, %s, %s, %s\n", c->new_status, c->new_method[0] ? c->new_method : "-",
        c->new_drive[0] ? c->new_drive : "-", c->new_at);
    for (;;) {
        say("  N keep the new read   -   O keep the old one\n  > ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return 0;    /* no input: nothing changes */
        if (line[0] == 'n' || line[0] == 'N') return 1;
        if (line[0] == 'o' || line[0] == 'O') return 0;
    }
}

/* "floppy3", "cd1", "dvd2" or "missing" into reread[]; -1 with the reason */
static int parse_reread(const char *list, const meta_box *b, char reread[META_MAXDISK + 1])
{
    char item[32], comp[80]; const char *p = list, *e; int k, num, n, any = 0;
    for (; *p; p = *e ? e + 1 : e) {
        e = p + strcspn(p, ",");
        snprintf(item, sizeof(item), "%.*s", (int)(e - p), p);
        if (!strcmp(item, "missing")) {
            for (n = 1; n <= b->media_expected; n++) {
                const meta_disk *d = NULL; int i;
                for (i = 0; i < b->ndisk; i++) if (b->disk[i].n == n) d = &b->disk[i];
                if (!d || !meta_disk_done(d)) { reread[n] = 1; any = 1; }
            }
            continue;
        }
        for (k = 0; k < MEDIA_KINDS; k++) {
            size_t l = strlen(meta_kind_key(k));
            if (!strncmp(item, meta_kind_key(k), l) && item[l] >= '1' && item[l] <= '9') break;
        }
        meta_composition(b, comp, sizeof(comp));
        if (k == MEDIA_KINDS) {
            say("  ERROR: \"%s\" is not a medium: write floppy3, cd1, dvd2 or missing.\n", item);
            return -1;
        }
        num = atoi(item + strlen(meta_kind_key(k)));
        if (num > meta_count(b, k)) {
            say("  ERROR: the box has no %s %d (it has %s): check --reread.\n", meta_kind_label(k), num, comp);
            return -1;
        }
        for (n = num, k = k - 1; k >= 0; k--) n += meta_count(b, k);
        reread[n] = 1;
        any = 1;
    }
    if (!any) say("  The box has no missing media: nothing to read.\n");
    return 0;
}

/* --convert: one box; 0 converted, 1 already current, -1 not converted */
static int convert_one(const char *bag)
{
    arc_box *a = (arc_box *)calloc(1, sizeof(arc_box)); strbuf err; int rc;
    const char *name = bag + strlen(bag);
    while (name > bag && name[-1] != '/' && name[-1] != '\\') name--;
    if (a && arc_load(bag, a)) name = a->title;
    sb_init(&err);
    rc = boxedit_convert(bag, NULL, &err);
    if (rc == 0) say("  Converted: %s\n", name);
    else if (rc == 1) say("  Already in the current format: %s\n", name);
    else say("  Not converted: %s: %s.\n", name, err.s ? err.s : "");
    sb_free(&err);
    free(a);
    return rc;
}

typedef struct { const char *root; int done, failed; } convert_ctx;

static void convert_entry(void *c, const char *name, int isdir)
{
    convert_ctx *x = (convert_ctx *)c; char bag[1024];
    char ed[1100], fin[1040]; strbuf log;
    if (!isdir) return;
    u8_join(bag, sizeof(bag), x->root, name);
    /* a conversion cut by a power failure looks open: put it in order first */
    u8_join(ed, sizeof(ed), bag, BOXTXN_DIR);
    if (u8_isdir(ed)) {
        sb_init(&log);
        if (boxtxn_recover(bag, boxedit_tech, NULL, fin, sizeof(fin), &log) > 0 && log.s) say("  %s", log.s);
        sb_free(&log);
        str_copy(bag, sizeof(bag), fin);
    }
    if (!boxedit_is_old(bag)) return;
    if (convert_one(bag) >= 0) x->done++; else x->failed++;   /* 1: finished by the recovery */
}

/* the boxes of 261017 in the current format: all those of outdir, or one */
static int run_convert(const char *outdir, const char *title, const char *edition, const char *language)
{
    convert_ctx x; char bag[1024]; int rc; strbuf why;
    if (title[0]) {
        sb_init(&why);
        rc = session_resolve(outdir, title, edition, language, bag, sizeof(bag), &why);
        if (rc) say("  ERROR: %s\n",why.s ? why.s : "");
        sb_free(&why);
        if (rc) return 2;
        if (!u8_isdir(bag)) {
            say("  ERROR: there is no box \"%s\" in %s: check the title, the edition, the language and -o.\n", title, outdir);
            return 2;
        }
        rc = convert_one(bag);
        return rc < 0 ? 1 : 0;
    }
    x.root = outdir; x.done = x.failed = 0;
    if (u8_listdir(outdir, convert_entry, &x)) {
        say("  ERROR: cannot read the folder %s: check -o.\n", outdir);
        return 2;
    }
    if (!x.done && !x.failed) say("  No box made by 261017 in %s: nothing to convert.\n", outdir);
    else say("\n  %d converted, %d not converted%s\n", x.done, x.failed,
             x.failed ? ": the reasons are above; a damaged box is never touched." : ".");
    return x.failed ? 1 : 0;
}

/* --free-space: what is superseded in set-aside/ of the closed boxes. The
 * folder is walked twice, once to list and once to delete, so nothing is
 * kept between the question and the answer */
typedef struct {
    const char *root; int del;
    int boxes, entries, kept, failed; long long bytes, kept_bytes;
} free_ctx;

static double mb(long long bytes) { return (double)bytes / (1024.0 * 1024.0); }

static void free_one(free_ctx *x, const char *bag)
{
    clean_plan p; arc_box *a; int i;
    if (boxclean_plan(bag, &p) || !p.n) { boxclean_free(&p); return; }
    x->boxes++; x->entries += p.n; x->bytes += p.bytes;
    x->kept += p.kept; x->kept_bytes += p.kept_bytes;
    if (x->del) {
        x->failed += boxclean_run(bag, &p, boxclean_rm_delete, NULL);
    } else {
        a = (arc_box *)calloc(1, sizeof(arc_box));
        say("  %s\n", a && arc_load(bag, a) ? a->title : bag);
        say("    in %s\n", bag);
        for (i = 0; i < p.n; i++)
            say("    %-44s %9.1f MB%s\n", p.item[i].name, mb(p.item[i].bytes),
                p.item[i].partial ? "  (its .txt logs stay)" : "");
        free(a);
    }
    boxclean_free(&p);
}

static void free_entry(void *c, const char *name, int isdir)
{
    free_ctx *x = (free_ctx *)c; char bag[1024];
    if (!isdir) return;
    u8_join(bag, sizeof(bag), x->root, name);
    free_one(x, bag);
}

/* 0 ok (also when nothing was deleted), 1 something could not be deleted, 2 error */
static int free_pass(free_ctx *x, const char *outdir, const char *bag, int del)
{
    memset(x, 0, sizeof(*x));
    x->root = outdir; x->del = del;
    if (bag) free_one(x, bag);
    else if (u8_listdir(outdir, free_entry, x)) {
        say("  ERROR: cannot read the folder %s: check -o.\n", outdir);
        return 2;
    }
    return 0;
}

static int run_free(const char *outdir, const char *title, const char *edition, const char *language)
{
    free_ctx x; char bag[1024], line[40]; strbuf why; clean_plan p; int rc;
    if (title[0]) {
        sb_init(&why);
        rc = session_resolve(outdir, title, edition, language, bag, sizeof(bag), &why);
        if (rc) say("  ERROR: %s\n", why.s ? why.s : "");
        sb_free(&why);
        if (rc) return 2;
        if (!u8_isdir(bag)) {
            say("  ERROR: there is no box \"%s\" in %s: check the title, the edition, the language and -o.\n", title, outdir);
            return 2;
        }
        rc = boxclean_plan(bag, &p);
        boxclean_free(&p);
        if (rc) {
            say("  ERROR: the box \"%s\" cannot be cleaned: it must be closed, in the current format\n"
                "  (dskrun --convert) and with no edit left halfway.\n", title);
            return 2;
        }
    }
    if (free_pass(&x, outdir, title[0] ? bag : NULL, 0)) return 2;
    if (!x.entries) {
        say("  Nothing to free in %s: no closed box has superseded reads in set-aside.\n", title[0] ? bag : outdir);
        return 0;
    }
    say("\n  To delete: %d in %d box(es), %.1f MB. They are reads identical to the one kept, and\n"
        "  duplicate or failed reads of media that ended clean. data, the manifests and\n"
        "  metadata.json are not touched: the boxes stay valid.\n", x.entries, x.boxes, mb(x.bytes));
    if (x.kept)
        say("  Staying in set-aside: %d (%.1f MB). They are damaged, replaced, rejected or removed\n"
            "  reads, or reads of a medium that did not end clean.\n", x.kept, mb(x.kept_bytes));
    say("\n  Delete them? They are deleted for good: there is no Recycle Bin here.  Y/N\n  > ");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin) || (line[0] != 'y' && line[0] != 'Y')) {
        say("  Nothing deleted.\n");
        return 0;
    }
    if (free_pass(&x, outdir, title[0] ? bag : NULL, 1)) return 2;
    if (x.failed) {
        say("  %d file(s) could not be deleted: is one open in another program? Close it and run the\n"
            "  same command again.\n", x.failed);
        return 1;
    }
    say("  Freed %.1f MB.\n", mb(x.bytes));
    return 0;
}

/* after a box is closed: if something can go, the command that does it */
static void free_hint(const char *outdir, const meta_box *box)
{
    char bag[1024]; strbuf why; clean_plan p;
    sb_init(&why);
    if (session_resolve(outdir, box->title, box->edition, box->language, bag, sizeof(bag), &why) == 0
        && boxclean_plan(bag, &p) == 0) {
        if (p.n) {
            say("    %.1f MB of superseded reads can go from set-aside:\n", mb(p.bytes));
            say("      dskrun --free-space -o \"%s\" -t \"%s\"", outdir, box->title);
            if (box->edition[0]) say(" -e \"%s\"", box->edition);
            if (box->language[0]) say(" -l \"%s\"", box->language);
            say("\n");
        }
        boxclean_free(&p);
    }
    sb_free(&why);
}

/* ---- the images of a closed box: --images, --add-image, --remove-image ---- */

/* the box of -t, with a change left halfway put in order first; NULL (and
 * the error printed) if there is none. bag: its folder */
static meta_box *image_box(const char *outdir, const char *title, const char *edition, const char *language,
                           char *bag, size_t n)
{
    char data[1040], fin[1040]; strbuf log; meta_box *m;
    sb_init(&log);
    if (session_resolve(outdir, title, edition, language, bag, n, &log)) {
        say("  ERROR: %s\n", log.s ? log.s : "");
        sb_free(&log);
        return NULL;
    }
    if (boxtxn_recover(bag, boxedit_tech, NULL, fin, sizeof(fin), &log) > 0) {
        say("  %s", log.s ? log.s : "");
        str_copy(bag, n, fin);
    }
    sb_clear(&log);
    u8_join(data, sizeof(data), bag, "data");
    m = meta_new();
    if (!m || meta_load(m, data, &log)) {
        say("  ERROR: there is no box \"%s\" in %s: check the title, the edition, the language and -o.\n", title, outdir);
        meta_delete(m);
        m = NULL;
    }
    sb_free(&log);
    return m;
}

static void image_names(const meta_box *m)
{
    int i;
    if (!m->nimage) { say("  The box has no images.\n"); return; }
    say("  Its images:");
    for (i = 0; i < m->nimage; i++) say(" %s", m->image[i].file);
    say("\n");
}

static int run_images(const char *outdir, const char *title, const char *edition, const char *language)
{
    char bag[1024], what[64]; int idx[META_MAXIMG], i;
    meta_box *m = image_box(outdir, title, edition, language, bag, sizeof(bag));
    if (!m) return 2;
    if (!m->nimage) say("  The box \"%s\" has no images: attach one with --add-image FILE --as WHAT.\n", m->title);
    else say("  Images of the box \"%s\": %d, in %s%sdata%simages\n", m->title, m->nimage, bag, U8_SEP, U8_SEP);
    boximg_order(m, idx);
    for (i = 0; i < m->nimage; i++) {
        const meta_image *im = &m->image[idx[i]];
        boximg_what_text(im, what, sizeof(what));
        say("    %-24s %-18s %9.1f KB", im->file, what, (double)im->bytes / 1024.0);
        if (im->note[0]) say("  %s", im->note);
        if (im->original[0]) say("  (was %s)", im->original);
        say("\n");
    }
    meta_delete(m);
    return 0;
}

#define WHAT_FORMS "box-front, box-back, box-spine, box-top, box-bottom, box-other, cd2-front, floppy3-back or insert"

static int run_add_image(const char *outdir, const char *title, const char *edition, const char *language,
                         const char *file, const char *as, const char *note, const char *reason)
{
    char bag[1024], what[64]; meta_image im; img_list l; img_result res; strbuf err; meta_box *m; int rc;
    if (boximg_parse_what(as, &im)) {
        say("  ERROR: --as \"%s\" does not say what the image shows: write %s.\n", as, WHAT_FORMS);
        return 2;
    }
    if (!(m = image_box(outdir, title, edition, language, bag, sizeof(bag)))) return 2;
    sb_init(&err);
    str_copy(im.note, sizeof(im.note), note);
    boximg_list_load(m, &l);
    rc = boximg_list_add(&l, m, file, &im, &err);
    if (rc == 0) rc = boximg_save(bag, &l, reason, NULL, &res, &err);
    if (rc) {
        say("  ERROR: %s.\n", err.s ? err.s : "the image was not attached");
        rc = 2;
    } else if (res.skipped) {
        say("  Not attached: it is the same file as an image the box already has (%s).\n", res.skipped_names);
    } else {
        meta_box *now = image_box(outdir, title, edition, language, bag, sizeof(bag));
        boximg_what_text(&im, what, sizeof(what));
        if (now && now->nimage)
            say("  Attached as %s: data%simages%s%s. The box is closed and verified.\n", what, U8_SEP, U8_SEP,
                now->image[now->nimage - 1].file);
        meta_delete(now);
    }
    boximg_list_free(&l);
    sb_free(&err);
    meta_delete(m);
    return rc;
}

static int run_remove_image(const char *outdir, const char *title, const char *edition, const char *language,
                            const char *name, const char *reason)
{
    char bag[1024]; img_list l; img_result res; strbuf err; meta_box *m; int i, rc = 2;
    if (!(m = image_box(outdir, title, edition, language, bag, sizeof(bag)))) return 2;
    sb_init(&err);
    boximg_list_load(m, &l);
    for (i = 0; i < l.n && strcmp(l.item[i].was, name); i++) ;
    if (i == l.n) {
        say("  ERROR: the box has no image \"%s\": write the name as --images lists it.\n", name);
        image_names(m);
    } else {
        boximg_list_remove(&l, i);
        if (boximg_save(bag, &l, reason, NULL, &res, &err))
            say("  ERROR: %s.\n", err.s ? err.s : "the image was not removed");
        else {
            say("  Removed: %s is now in set-aside (nothing is deleted). The box is closed and verified.\n", name);
            rc = 0;
        }
    }
    boximg_list_free(&l);
    sb_free(&err);
    meta_delete(m);
    return rc;
}

/* --reread: an edit left halfway is finished or undone first, then the
 * media to read are found in the box's own composition */
static int open_reread(session_cfg *cfg, meta_box *box, const char *outdir, const char *list)
{
    char bag[1024], data[1040], fin[1040]; strbuf log; meta_box *m = meta_new(); int rc = -1;
    sb_init(&log);
    if (session_resolve(outdir, box->title, box->edition, box->language, bag, sizeof(bag), &log)) {
        say("  ERROR: %s\n",log.s ? log.s : "");
        meta_delete(m);
        sb_free(&log);
        return -1;
    }
    /* the recovery of an interrupted edit may finish a rename: fin is where the box is now */
    if (boxtxn_recover(bag, boxedit_tech, NULL, fin, sizeof(fin), &log) > 0) {
        say("  %s", log.s ? log.s : "");
        str_copy(bag, sizeof(bag), fin);
    }
    sb_clear(&log);
    u8_join(data, sizeof(data), bag, "data");
    if (!m || meta_load(m, data, &log)) {
        say("  ERROR: there is no box \"%s\" in %s: check the title, the edition, the language and -o.\n", box->title, outdir);
    } else if (parse_reread(list, m, cfg->reread) == 0) {
        cfg->edit = 1;
        meta_set_counts(box, meta_count(m, MEDIA_FLOPPY), meta_count(m, MEDIA_CD), meta_count(m, MEDIA_DVD));
        rc = 0;
    }
    meta_delete(m);
    sb_free(&log);
    return rc;
}

static void usage(void)
{
    say("  usage: dskrun NUM_FLOPPIES -t TITLE [options]\n");
    say("         dskrun --reread MEDIA -t TITLE [-e EDITION] [-l LANGUAGE] [options]\n");
    say("         dskrun --convert [-t TITLE [-e EDITION] [-l LANGUAGE]] [-o DIR]\n");
    say("         dskrun --free-space [-t TITLE [-e EDITION] [-l LANGUAGE]] [-o DIR]\n");
    say("         dskrun --add-image FILE --as WHAT [--note TEXT] -t TITLE [-e ..] [-l ..] [-o DIR]\n");
    say("         dskrun --images -t TITLE [-e EDITION] [-l LANGUAGE] [-o DIR]\n");
    say("         dskrun --remove-image NAME -t TITLE [-e EDITION] [-l LANGUAGE] [-o DIR]\n\n");
    say("    --add-image FILE  attach a scan (JPEG, PNG, TIFF, BMP, GIF; a PDF as an\n");
    say("               insert) to a closed box: copied as it is into data\\images,\n");
    say("               with its fingerprint in the manifest. --as WHAT says what it\n");
    say("               shows: box-front, box-back, box-spine, box-top, box-bottom,\n");
    say("               box-other, cd2-front, floppy3-back, insert. --note TEXT adds\n");
    say("               a description, --reason TEXT goes in the history of the box\n");
    say("    --images   list the images of a box\n");
    say("    --remove-image NAME  take an image out of the box: it goes to\n");
    say("               set-aside, it is not deleted\n");
    say("    --free-space  list what is superseded in set-aside of the closed boxes\n");
    say("               (all those of DIR, or the one of -t) and ask before deleting\n");
    say("               it: reads identical to the one kept, and duplicate or failed\n");
    say("               reads of media that ended clean. Nothing else is touched\n");
    say("    --convert  bring the boxes made by 261017 to the current format\n");
    say("               (all those of DIR, or the one of -t); a damaged box\n");
    say("               is not touched. Exit code 1 if one was not converted\n");
    say("    --reread MEDIA  read again media of a closed box: floppy3,cd1,dvd2\n");
    say("               or missing. Each medium is saved on its own; the better\n");
    say("               read is kept and the other goes to set-aside/\n");
    say("    NUM_FLOPPIES  how many floppies the box has (even 0 with -C or -D)\n");
    say("    -f N       same as NUM_FLOPPIES: the floppies in the box\n");
    say("    -C N       the CDs in the box          (default 0)\n");
    say("    -D N       the DVDs in the box         (default 0)\n");
    say("    -t TITLE   title of the box: it becomes the package folder\n");
    say("    -e TEXT    edition or version: two boxes with the same title and\n");
    say("               different editions get two folders, TITLE (EDITION)\n");
    say("    -l TEXT    language of the box: the same title and edition in two\n");
    say("               languages are two boxes, TITLE (EDITION) [LANGUAGE]\n");
    say("    -P NAME    platform (MS-DOS, Amiga, ...)\n");
    say("    -p PATTERN floppy file names, with placeholders (default FLOPPY{nn})\n");
    say("    -o DIR     archive folder (default the current one)\n");
    say("    -s N       start from medium N (default 1; CDs come after floppies)\n");
    say("    -m N       maximum reads per disk      (default 3)\n");
    say("    -c N       force the cylinder count for the comparison\n");
    say("    -k         keep all reads, not only the masters\n");
    say("    -r         ask again for a failed disk at once: ENTER retries, S skips\n");
    say("    -x FILE    SAMdisk program to use      (default samdisk)\n");
    say("    -a SRC     where to read from: drive or image file (default a:)\n");
    say("    -X FILE    DiscImageCreator program    (default DiscImageCreator)\n");
    say("    -d DRIVE   CD and DVD drive            (default d:)\n");
    say("    -R DIR     folder of the redump DATs   (default the dskrun folder)\n");
    say("    --copy-protection=auto|on|off\n");
    say("               look for CD copy protections (DiscImageCreator /sf); auto:\n");
    say("               only if the disc has files of a known protection (default)\n");
    say("    --drive-offset=N\n");
    say("               the CD drive's read offset in samples, for a drive that\n");
    say("               DiscImageCreator does not know (AccurateRip or redump list)\n");
    say("    --cd-speed=N\n");
    say("               CD read speed, 1 to 48 (default 16): 4 or 8 for a scratched\n");
    say("               CD that DiscImageCreator stops on with C2 errors\n\n");
    say("  Pattern placeholders:\n");
    say("    {n} {nn} {nnn}   floppy number, with leading zeros\n");
    say("    {set}            the title\n");
    say("    {date}           today's date\n\n");
    say("  Examples:\n");
    say("    dskrun 4 -t \"Monkey Island\" -P MS-DOS -o D:\\Archive\n");
    say("    dskrun 3 -C 1 -t \"Windows NT Workstation 4.0\" -P \"Windows NT\"\n\n");
    say("  Each box becomes a BagIt package in DIR\\TITLE (EDITION), with\n");
    say("  the images, the scans and metadata.json. If you stop it (Q), run\n");
    say("  the same command again to resume from the missing disk.\n");
    say("  CDs and DVDs are asked for after the floppies and read with\n");
    say("  DiscImageCreator, each into its own folder (data\\CD01\\...):\n");
    say("  if the disc is in the redump DAT one read is enough, otherwise\n");
    say("  two are made and compared.\n\n");
    say("  Exit code: 0 box closed, 1 left open, 2 error.\n\n");
}

int main(int argc, char **argv)
{
    session_cfg cfg; session_host host; meta_box *box;
    char title[256] = "", edition[128] = "", language[128] = "", platform[64] = "unspecified", tpl[256] = "FLOPPY{nn}", outdir[512] = ".";
    char samdisk[512] = "", source[512] = "", comp[80], dic[512] = "", drive[16] = "", dats[512] = "";
    char reread[256] = "", add_image[1024] = "", image_as[64] = "", image_note[256] = "";
    char remove_image[128] = "", reason[256] = "";
    int list_images = 0;
    int convert = 0, free_space = 0;
    int i, rc, tot = 0, ncd = 0, ndvd = 0;

    say("\ndskrun 2.2 - batch archiving of floppies, CDs and DVDs\n\n");
    memset(&cfg, 0, sizeof(cfg));
    memset(&host, 0, sizeof(host));
    cfg.maxread = 3; cfg.start = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc)      to_utf8(argv[++i], tpl, sizeof(tpl), ARG_CP);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) to_utf8(argv[++i], outdir, sizeof(outdir), ARG_CP);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) to_utf8(argv[++i], title, sizeof(title), ARG_CP);
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) to_utf8(argv[++i], platform, sizeof(platform), ARG_CP);
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) to_utf8(argv[++i], edition, sizeof(edition), ARG_CP);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) to_utf8(argv[++i], language, sizeof(language), ARG_CP);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) cfg.start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) cfg.maxread = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cfg.forced_cyl = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) tot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-C") && i + 1 < argc) ncd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) ndvd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) to_utf8(argv[++i], samdisk, sizeof(samdisk), ARG_CP);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) to_utf8(argv[++i], source, sizeof(source), ARG_CP);
        else if (!strcmp(argv[i], "-X") && i + 1 < argc) to_utf8(argv[++i], dic, sizeof(dic), ARG_CP);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) to_utf8(argv[++i], drive, sizeof(drive), ARG_CP);
        else if (!strcmp(argv[i], "-R") && i + 1 < argc) to_utf8(argv[++i], dats, sizeof(dats), ARG_CP);
        else if (!strcmp(argv[i], "--reread") && i + 1 < argc) to_utf8(argv[++i], reread, sizeof(reread), ARG_CP);
        else if (!strcmp(argv[i], "--add-image") && i + 1 < argc) to_utf8(argv[++i], add_image, sizeof(add_image), ARG_CP);
        else if (!strcmp(argv[i], "--as") && i + 1 < argc) to_utf8(argv[++i], image_as, sizeof(image_as), ARG_CP);
        else if (!strcmp(argv[i], "--note") && i + 1 < argc) to_utf8(argv[++i], image_note, sizeof(image_note), ARG_CP);
        else if (!strcmp(argv[i], "--reason") && i + 1 < argc) to_utf8(argv[++i], reason, sizeof(reason), ARG_CP);
        else if (!strcmp(argv[i], "--remove-image") && i + 1 < argc) to_utf8(argv[++i], remove_image, sizeof(remove_image), ARG_CP);
        else if (!strcmp(argv[i], "--images"))           list_images = 1;
        else if (!strcmp(argv[i], "--convert"))          convert = 1;
        else if (!strcmp(argv[i], "--free-space"))       free_space = 1;
        else if (!strcmp(argv[i], "-k"))                 cfg.keepall = 1;
        else if (!strcmp(argv[i], "-r"))                 host.ask_again = 1;
        else if (!strncmp(argv[i], "--copy-protection=", 18)) {
            if (meta_prot_parse(argv[i] + 18) < 0) { usage(); return 2; }
            cfg.copy_protection = argv[i] + 18;
        } else if (!strncmp(argv[i], "--drive-offset=", 15)) {
            if (optical_set_drive_offset(argv[i] + 15)) { usage(); return 2; }
        } else if (!strncmp(argv[i], "--cd-speed=", 11)) {
            if (optical_set_cd_speed(atoi(argv[i] + 11))) { usage(); return 2; }
        }
        else if (argv[i][0] != '-')                      tot = atoi(argv[i]);
    }
    if (cfg.maxread < 2) cfg.maxread = 2;
    if (cfg.maxread > META_MAXREAD) cfg.maxread = META_MAXREAD;
    if (convert) return run_convert(outdir, title, edition, language);
    if (free_space) return run_free(outdir, title, edition, language);
    if (add_image[0] || remove_image[0] || list_images) {
        if (!title[0]) { usage(); return 2; }
        if (list_images) return run_images(outdir, title, edition, language);
        if (remove_image[0]) return run_remove_image(outdir, title, edition, language, remove_image, reason);
        return run_add_image(outdir, title, edition, language, add_image, image_as, image_note, reason);
    }
    if (!title[0] || (!reread[0] && (tot < 0 || ncd < 0 || ndvd < 0 || tot + ncd + ndvd < 1 ||
                                     tot + ncd + ndvd > META_MAXDISK))) { usage(); return 2; }

    box = meta_new();
    if (!box) { say("  ERROR: out of memory\n"); return 2; }
    str_copy(box->title, sizeof(box->title), title);
    str_copy(box->platform, sizeof(box->platform), platform);
    str_copy(box->edition, sizeof(box->edition), edition);
    str_copy(box->language, sizeof(box->language), language);
    meta_set_counts(box, tot, ncd, ndvd);
    g_box = box;
    if (samdisk[0]) floppy_set_samdisk(samdisk);
    if (source[0]) {
        floppy_set_source(source);
        say("  Reading from: %s\n", source);
    }

    cfg.box = box; cfg.outroot = outdir;
    cfg.names[MEDIA_FLOPPY] = tpl;          /* CD and DVD: CD{nn}, DVD{nn} */
    cfg.media[MEDIA_FLOPPY] = &floppy_media;
    cfg.media[MEDIA_CD] = &cd_media;
    cfg.media[MEDIA_DVD] = &dvd_media;
    if (dic[0]) optical_set_dic(dic);
    if (drive[0]) optical_set_drive(drive);
    if (dats[0]) optical_set_datdir(dats);
    host.event = on_event; host.wait_disk = wait_disk; host.ctx = NULL;
    host.choose = choose;
    if (reread[0] && open_reread(&cfg, box, outdir, reread)) { meta_delete(box); return 2; }

    meta_composition(box, comp, sizeof(comp));
    say("  Box: %s\n", title);
    say("  %s, at most %d reads per medium.\n", comp, cfg.maxread);
    if (ncd || ndvd) {
        char dat[600], date[40];
        if (optical_find_dat(dat, sizeof(dat), date, sizeof(date)))
            say("  Redump DAT: %s (%s)\n", dat, date[0] ? date : "undated");
        else
            say("  No redump DAT: every CD and DVD is read twice.\n");
    }
    say("  Archive folder: %s\n", outdir);
    catch_ctrlc();
    rc = session_run(&cfg, &host);

    beep2();
    say("\n  ============================================================\n");
    say("    %s\n", rc == SESSION_CLOSED ? (cfg.edit ? "Box edited: closed and verified." : "Box closed.") :
                       rc == SESSION_OPEN ? "Box left open: you can resume it." :
                       "Archiving failed: read the error above.");
    say("    %d clean, %d marginal, %d with errors, %d skipped or to redo\n",
           nclean, nmarginal, nerror, nother);
    if (rc == SESSION_CLOSED) free_hint(outdir, box);
    say("  ============================================================\n\n");
    meta_delete(box);
    return rc;
}
