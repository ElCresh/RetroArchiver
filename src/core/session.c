#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif
#include "session.h"
#include "bagit.h"
#include "fsu8.h"
#include "runner.h"
#include "sha256.h"

#define CP(dst, src) str_copy(dst, sizeof(dst), src)

typedef struct {
    const session_cfg *cfg;
    const session_host *host;
    char bagdir[1024], datadir[1024];
    int cur;                    /* disk being processed, for the events */
    char last[640];             /* last line of the reader: the reason for MEDIA_NOTOOL */
    int stuck;                  /* a leftover in data/ cannot be removed */
    const media_ops *mcur;      /* who is reading: its lines can open a phase */
    char phase[64];             /* the last announced phase */
    int prot_retry;             /* the disk to read again at once, with or without the scan */
    int prot_retry_on;          /* 1 with the copy protection scan, 0 without */
    strbuf msg;
} run_ctx;

void session_expand(char *out, size_t n, const char *tpl, int disk, const char *title)
{
    strbuf b; const char *p = tpl; char now[20];
    sb_init(&b);
    while (*p) {
        if      (!strncmp(p, "{nnn}", 5)) { sb_printf(&b, "%03d", disk); p += 5; }
        else if (!strncmp(p, "{nn}", 4))  { sb_printf(&b, "%02d", disk); p += 4; }
        else if (!strncmp(p, "{n}", 3))   { sb_printf(&b, "%d", disk);   p += 3; }
        else if (!strncmp(p, "{set}", 5)) { sb_puts(&b, title);          p += 5; }
        else if (!strncmp(p, "{date}", 6) || !strncmp(p, "{data}", 6)) {
            meta_now(now); now[10] = 0; sb_puts(&b, now); p += 6;
        } else {
            char c[2]; c[0] = *p++; c[1] = 0;
            sb_puts(&b, c);
        }
    }
    str_copy(out, n, b.s);
    sb_free(&b);
}

/* 1 if the bag in bagdir was opened with this edition */
static int bag_has_edition(const char *bagdir, const char *edition)
{
    char data[1040]; meta_box *m = meta_new(); strbuf err; int same = 0;
    if (!m) return 0;
    sb_init(&err);
    u8_join(data, sizeof(data), bagdir, "data");
    if (meta_load(m, data, &err) == 0) same = !strcmp(m->edition, edition);
    meta_delete(m);
    sb_free(&err);
    return same;
}

/* Two editions of the same title are two boxes: "Microsoft Office (97)"
 * and "Microsoft Office (2000)". Up to 261017 the folder was the title
 * alone: a box made that way keeps its folder if it has the same edition. */
void session_bagdir(char *out, size_t n, const char *outroot, const char *title, const char *edition)
{
    char name[96], ed[48], both[150];
    bag_dirname(title, name, 81);
    u8_join(out, n, outroot, name);
    if (!edition || !edition[0] || (u8_exists(out) && bag_has_edition(out, edition))) return;
    /* the edition is never cut away by a long title: it is what tells them apart */
    bag_dirname(edition, ed, 41);
    bag_dirname(title, name, 81 - strlen(ed) - 3);
    snprintf(both, sizeof(both), "%s (%s)", name, ed);
    u8_join(out, n, outroot, both);
}

static const char *const default_names[MEDIA_KINDS] = { "FLOPPY{nn}", "CD{nn}", "DVD{nn}" };

/* "floppy 3", "CD 1": the medium at position n */
static const char *name_of(const meta_box *b, int n, char *out, size_t len)
{
    int kind, num;
    meta_slot(b, n, &kind, &num);
    meta_disk_name(kind, num, out, len);
    return out;
}

const char *session_name_tpl(const meta_box *b, const char *const names[MEDIA_KINDS], int kind)
{
    if (b->names[kind][0]) return b->names[kind];
    if (names && names[kind] && names[kind][0]) return names[kind];
    return default_names[kind];
}

void session_disk_base(const meta_box *b, const char *const names[MEDIA_KINDS], int kind, int num,
                       char *base, size_t n)
{
    char exp[256], name[96];
    session_expand(exp, sizeof(exp), session_name_tpl(b, names, kind), num, b->title);
    bag_dirname(exp, name, 81);
    str_copy(base, n, name);
}

static const char *tpl_for(const run_ctx *r, int kind)
{
    return session_name_tpl(r->cfg->box, r->cfg->names, kind);
}

static void disk_base(const run_ctx *r, int kind, int num, char base[96])
{
    session_disk_base(r->cfg->box, r->cfg->names, kind, num, base, 96);
}

static void ev(run_ctx *r, event_kind k, int disk, const char *fmt, ...)
{
    strbuf b; va_list ap;
    sb_init(&b);
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    if (k == EV_PHASE) CP(r->phase, b.s);
    /* a sentence that starts with a medium's name ("floppy 3 is...") gets its capital */
    if ((k == EV_LINE || k == EV_ERROR) && b.s && b.s[0] >= 'a' && b.s[0] <= 'z') b.s[0] = (char)(b.s[0] - 'a' + 'A');
    r->host->event(r->host->ctx, k, disk, b.s);
    sb_free(&b);
}

static void on_line(void *c, const char *line)
{
    run_ctx *r = (run_ctx *)c; const char *ph;
    CP(r->last, line);
    /* DiscImageCreator moves on its own from reading to the C2 check */
    if (r->mcur && r->mcur->phase_of && (ph = r->mcur->phase_of(line)) != NULL && strcmp(ph, r->phase))
        ev(r, EV_PHASE, r->cur, "%s", ph);
    r->host->event(r->host->ctx, EV_LINE, r->cur, line);
}

