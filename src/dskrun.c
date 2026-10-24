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
static int run_convert(const char *outdir, const char *title, const char *edition)
{
    convert_ctx x; char bag[1024]; int rc;
    if (title[0]) {
        session_bagdir(bag, sizeof(bag), outdir, title, edition);
        if (!u8_isdir(bag)) {
            say("  ERROR: there is no box \"%s\" in %s: check the title, the edition and -o.\n", title, outdir);
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

/* --reread: an edit left halfway is finished or undone first, then the
 * media to read are found in the box's own composition */
static int open_reread(session_cfg *cfg, meta_box *box, const char *outdir, const char *list)
{
    char bag[1024], data[1040], fin[1040]; strbuf log; meta_box *m = meta_new(); int rc = -1;
    sb_init(&log);
    session_bagdir(bag, sizeof(bag), outdir, box->title, box->edition);
    if (boxtxn_recover(bag, boxedit_tech, NULL, fin, sizeof(fin), &log) > 0) {
        say("  %s", log.s ? log.s : "");
        session_bagdir(bag, sizeof(bag), outdir, box->title, box->edition);
    }
    u8_join(data, sizeof(data), bag, "data");
    if (!m || meta_load(m, data, &log)) {
        say("  ERROR: there is no box \"%s\" in %s: check the title, the edition and -o.\n", box->title, outdir);
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
    say("         dskrun --reread MEDIA -t TITLE [-e EDITION] [options]\n");
    say("         dskrun --convert [-t TITLE [-e EDITION]] [-o DIR]\n\n");
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
    char title[256] = "", edition[128] = "", platform[64] = "unspecified", tpl[256] = "FLOPPY{nn}", outdir[512] = ".";
    char samdisk[512] = "", source[512] = "", comp[80], dic[512] = "", drive[16] = "", dats[512] = "";
    char reread[256] = "";
    int convert = 0;
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
        else if (!strcmp(argv[i], "--convert"))          convert = 1;
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
    if (convert) return run_convert(outdir, title, edition);
    if (!title[0] || (!reread[0] && (tot < 0 || ncd < 0 || ndvd < 0 || tot + ncd + ndvd < 1 ||
                                     tot + ncd + ndvd > META_MAXDISK))) { usage(); return 2; }

    box = meta_new();
    if (!box) { say("  ERROR: out of memory\n"); return 2; }
    str_copy(box->title, sizeof(box->title), title);
    str_copy(box->platform, sizeof(box->platform), platform);
    str_copy(box->edition, sizeof(box->edition), edition);
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
    say("  ============================================================\n\n");
    meta_delete(box);
    return rc;
}
