#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "meta.h"
#include "fsu8.h"

#ifdef _WIN32
#include <io.h>
#define SYNC_FILE(f) _commit(_fileno(f))
#else
#include <unistd.h>
#define SYNC_FILE(f) fsync(fileno(f))
#endif

#define CP(dst, src) str_copy(dst, sizeof(dst), src)
#define FIXIT ": if the box is damaged, move its folder away and start over"

static const char *const kind_keys[MEDIA_KINDS] = { "floppy", "cd", "dvd" };
static const char *const kind_labels[MEDIA_KINDS] = { "floppy", "CD", "DVD" };

const char *meta_kind_key(int kind)
{
    return kind >= 0 && kind < MEDIA_KINDS ? kind_keys[kind] : "floppy";
}

const char *meta_kind_label(int kind)
{
    return kind >= 0 && kind < MEDIA_KINDS ? kind_labels[kind] : "floppy";
}

int meta_kind_parse(const char *key)
{
    int k;
    for (k = 0; key && k < MEDIA_KINDS; k++) if (!strcmp(key, kind_keys[k])) return k;
    return -1;
}

static const char *const prot_keys[] = { "auto", "on", "off" };

const char *meta_prot_key(int mode)
{
    return mode >= META_PROT_AUTO && mode <= META_PROT_OFF ? prot_keys[mode] : "auto";
}

int meta_prot_parse(const char *key)
{
    int k;
    for (k = 0; key && k < 3; k++) if (!strcmp(key, prot_keys[k])) return k;
    return -1;
}

void meta_set_counts(meta_box *b, int floppy, int cd, int dvd)
{
    b->count[MEDIA_FLOPPY] = floppy;
    b->count[MEDIA_CD] = cd;
    b->count[MEDIA_DVD] = dvd;
    b->media_expected = floppy + cd + dvd;
}

/* a box that only sets media_expected (the old boxes) is all floppies */
static void counts_of(const meta_box *b, int c[MEDIA_KINDS])
{
    int k, sum = 0;
    for (k = 0; k < MEDIA_KINDS; k++) sum += c[k] = b->count[k];
    if (sum != b->media_expected) {
        memset(c, 0, MEDIA_KINDS * sizeof(int));
        c[MEDIA_FLOPPY] = b->media_expected;
    }
}

int meta_count(const meta_box *b, int kind)
{
    int c[MEDIA_KINDS];
    counts_of(b, c);
    return kind >= 0 && kind < MEDIA_KINDS ? c[kind] : 0;
}

void meta_slot_counts(const int count[MEDIA_KINDS], int n, int *kind, int *num)
{
    int k;
    for (k = 0; k < MEDIA_KINDS; k++) {
        if (n <= count[k]) { *kind = k; *num = n; return; }
        n -= count[k];
    }
    *kind = MEDIA_FLOPPY;                   /* beyond the composition: never happens */
    *num = n;
}

void meta_slot(const meta_box *b, int n, int *kind, int *num)
{
    int c[MEDIA_KINDS];
    counts_of(b, c);
    meta_slot_counts(c, n, kind, num);
}

void meta_disk_name(int kind, int num, char *out, size_t n)
{
    snprintf(out, n, "%s %d", meta_kind_label(kind), num);
}

void meta_composition_counts(const int count[MEDIA_KINDS], char *out, size_t n)
{
    strbuf s; int k;
    sb_init(&s);
    for (k = 0; k < MEDIA_KINDS; k++)
        if (count[k]) sb_printf(&s, "%s%d %s", s.len ? ", " : "", count[k], meta_kind_label(k));
    str_copy(out, n, s.len ? s.s : "no media");
    sb_free(&s);
}

void meta_composition(const meta_box *b, char *out, size_t n)
{
    int c[MEDIA_KINDS];
    counts_of(b, c);
    meta_composition_counts(c, out, n);
}

int meta_disk_done(const meta_disk *d)
{
    return meta_status_done(d->status);
}

/* the floppy outcomes and the CD and DVD ones (optical.c) */
const char *meta_status_english(const char *s)
{
    static const char *const old[][2] = {
        { "pulito", ST_CLEAN },
        { "marginale", ST_MARGINAL },
        { "errore", ST_ERROR },
        { "errori", ST_ERRORS },
        { "instabile", ST_UNSTABLE },
        { "letto una volta", ST_READ_ONCE },
        { "verificato redump", ST_REDUMP },
        { "lettura fallita", ST_FAILED },
        { "interrotto", ST_INTERRUPTED },
        { "mancante", ST_MISSING },
        { "duplicato", ST_DUPLICATE },
        { "da leggere", ST_TO_READ },
        { NULL, NULL }
    };
    int i;
    for (i = 0; old[i][0]; i++) if (!strcmp(s, old[i][0])) return old[i][1];
    return s;
}