/* the technical data is known by whoever reads that kind of medium */
static void tech_of(void *ctx, const meta_disk *d, const char *img, json_writer *w)
{
    const media_ops *m = ((run_ctx *)ctx)->cfg->media[d->kind];
    if (m && m->tech) m->tech(NULL, img, w);
}

static int save(run_ctx *r)
{
    sb_clear(&r->msg);
    if (meta_save(r->cfg->box, r->datadir, tech_of, r, &r->msg)) {
        r->host->event(r->host->ctx, EV_ERROR, r->cur, r->msg.s);
        return -1;
    }
    return 0;
}

/* fingerprint and size taken right away: bag_close checks them again */
static int record(run_ctx *r, meta_disk *d, const char *file)
{
    char path[1024]; meta_read *m;
    if (d->nreads >= META_MAXREAD) return -1;
    m = &d->reads[d->nreads];
    u8_join(path, sizeof(path), r->datadir, file);
    if (sha256_path(path, m->sha256, &m->bytes)) return -1;
    CP(m->file, file);
    meta_now(m->at);
    d->nreads++;
    return 0;
}

static void write_report(run_ctx *r, meta_disk *d, const char *base, const char *text)
{
    char name[128], path[1024]; FILE *f;
    snprintf(name, sizeof(name), "%s.diff.txt", base);
    u8_join(path, sizeof(path), r->datadir, name);
    f = u8_fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    if (fclose(f) == 0) CP(d->report, name);
}

static void csv_row(run_ctx *r, const meta_disk *d)
{
    char p[1024], ts[20], title[256]; FILE *f; long long sz; char *q;
    u8_join(p, sizeof(p), r->cfg->outroot, SESSION_CSV);
    sz = u8_filesize(p);
    f = u8_fopen(p, "ab");
    if (!f) { ev(r, EV_LINE, d->n, "WARNING: cannot write %s", p); return; }
    if (sz <= 0) fputs("disk;master;reads;status;when;box\n", f);
    meta_now(ts); ts[10] = ' '; ts[16] = 0;        /* "2026-10-15 21:04" as before */
    CP(title, r->cfg->box->title);
    for (q = title; *q; q++) if (*q == ';') *q = ',';
    fprintf(f, "%d;%s;%d;%s;%s;%s\n", d->n, d->master, d->nreads, d->status, ts, title);
    fclose(f);
}

/* free name for a new read: base.dsk, then base_b.dsk, base_c.dsk...
 * Reads already done are never touched: on a dying disk they may be the
 * only good ones. -1 if there are already META_MAXREAD. */
static int free_slot(const meta_disk *d, const char *base, const char *ext, char *file, size_t n)
{
    int s, k;
    if (d->nreads >= META_MAXREAD) return -1;
    for (s = 0; s < META_MAXREAD; s++) {
        if (s == 0) snprintf(file, n, "%s%s", base, ext);
        else snprintf(file, n, "%s_%c%s", base, 'a' + s, ext);
        for (k = 0; k < d->nreads && strcmp(d->reads[k].file, file); k++) ;
        if (k == d->nreads) return 0;
    }
    return -1;
}

/* Tests only: with RETROARCHIVER_CONFRONTO_LENTO=ms the comparison waits
 * before starting. With the fake SAMdisk it takes an instant, and one
 * could not see what happens when pressing "Interrompi" (or Ctrl+C)
 * during it. */
#ifdef _WIN32
static void test_pause(void)
{
    const char *ms = getenv("RETROARCHIVER_CONFRONTO_LENTO");
    if (ms && atoi(ms) > 0) Sleep((DWORD)atoi(ms));
}
#else
static void test_pause(void)
{
    const char *ms = getenv("RETROARCHIVER_CONFRONTO_LENTO");
    struct timespec t;
    if (!ms || atoi(ms) <= 0) return;
    t.tv_sec = atoi(ms) / 1000; t.tv_nsec = (long)(atoi(ms) % 1000) * 1000000L;
    while (nanosleep(&t, &t) && errno == EINTR) ;  /* Ctrl+C does not shorten the comparison */
}
#endif

/* compares all reads of the disk, old and new; 0 1 2 like compare */
static int compare_reads(run_ctx *r, meta_disk *d, const char *base)
{
    char paths[META_MAXREAD][1024]; const char *imgs[META_MAXREAD], *names[META_MAXREAD];
    compare_result res; int i, cmp;

    for (i = 0; i < d->nreads; i++) {
        u8_join(paths[i], sizeof(paths[0]), r->datadir, d->reads[i].file);
        imgs[i] = paths[i];
        names[i] = d->reads[i].file;
    }
    /* a single read is compared only against the redump DAT */
    ev(r, EV_PHASE, d->n, d->nreads == 1 ? "Redump" : "Compare");
    test_pause();
    compare_init(&res);
    res.names = names;          /* in the package's .diff.txt the names in data/, not the PC's paths */
    cmp = r->cfg->media[d->kind]->compare(imgs, d->nreads, r->cfg->forced_cyl, &res);
    write_report(r, d, base, res.detail.s);
    CP(d->verdict, res.verdict_key[0] ? res.verdict_key : verdict_name(res.v));
    d->agree = res.agree;
    d->unstable_tracks = res.unstable_tracks;
    if (res.outcome[0]) CP(d->status, res.outcome);
    else CP(d->status, cmp == 0 ? ST_CLEAN : cmp == 1 ? ST_MARGINAL : ST_ERROR);
    /* the read redump knows becomes the master (the others are removed) */
    if (res.master > 0 && res.master <= d->nreads) CP(d->master, d->reads[res.master - 1].file);
    compare_free(&res);
    return cmp;
}

/* clean disk: the master is enough, the other reads are removed */
static void keep_master_only(run_ctx *r, meta_disk *d)
{
    char path[1024]; int i, k = 0;
    for (i = 0; i < d->nreads; i++) {
        if (!strcmp(d->reads[i].file, d->master)) { d->reads[k++] = d->reads[i]; continue; }
        u8_join(path, sizeof(path), r->datadir, d->reads[i].file);
        u8_rmtree(path);
    }
    d->nreads = k;
}

