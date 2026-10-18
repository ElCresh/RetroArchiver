#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "archive.h"
#include "bagit.h"
#include "fsu8.h"
#include "sha256.h"

#define CP(dst, src) str_copy(dst, sizeof(dst), src)

/* ---- bag-info.txt ---- */

typedef struct {
    char title[256], platform[64], publisher[128], year[16], language[64];
    char edition[128], notes[512], status[24], date[20], media[80];
    int expected, archived;
} bag_info;

static void info_line(bag_info *bi, const char *key, const char *val)
{
    if (!strcmp(key, "Title")) CP(bi->title, val);
    else if (!strcmp(key, "Platform")) CP(bi->platform, val);
    else if (!strcmp(key, "Publisher")) CP(bi->publisher, val);
    else if (!strcmp(key, "Year")) CP(bi->year, val);
    else if (!strcmp(key, "Language")) CP(bi->language, val);
    else if (!strcmp(key, "Edition")) CP(bi->edition, val);
    else if (!strcmp(key, "Notes")) CP(bi->notes, val);
    else if (!strcmp(key, "Box-Status")) CP(bi->status, val);
    else if (!strcmp(key, "Bagging-Date")) CP(bi->date, val);
    else if (!strcmp(key, "Media-Type")) CP(bi->media, val);
    else if (!strcmp(key, "Media-Expected")) bi->expected = atoi(val);
    else if (!strcmp(key, "Media-Archived")) bi->archived = atoi(val);
}

/* 0 if the file exists; the "Key: value" lines, the others are ignored */
static int info_load(const char *bagdir, bag_info *bi)
{
    char p[1024], line[1200], *colon, *e;
    FILE *f;
    memset(bi, 0, sizeof(*bi));
    u8_join(p, sizeof(p), bagdir, "bag-info.txt");
    if (!(f = u8_fopen(p, "rb"))) return -1;
    while (fgets(line, sizeof(line), f)) {
        e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (!(colon = strstr(line, ": "))) continue;
        *colon = 0;
        info_line(bi, line, colon + 2);
    }
    fclose(f);
    return 0;
}

/* ---- one bag ---- */


static char disk_char(const char *status)
{
    return meta_status_class(status);
}

static const char kind_char[MEDIA_KINDS] = { 'f', 'c', 'd' };

static const meta_disk *disk_n(const meta_box *m, int n)
{
    int i;
    for (i = 0; i < m->ndisk; i++) if (m->disk[i].n == n) return &m->disk[i];
    return NULL;
}

/* the time of the last read: for an open box it is the last modification */
static void last_read(const meta_box *m, char out[20])
{
    int i, k;
    str_copy(out, 20, m->started);
    for (i = 0; i < m->ndisk; i++)
        for (k = 0; k < m->disk[i].nreads; k++)
            if (strcmp(m->disk[i].reads[k].at, out) > 0) str_copy(out, 20, m->disk[i].reads[k].at);
}

static void from_meta(arc_box *b, const meta_box *m)
{
    int n, k, kind, num;
    CP(b->title, m->title);          CP(b->publisher, m->publisher);
    CP(b->year, m->year);            CP(b->platform, m->platform);
    CP(b->language, m->language);    CP(b->edition, m->edition);
    CP(b->notes, m->notes);
    b->expected = m->media_expected > META_MAXDISK ? META_MAXDISK : m->media_expected;
    for (k = 0; k < MEDIA_KINDS; k++) b->count[k] = meta_count(m, k);
    for (n = 1; n <= b->expected; n++) {
        const meta_disk *d = disk_n(m, n);
        char c = d ? disk_char(d->status) : '-';
        meta_slot(m, n, &kind, &num);
        b->disks[n - 1] = c;
        b->kinds[n - 1] = kind_char[kind];
        if (d && d->master[0]) { b->archived++; b->kind_archived[kind]++; }
        if (c == 'p') { b->clean++; if (!strcmp(d->status, ST_REDUMP)) b->redump++; }
        else if (c == 'm') b->marginal++;
        else if (c == 'f') b->fragile++;
        else if (c == 'e') b->errors++;
        else if (!b->first_todo) b->first_todo = n;
    }
    b->disks[b->expected] = 0;
    b->kinds[b->expected] = 0;
}

