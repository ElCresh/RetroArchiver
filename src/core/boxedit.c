#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boxedit.h"
#include "archive.h"
#include "fsu8.h"
#include "media.h"
#include "session.h"

#define CP(dst, src) str_copy(dst, sizeof(dst), src)

/* the technical data of the media an edit does not read: the old ones */
typedef struct {
    jval *old;                  /* the metadata.json before the edit */
    int kind, num;              /* the medium read again: its data is computed; kind -1 none */
    meta_tech_fn inner;
    void *ctx;
} keep_tech;

int boxedit_grade(const char *status, const char *method)
{
    if (!strcmp(status, ST_REDUMP)) return 6;
    if (!strcmp(status, ST_CLEAN)) return method && !strcmp(method, "dic-data") ? 4 : 5;
    if (!strcmp(status, ST_MARGINAL) || !strcmp(status, ST_UNSTABLE)) return 3;
    if (!strcmp(status, ST_FRAGILE) || !strcmp(status, ST_READ_ONCE)) return 2;
    if (!strcmp(status, ST_ERROR) || !strcmp(status, ST_ERRORS)) return 1;
    return 0;
}

int boxedit_decide(int old_grade, const char *old_key, int new_grade, const char *new_key)
{
    if (new_grade > old_grade) return KEEP_NEW;
    if (new_grade < old_grade || new_grade == 0) return KEEP_OLD;
    return strcmp(old_key, new_key) ? KEEP_ASK : KEEP_OLD_IDENTICAL;
}

int boxedit_editable(const char *bagdir, strbuf *why)
{
    arc_box *a = (arc_box *)calloc(1, sizeof(arc_box)); int ok = 0;
    if (!a) return 0;
    if (!arc_load(bagdir, a)) sb_printf(why, "%s is not a box", bagdir);
    else if (a->editing)
        sb_puts(why, "an edit of this box was interrupted: open the archive again to finish or undo it");
    else if (a->state == ARC_OPEN) sb_puts(why, "the box is still open: resume it instead");
    else if (a->state == ARC_BROKEN) sb_printf(why, "the box cannot be read: %s", a->error);
    else if (a->old_format) sb_puts(why, "the box was made by RetroArchiver 261017: convert it first");
    else ok = 1;
    free(a);
    return ok;
}

void boxedit_tech(void *ctx, const meta_disk *d, const char *img, json_writer *w)
{
    static const media_ops *const ops[MEDIA_KINDS] = { &floppy_media, &cd_media, &dvd_media };
    (void)ctx;
    if (d->kind >= 0 && d->kind < MEDIA_KINDS && ops[d->kind]->tech) ops[d->kind]->tech(NULL, img, w);
}

/* ---- the technical data, as it was ----
 * An edit writes metadata.json again: the media it does not read keep the
 * technical data written when they were read (computed again now, a CD
 * could lose its redump match because the DATs moved) */

/* the fields of a medium in metadata.json that are the model, not technical data */
static int model_key(const char *k)
{
    static const char *const keys[] = { "n", "type", "number", "label", "note", "status", "duplicate_of",
                                        "copy_protection_scan", "master", "scan", "reads", "compare" };
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) if (k && !strcmp(k, keys[i])) return 1;
    return 0;
}

static void keep_tech_open(keep_tech *k, const char *bagdir, int kind, int num, meta_tech_fn inner, void *ctx)
{
    char data[1040], p[1100], perr[160]; FILE *f; long n; char *s;
    memset(k, 0, sizeof(*k));
    k->kind = kind; k->num = num; k->inner = inner; k->ctx = ctx;
    u8_join(data, sizeof(data), bagdir, "data");
    u8_join(p, sizeof(p), data, "metadata.json");
    if (!(f = u8_fopen(p, "rb"))) return;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0 && (s = (char *)malloc((size_t)n + 1)) != NULL) {
        if (fread(s, 1, (size_t)n, f) == (size_t)n) { s[n] = 0; k->old = json_parse(s, perr, sizeof(perr)); }
        free(s);
    }
    fclose(f);
}