/* one disk: scan, reads, comparisons, outcome. If the disk had already
 * been attempted, the new reads are added to the old ones.
 * MEDIA_NOTOOL if the reading program does not start: the disk stays as
 * it was. */
/* label not typed: the volume label of the disk read, if it has one */
static void label_from_disk(run_ctx *r, meta_disk *d, const char *who)
{
    const media_ops *m = r->cfg->media[d->kind];
    char path[1024], lab[128];
    if (d->label[0] || !d->master[0] || !m->volume_label) return;
    u8_join(path, sizeof(path), r->datadir, d->master);
    if (!m->volume_label(path, lab, sizeof(lab))) return;
    CP(d->label, lab);
    ev(r, EV_LINE, d->n, "Label of %s taken from the disk: %s", who, lab);
}

static int move_aside(run_ctx *r, const char *name, const char *suffix, char *rel, size_t n);

/* After a failed read nothing half-written may stay in data/. What the
 * read left (for a CD: DiscImageCreator's logs and dic-console.txt) is
 * what explains the failure: it goes to set-aside as FILE.failed instead
 * of being deleted. An interrupted read (rc 1) was stopped by the user and
 * is simply removed. */
static void keep_failed(run_ctx *r, const meta_disk *d, const char *file, const char *path, int rc)
{
    char rel[400];
    if (!u8_exists(path)) return;
    if (rc != 1 && move_aside(r, file, SESSION_FAILED, rel, sizeof(rel)) == 0) {
        ev(r, EV_LINE, d->n, "What the failed read left (the reading program's logs) is in %s.", rel);
        return;
    }
    u8_rmtree(path);
}

/* The safety net of the copy protection scan, after a read that failed or
 * has errors:
 * - made without the scan (the files on the disc said nothing): a
 *   protection without signature files can do that, the disc is read again
 *   with it. Not if the box turned it off: that is the user's choice;
 * - made with it, and the drive reset itself over and over (seen on the PC:
 *   UNIT_ATTENTION, timeouts, a minute's wait each): the scan is the cause,
 *   the disc is read again without it.
 * The choice stays on the disk from then on. A read without the scan where
 * the drive reset itself gets nothing: the scan would make it worse */
static void protection_net(run_ctx *r, meta_disk *d, const char *who)
{
    const media_ops *m = r->cfg->media[d->kind];
    int failed = !strcmp(d->status, ST_FAILED), used;
    if (!m->protection || !m->protection_used) return;
    if (!failed && strcmp(d->status, ST_ERRORS)) return;
    used = m->protection_used();
    if (used == 2 && d->copy_protection != META_PROT_OFF) {
        d->copy_protection = META_PROT_OFF;
        r->prot_retry = d->n;
        r->prot_retry_on = 0;
        ev(r, EV_LINE, d->n, "The copy protection scan made the drive reset itself while reading %s: "
                             "reading it again without it.", who);
        return;
    }
    if (used != 0 || r->cfg->box->copy_protection == META_PROT_OFF || d->copy_protection) return;
    d->copy_protection = META_PROT_ON;
    r->prot_retry = d->n;
    r->prot_retry_on = 1;
    ev(r, EV_LINE, d->n, "The read of %s without the copy protection scan %s: reading it again looks "
                         "for protections (DiscImageCreator /sf).", who, failed ? "failed" : "has errors");
}