/* Media-Type: "floppy" in the older bags, "3 floppy, 1 CD" today */
static void parse_media(arc_box *b, const char *media)
{
    char part[40]; const char *p = media, *e; int n, k;
    memset(b->count, 0, sizeof(b->count));
    for (; *p; p = *e ? e + 2 : e) {
        if (!(e = strstr(p, ", "))) e = p + strlen(p);
        snprintf(part, sizeof(part), "%.*s", (int)(e - p), p);
        if (sscanf(part, "%d", &n) != 1) continue;
        for (k = 0; k < MEDIA_KINDS; k++)
            if (!strcmp(strchr(part, ' ') ? strchr(part, ' ') + 1 : "", meta_kind_label(k))) b->count[k] = n;
    }
}

static void from_info(arc_box *b, const bag_info *bi)
{
    CP(b->title, bi->title);         CP(b->publisher, bi->publisher);
    CP(b->year, bi->year);           CP(b->platform, bi->platform);
    CP(b->language, bi->language);   CP(b->edition, bi->edition);
    CP(b->notes, bi->notes);
    b->expected = bi->expected;
    b->archived = bi->archived;
    parse_media(b, bi->media);
}

static const char *base_name(const char *path)
{
    const char *p = path + strlen(path);
    while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
    return p;
}

int arc_load(const char *bagdir, arc_box *b)
{
    char p[1024], data[1024];
    bag_info bi; meta_box *m; strbuf err;
    int open = bag_is_open(bagdir), has_info;

    u8_join(p, sizeof(p), bagdir, "bagit.txt");
    if (!open && !u8_exists(p)) return 0;
    memset(b, 0, sizeof(*b));
    CP(b->dir, bagdir);
    has_info = info_load(bagdir, &bi) == 0;
    u8_join(data, sizeof(data), bagdir, "data");
    /* the CD and DVD folders too */
    if ((b->bytes = u8_treesize(data)) < 0) b->bytes = 0;

    sb_init(&err);
    m = meta_new();
    if (m && meta_load(m, data, &err) == 0) {
        from_meta(b, m);
        if (open) { b->state = ARC_OPEN; last_read(m, b->date); }
        else {
            /* Box-Status was written by bag_close: that one wins */
            int all = b->first_todo == 0;
            b->state = has_info && bi.status[0] ? (bag_status_complete(bi.status) ? ARC_COMPLETE : ARC_INCOMPLETE)
                                                : (all ? ARC_COMPLETE : ARC_INCOMPLETE);
            CP(b->date, m->closed[0] ? m->closed : (has_info ? bi.date : ""));
        }
    } else {
        b->state = ARC_BROKEN;
        if (has_info) { from_info(b, &bi); CP(b->date, bi.date); }
        if (!b->title[0]) CP(b->title, base_name(bagdir));
        CP(b->error, err.s ? err.s : "out of memory");
    }
    meta_delete(m);
    sb_free(&err);
    return 1;
}

/* ---- the archive folder ---- */

typedef struct { const char *root; arc_list *l; } scan_ctx;

static void add_bag(void *ctx, const char *name, int isdir)
{
    scan_ctx *s = (scan_ctx *)ctx;
    arc_list *l = s->l;
    char dir[600];
    if (!isdir) return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        arc_box *nb = (arc_box *)realloc(l->box, (size_t)cap * sizeof(arc_box));
        if (!nb) return;
        l->box = nb; l->cap = cap;
    }
    u8_join(dir, sizeof(dir), s->root, name);
    if (arc_load(dir, &l->box[l->n])) l->n++;
}

/* most recent first; on equal dates, by title */
static int by_date(const void *a, const void *b)
{
    const arc_box *x = (const arc_box *)a, *y = (const arc_box *)b;
    int c = strcmp(y->date, x->date);
    return c ? c : strcmp(x->title, y->title);
}

int arc_scan(const char *root, arc_list *out)
{
    scan_ctx s;
    memset(out, 0, sizeof(*out));
    s.root = root; s.l = out;
    if (u8_listdir(root, add_bag, &s)) return -1;
    if (out->n > 1) qsort(out->box, (size_t)out->n, sizeof(arc_box), by_date);
    return 0;
}

void arc_free(arc_list *l)
{
    free(l->box);
    memset(l, 0, sizeof(*l));
}

/* ---- texts ---- */