static void keep_tech_close(keep_tech *k)
{
    json_free(k->old);
    k->old = NULL;
}

/* the old technical data of d, or the inner function for the medium just read */
static void kept_tech(void *c, const meta_disk *d, const char *img, json_writer *w)
{
    keep_tech *k = (keep_tech *)c; const jval *media, *e, *f;
    if (k->old && !(d->kind == k->kind && d->num == k->num)) {
        media = json_get(k->old, "media");
        for (e = media ? media->child : NULL; e; e = e->next) {
            const char *t = json_str(e, "type");
            /* retroarchiver-floppy-1 has neither type nor number: all floppies, by position */
            if (t ? strcmp(t, meta_kind_key(d->kind)) || json_int(e, "number", 0) != d->num
                  : d->kind != MEDIA_FLOPPY || json_int(e, "n", 0) != d->n) continue;
            for (f = e->child; f; f = f->next) if (!model_key(f->key)) json_write_val(w, f->key, f);
            return;
        }
    }
    if (k->inner) k->inner(k->ctx, d, img, w);
}

/* ---- data and composition ---- */

static const char *const field_keys[] = { "title", "platform", "publisher", "year", "language", "edition", "notes" };
#define NFIELD 7

static const char *field_of(const meta_box *b, int i)
{
    switch (i) {
    case 0: return b->title;     case 1: return b->platform;
    case 2: return b->publisher; case 3: return b->year;
    case 4: return b->language;  case 5: return b->edition;
    default: return b->notes;
    }
}

static void set_field(meta_box *b, int i, const char *v)
{
    switch (i) {
    case 0: CP(b->title, v);     break; case 1: CP(b->platform, v); break;
    case 2: CP(b->publisher, v); break; case 3: CP(b->year, v);     break;
    case 4: CP(b->language, v);  break; case 5: CP(b->edition, v);  break;
    default: CP(b->notes, v);
    }
}

static void parent_dir(const char *bagdir, char *out, size_t n)
{
    char *s;
    str_copy(out, n, bagdir);
    s = out + strlen(out);
    while (s > out && s[-1] != '/' && s[-1] != '\\') s--;
    if (s > out) s--;
    *s = 0;
}

static const char *base_of(const char *p)
{
    const char *s = p + strlen(p);
    while (s > p && s[-1] != '/' && s[-1] != '\\') s--;
    return s;
}

/* equal paths, with '/' and '\' the same separator; the case counts: a
 * title that only changes case renames the folder */
static int same_path(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (*a != *b && !((*a == '/' || *a == '\\') && (*b == '/' || *b == '\\'))) return 0;
    return *a == *b;
}

/* the folder the edited box gets, "" if it stays where it is */
static void new_folder(const char *bagdir, const meta_box *old, const meta_box *b, char *out, size_t n)
{
    char root[1024];
    out[0] = 0;
    if (!strcmp(old->title, b->title) && !strcmp(old->edition, b->edition)) return;
    parent_dir(bagdir, root, sizeof(root));
    session_bagdir_fresh(out, n, root, b->title, b->edition);
    if (same_path(out, bagdir)) out[0] = 0;
}

static edit_change *slot_at(edit_change *out, int max, int n, int what)
{
    if (n >= max) return NULL;
    memset(&out[n], 0, sizeof(out[n]));
    out[n].what = what;
    return &out[n];
}

int boxedit_diff(const char *bagdir, const meta_box *old, const meta_box *edited, edit_change *out, int max)
{
    char nd[1024]; int i, k, num, n = 0; edit_change *c;
    for (i = 0; i < NFIELD; i++) {
        if (!strcmp(field_of(old, i), field_of(edited, i))) continue;
        if ((c = slot_at(out, max, n, CH_FIELD)) != NULL) {
            CP(c->field, field_keys[i]);
            CP(c->from, field_of(old, i));
            CP(c->to, field_of(edited, i));
        }
        n++;
    }
    for (k = 0; k < MEDIA_KINDS; k++) {
        int was = meta_count(old, k), now = meta_count(edited, k);
        for (num = now + 1; num <= was; num++, n++)
            if ((c = slot_at(out, max, n, CH_REMOVED)) != NULL) { c->kind = k; c->num = num; }
        for (num = was + 1; num <= now; num++, n++)
            if ((c = slot_at(out, max, n, CH_ADDED)) != NULL) { c->kind = k; c->num = num; }
    }
    new_folder(bagdir, old, edited, nd, sizeof(nd));
    if (nd[0]) {
        if ((c = slot_at(out, max, n, CH_FOLDER)) != NULL) {
            CP(c->from, base_of(bagdir));
            CP(c->to, base_of(nd));
        }
        n++;
    }
    return n;
}