static int process(run_ctx *r, meta_disk *d)
{
    const session_cfg *c = r->cfg; const media_ops *m = c->media[d->kind];
    char base[96], name[128], path[1024], file[128], before[24], who[32];
    int pass, rc, cmp = -1, bad = 0, limit = c->maxread, first = 0;

    runner_clear();                         /* the user has just said Read */
    CP(before, d->status);
    r->prot_retry = 0;

    disk_base(r, d->kind, d->num, base);
    meta_disk_name(d->kind, d->num, who, sizeof(who));
    r->cur = d->n;
    if (d->nreads)
        ev(r, EV_LINE, d->n, "%s already has %d reads: keeping them and comparing them with the new ones.",
           who, d->nreads);

    r->mcur = m;
    ev(r, EV_PHASE, d->n, "%s", m->scan_phase ? m->scan_phase : "Scan");
    snprintf(name, sizeof(name), "%s.scan.txt", base);
    u8_join(path, sizeof(path), r->datadir, name);
    rc = m->scan(path, on_line, r);
    if (rc == MEDIA_NOTOOL) {
        if (!strcmp(d->scan, name)) d->scan[0] = 0;
        return MEDIA_NOTOOL;
    }
    if (rc == 1) { CP(d->status, ST_INTERRUPTED); return 0; }
    if (rc == 0) CP(d->scan, name);
    else ev(r, EV_LINE, d->n, "WARNING: the scan did not start, going on with the read.");

    /* the scan is a survey: if the disk already has damaged sectors every
     * extra pass risks finishing it off, so a single read that insists on
     * the bad sectors */
    if (rc == 0 && m->problems && (bad = m->problems(path)) > 0) {
        ev(r, EV_LINE, d->n, "The survey found bad sectors on %d tracks: "
                             "fragile disk, a single read with heavy retries.", bad);
        limit = 1;
        if (m->careful) m->careful(1);
    }

    CP(d->status, ST_ERROR);
    for (pass = 0; pass < limit; pass++) {
        if (free_slot(d, base, m->ext, file, sizeof(file))) {
            ev(r, EV_LINE, d->n, "WARNING: %s already has %d reads, the maximum: "
                                 "keeping those and making no more.", who, d->nreads);
            if (cmp < 0 && d->nreads >= 2) cmp = compare_reads(r, d, base);
            break;
        }
        u8_join(path, sizeof(path), r->datadir, file);
        u8_rmtree(path);                    /* unrecorded leftover of an interruption */

        ev(r, EV_PHASE, d->n, "Read %d", d->nreads + 1);
        if (m->protection) m->protection(d->copy_protection ? d->copy_protection : c->box->copy_protection);
        rc = m->read(path, on_line, r);
        if (rc == MEDIA_NOTOOL) {
            if (m->careful) m->careful(0);
            CP(d->status, before);
            return MEDIA_NOTOOL;
        }
        if (rc != 0) {
            keep_failed(r, d, file, path, rc);  /* no half-written files in the bag */
            CP(d->status, rc == 1 ? ST_INTERRUPTED : ST_FAILED);
            break;
        }
        if (record(r, d, file)) {
            ev(r, EV_ERROR, d->n, "cannot read back %s, just written: "
                                  "check the destination disk.", file);
            u8_rmtree(path);
            CP(d->status, ST_FAILED);
            break;
        }
        if (!d->master[0]) CP(d->master, file);
        if (d->nreads < 2 && !m->one_read) continue;

        cmp = compare_reads(r, d, base);
        if (d->nreads == 1) {
            /* the CD is in the redump DAT: the read is already verified */
            first = cmp;            /* 1: good but to be confirmed; 2: broken read */
            if (cmp == 0) break;
            if (pass + 1 < limit)
                ev(r, EV_LINE, d->n, "%s is not verified by redump: making a second read "
                                     "to compare the two.", who);
            continue;
        }
        if (cmp != 1) break;
        if (pass + 1 < limit)
            ev(r, EV_LINE, d->n, "The reads do not match: making another read.");
    }
    if (m->careful) m->careful(0);

    /* fragile: one successful read and no comparison possible */
    if (bad > 0 && !strcmp(d->status, ST_ERROR) && d->nreads == 1) {
        CP(d->status, "fragile");
        ev(r, EV_LINE, d->n, "%s was read only once: not verified by a comparison.", who);
    }
    /* CD: the second read failed, the first stays good */
    if (m->one_read && d->nreads == 1 && first == 1 && !strcmp(d->status, ST_FAILED)) {
        CP(d->status, ST_READ_ONCE);
        ev(r, EV_LINE, d->n, "%s was read only once: not verified by a comparison.", who);
    }
    label_from_disk(r, d, who);
    protection_net(r, d, who);

    if ((!strcmp(d->status, ST_CLEAN) || !strcmp(d->status, ST_REDUMP)) && !c->keepall)
        keep_master_only(r, d);
    return 0;
}

static int referenced(const meta_box *b, const char *name)
{
    int i, k;
    if (!strcmp(name, "metadata.json")) return 1;
    for (i = 0; i < b->ndisk; i++) {
        const meta_disk *d = &b->disk[i];
        if (!strcmp(name, d->scan) || !strcmp(name, d->report)) return 1;
        for (k = 0; k < d->nreads; k++) if (!strcmp(name, d->reads[k].file)) return 1;
    }
    return 0;
}

static int same_name(const char *a, const char *b);

/* the name of a possible read of an optical medium in the box: the base
 * (CD01) or the base with _b, _c... as free_slot names them */
static int read_dir_name(const run_ctx *r, const char *name)
{
    const meta_box *b = r->cfg->box; char base[96]; int kind, num, s; size_t k;
    for (kind = MEDIA_CD; kind < MEDIA_KINDS; kind++)
        for (num = 1; num <= meta_count(b, kind); num++) {
            disk_base(r, kind, num, base);
            k = strlen(base);
            if (!same_name(name, base) && !(strlen(name) == k + 2 && !strncmp(name, base, k) && name[k] == '_'))
                continue;
            if (same_name(name, base)) return 1;
            for (s = 1; s < META_MAXREAD; s++) if (name[k + 1] == 'a' + s) return 1;
        }
    return 0;
}

/* Reads that no longer match their fingerprint stay out of the payload,
 * in a tag folder of the bag next to data/: BagIt 1.0 (RFC 8493, 2.2.4)
 * allows other tag files, in subfolders too, and verifiers ignore those
 * the tagmanifest does not list. So they stay with their box without
 * ending up in the data manifest.
 * name is an entry of data/, suffix SESSION_DAMAGED or SESSION_DUPLICATE;
 * rel gets the new path, relative to the bag */
const char *session_aside_dir(const char *bagdir)
{
    char p[1024];
    u8_join(p, sizeof(p), bagdir, SESSION_ASIDE_DIR);
    if (u8_isdir(p)) return SESSION_ASIDE_DIR;
    u8_join(p, sizeof(p), bagdir, SESSION_ASIDE_DIR_OLD);
    return u8_isdir(p) ? SESSION_ASIDE_DIR_OLD : SESSION_ASIDE_DIR;
}

static int move_aside(run_ctx *r, const char *name, const char *suffix, char *rel, size_t n)
{
    char from[1024], dir[1024], to[1200], file[300]; int j;
    const char *aside = session_aside_dir(r->bagdir);
    u8_join(from, sizeof(from), r->datadir, name);
    u8_join(dir, sizeof(dir), r->bagdir, aside);
    if (u8_mkdir(dir)) return -1;
    if (strstr(name, suffix)) CP(file, name);              /* already set aside before */
    else snprintf(file, sizeof(file), "%s%s", name, suffix);
    u8_join(to, sizeof(to), dir, file);
    for (j = 2; u8_exists(to) && j < 100; j++) {
        char alt[320];
        snprintf(alt, sizeof(alt), "%s%d", file, j);
        u8_join(to, sizeof(to), dir, alt);
    }
    if (u8_exists(to) || u8_rename_replace(from, to)) return -1;
    snprintf(rel, n, "%s/%s", aside, to + strlen(dir) + 1);
    return 0;
}