int meta_status_done(const char *s)
{
    return !strcmp(s, ST_CLEAN) || !strcmp(s, ST_MARGINAL) ||
           !strcmp(s, ST_ERROR) || !strcmp(s, "fragile") ||
           !strcmp(s, ST_REDUMP) || !strcmp(s, ST_UNSTABLE) ||
           !strcmp(s, ST_ERRORS) || !strcmp(s, ST_READ_ONCE);
}

char meta_status_class(const char *s)
{
    if (!strcmp(s, ST_CLEAN) || !strcmp(s, ST_REDUMP)) return 'p';
    if (!strcmp(s, ST_MARGINAL) || !strcmp(s, ST_UNSTABLE)) return 'm';
    if (!strcmp(s, "fragile") || !strcmp(s, ST_READ_ONCE)) return 'f';
    if (!strcmp(s, ST_ERROR) || !strcmp(s, ST_ERRORS)) return 'e';
    return '-';
}

meta_box *meta_new(void)
{
    return (meta_box *)calloc(1, sizeof(meta_box));
}

void meta_delete(meta_box *b) { free(b); }

/* the type follows position and composition: always recomputed */
meta_disk *meta_disk_get(meta_box *b, int n)
{
    int i, j;
    meta_disk *d = NULL;
    if (n < 1) return NULL;
    for (i = 0; i < b->ndisk; i++) if (b->disk[i].n == n) d = &b->disk[i];
    if (!d) {
        if (b->ndisk >= META_MAXDISK) return NULL;
        for (i = 0; i < b->ndisk && b->disk[i].n < n; i++) ;
        for (j = b->ndisk; j > i; j--) b->disk[j] = b->disk[j - 1];
        d = &b->disk[i];
        memset(d, 0, sizeof(meta_disk));
        d->n = n;
        b->ndisk++;
    }
    meta_slot(b, n, &d->kind, &d->num);
    return d;
}

void meta_now(char out[20])
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(out, 20, "%Y-%m-%dT%H:%M:%S", lt);
}

/* empty optional fields become null */
static void opt(json_writer *w, const char *key, const char *v)
{
    if (v[0]) jw_str(w, key, v); else jw_null(w, key);
}

static void write_disk(json_writer *w, const meta_disk *d, const char *datadir,
                       meta_tech_fn tech, void *ctx)
{
    char mp[1024]; int k;
    jw_begin_obj(w, NULL);
    jw_int(w, "n", d->n);
    jw_str(w, "type", meta_kind_key(d->kind));
    jw_int(w, "number", d->num);
    jw_str(w, "label", d->label);
    jw_str(w, "note", d->note);
    jw_str(w, "status", d->status[0] ? d->status : ST_TO_READ);
    /* only when there is one: older readers do not know the field */
    if (d->duplicate_of > 0) jw_int(w, "duplicate_of", d->duplicate_of);
    if (d->copy_protection) jw_str(w, "copy_protection_scan", meta_prot_key(d->copy_protection));
    opt(w, "master", d->master);
    opt(w, "scan", d->scan);
    jw_begin_arr(w, "reads");
    for (k = 0; k < d->nreads; k++) {
        jw_begin_obj(w, NULL);
        jw_str(w, "file", d->reads[k].file);
        jw_int(w, "bytes", d->reads[k].bytes);
        jw_str(w, "sha256", d->reads[k].sha256);
        jw_str(w, "at", d->reads[k].at);
        jw_end_obj(w);
    }
    jw_end_arr(w);
    if (d->verdict[0]) {
        jw_begin_obj(w, "compare");
        jw_str(w, "verdict", d->verdict);
        jw_int(w, "agree", d->agree);
        jw_int(w, "unstable_tracks", d->unstable_tracks);
        opt(w, "report", d->report);
        jw_end_obj(w);
    } else {
        jw_null(w, "compare");
    }
    /* the technical data come from the master image: not part of the model */
    if (tech && d->master[0]) {
        u8_join(mp, sizeof(mp), datadir, d->master);
        tech(ctx, d, mp, w);
    }
    jw_end_obj(w);
}