/* "floppy 3", as in the history */
static void medium_key(int kind, int num, char *out, size_t n)
{
    snprintf(out, n, "%s %d", meta_kind_key(kind), num);
}

static int position(const int c[MEDIA_KINDS], int kind, int num)
{
    int k, pos = num;
    for (k = 0; k < kind; k++) pos += c[k];
    return pos;
}

/* every file of d out of data/, as NAME.removed, listed in w */
static void remove_disk(const char *bagdir, const meta_disk *d, txn_moves *m, json_writer *w)
{
    const char *files[META_MAXREAD + 2]; char from[400], to[400]; int i, nf = 0;
    for (i = 0; i < d->nreads; i++) files[nf++] = d->reads[i].file;
    if (d->scan[0]) files[nf++] = d->scan;
    if (d->report[0]) files[nf++] = d->report;
    jw_begin_arr(w, "set_aside");
    for (i = 0; i < nf; i++) {
        snprintf(from, sizeof(from), "data/%s", files[i]);
        txn_aside_name(bagdir, m, files[i], EDIT_REMOVED, to, sizeof(to));
        txn_add(m, from, to);
        jw_str(w, NULL, to);
    }
    jw_end_arr(w);
}

static int by_n(const void *a, const void *b)
{
    return ((const meta_disk *)a)->n - ((const meta_disk *)b)->n;
}

/* the media of old in the composition of nb: the removed ones (the last of
 * each kind) out of data/, the added ones missing, all at their new position */
static void recompose(meta_box *nb, const meta_box *old, const char *bagdir, txn_moves *m, json_writer *w)
{
    int c[MEDIA_KINDS], map[META_MAXDISK + 1], i, k, num, nd = 0;
    char who[32];
    meta_disk *out = (meta_disk *)calloc(META_MAXDISK, sizeof(meta_disk));
    if (!out) return;
    for (k = 0; k < MEDIA_KINDS; k++) c[k] = meta_count(nb, k);
    memset(map, 0, sizeof(map));
    for (i = 0; i < old->ndisk; i++) {
        const meta_disk *d = &old->disk[i];
        if (d->num > c[d->kind]) {
            jw_begin_obj(w, NULL);
            medium_key(d->kind, d->num, who, sizeof(who));
            jw_str(w, "medium", who);
            jw_str(w, "change", "removed");
            remove_disk(bagdir, d, m, w);
            jw_end_obj(w);
            continue;
        }
        if (nd < META_MAXDISK) {
            out[nd] = *d;
            out[nd].n = position(c, d->kind, d->num);
            if (d->n >= 1 && d->n <= META_MAXDISK) map[d->n] = out[nd].n;
            nd++;
        }
    }
    for (k = 0; k < MEDIA_KINDS; k++)
        for (num = meta_count(old, k) + 1; num <= c[k] && nd < META_MAXDISK; num++) {
            memset(&out[nd], 0, sizeof(out[nd]));
            out[nd].kind = k; out[nd].num = num; out[nd].n = position(c, k, num);
            CP(out[nd].status, ST_MISSING);
            nd++;
            jw_begin_obj(w, NULL);
            medium_key(k, num, who, sizeof(who));
            jw_str(w, "medium", who);
            jw_str(w, "change", "added");
            jw_end_obj(w);
        }
    for (i = 0; i < nd; i++)
        if (out[i].duplicate_of > 0 && out[i].duplicate_of <= META_MAXDISK) out[i].duplicate_of = map[out[i].duplicate_of];
    qsort(out, (size_t)nd, sizeof(meta_disk), by_n);
    memcpy(nb->disk, out, (size_t)nd * sizeof(meta_disk));
    nb->ndisk = nd;
    free(out);
}