static void orphan(void *c, const char *name, int isdir)
{
    run_ctx *r = (run_ctx *)c; char p[1024], rel[400];
    if (referenced(r->cfg->box, name)) return;
    /* a damaged read left in data/ by an earlier version: out of the payload */
    if (strstr(name, SESSION_DAMAGED) || strstr(name, SESSION_DAMAGED_OLD)) {
        if (move_aside(r, name, strstr(name, SESSION_DAMAGED) ? SESSION_DAMAGED : SESSION_DAMAGED_OLD,
                       rel, sizeof(rel)) == 0) {
            ev(r, EV_LINE, 0, "Moving data/%s to %s: out of the data package.", name, rel);
            return;
        }
        r->stuck = 1;
        ev(r, EV_ERROR, 0, "cannot move data/%s to %s: is it open in another program? "
                           "close it and start again.", name, session_aside_dir(r->bagdir));
        return;
    }
    /* a folder is removed only if it is a half-done read of a CD or DVD
     * (CD01, CD01_b...): RetroArchiver did not make the others */
    if (isdir && !read_dir_name(r, name)) {
        ev(r, EV_LINE, 0, "Leaving data/%s: it is not a read of this box, but it will end up in the "
                          "package. If it is not needed, move it out of data.", name);
        return;
    }
    u8_join(p, sizeof(p), r->datadir, name);
    if ((isdir ? u8_rmtree(p) : u8_remove(p)) == 0) {
        ev(r, EV_LINE, 0, "Removing data/%s: it is not in metadata.json "
                          "(left over from an interruption).", name);
        return;
    }
    r->stuck = 1;
    ev(r, EV_ERROR, 0, "cannot remove data/%s: is it open in another program? "
                       "close it and start again.", name);
}

/* the reads of disk n that no longer match their fingerprint are set
 * aside as NAME.damaged in the set-aside folder (never deleted) and the disk
 * goes back to "to be read"; the other reads stay */
static void set_aside(run_ctx *r, int n)
{
    meta_disk *d = meta_disk_get(r->cfg->box, n);
    char p[1024], rel[400], hex[65]; int i, k = 0;
    if (!d) return;
    for (i = 0; i < d->nreads; i++) {
        u8_join(p, sizeof(p), r->datadir, d->reads[i].file);
        if (sha256_path(p, hex, NULL) == 0 && !strcmp(hex, d->reads[i].sha256)) {
            d->reads[k++] = d->reads[i];
            continue;
        }
        if (u8_exists(p)) {
            if (move_aside(r, d->reads[i].file, SESSION_DAMAGED, rel, sizeof(rel))) {
                ev(r, EV_LINE, n, "WARNING: cannot move data/%s to %s: is it open in "
                                  "another program? close it and start again.", d->reads[i].file, session_aside_dir(r->bagdir));
                d->reads[k++] = d->reads[i];
                continue;
            }
            ev(r, EV_LINE, n, "Setting data/%s aside as %s, out of the data package: "
                              "it is no longer what was read.", d->reads[i].file, rel);
        }
    }
    d->nreads = k;
    for (i = 0; i < k && strcmp(d->reads[i].file, d->master); i++) ;
    if (i == k) CP(d->master, k ? d->reads[0].file : "");
    d->status[0] = d->verdict[0] = d->report[0] = 0;
    d->agree = d->unstable_tracks = 0;
}

typedef struct { strbuf *out; int n; } aside_ctx;

static void aside_one(void *c, const char *name, int isdir)
{
    aside_ctx *a = (aside_ctx *)c;
    (void)isdir;
    sb_printf(a->out, "%s\n", name);
    a->n++;
}

int session_aside_list(const char *bagdir, strbuf *out)
{
    char dir[1024]; aside_ctx a;
    a.out = out; a.n = 0;
    u8_join(dir, sizeof(dir), bagdir, session_aside_dir(bagdir));
    if (u8_isdir(dir)) u8_listdir(dir, aside_one, &a);
    return a.n;
}

/* once the package is closed: what is outside the data, with the path */
static void tell_aside(run_ctx *r)
{
    strbuf s; char *p, *nl, dir[1024], full[1300]; int n;
    sb_init(&s);
    n = session_aside_list(r->bagdir, &s);
    if (n) {
        u8_join(dir, sizeof(dir), r->bagdir, session_aside_dir(r->bagdir));
        ev(r, EV_LINE, 0, "Set aside, out of the data package (not in the manifest): %d file(s). "
                          "Look at them and, if they are not needed, delete them:", n);
        for (p = s.s; *p; p = nl + 1) {
            nl = strchr(p, '\n');
            *nl = 0;
            u8_join(full, sizeof(full), dir, p);
            ev(r, EV_LINE, 0, "  %s", full);
        }
    }
    sb_free(&s);
}

static int all_done(const meta_box *b)
{
    int n, i, found;
    for (n = 1; n <= b->media_expected; n++) {
        for (i = 0, found = 0; i < b->ndisk; i++)
            if (b->disk[i].n == n && meta_disk_done(&b->disk[i])) found = 1;
        if (!found) return 0;
    }
    return 1;
}

/* closed with F: disks never read become missing, not ST_TO_READ;
 * those with reads keep their outcome */
static void mark_missing(meta_box *b)
{
    int n; meta_disk *d;
    for (n = 1; n <= b->media_expected; n++) {
        d = meta_disk_get(b, n);
        if (d && !meta_disk_done(d) && !d->nreads) CP(d->status, ST_MISSING);
    }
}