int meta_save(const meta_box *b, const char *datadir, meta_tech_fn tech, void *ctx, strbuf *err)
{
    json_writer w; char path[1024], tmp[1040]; FILE *f; int i, k, ok, c[MEDIA_KINDS];

    for (k = 0; k < MEDIA_KINDS; k++) c[k] = meta_count(b, k);

    jw_init(&w);
    jw_begin_obj(&w, NULL);
    jw_str(&w, "schema", META_SCHEMA);
    jw_begin_obj(&w, "box");
    jw_str(&w, "title", b->title);
    opt(&w, "publisher", b->publisher);
    opt(&w, "year", b->year);
    jw_str(&w, "platform", b->platform);
    opt(&w, "language", b->language);
    opt(&w, "edition", b->edition);
    opt(&w, "notes", b->notes);
    jw_begin_arr(&w, "media");
    for (k = 0; k < MEDIA_KINDS; k++) {
        if (!c[k]) continue;
        jw_begin_obj(&w, NULL);
        jw_str(&w, "type", meta_kind_key(k));
        jw_int(&w, "count", c[k]);
        jw_end_obj(&w);
    }
    jw_end_arr(&w);
    jw_int(&w, "media_expected", b->media_expected);
    jw_begin_obj(&w, "names");
    for (k = 0; k < MEDIA_KINDS; k++)
        if (c[k] && b->names[k][0]) jw_str(&w, meta_kind_key(k), b->names[k]);
    jw_end_obj(&w);
    jw_str(&w, "copy_protection_scan", meta_prot_key(b->copy_protection));
    opt(&w, "started", b->started);
    opt(&w, "closed", b->closed);
    jw_end_obj(&w);
    jw_begin_obj(&w, "software");
    jw_str(&w, "retroarchiver", RA_VERSION);
    jw_null(&w, "samdisk");
    jw_end_obj(&w);
    if (b->history[0]) {
        char perr[160]; jval *h = json_parse(b->history, perr, sizeof(perr));
        if (h) json_write_val(&w, "history", h);
        json_free(h);
    }
    jw_begin_arr(&w, "media");
    for (i = 0; i < b->ndisk; i++) write_disk(&w, &b->disk[i], datadir, tech, ctx);
    jw_end_arr(&w);
    jw_end_obj(&w);

    /* first a temporary file, then the rename: metadata.json is never
     * left half-written, not even on a power cut */
    u8_join(path, sizeof(path), datadir, "metadata.json");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = u8_fopen(tmp, "wb");
    ok = f && fwrite(w.out.s, 1, w.out.len, f) == w.out.len;
    if (ok && (fflush(f) != 0 || SYNC_FILE(f) != 0)) ok = 0;   /* on disk before the rename */
    if (f && fclose(f) != 0) ok = 0;
    if (ok && u8_rename_replace(tmp, path) != 0) ok = 0;
    if (!ok) {
        sb_printf(err, "cannot write %s: is the destination disk full or write-protected?", path);
        u8_remove(tmp);
    }
    jw_free(&w);
    return ok ? 0 : -1;
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

static void load_disk(meta_box *b, const jval *e)
{
    meta_disk *d = meta_disk_get(b, (int)json_int(e, "n", 0));
    const jval *r, *cmp;
    if (!d) return;
    CP(d->label, json_str(e, "label"));
    CP(d->note, json_str(e, "note"));
    CP(d->status, meta_status_english(json_str(e, "status")));
    if (!strcmp(d->status, ST_TO_READ)) d->status[0] = 0;
    d->duplicate_of = (int)json_int(e, "duplicate_of", 0);     /* absent in older boxes */
    d->copy_protection = meta_prot_parse(json_str(e, "copy_protection_scan"));
    if (d->copy_protection < 0) d->copy_protection = META_PROT_AUTO;      /* the box decides */
    CP(d->master, json_str(e, "master"));
    CP(d->scan, json_str(e, "scan"));
    r = json_get(e, "reads");
    for (r = r ? r->child : NULL; r && d->nreads < META_MAXREAD; r = r->next) {
        meta_read *m = &d->reads[d->nreads++];
        CP(m->file, json_str(r, "file"));
        m->bytes = json_int(r, "bytes", 0);
        CP(m->sha256, json_str(r, "sha256"));
        CP(m->at, json_str(r, "at"));
    }
    cmp = json_get(e, "compare");
    if (cmp && cmp->t == J_OBJ) {
        CP(d->verdict, json_str(cmp, "verdict"));
        d->agree = (int)json_int(cmp, "agree", 0);
        d->unstable_tracks = (int)json_int(cmp, "unstable_tracks", 0);
        CP(d->report, json_str(cmp, "report"));
    }
}

/* box.media: [{"type": "floppy", "count": 3}, ...] and box.names */
static void load_counts(meta_box *b, const jval *box)
{
    const jval *m = json_get(box, "media"), *e, *names = json_get(box, "names");
    int c[MEDIA_KINDS] = { 0 }, k;
    for (e = m && m->t == J_ARR ? m->child : NULL; e; e = e->next) {
        k = meta_kind_parse(json_str(e, "type"));
        if (k >= 0) c[k] += (int)json_int(e, "count", 0);
    }
    meta_set_counts(b, c[MEDIA_FLOPPY], c[MEDIA_CD], c[MEDIA_DVD]);
    for (k = 0; names && names->t == J_OBJ && k < MEDIA_KINDS; k++)
        CP(b->names[k], json_str(names, meta_kind_key(k)));
}

static void load_history(meta_box *b, const jval *h)
{
    json_writer w;
    if (!h || h->t != J_ARR || !h->child) return;
    jw_init(&w);
    json_write_val(&w, NULL, h);
    if (w.out.s && w.out.len < sizeof(b->history)) CP(b->history, w.out.s);
    jw_free(&w);
}

int meta_history_add(meta_box *b, const char *entry)
{
    char perr[160]; jval *e = json_parse(entry, perr, sizeof(perr));
    size_t have = strlen(b->history), add = strlen(entry);
    char *end;
    if (!e || e->t != J_OBJ) { json_free(e); return -1; }
    json_free(e);
    if (have + add + 8 > sizeof(b->history)) return -1;
    if (!have) { snprintf(b->history, sizeof(b->history), "[%s]", entry); return 0; }
    /* "[..., entry]": the array always ends with ']' and maybe a newline */
    end = strrchr(b->history, ']');
    if (!end) return -1;
    snprintf(end, sizeof(b->history) - (size_t)(end - b->history), ",%s]", entry);
    return 0;
}

int meta_load(meta_box *b, const char *datadir, strbuf *err)
{
    char path[1024], perr[160]; char *text; jval *root; const jval *box, *media, *e;
    const char *schema; int old;

    u8_join(path, sizeof(path), datadir, "metadata.json");
    text = slurp(path);
    if (!text) { sb_printf(err, "cannot find %s" FIXIT, path); return -1; }
    root = json_parse(text, perr, sizeof(perr));
    free(text);
    if (!root) { sb_printf(err, "%s cannot be read (%s)" FIXIT, path, perr); return -1; }
    schema = json_str(root, "schema");
    old = schema && !strcmp(schema, "retroarchiver-floppy-1");
    if (!schema || (!old && strcmp(schema, META_SCHEMA) && strcmp(schema, META_SCHEMA_2))) {
        if (schema && !strncmp(schema, "retroarchiver-", 14))
            sb_printf(err, "%s comes from a newer version of RetroArchiver: "
                           "update the program to open this box", path);
        else
            sb_printf(err, "%s is not a RetroArchiver box" FIXIT, path);
        json_free(root);
        return -1;
    }
    memset(b, 0, sizeof(*b));
    CP(b->schema, schema);
    load_history(b, json_get(root, "history"));
    box = json_get(root, "box");
    CP(b->title, json_str(box, "title"));
    CP(b->publisher, json_str(box, "publisher"));
    CP(b->year, json_str(box, "year"));
    CP(b->platform, json_str(box, "platform"));
    CP(b->language, json_str(box, "language"));
    CP(b->edition, json_str(box, "edition"));
    CP(b->notes, json_str(box, "notes"));
    CP(b->started, json_str(box, "started"));
    CP(b->closed, json_str(box, "closed"));
    /* boxes made before the choice: the files on the disc decide */
    b->copy_protection = meta_prot_parse(json_str(box, "copy_protection_scan"));
    if (b->copy_protection < 0) b->copy_protection = META_PROT_AUTO;
    b->media_expected = (int)json_int(box, "media_expected", 0);
    /* retroarchiver-floppy-1: all floppies, and the names stay those of the files */
    if (old) meta_set_counts(b, b->media_expected, 0, 0);
    else load_counts(b, box);
    media = json_get(root, "media");
    for (e = media ? media->child : NULL; e; e = e->next) load_disk(b, e);
    json_free(root);
    return 0;
}