/* a naming scheme with {set} keeps the old title: the files already read
 * and the new reads of the box keep the same kind of name */
static int freeze_set(meta_box *nb, const char *old_title, strbuf *err)
{
    char tpl[400], *s; int k;
    for (k = 0; k < MEDIA_KINDS; k++) {
        if (!(s = strstr(nb->names[k], "{set}"))) continue;
        snprintf(tpl, sizeof(tpl), "%.*s%s%s", (int)(s - nb->names[k]), nb->names[k], old_title, s + 5);
        if (strlen(tpl) >= sizeof(nb->names[k])) {
            sb_printf(err, "the file names of the %s use the title, and with \"%s\" they would be too long: "
                           "choose a shorter title", meta_kind_label(k), old_title);
            return -1;
        }
        CP(nb->names[k], tpl);
    }
    return 0;
}

static void entry_head(json_writer *w, const char *action, const char *reason)
{
    char now[20];
    meta_now(now);
    jw_begin_obj(w, NULL);
    jw_str(w, "at", now);
    jw_str(w, "action", action);
    jw_str(w, "software", "RetroArchiver " RA_VERSION);
    if (reason && reason[0]) jw_str(w, "reason", reason);
}

/* the box's own fields in nb, and their entries in w */
static void apply_fields(meta_box *nb, const meta_box *edited, json_writer *w)
{
    int i, any = 0;
    for (i = 0; i < NFIELD; i++) {
        if (!strcmp(field_of(nb, i), field_of(edited, i))) continue;
        if (!any++) jw_begin_arr(w, "box");
        jw_begin_obj(w, NULL);
        jw_str(w, "field", field_keys[i]);
        jw_str(w, "from", field_of(nb, i));
        jw_str(w, "to", field_of(edited, i));
        jw_end_obj(w);
        set_field(nb, i, field_of(edited, i));
    }
    if (any) jw_end_arr(w);
}

static int damaged(const char *bagdir, bag_digests *ok, strbuf *err)
{
    arc_check c;
    if (arc_verify_digests(bagdir, &c, ok, err)) return -1;
    if (!c.changed && !c.lost && !c.extra) return 0;
    sb_printf(err, "%s does not match the manifest: read that medium again with Edit, "
                   "without changing the data of the box", c.first);
    return -1;
}

int boxedit_save_data(const char *bagdir, const meta_box *edited, const char *reason,
                      char *newbagdir, size_t n, strbuf *err)
{
    meta_box *old = meta_new(), *nb = meta_new();
    char data[1040], nd[1024]; json_writer w; txn_moves m; bag_digests ok; edit_change one; keep_tech kt;
    int rc = -1, rc2;

    str_copy(newbagdir, n, bagdir);
    memset(&m, 0, sizeof(m)); memset(&ok, 0, sizeof(ok));
    jw_init(&w);
    if (!old || !nb) { sb_puts(err, "out of memory"); goto out; }
    if (!boxedit_editable(bagdir, err)) goto out;
    u8_join(data, sizeof(data), bagdir, "data");
    if (meta_load(old, data, err)) goto out;
    if (!boxedit_diff(bagdir, old, edited, &one, 1)) { rc = 0; goto out; }
    if (!edited->title[0] || !edited->platform[0]) {
        sb_puts(err, "the title and the platform cannot be empty");
        goto out;
    }
    if (damaged(bagdir, &ok, err)) goto out;

    *nb = *old;
    new_folder(bagdir, old, edited, nd, sizeof(nd));
    entry_head(&w, "edit", reason);
    if (nd[0]) {
        jw_begin_obj(&w, "folder");
        jw_str(&w, "from", base_of(bagdir));
        jw_str(&w, "to", base_of(nd));
        jw_end_obj(&w);
    }
    if (strcmp(old->title, edited->title) && freeze_set(nb, old->title, err)) goto out;
    apply_fields(nb, edited, &w);
    meta_set_counts(nb, meta_count(edited, MEDIA_FLOPPY), meta_count(edited, MEDIA_CD), meta_count(edited, MEDIA_DVD));
    jw_begin_arr(&w, "media");
    recompose(nb, old, bagdir, &m, &w);
    jw_end_arr(&w);
    jw_end_obj(&w);
    if (meta_history_add(nb, w.out.s)) {
        sb_puts(err, "the history of the box is full: no more changes can be recorded");
        goto out;
    }
    meta_now(nb->closed);
    keep_tech_open(&kt, bagdir, -1, 0, boxedit_tech, NULL);
    rc2 = boxtxn_commit(bagdir, &m, nb, kept_tech, &kt, &ok, nd[0] ? nd : NULL, err);
    keep_tech_close(&kt);
    if (rc2) goto out;
    if (nd[0]) str_copy(newbagdir, n, nd);
    {
        /* archive.csv is a log: a row for each medium removed */
        char root[1024]; int i;
        parent_dir(bagdir, root, sizeof(root));
        for (i = 0; i < old->ndisk; i++)
            if (old->disk[i].num > meta_count(nb, old->disk[i].kind))
                session_csv_row(root, nb->title, &old->disk[i], "removed");
    }
    rc = 0;
out:
    jw_free(&w);
    txn_free(&m);
    bag_digests_free(&ok);
    meta_delete(old);
    meta_delete(nb);
    return rc;
}