int arc_tone(const arc_box *b)
{
    if (b->state == ARC_OPEN) return TONE_OPEN;
    if (b->state == ARC_BROKEN || b->errors) return TONE_BAD;
    if (b->state == ARC_INCOMPLETE || b->marginal || b->fragile) return TONE_WARN;
    return TONE_OK;
}

static void add_part(strbuf *s, int k, const char *one, const char *many)
{
    if (!k) return;
    sb_printf(s, "%s%d %s", s->len ? ", " : "", k, k == 1 ? one : many);
}

void arc_outcome(const arc_box *b, char *out, size_t n)
{
    strbuf s;
    sb_init(&s);
    add_part(&s, b->clean, "clean", "clean");
    if (b->redump) sb_printf(&s, " (%d verified with redump)", b->redump);
    add_part(&s, b->marginal, "marginal", "marginal");
    add_part(&s, b->fragile, "fragile", "fragile");
    add_part(&s, b->errors, "with errors", "with errors");
    if (!s.len) sb_puts(&s, b->state == ARC_BROKEN ? "unreadable" : "no media read");
    str_copy(out, n, s.s);
    sb_free(&s);
}

/* boxes built by hand (or older ones) without a composition: all floppies */
void arc_slot(const arc_box *b, int n, int *kind, int *num)
{
    int c[MEDIA_KINDS], k, sum = 0;
    for (k = 0; k < MEDIA_KINDS; k++) sum += c[k] = b->count[k];
    if (sum != b->expected) { memset(c, 0, sizeof(c)); c[MEDIA_FLOPPY] = b->expected; }
    meta_slot_counts(c, n, kind, num);
}

/* until optical.c exists, CDs and DVDs are asked for but not read */
static int readable(int kind) { return kind == MEDIA_FLOPPY; }

/* "floppies" for the counts in the texts: meta_kind_label is the singular,
 * also stored in Media-Type, so it stays as it is */
static const char *kind_word(int kind, int many)
{
    static const char *plural[MEDIA_KINDS] = { "floppies", "CDs", "DVDs" };
    if (!many || kind < 0 || kind >= MEDIA_KINDS) return meta_kind_label(kind);
    return plural[kind];
}

/* the media with that character: "floppy 3", "floppy 3, 5" if they are of
 * the same type, "floppy 3, CD 1" if not. *same: all of the same type */
static int list_of(const arc_box *b, char c, strbuf *s, int *same)
{
    int i, k = 0, kind, num, first = -1;
    *same = 1;
    for (i = 0; b->disks[i]; i++) {
        if (b->disks[i] != c) continue;
        arc_slot(b, i + 1, &kind, &num);
        if (first < 0) first = kind;
        else if (kind != first) *same = 0;
    }
    for (i = 0; b->disks[i]; i++) {
        if (b->disks[i] != c) continue;
        arc_slot(b, i + 1, &kind, &num);
        if (!k || !*same) sb_printf(s, k ? ", %s %d" : "%s %d", meta_kind_label(kind), num);
        else sb_printf(s, ", %d", num);
        k++;
    }
    return k;
}

void arc_missing(const arc_box *b, char *out, size_t n)
{
    strbuf s; int same;
    sb_init(&s);
    list_of(b, '-', &s, &same);
    str_copy(out, n, s.s ? s.s : "");
    sb_free(&s);
}

void arc_media(const arc_box *b, char *out, size_t n)
{
    strbuf s; int k;
    sb_init(&s);
    for (k = 0; k < MEDIA_KINDS; k++)
        if (b->count[k])
            sb_printf(&s, "%s%d / %d %s", s.len ? ", " : "", b->kind_archived[k], b->count[k],
                      meta_kind_label(k));
    /* without a composition (unreadable bag): just the numbers */
    if (!s.len && b->expected) sb_printf(&s, "%d / %d", b->archived, b->expected);
    str_copy(out, n, s.s ? s.s : "");
    sb_free(&s);
}

/* "CD 1 is missing", "floppy 5, 7 are missing", "floppy 5, CD 1 are missing" */
static void missing_phrase(const arc_box *b, strbuf *s)
{
    strbuf l; int k, same;
    sb_init(&l);
    k = list_of(b, '-', &l, &same);
    if (k == 1) sb_printf(s, "%s is missing", l.s);
    else if (k) sb_printf(s, "%s are missing", l.s);
    sb_free(&l);
}