/* "Title" or "Title" (edition), for the messages */
static void box_name(const char *title, const char *edition, char *out, size_t n)
{
    if (edition[0]) snprintf(out, n, "\"%s\" (%s)", title, edition);
    else snprintf(out, n, "\"%s\"", title);
}

/* new or resumed bag; -1 if it cannot go on */
static int open_bag(run_ctx *r)
{
    meta_box *box = r->cfg->box;
    char want[sizeof(box->title)], want_ed[sizeof(box->edition)], mp[1040], was[80], now[80];
    int nwant = box->media_expected;
    if (u8_exists(r->bagdir)) {
        if (!bag_is_open(r->bagdir)) {
            ev(r, EV_ERROR, 0, "The box \"%s\" is already closed in %s: choose another title, "
                               "or tell it apart with the edition.", box->title, r->bagdir);
            return -1;
        }
        u8_join(mp, sizeof(mp), r->datadir, "metadata.json");
        if (!u8_exists(mp)) {
            /* the first save failed: start over as new */
            ev(r, EV_LINE, 0, "metadata.json is missing in %s: starting this box over from scratch.",
               r->bagdir);
            if (u8_mkdir(r->datadir)) {
                ev(r, EV_ERROR, 0, "Cannot create the folder %s: check the permissions.", r->datadir);
                return -1;
            }
            meta_now(box->started);
            return save(r);
        }
        CP(want, box->title);               /* meta_load rewrites the box */
        CP(want_ed, box->edition);
        meta_composition(box, now, sizeof(now));
        if (meta_load(box, r->datadir, &r->msg)) {
            ev(r, EV_ERROR, 0, "%s", r->msg.s);
            return -1;
        }
        /* different titles can give the same folder ("A: B" and "A_ B"),
         * and so can an edition typed in the title ("A (2)" and "A", 2) */
        if (strcmp(box->title, want) || strcmp(box->edition, want_ed)) {
            char had[400], asked[400];
            box_name(box->title, box->edition, had, sizeof(had));
            box_name(want, want_ed, asked, sizeof(asked));
            ev(r, EV_ERROR, 0, "The folder %s holds the box %s, not %s: choose a different "
                               "title, or resume that box with its exact title and edition.",
               r->bagdir, had, asked);
            return -1;
        }
        meta_composition(box, was, sizeof(was));
        if (nwant && strcmp(was, now))
            ev(r, EV_LINE, 0, "The box was opened with %s: going on with %s.", was, was);
        ev(r, EV_LINE, 0, "Resuming the box \"%s\" from %s", box->title, r->bagdir);
        u8_listdir(r->datadir, orphan, r);
        return 0;
    }
    if (bag_create(r->bagdir, &r->msg)) { ev(r, EV_ERROR, 0, "%s", r->msg.s); return -1; }
    meta_now(box->started);
    return save(r);
}

/* equal for Windows, which is case-insensitive */
static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* two media with the same file name would overwrite each other */
static int names_collide(run_ctx *r)
{
    const meta_box *b = r->cfg->box; char base[META_MAXDISK][96];
    int n = b->media_expected, i, k, kind, num;
    if (n > META_MAXDISK) n = META_MAXDISK;
    for (i = 0; i < n; i++) {
        meta_slot(b, i + 1, &kind, &num);
        disk_base(r, kind, num, base[i]);
        for (k = 0; k < i; k++) {
            if (!same_name(base[i], base[k])) continue;
            ev(r, EV_ERROR, 0, "The naming scheme \"%s\" gives the same name to different media: "
                               "use {n}, {nn} or {nnn} and a shorter title.", tpl_for(r, kind));
            return 1;
        }
    }
    return 0;
}

/* the scheme from a name already used: "DISK007" of floppy 7 gives
 * "DISK{nnn}"; 0 if the name does not contain the number */
static int tpl_from_name(const meta_box *b, const meta_disk *d, char *out, size_t n)
{
    static const char *const ph[] = { "{n}", "{nn}", "{nnn}" };
    char stem[128], back[256], *dot; size_t i, j;
    CP(stem, d->master[0] ? d->master : d->reads[0].file);
    if ((dot = strrchr(stem, '.')) != NULL) *dot = 0;
    for (j = strlen(stem); j > 0 && !isdigit((unsigned char)stem[j - 1]); j--) ;
    for (i = j; i > 0 && isdigit((unsigned char)stem[i - 1]); i--) ;
    if (j == 0 || j - i > 3 || atoi(stem + i) != d->num || strchr(stem, '{')) return 0;
    snprintf(out, n, "%.*s%s%s", (int)i, stem, ph[j - i - 1], stem + j);
    /* it must give back exactly that name */
    session_expand(back, sizeof(back), out, d->num, b->title);
    return !strcmp(back, stem);
}

/* names are fixed at the first opening: whoever resumes finds the same ones */
void session_settle_names(meta_box *b, const char *const names[MEDIA_KINDS])
{
    char tpl[64]; int i, k;
    for (i = 0; i < b->ndisk && !b->names[MEDIA_FLOPPY][0]; i++) {
        const meta_disk *d = &b->disk[i];
        if (d->kind == MEDIA_FLOPPY && d->nreads && tpl_from_name(b, d, tpl, sizeof(tpl)))
            CP(b->names[MEDIA_FLOPPY], tpl);
    }
    for (k = 0; k < MEDIA_KINDS; k++)
        if (!b->names[k][0] && meta_count(b, k)) CP(b->names[k], session_name_tpl(b, names, k));
}

/* the SHA-256 of a disk's master read; NULL without one */
static const char *master_sha(const meta_disk *d)
{
    int i;
    if (!d->master[0]) return NULL;
    for (i = 0; i < d->nreads; i++)
        if (!strcmp(d->reads[i].file, d->master) && d->reads[i].sha256[0]) return d->reads[i].sha256;
    return NULL;
}