/* ---- one medium read again ---- */

/* the files of a medium in its folder: the reads, the scan, the report */
static int disk_files(const meta_disk *d, const char *files[META_MAXREAD + 2])
{
    int i, n = 0;
    for (i = 0; i < d->nreads; i++) files[n++] = d->reads[i].file;
    if (d->scan[0]) files[n++] = d->scan;
    if (d->report[0]) files[n++] = d->report;
    return n;
}

static void rel_path(const char *bagdir, const char *rel, char *out, size_t n)
{
    char r[400], *q;
    CP(r, rel);
    for (q = r; *q; q++) if (*q == '/') *q = U8_SEP[0];
    u8_join(out, n, bagdir, r);
}

/* rel, or a file under it, did not match the manifest */
static int is_bad(const bag_digests *bad, const char *rel)
{
    size_t l = strlen(rel); int i;
    for (i = 0; bad && i < bad->n; i++)
        if (!strncmp(bad->rel[i], rel, l) && (!bad->rel[i][l] || bad->rel[i][l] == '/')) return 1;
    return 0;
}

/* the new reads out of editing/data, into set-aside with suffix; one that
 * cannot be moved stays where it is, never deleted */
static void drop_new(const char *bagdir, const meta_disk *now, const char *suffix)
{
    const char *files[META_MAXREAD + 2]; char from[1024], to[1024], rel[400], aside[400];
    int i, n = disk_files(now, files);
    for (i = 0; i < n; i++) {
        snprintf(rel, sizeof(rel), BOXTXN_STAGE "/%s", files[i]);
        rel_path(bagdir, rel, from, sizeof(from));
        if (!u8_exists(from)) continue;
        txn_aside_name(bagdir, NULL, files[i], suffix, aside, sizeof(aside));
        rel_path(bagdir, aside, to, sizeof(to));
        {
            char dir[1024];
            rel_path(bagdir, session_aside_dir(bagdir), dir, sizeof(dir));
            u8_mkdir(dir);
        }
        u8_rename_replace(from, to);
    }
}

static const char *master_sha(const meta_disk *d)
{
    int i;
    for (i = 0; i < d->nreads; i++) if (!strcmp(d->reads[i].file, d->master)) return d->reads[i].sha256;
    return "";
}

static const char *master_at(const meta_disk *d)
{
    int i;
    for (i = 0; i < d->nreads; i++) if (!strcmp(d->reads[i].file, d->master)) return d->reads[i].at;
    return "";
}