/* the media types of the composition: one name if there is only one */
static const char *noun(const arc_box *b, int many)
{
    int k, kinds = 0, last = MEDIA_FLOPPY;
    for (k = 0; k < MEDIA_KINDS; k++) if (b->count[k]) { kinds++; last = k; }
    if (kinds > 1) return many ? "media" : "medium";
    return kind_word(last, many);
}

/* "3 floppies out of 3": the types with at least one archived medium */
static int done_parts(const arc_box *b, strbuf *s)
{
    int k, i, kind, num, done[MEDIA_KINDS] = { 0 }, tot = 0, count[MEDIA_KINDS] = { 0 };
    for (i = 0; b->disks[i]; i++) {
        arc_slot(b, i + 1, &kind, &num);
        count[kind]++;
        if (b->disks[i] != '-') { done[kind]++; tot++; }
    }
    for (k = 0; k < MEDIA_KINDS; k++)
        if (done[k]) sb_printf(s, "%s%d %s out of %d", s->len ? ", " : "", done[k], kind_word(k, done[k] != 1), count[k]);
    return tot;
}

static void advice_open(const arc_box *b, strbuf *s)
{
    int kind, num, done, i, cd = 0, dvd = 0;
    strbuf p;
    if (!b->first_todo) {
        if (b->expected == 1) sb_puts(s, "Open: archived, only the closing is left. Resume to close it.");
        else sb_printf(s, "Open: all %d %s archived, only the closing is left. "
                          "Resume to close it.", b->expected, noun(b, 1));
        return;
    }
    sb_init(&p);
    done = done_parts(b, &p);
    arc_slot(b, b->first_todo, &kind, &num);
    if (readable(kind)) {
        if (!done) sb_printf(s, "Open: no %s archived yet.", noun(b, 0));
        else sb_printf(s, "Open: %s archived.", p.s);
        sb_printf(s, " Resume from %s %d.", meta_kind_label(kind), num);
    } else {
        /* only media the program cannot read yet are left */
        sb_puts(s, "Open: ");
        if (done) sb_printf(s, "%s archived, ", p.s);
        for (i = 0; b->disks[i]; i++) {
            if (b->disks[i] != '-') continue;
            arc_slot(b, i + 1, &kind, &num);
            if (kind == MEDIA_CD) cd = 1;
            if (kind == MEDIA_DVD) dvd = 1;
        }
        missing_phrase(b, s);
        sb_printf(s, ". Resume it when reading %s is available.",
                  cd && dvd ? "CDs and DVDs" : dvd ? "DVDs" : "CDs");
    }
    sb_free(&p);
}

/* " Floppy 3", " Floppy 3, 5", " Floppy 3, CD 1": the subject of a
 * sentence, so the first letter is a capital */
static int subject(const arc_box *b, char c, strbuf *s)
{
    strbuf l; int k, same;
    sb_init(&l);
    k = list_of(b, c, &l, &same);
    if (k) {
        if (l.s[0] >= 'a' && l.s[0] <= 'z') l.s[0] = (char)(l.s[0] - 'a' + 'A');
        sb_printf(s, " %s", l.s);
    }
    sb_free(&l);
    return k;
}

/* among those read once there is a CD or a DVD: for them it is not a
 * cautious read but a second read that never happened */
static int optical_once(const arc_box *b)
{
    int i;
    for (i = 0; b->disks[i]; i++) if (b->disks[i] == 'f' && (b->kinds[i] == 'c' || b->kinds[i] == 'd')) return 1;
    return 0;
}

/* errors and media to look at, appended to the state */
static void advice_damage(const arc_box *b, strbuf *s)
{
    int k;
    if ((k = subject(b, 'e', s)) > 0)
        sb_puts(s, k == 1 ? " has damaged data: keep it to try again with another drive."
                          : " have damaged data: keep them to try again with another drive.");
    if ((k = subject(b, 'm', s)) > 0)
        sb_puts(s, k == 1 ? " is marginal: the reads did not fully match."
                          : " are marginal: the reads did not fully match.");
    if ((k = subject(b, 'f', s)) > 0) {
        if (!optical_once(b))
            sb_puts(s, k == 1 ? " is fragile: only one cautious read."
                              : " are fragile: only one cautious read.");
        else
            sb_puts(s, k == 1 ? " was read only once: not verified by a comparison."
                              : " were read only once: not verified by a comparison.");
    }
}