int session_twin(const meta_box *b, const meta_disk *d)
{
    const char *h = master_sha(d), *o;
    int i;
    if (!h) return 0;
    for (i = 0; i < b->ndisk; i++) {
        if (b->disk[i].n == d->n) continue;
        if ((o = master_sha(&b->disk[i])) != NULL && !strcmp(o, h)) return b->disk[i].n;
    }
    return 0;
}


/* one file of a duplicate disk out of data/; its name is cleared if moved */
static void aside_dup(run_ctx *r, const meta_disk *d, char *name, size_t len)
{
    char rel[400];
    if (!name[0] || move_aside(r, name, SESSION_DUPLICATE, rel, sizeof(rel))) {
        if (name[0]) ev(r, EV_LINE, d->n, "WARNING: cannot move data/%s to %s: is it open in "
                                          "another program?", name, session_aside_dir(r->bagdir));
        return;
    }
    ev(r, EV_LINE, d->n, "Setting data/%s aside as %s, out of the data package.", name, rel);
    name[0] = 0;
    (void)len;
}

/* The same image as another disk of the box: usually the same disk inserted
 * twice. Its reads are set aside, never deleted, and it goes back to be read
 * from zero, so the right disk is not compared with the wrong reads; the
 * status says whose duplicate it was, also if the box is closed without it */
static void check_twin(run_ctx *r, meta_disk *d)
{
    char who[32], other[32], file[128];
    int t = session_twin(r->cfg->box, d), i, k = 0;
    if (!t) { d->duplicate_of = 0; return; }
    name_of(r->cfg->box, d->n, who, sizeof(who));
    name_of(r->cfg->box, t, other, sizeof(other));
    /* read again and still the same: the two disks really are identical */
    if (d->duplicate_of == t) {
        ev(r, EV_LINE, d->n, "%s is again identical to %s: keeping it, the two disks really are the same.",
           who, other);
        d->duplicate_of = 0;
        return;
    }
    ev(r, EV_LINE, d->n, "WARNING: %s is identical to %s (same SHA-256 fingerprint): is it the same disk "
                         "inserted twice? %s is still to be read: insert the right one. If the two "
                         "disks really are the same, read it again: the second time it is kept.", who, other, who);
    for (i = 0; i < d->nreads; i++) {
        CP(file, d->reads[i].file);
        aside_dup(r, d, file, sizeof(file));
        if (file[0]) d->reads[k++] = d->reads[i];       /* could not be moved: it stays recorded */
    }
    d->nreads = k;
    aside_dup(r, d, d->scan, sizeof(d->scan));
    aside_dup(r, d, d->report, sizeof(d->report));
    CP(d->master, k ? d->reads[0].file : "");
    d->verdict[0] = 0;
    d->agree = d->unstable_tracks = 0;
    d->duplicate_of = t;
    CP(d->status, ST_DUPLICATE);
}

enum { DISK_NEXT, DISK_QUIT, DISK_FINISH, DISK_FAIL };

/* one round on the disk: asks for it, reads or skips it, records the outcome */
static int ask_and_do(run_ctx *r, meta_disk *d, int *cmd, char *bye, size_t blen)
{
    const session_host *host = r->host; char label[128], note[256];
    disk_turn t;
    t.n = d->n; t.total = r->cfg->box->media_expected;
    t.kind = d->kind; t.num = d->num; t.count = meta_count(r->cfg->box, d->kind);
    t.readable = r->cfg->media[d->kind] != NULL;
    t.protection_retry = r->prot_retry != d->n ? 0 : r->prot_retry_on ? 1 : 2;
    CP(label, d->label);
    CP(note, d->note);
    r->cur = d->n;
    *cmd = host->wait_disk(host->ctx, &t, label, sizeof(label), note, sizeof(note));
    if (*cmd != CMD_READ) r->prot_retry = 0;
    CP(d->label, label);
    CP(d->note, note);
    if (*cmd == CMD_QUIT)   return DISK_QUIT;
    if (*cmd == CMD_FINISH) return DISK_FINISH;
    /* cannot be read yet: it stays "to be read", not missing, and the box
     * stays open. Label and note typed now are kept. */
    if (!t.readable) {
        *cmd = CMD_SKIP;
        if (save(r)) return DISK_FAIL;
        host->event(host->ctx, EV_DISK_DONE, d->n, d->status[0] ? d->status : ST_TO_READ);
        return DISK_NEXT;
    }
    /* skipped: reads already done stay, and with them their outcome */
    if (*cmd == CMD_SKIP) { if (!d->nreads) CP(d->status, ST_MISSING); }
    else if (process(r, d) == MEDIA_NOTOOL) {
        const char *tool = r->cfg->media[d->kind]->tool;
        ev(r, EV_ERROR, d->n, "%s", r->last);
        snprintf(bye, blen, "The box stays open: once %s is fixed, "
                            "start again with the same title.", tool ? tool : "the reading program");
        return DISK_QUIT;
    } else check_twin(r, d);
    csv_row(r, d);
    if (save(r)) return DISK_FAIL;
    host->event(host->ctx, EV_DISK_DONE, d->n, d->status);
    return DISK_NEXT;
}

/* read but not successful, or to read again with the copy protection scan:
 * with ask_again it is asked for again right away */
static int retry_wanted(const run_ctx *r, const meta_disk *d, int cmd)
{
    return r->host->ask_again && cmd == CMD_READ
        && (!strcmp(d->status, ST_FAILED) || !strcmp(d->status, ST_INTERRUPTED)
            || !strcmp(d->status, ST_DUPLICATE) || r->prot_retry == d->n);
}