static void side(json_writer *w, const char *key, const meta_disk *d, const media_identity *id)
{
    jw_begin_obj(w, key);
    jw_str(w, "master", d->master);
    jw_str(w, "sha256", master_sha(d));
    jw_str(w, "status", d->status[0] ? d->status : ST_TO_READ);
    if (id->method[0]) jw_str(w, "read_method", id->method);
    if (id->drive[0]) jw_str(w, "drive", id->drive);
    if (master_at(d)[0]) jw_str(w, "read_at", master_at(d));
    jw_end_obj(w);
}

/* the old read's files out of data/: .replaced, or .damaged for those
 * that no longer match the manifest; 1 if one of them was damaged */
static int old_files_aside(const char *bagdir, const meta_disk *old, const bag_digests *bad,
                           txn_moves *m, json_writer *w)
{
    const char *files[META_MAXREAD + 2]; char from[400], to[400], p[1024]; int i, n, damaged = 0;
    n = disk_files(old, files);
    for (i = 0; i < n; i++) {
        snprintf(from, sizeof(from), "data/%s", files[i]);
        if (is_bad(bad, from)) damaged = 1;
        rel_path(bagdir, from, p, sizeof(p));
        if (!u8_exists(p)) continue;            /* lost: nothing to set aside */
        txn_aside_name(bagdir, m, files[i], is_bad(bad, from) ? SESSION_DAMAGED : EDIT_REPLACED, to, sizeof(to));
        txn_add(m, from, to);
        jw_str(w, NULL, to);
    }
    return damaged;
}

/* the new read's files from editing/data: into data/, or into set-aside with suffix */
static void new_files(const char *bagdir, const meta_disk *now, const char *suffix, txn_moves *m, json_writer *w)
{
    const char *files[META_MAXREAD + 2]; char from[400], to[400]; int i, n = disk_files(now, files);
    for (i = 0; i < n; i++) {
        snprintf(from, sizeof(from), BOXTXN_STAGE "/%s", files[i]);
        if (suffix) {
            txn_aside_name(bagdir, m, files[i], suffix, to, sizeof(to));
            jw_str(w, NULL, to);
        } else {
            snprintf(to, sizeof(to), "data/%s", files[i]);
        }
        txn_add(m, from, to);
    }
}

/* known plus the damaged files with their digest from the old manifest: a
 * transaction of another medium must not approve their changed bytes */
static void with_bad(const bag_digests *known, const bag_digests *bad, bag_digests *out)
{
    int i;
    for (i = 0; known && i < known->n; i++) bag_digests_add(out, known->rel[i], known->hex[i]);
    for (i = 0; bad && i < bad->n; i++)
        if (bag_digests_find(out, bad->rel[i]) < 0) bag_digests_add(out, bad->rel[i], bad->hex[i]);
}

int boxedit_commit_disk(const char *bagdir, meta_box *box, const edit_disk *e, bag_digests *known,
                        const bag_digests *bad, meta_tech_fn tech, void *ctx, strbuf *err)
{
    char who[32]; meta_box *nb; meta_disk *d; json_writer w; txn_moves m; bag_digests trust;
    keep_tech kt; int rc = -1, damaged = 0, same = e->keep == KEEP_OLD_IDENTICAL;

    if (e->keep == KEEP_OLD) { drop_new(bagdir, e->now, EDIT_REJECTED); return 0; }
    if (!(nb = meta_new())) { sb_puts(err, "out of memory"); return -1; }
    *nb = *box;
    memset(&m, 0, sizeof(m)); memset(&trust, 0, sizeof(trust));
    jw_init(&w);
    entry_head(&w, "edit", e->reason);
    jw_begin_arr(&w, "media");
    jw_begin_obj(&w, NULL);
    medium_key(e->now->kind, e->now->num, who, sizeof(who));
    jw_str(&w, "medium", who);
    jw_str(&w, "change", e->old->master[0] ? "reread" : "read");
    /* identical: kept, not deleted; the history says the old read was confirmed */
    jw_str(&w, "kept", same ? "old-identical" : "new");
    if (e->old->master[0]) side(&w, "old", e->old, &e->old_id);
    side(&w, "new", e->now, &e->new_id);
    jw_begin_arr(&w, "set_aside");
    if (same) new_files(bagdir, e->now, EDIT_IDENTICAL, &m, &w);
    else damaged = old_files_aside(bagdir, e->old, bad, &m, &w);
    jw_end_arr(&w);
    if (damaged) jw_bool(&w, "old_damaged", 1);
    jw_end_obj(&w);
    jw_end_arr(&w);
    jw_end_obj(&w);
    if (!same) new_files(bagdir, e->now, NULL, &m, NULL);
    d = meta_disk_get(nb, e->now->n);
    if (d && !same) *d = *e->now;
    meta_now(nb->closed);
    with_bad(known, bad, &trust);
    keep_tech_open(&kt, bagdir, same ? -1 : e->now->kind, e->now->num, tech, ctx);
    if (!d || meta_history_add(nb, w.out.s)) {
        sb_puts(err, "the history of the box is full: no more changes can be recorded");
    } else if (boxtxn_commit(bagdir, &m, nb, kept_tech, &kt, &trust, NULL, err) == 0) {
        *box = *nb;
        bag_digests_free(known);
        bag_manifest_load(bagdir, "manifest-sha256.txt", known);
        rc = 0;
    }
    keep_tech_close(&kt);
    bag_digests_free(&trust);
    jw_free(&w);
    txn_free(&m);
    meta_delete(nb);
    return rc;
}