void arc_advice(const arc_box *b, char *out, size_t n)
{
    strbuf s;
    sb_init(&s);
    switch (b->state) {
    case ARC_OPEN:
        advice_open(b, &s);
        break;
    case ARC_BROKEN:
        sb_printf(&s, "Cannot read this box: %s.", b->error);
        break;
    case ARC_INCOMPLETE:
        sb_puts(&s, "Closed incomplete");
        if (strchr(b->disks, '-')) { sb_puts(&s, ": "); missing_phrase(b, &s); }
        sb_puts(&s, ". Package closed and verified.");
        advice_damage(b, &s);
        break;
    default:
        if (b->errors || b->marginal || b->fragile) {
            sb_puts(&s, "Closed complete.");
            advice_damage(b, &s);
        } else {
            sb_printf(&s, "Closed complete: all %s read and verified.", noun(b, 1));
        }
    }
    str_copy(out, n, s.s ? s.s : "");
    sb_free(&s);
}

/* ---- digest recheck ---- */

static void check_file(const char *bagdir, const char *rel, const char *want, arc_check *c)
{
    char p[1024], rp[512], hex[65], *q;
    str_copy(rp, sizeof(rp), rel);
    for (q = rp; *q; q++) if (*q == '/') *q = U8_SEP[0];
    u8_join(p, sizeof(p), bagdir, rp);
    c->files++;
    if (sha256_path(p, hex, NULL)) c->lost++;
    else if (strcmp(hex, want)) c->changed++;
    else return;
    if (!c->first[0]) str_copy(c->first, sizeof(c->first), rel);
}

/* "digest  path" per line; names collects the paths, if it is not NULL */
static int check_manifest(const char *bagdir, const char *name, arc_check *c, strbuf *names)
{
    char p[1024], line[1200], *sp, *e;
    FILE *f;
    u8_join(p, sizeof(p), bagdir, name);
    if (!(f = u8_fopen(p, "rb"))) return -1;
    while (fgets(line, sizeof(line), f)) {
        e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (!(sp = strchr(line, ' '))) continue;
        *sp++ = 0;
        while (*sp == ' ' || *sp == '*') sp++;
        check_file(bagdir, sp, line, c);
        if (names) { sb_puts(names, sp); sb_puts(names, "\n"); }
    }
    fclose(f);
    return 0;
}

typedef struct { const char *names; arc_check *c; } extra_ctx;

/* a file in data/ that the manifest does not list */
static void find_extra(void *ctx, const char *name)
{
    extra_ctx *x = (extra_ctx *)ctx;
    char want[300];
    snprintf(want, sizeof(want), "data/%s\n", name);
    if (x->names && strstr(x->names, want)) return;
    x->c->extra++;
    if (!x->c->first[0]) snprintf(x->c->first, sizeof(x->c->first), "data/%s", name);
}

static int verify_closed(const char *bagdir, arc_check *c, strbuf *err)
{
    char data[1024];
    strbuf names; extra_ctx x;
    sb_init(&names);
    if (check_manifest(bagdir, "manifest-sha256.txt", c, &names)) {
        sb_printf(err, "manifest-sha256.txt is missing in %s: the package was not closed properly", bagdir);
        sb_free(&names);
        return -1;
    }
    check_manifest(bagdir, "tagmanifest-sha256.txt", c, NULL);
    u8_join(data, sizeof(data), bagdir, "data");
    x.names = names.s; x.c = c;
    u8_walk(data, find_extra, &x);
    sb_free(&names);
    return 0;
}

static int verify_open(const char *bagdir, arc_check *c, strbuf *err)
{
    char data[1024], rel[200];
    meta_box *m = meta_new();
    int i, k;
    u8_join(data, sizeof(data), bagdir, "data");
    if (!m || meta_load(m, data, err)) { meta_delete(m); return -1; }
    for (i = 0; i < m->ndisk; i++)
        for (k = 0; k < m->disk[i].nreads; k++) {
            snprintf(rel, sizeof(rel), "data/%s", m->disk[i].reads[k].file);
            check_file(bagdir, rel, m->disk[i].reads[k].sha256, c);
        }
    meta_delete(m);
    return 0;
}

int arc_verify(const char *bagdir, arc_check *c, strbuf *err)
{
    memset(c, 0, sizeof(*c));
    return bag_is_open(bagdir) ? verify_open(bagdir, c, err) : verify_closed(bagdir, c, err);
}