/* after a read: did the user ask to stop? */
static int stop_after(const run_ctx *r, const meta_disk *d, int cmd, char *bye, size_t blen)
{
    char who[32];
    if (cmd != CMD_READ) return 0;
    /* stopped by the user (Ctrl+C, Cancel): the next disk is not asked for */
    if (!strcmp(d->status, ST_INTERRUPTED)) {
        snprintf(bye, blen, "Read interrupted: start again to resume from %s.",
                 name_of(r->cfg->box, d->n, who, sizeof(who)));
        return 1;
    }
    /* canceled with the work almost done (e.g. during the comparison): the
     * disk is closed, stop before the next one. With ask_again the next
     * request is already a stopping point. */
    if (!runner_stopped() || r->host->ask_again) return 0;
    if (d->n < r->cfg->box->media_expected)
        snprintf(bye, blen, "Stopped: start again to resume from %s.",
                 name_of(r->cfg->box, meta_disk_done(d) ? d->n + 1 : d->n, who, sizeof(who)));
    else
        str_copy(bye, blen, "Stopped: start again with the same title to close the box.");
    return 1;
}

/* only media that cannot be read yet are left: say so */
static void still_to_read(const run_ctx *r, char *bye, size_t blen)
{
    const meta_box *b = r->cfg->box; strbuf s; char who[32]; int n, k = 0;
    sb_init(&s);
    for (n = 1; n <= b->media_expected; n++) {
        int kind, num, i, done = 0;
        for (i = 0; i < b->ndisk; i++) if (b->disk[i].n == n) done = meta_disk_done(&b->disk[i]);
        if (done) continue;
        meta_slot(b, n, &kind, &num);
        if (r->cfg->media[kind]) { sb_free(&s); return; }
        meta_disk_name(kind, num, who, sizeof(who));
        sb_printf(&s, k++ ? ", %s" : "%s", who);
    }
    if (k)
        snprintf(bye, blen, "Still to read: %s. The box stays open: resume it when RetroArchiver "
                            "can read %s, or close it with F.", s.s, k == 1 ? "it" : "them");
    sb_free(&s);
}

int session_run(const session_cfg *cfg, const session_host *host)
{
    run_ctx r; meta_box *box = cfg->box;
    int n, cmd, quit = 0, finish = 0, bad = 0, rc = SESSION_ERROR, act, cp;
    char bye[400] = "";

    memset(&r, 0, sizeof(r));
    r.cfg = cfg; r.host = host;
    sb_init(&r.msg);
    runner_clear();
    if (u8_mkdirs(cfg->outroot)) {
        ev(&r, EV_ERROR, 0, "Cannot create the folder %s: check the path.", cfg->outroot);
        goto out;
    }
    session_bagdir(r.bagdir, sizeof(r.bagdir), cfg->outroot, box->title, box->edition);
    u8_join(r.datadir, sizeof(r.datadir), r.bagdir, "data");
    if (names_collide(&r)) goto out;
    cp = cfg->copy_protection ? meta_prot_parse(cfg->copy_protection) : -1;
    if (cp >= 0) box->copy_protection = cp;
    if (open_bag(&r)) goto out;
    /* resumed: the choice made now wins over the one the box was opened with */
    if (cp >= 0 && box->copy_protection != cp) {
        ev(&r, EV_LINE, 0, "Copy protection scan of the CDs: %s (the box had %s).", meta_prot_key(cp),
           meta_prot_key(box->copy_protection));
        box->copy_protection = cp;
    }
    session_settle_names(box, cfg->names);
    if (names_collide(&r)) goto out;        /* resumed with other media or other names */

    for (n = cfg->start > 0 ? cfg->start : 1; n <= box->media_expected; n++) {
        meta_disk *d = meta_disk_get(box, n);
        if (!d) { ev(&r, EV_ERROR, n, "Too many media: at most %d per box.", META_MAXDISK); break; }
        if (meta_disk_done(d)) continue;
        do act = ask_and_do(&r, d, &cmd, bye, sizeof(bye));
        while (act == DISK_NEXT && retry_wanted(&r, d, cmd));
        if (act == DISK_FAIL) goto out;
        if (act == DISK_FINISH) { finish = 1; break; }
        if (act == DISK_QUIT || stop_after(&r, d, cmd, bye, sizeof(bye))) { quit = 1; break; }
    }

    if (quit || (!finish && !all_done(box))) {
        save(&r);
        if (!bye[0] && !quit) still_to_read(&r, bye, sizeof(bye));
        ev(&r, EV_LINE, 0, "%s", bye[0] ? bye : quit
           ? "Box left open: start again with the same title to resume it."
           : "Some media are missing: the box stays open. Start again with the same title "
             "to read them, or close it with F.");
        rc = SESSION_OPEN;
        goto out;
    }

    if (finish) mark_missing(box);
    r.stuck = 0;
    u8_listdir(r.datadir, orphan, &r);      /* no leftovers in the closed bag */
    if (r.stuck) {                          /* it would end up in the manifest */
        save(&r);
        rc = SESSION_OPEN;
        goto out;
    }
    meta_now(box->closed);
    sb_clear(&r.msg);
    if (save(&r) == 0 && bag_close(r.bagdir, box, &r.msg, &bad) == 0) {
        tell_aside(&r);
        host->event(host->ctx, EV_BOX_DONE, 0, all_done(box) ? BAG_COMPLETE : BAG_INCOMPLETE);
        rc = SESSION_CLOSED;
    } else {
        box->closed[0] = 0;
        if (bad > 0) {
            set_aside(&r, bad);
            char who[32];
            ev(&r, EV_ERROR, bad, "%s. Start again with the same title: %s will be "
                                  "asked for again.", r.msg.s, name_of(box, bad, who, sizeof(who)));
        } else if (r.msg.len) {
            ev(&r, EV_ERROR, 0, "%s", r.msg.s);
        }
        save(&r);
        rc = SESSION_OPEN;
    }
out:
    sb_free(&r.msg);
    return rc;
}