/* ---- the boxes of 261017 ---- */

int boxedit_is_old(const char *bagdir)
{
    arc_box *a = (arc_box *)calloc(1, sizeof(arc_box)); int old;
    if (!a) return 0;
    old = arc_load(bagdir, a) && a->old_format && (a->state == ARC_COMPLETE || a->state == ARC_INCOMPLETE);
    free(a);
    return old;
}

typedef struct { char (*name)[300]; int n, cap; } conv_list;

static void conv_name(void *c, const char *name, int isdir)
{
    conv_list *l = (conv_list *)c;
    (void)isdir;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->name = (char (*)[300])realloc(l->name, (size_t)l->cap * sizeof(*l->name));
    }
    CP(l->name[l->n], name);
    l->n++;
}

/* every entry of messi-da-parte/ into set-aside/ with its new name, free
 * there and among the moves already planned; listed in w */
static void aside_moves(const char *bagdir, txn_moves *m, json_writer *w)
{
    char dir[1024], nn[300], from[400], to[400], p[1100]; conv_list l; int i, j, k;
    u8_join(dir, sizeof(dir), bagdir, SESSION_ASIDE_DIR_OLD);
    memset(&l, 0, sizeof(l));
    u8_listdir(dir, conv_name, &l);
    for (i = 0; i < l.n; i++) {
        session_aside_rename(l.name[i], nn, sizeof(nn));
        snprintf(from, sizeof(from), SESSION_ASIDE_DIR_OLD "/%s", l.name[i]);
        snprintf(to, sizeof(to), SESSION_ASIDE_DIR "/%s", nn);
        for (j = 2; j < 1000; j++) {
            int taken = 0;
            rel_path(bagdir, to, p, sizeof(p));
            for (k = 0; k < m->n && !taken; k++) taken = !strcmp(m->to[k], to);
            if (!taken && !u8_exists(p)) break;
            snprintf(to, sizeof(to), SESSION_ASIDE_DIR "/%s%d", nn, j);
        }
        txn_add(m, from, to);
        jw_begin_obj(w, NULL);
        jw_str(w, "from", from);
        jw_str(w, "to", to);
        jw_end_obj(w);
    }
    free(l.name);
}

#define WHY(c) (why->code = (c))

int boxedit_convert(const char *bagdir, conv_why *whyp, strbuf *err)
{
    char dir[1024], fin[1040], data[1040], p[1100]; strbuf log; arc_box *a; meta_box *old = NULL, *nb = NULL;
    json_writer w; txn_moves m; bag_digests ok; keep_tech kt; arc_check ck; conv_why spare; int rc = -1;
    conv_why *why = whyp ? whyp : &spare;

    memset(why, 0, sizeof(*why));
    WHY(CONV_FAILED);
    memset(&m, 0, sizeof(m)); memset(&ok, 0, sizeof(ok));
    jw_init(&w);
    str_copy(dir, sizeof(dir), bagdir);
    /* an edit left halfway first: finished or undone */
    sb_init(&log);
    if (boxtxn_recover(dir, boxedit_tech, NULL, fin, sizeof(fin), &log) < 0) {
        sb_printf(err, "an interrupted change could not be put in order: %s", log.s ? log.s : "");
        WHY(CONV_UNFINISHED);
        sb_free(&log);
        goto out;
    }
    sb_free(&log);
    str_copy(dir, sizeof(dir), fin);
    if (!(a = (arc_box *)calloc(1, sizeof(arc_box)))) { sb_puts(err, "out of memory"); goto out; }
    if (!arc_load(dir, a)) { sb_printf(err, "%s is not a box", dir); WHY(CONV_NOT_A_BOX); }
    else if (a->state == ARC_OPEN) { sb_puts(err, "the box is still open: resume it, closing it converts it"); WHY(CONV_OPEN); }
    else if (a->state == ARC_BROKEN) { sb_printf(err, "the box cannot be read: %s", a->error); WHY(CONV_BROKEN); }
    else if (!a->old_format) { rc = 1; WHY(CONV_CURRENT); }
    else rc = 0;
    free(a);
    if (rc != 0) goto out;
    rc = -1;
    u8_join(data, sizeof(data), dir, "data");
    if (!(old = meta_new()) || !(nb = meta_new())) { sb_puts(err, "out of memory"); goto out; }
    if (meta_load(old, data, err)) { WHY(CONV_BROKEN); goto out; }
    /* already schema 3, an empty messi-da-parte/ left (a conversion finished
     * by a recovery): only the folder goes, no second entry in the history */
    if (!strcmp(old->schema, META_SCHEMA)) {
        conv_list left;
        memset(&left, 0, sizeof(left));
        rel_path(dir, SESSION_ASIDE_DIR_OLD, p, sizeof(p));
        u8_listdir(p, conv_name, &left);
        if (!left.n) {
            if (u8_isdir(p)) u8_rmtree(p);
            free(left.name);
            WHY(CONV_CURRENT);
            rc = 1;
            goto out;
        }
        free(left.name);
    }
    if (arc_verify_digests(dir, &ck, &ok, err)) { WHY(CONV_BROKEN); goto out; }
    if (ck.changed || ck.lost || ck.extra) {
        sb_printf(err, "%s does not match the manifest: the box is not touched", ck.first);
        WHY(CONV_DAMAGED);
        str_copy(why->file, sizeof(why->file), ck.first);
        goto out;
    }
    *nb = *old;
    entry_head(&w, "convert", NULL);
    jw_str(&w, "from", old->schema[0] ? old->schema : "unknown");
    jw_begin_arr(&w, "set_aside");
    aside_moves(dir, &m, &w);
    jw_end_arr(&w);
    jw_end_obj(&w);
    if (meta_history_add(nb, w.out.s)) { sb_puts(err, "the history of the box is full"); goto out; }
    /* set-aside/ before the transaction: the old tag files go there, not
     * into messi-da-parte/ */
    rel_path(dir, SESSION_ASIDE_DIR, p, sizeof(p));
    if (u8_mkdir(p)) { sb_printf(err, "cannot create %s", p); goto out; }
    keep_tech_open(&kt, dir, -1, 0, boxedit_tech, NULL);
    rc = boxtxn_commit(dir, &m, nb, kept_tech, &kt, &ok, NULL, err) ? -1 : 0;
    keep_tech_close(&kt);
    if (rc == 0) {
        conv_list left;
        WHY(CONV_DONE);
        memset(&left, 0, sizeof(left));
        rel_path(dir, SESSION_ASIDE_DIR_OLD, p, sizeof(p));
        u8_listdir(p, conv_name, &left);
        if (u8_isdir(p) && !left.n) u8_rmtree(p);  /* only an empty folder goes */
        free(left.name);
    }
out:
    jw_free(&w);
    txn_free(&m);
    bag_digests_free(&ok);
    meta_delete(old);
    meta_delete(nb);
    return rc;
}
