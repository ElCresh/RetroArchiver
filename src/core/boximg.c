#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boximg.h"
#include "boxedit.h"
#include "boxtxn.h"
#include "fsu8.h"
#include "messages.h"
#include "session.h"
#include "sha256.h"

#ifdef _WIN32
#include <io.h>
#define SYNC_FILE(f) _commit(_fileno(f))
#else
#include <unistd.h>
#define SYNC_FILE(f) fsync(fileno(f))
#endif

#define CP(dst, src) str_copy(dst, sizeof(dst), src)
#define STAGE_IMG BOXTXN_STAGE "/" BOXIMG_DIR

/* ---- names ---- */

static const char *base_of(const char *p)
{
    const char *s = p + strlen(p);
    while (s > p && s[-1] != '/' && s[-1] != '\\') s--;
    return s;
}

/* the extension with its dot, "" if none */
static const char *ext_of(const char *name)
{
    const char *b = base_of(name), *dot = strrchr(b, '.');
    return dot && dot != b ? dot : "";
}

static void lower(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

int boximg_file_type(const char *name)
{
    static const char *const pic[] = { ".jpg", ".jpeg", ".png", ".tif", ".tiff", ".bmp", ".gif" };
    char e[16]; size_t i;
    CP(e, ext_of(name));
    lower(e);
    for (i = 0; i < sizeof(pic) / sizeof(pic[0]); i++) if (!strcmp(e, pic[i])) return 1;
    return !strcmp(e, ".pdf") ? 2 : 0;
}

void boximg_base(const meta_image *im, char *out, size_t n)
{
    if (im->kind == IMG_BOX) snprintf(out, n, "box-%s", meta_img_side_key(im->side));
    else if (im->kind == IMG_MEDIUM)
        snprintf(out, n, "%s%02d-%s", meta_kind_key(im->mkind), im->mnum, meta_img_side_key(im->side));
    else if (im->kind == IMG_CASE) snprintf(out, n, "case-%s", meta_img_side_key(im->side));
    else snprintf(out, n, "insert");
}

/* the name without its extension, in lowercase */
static void stem_of(const char *name, char *out, size_t n)
{
    str_copy(out, n, name);
    out[strlen(out) - strlen(ext_of(out))] = 0;
    lower(out);
}

static int stem_taken(const char *stem, const char (*taken)[64], int ntaken)
{
    char s[64]; int i;
    for (i = 0; i < ntaken; i++) {
        stem_of(taken[i], s, sizeof(s));
        if (!strcmp(s, stem)) return 1;
    }
    return 0;
}

void boximg_name(const meta_image *im, const char *like, const char (*taken)[64], int ntaken,
                 char *out, size_t n)
{
    char base[40], stem[56], e[16]; int k;
    boximg_base(im, base, sizeof(base));
    CP(e, ext_of(like));
    lower(e);
    /* the inserts are all numbered; of the others only the ones after the first */
    for (k = 1; k < 10000; k++) {
        if (im->kind == IMG_INSERT) snprintf(stem, sizeof(stem), "%s-%02d", base, k);
        else if (k == 1) snprintf(stem, sizeof(stem), "%s", base);
        else snprintf(stem, sizeof(stem), "%s-%d", base, k);
        if (!stem_taken(stem, taken, ntaken)) break;
    }
    snprintf(out, n, "%s%s", stem, e);
}

int boximg_same_base(const meta_image *im)
{
    char base[40], stem[64]; size_t l; const char *p;
    boximg_base(im, base, sizeof(base));
    stem_of(im->file, stem, sizeof(stem));
    l = strlen(base);
    if (strncmp(stem, base, l)) return 0;
    p = stem + l;
    if (!*p) return im->kind != IMG_INSERT;
    if (*p++ != '-' || !*p) return 0;
    for (; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

int boximg_parse_what(const char *what, meta_image *im)
{
    int k, side; size_t l; const char *p; char *end; long num;
    memset(im, 0, sizeof(*im));
    if (!strcmp(what, "insert")) { im->kind = IMG_INSERT; im->side = IMG_OTHER; return 0; }
    if (!strncmp(what, "box-", 4) || !strncmp(what, "case-", 5)) {
        int box = what[0] == 'b';
        if ((side = meta_img_side_parse(what + (box ? 4 : 5))) < 0) return -1;
        im->kind = box ? IMG_BOX : IMG_CASE; im->side = side;
        return 0;
    }
    for (k = 0; k < MEDIA_KINDS; k++) {
        l = strlen(meta_kind_key(k));
        if (strncmp(what, meta_kind_key(k), l)) continue;
        p = what + l;
        if (!isdigit((unsigned char)*p)) return -1;
        num = strtol(p, &end, 10);
        if (num < 1 || num > META_MAXDISK || *end != '-') return -1;
        side = meta_img_side_parse(end + 1);
        if (side != IMG_FRONT && side != IMG_BACK) return -1;
        im->kind = IMG_MEDIUM; im->mkind = k; im->mnum = (int)num; im->side = side;
        return 0;
    }
    return -1;
}

/* word in s, not inside a longer run of letters on its left ("cd" in
 * "abcd" does not count); the position after it, NULL if absent */
static const char *word_in(const char *s, const char *word)
{
    const char *p = s; size_t l = strlen(word);
    while ((p = strstr(p, word)) != NULL) {
        if (p == s || !isalpha((unsigned char)p[-1])) return p + l;
        p++;
    }
    return NULL;
}

static int has_any(const char *s, const char *const *words)
{
    for (; *words; words++) if (strstr(s, *words)) return 1;
    return 0;
}

int boximg_guess(const char *filename, const meta_box *b, meta_image *im)
{
    static const struct { const char *word; int kind; } media[] = {
        { "floppy", MEDIA_FLOPPY }, { "dischetto", MEDIA_FLOPPY }, { "disk", MEDIA_FLOPPY },
        { "disco", MEDIA_FLOPPY }, { "dvd", MEDIA_DVD }, { "cd", MEDIA_CD }
    };
    static const char *const back[] = { "back", "retro", "rear", NULL };
    static const char *const front[] = { "front", "cover", "copertina", NULL };
    static const char *const spine[] = { "spine", "dorso", "costa", NULL };
    static const char *const top[] = { "top", "sopra", NULL };
    static const char *const bottom[] = { "bottom", "sotto", NULL };
    /* the jewel case of a CD, not the big box: "custodia cd1 retro" is the
     * case, so these words win over the name of a medium */
    static const char *const cases[] = { "jewel", "custodia", "case", "inlay", "booklet", NULL };
    char s[200]; size_t i; const char *p; int in_case;

    memset(im, 0, sizeof(*im));
    im->kind = IMG_INSERT; im->side = IMG_OTHER;
    stem_of(base_of(filename), s, sizeof(s));
    if (boximg_file_type(filename) == 2) return 1;          /* a PDF is an insert */
    in_case = has_any(s, cases);
    for (i = 0; !in_case && i < sizeof(media) / sizeof(media[0]); i++) {
        int num = 0, have = meta_count(b, media[i].kind);
        if (!(p = word_in(s, media[i].word))) continue;
        while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') p++;
        if (isdigit((unsigned char)*p)) num = atoi(p);
        else if (have == 1) num = 1;                        /* "cd.jpg" in a box with one CD */
        if (num < 1 || num > have) continue;
        im->kind = IMG_MEDIUM; im->mkind = media[i].kind; im->mnum = num;
        im->side = has_any(s, back) ? IMG_BACK : IMG_FRONT;
        return 1;
    }
    im->kind = in_case ? IMG_CASE : IMG_BOX;
    if (has_any(s, back)) im->side = IMG_BACK;              /* before "front": "retro" has no "front" in it */
    else if (has_any(s, front)) im->side = IMG_FRONT;
    else if (has_any(s, spine)) im->side = IMG_SPINE;
    else if (has_any(s, top)) im->side = IMG_TOP;
    else if (has_any(s, bottom)) im->side = IMG_BOTTOM;
    else if (in_case) im->side = strstr(s, "inlay") ? IMG_BACK : IMG_FRONT;
    else { im->kind = IMG_INSERT; im->side = IMG_OTHER; return 0; }
    return 1;
}

int boximg_valid(const meta_box *b, const meta_image *im, const char *filename, strbuf *err)
{
    char who[32]; int type = boximg_file_type(filename);
    if (!type) { sb_printf(err, M_IMG_TYPE, base_of(filename)); return -1; }
    if (type == 2 && im->kind != IMG_INSERT) { sb_printf(err, M_IMG_PDF, base_of(filename)); return -1; }
    if (im->kind == IMG_MEDIUM) {
        if (im->mkind < 0 || im->mkind >= MEDIA_KINDS || im->mnum < 1 || im->mnum > meta_count(b, im->mkind)) {
            meta_disk_name(im->mkind >= 0 && im->mkind < MEDIA_KINDS ? im->mkind : MEDIA_FLOPPY, im->mnum,
                           who, sizeof(who));
            sb_printf(err, M_IMG_NO_MEDIUM, who);
            return -1;
        }
        if (im->side != IMG_FRONT && im->side != IMG_BACK) { sb_puts(err, M_IMG_SIDE); return -1; }
    } else if (im->kind == IMG_BOX || im->kind == IMG_CASE) {
        if (im->side < 0 || im->side >= IMG_SIDES) { sb_puts(err, M_IMG_SIDE); return -1; }
    } else if (im->kind != IMG_INSERT) {
        sb_puts(err, M_IMG_SIDE);
        return -1;
    }
    return 0;
}

static int rank_of(const meta_box *b, const meta_image *im)
{
    int k, pos;
    if (im->kind == IMG_BOX) return im->side == IMG_FRONT ? 0 : 1;
    if (im->kind == IMG_CASE) return im->side == IMG_FRONT ? 2 : 3;     /* after the box, before the media */
    if (im->kind == IMG_INSERT) return 100000;
    for (pos = im->mnum, k = 0; k < im->mkind && k < MEDIA_KINDS; k++) pos += meta_count(b, k);
    return 10 + pos;
}

void boximg_order(const meta_box *b, int idx[META_MAXIMG])
{
    int i, j, t;
    for (i = 0; i < b->nimage; i++) idx[i] = i;
    /* insertion sort: stable, the list's order stays within a group */
    for (i = 1; i < b->nimage; i++) {
        t = idx[i];
        for (j = i; j > 0 && rank_of(b, &b->image[idx[j - 1]]) > rank_of(b, &b->image[t]); j--) idx[j] = idx[j - 1];
        idx[j] = t;
    }
}

void boximg_what_text(const meta_image *im, char *out, size_t n)
{
    char who[32];
    if (im->kind == IMG_BOX) snprintf(out, n, "box %s", meta_img_side_key(im->side));
    else if (im->kind == IMG_CASE) snprintf(out, n, "case %s", meta_img_side_key(im->side));
    else if (im->kind == IMG_MEDIUM) {
        meta_disk_name(im->mkind, im->mnum, who, sizeof(who));
        snprintf(out, n, "%s %s", who, meta_img_side_key(im->side));
    } else snprintf(out, n, "insert");
}

/* ---- the list ---- */

static img_item *list_slot(img_list *l)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        img_item *p = (img_item *)realloc(l->item, (size_t)cap * sizeof(img_item));
        if (!p) return NULL;
        l->item = p; l->cap = cap;
    }
    memset(&l->item[l->n], 0, sizeof(img_item));
    return &l->item[l->n++];
}

void boximg_list_load(const meta_box *b, img_list *l)
{
    int i; img_item *it;
    memset(l, 0, sizeof(*l));
    for (i = 0; i < b->nimage; i++) {
        if (!(it = list_slot(l))) return;
        it->im = b->image[i];
        CP(it->was, b->image[i].file);
    }
}

int boximg_list_add(img_list *l, const meta_box *b, const char *source, const meta_image *what, strbuf *err)
{
    img_item *it;
    if (boximg_valid(b, what, source, err)) return -1;
    if (l->n >= META_MAXIMG || !(it = list_slot(l))) { sb_printf(err, M_IMG_TOO_MANY, META_MAXIMG); return -1; }
    it->im = *what;
    it->im.file[0] = 0;
    CP(it->im.original, base_of(source));
    CP(it->source, source);
    return 0;
}

void boximg_list_remove(img_list *l, int i)
{
    if (i < 0 || i >= l->n) return;
    memmove(&l->item[i], &l->item[i + 1], (size_t)(l->n - i - 1) * sizeof(img_item));
    l->n--;
}

int boximg_list_move(img_list *l, int i, int dir)
{
    img_item t; int j = i + dir;
    if (i < 0 || i >= l->n || j < 0 || j >= l->n) return i;
    t = l->item[i]; l->item[i] = l->item[j]; l->item[j] = t;
    return j;
}

void boximg_list_free(img_list *l)
{
    free(l->item);
    memset(l, 0, sizeof(*l));
}

/* ---- what changes ---- */

static const meta_image *image_named(const meta_box *b, const char *file)
{
    int i;
    for (i = 0; i < b->nimage; i++) if (!strcmp(b->image[i].file, file)) return &b->image[i];
    return NULL;
}

static const img_item *item_was(const img_list *l, const char *file)
{
    int i;
    for (i = 0; i < l->n; i++) if (!strcmp(l->item[i].was, file)) return &l->item[i];
    return NULL;
}

static int same_what(const meta_image *a, const meta_image *b)
{
    if (a->kind != b->kind || strcmp(a->note, b->note)) return 0;
    if (a->kind != IMG_INSERT && a->side != b->side) return 0;
    return a->kind != IMG_MEDIUM || (a->mkind == b->mkind && a->mnum == b->mnum);
}

static img_change *change_at(img_change *out, int max, int n, int what)
{
    if (n >= max) return NULL;
    memset(&out[n], 0, sizeof(out[n]));
    out[n].what = what;
    return &out[n];
}

/* the images that stay come in another order than they had */
static int reordered(const meta_box *old, const img_list *want)
{
    int i, k = 0;
    for (i = 0; i < old->nimage; i++) {
        if (!item_was(want, old->image[i].file)) continue;
        while (k < want->n && !want->item[k].was[0]) k++;
        if (k >= want->n || strcmp(want->item[k].was, old->image[i].file)) return 1;
        k++;
    }
    return 0;
}

int boximg_diff(const meta_box *old, const img_list *want, img_change *out, int max)
{
    int i, n = 0; img_change *c; const meta_image *o;
    for (i = 0; i < want->n; i++) {
        const img_item *it = &want->item[i];
        if (!it->was[0]) {
            if ((c = change_at(out, max, n, IMGCH_ADDED)) != NULL) { c->to = it->im; CP(c->source, it->source); }
            n++;
        } else if ((o = image_named(old, it->was)) != NULL && !same_what(o, &it->im)) {
            if ((c = change_at(out, max, n, IMGCH_RELABELED)) != NULL) { c->from = *o; c->to = it->im; }
            n++;
        }
    }
    for (i = 0; i < old->nimage; i++)
        if (!item_was(want, old->image[i].file)) {
            if ((c = change_at(out, max, n, IMGCH_REMOVED)) != NULL) c->from = old->image[i];
            n++;
        }
    if (reordered(old, want)) { change_at(out, max, n, IMGCH_REORDERED); n++; }
    return n;
}

/* ---- saving ---- */

static void rel_path(const char *bagdir, const char *rel, char *out, size_t n)
{
    char r[400], *q;
    CP(r, rel);
    for (q = r; *q; q++) if (*q == '/') *q = U8_SEP[0];
    u8_join(out, n, bagdir, r);
}

typedef struct {
    const img_host *host;
    int k, n;
    long long done, total;
} copy_ctx;

/* source into dest, taking its SHA-256 on the way: 0 ok, 1 stopped by the
 * host, -1 the source does not read, -2 the copy cannot be written */
static int copy_hash(const char *source, const char *dest, const char *name, copy_ctx *cc,
                     char hex[65], long long *bytes)
{
    static char buf[65536];
    FILE *in = u8_fopen(source, "rb"), *out; sha256_ctx sc; unsigned char dg[32];
    size_t k; int rc = 0;
    if (!in) return -1;
    if (!(out = u8_fopen(dest, "wb"))) { fclose(in); return -2; }
    sha256_init(&sc);
    *bytes = 0;
    for (;;) {
        if (cc->host && cc->host->progress &&
            cc->host->progress(cc->host->ctx, cc->k, cc->n, name, cc->done, cc->total)) { rc = 1; break; }
        k = fread(buf, 1, sizeof(buf), in);
        if (!k) { if (ferror(in)) rc = -1; break; }
        if (fwrite(buf, 1, k, out) != k) { rc = -2; break; }
        sha256_update(&sc, buf, k);
        *bytes += (long long)k; cc->done += (long long)k;
    }
    fclose(in);
    if (rc == 0 && (fflush(out) != 0 || SYNC_FILE(out) != 0)) rc = -2;
    if (fclose(out) != 0 && rc == 0) rc = -2;
    sha256_final(&sc, dg);
    sha256_hex(dg, hex);
    return rc;
}

/* editing/ goes if no transaction is left in it: the copies made there are
 * copies, the files they came from are where they were */
static void drop_stage(const char *bagdir)
{
    char p[1100];
    rel_path(bagdir, BOXTXN_DIR "/journal.txt", p, sizeof(p));
    if (u8_exists(p)) return;
    rel_path(bagdir, BOXTXN_DIR, p, sizeof(p));
    u8_rmtree(p);
}

static void entry_image(json_writer *w, const char *change, const char *file)
{
    char rel[100];
    snprintf(rel, sizeof(rel), BOXIMG_DIR "/%s", file);
    jw_begin_obj(w, NULL);
    jw_str(w, "change", change);
    jw_str(w, "file", rel);
}

static void skipped_add(img_result *res, const char *source, const char *same)
{
    size_t l;
    res->skipped++;
    l = strlen(res->skipped_names);
    snprintf(res->skipped_names + l, sizeof(res->skipped_names) - l, "%s%s = %s", l ? "; " : "",
             base_of(source), same);
}

/* the image of nb with that SHA-256, NULL if none */
static const meta_image *same_bytes(const meta_box *nb, const char *hex)
{
    int i;
    for (i = 0; i < nb->nimage; i++) if (!strcmp(nb->image[i].sha256, hex)) return &nb->image[i];
    return NULL;
}

typedef struct {
    const char *bagdir;
    const meta_box *old;
    const img_list *want;
    meta_box *nb;
    txn_moves first, last;      /* out of data/images (removed, renamed), then into it */
    json_writer w;
    char (*taken)[64]; int ntaken;
    img_result *res;
} save_ctx;

static void take(save_ctx *s, const char *name)
{
    if (s->ntaken < 2 * META_MAXIMG) CP(s->taken[s->ntaken++], name);
}

/* the images of the box that go: to set-aside as NAME.removed */
static void plan_removed(save_ctx *s, const img_list *want)
{
    char from[120], to[400]; int i;
    for (i = 0; i < s->old->nimage; i++) {
        const meta_image *o = &s->old->image[i];
        if (item_was(want, o->file)) continue;
        snprintf(from, sizeof(from), "data/" BOXIMG_DIR "/%s", o->file);
        txn_aside_name(s->bagdir, &s->first, o->file, EDIT_REMOVED, to, sizeof(to));
        txn_add(&s->first, from, to);
        entry_image(&s->w, "removed", o->file);
        jw_str(&s->w, "set_aside", to);
        jw_end_obj(&s->w);
        s->res->removed++;
    }
}

/* an image of the box that stays: as it is, or renamed through editing/ when
 * what it shows changed (two can then swap their names) */
static void plan_kept(save_ctx *s, const img_item *it)
{
    const meta_image *o = image_named(s->old, it->was);
    meta_image *im = &s->nb->image[s->nb->nimage++];
    char from[120], mid[160], to[120];
    *im = *o;
    im->kind = it->im.kind; im->side = it->im.side; im->mkind = it->im.mkind; im->mnum = it->im.mnum;
    CP(im->note, it->im.note);
    if (same_what(o, im)) return;
    s->res->relabeled++;
    if (boximg_same_base(im)) {                 /* only the description changed */
        entry_image(&s->w, "relabeled", im->file);
        jw_end_obj(&s->w);
        return;
    }
    boximg_name(im, o->file, (const char (*)[64])s->taken, s->ntaken, im->file, sizeof(im->file));
    take(s, im->file);
    snprintf(from, sizeof(from), "data/" BOXIMG_DIR "/%s", o->file);
    snprintf(mid, sizeof(mid), STAGE_IMG "/%s", im->file);
    snprintf(to, sizeof(to), "data/" BOXIMG_DIR "/%s", im->file);
    txn_add(&s->first, from, mid);
    txn_add(&s->last, mid, to);
    entry_image(&s->w, "relabeled", im->file);
    snprintf(from, sizeof(from), BOXIMG_DIR "/%s", o->file);
    jw_str(&s->w, "from", from);
    jw_end_obj(&s->w);
}

/* a new image: copied into editing/ under its name. 0 ok or skipped, 1
 * stopped, -1 with the reason */
static int plan_new(save_ctx *s, const img_item *it, copy_ctx *cc, strbuf *err)
{
    meta_image im = it->im; const meta_image *same;
    char dir[1100], dest[1200], mid[160], to[120]; int rc;
    if (s->nb->nimage >= META_MAXIMG) { sb_printf(err, M_IMG_TOO_MANY, META_MAXIMG); return -1; }
    if (boximg_valid(s->old, &im, it->source, err)) return -1;
    boximg_name(&im, it->source, (const char (*)[64])s->taken, s->ntaken, im.file, sizeof(im.file));
    rel_path(s->bagdir, STAGE_IMG, dir, sizeof(dir));
    if (u8_mkdirs(dir)) { sb_printf(err, M_TXN_MKDIR, dir); return -1; }
    u8_join(dest, sizeof(dest), dir, im.file);
    cc->k++;
    rc = copy_hash(it->source, dest, base_of(it->source), cc, im.sha256, &im.bytes);
    if (rc == 1) return 1;
    if (rc) { sb_printf(err, rc == -1 ? M_IMG_SOURCE : M_IMG_COPY, it->source); return -1; }
    /* twice in this save, or already attached and not being removed now */
    same = same_bytes(s->nb, im.sha256);
    if (!same && (same = same_bytes(s->old, im.sha256)) != NULL && !item_was(s->want, same->file)) same = NULL;
    if (same) {
        u8_remove(dest);
        skipped_add(s->res, it->source, same->file);
        return 0;
    }
    if (!im.original[0]) CP(im.original, base_of(it->source));
    meta_now(im.at);
    take(s, im.file);
    s->nb->image[s->nb->nimage++] = im;
    snprintf(mid, sizeof(mid), STAGE_IMG "/%s", im.file);
    snprintf(to, sizeof(to), "data/" BOXIMG_DIR "/%s", im.file);
    txn_add(&s->last, mid, to);
    entry_image(&s->w, "added", im.file);
    if (im.original[0]) jw_str(&s->w, "original_name", im.original);
    jw_str(&s->w, "sha256", im.sha256);
    jw_end_obj(&s->w);
    s->res->added++;
    return 0;
}

/* data/images/ left empty by the last image removed goes too */
static void tidy(const char *bagdir, const meta_box *nb)
{
    char p[1100];
    if (nb->nimage) return;
    rel_path(bagdir, "data/" BOXIMG_DIR, p, sizeof(p));
    if (u8_isdir(p) && u8_treesize(p) == 0) u8_rmtree(p);
}

int boximg_save(const char *bagdir, const img_list *want, const char *reason, const img_host *host,
                img_result *res, strbuf *err)
{
    save_ctx s; meta_box *old = meta_new(), *nb = meta_new(); img_change one; img_result spare;
    bag_digests ok; boxedit_keep kt; copy_ctx cc; txn_moves all; char data[1040];
    int i, rc = -1, staged = 0;

    memset(&s, 0, sizeof(s)); memset(&ok, 0, sizeof(ok)); memset(&all, 0, sizeof(all));
    memset(&cc, 0, sizeof(cc));
    s.res = res ? res : &spare;
    memset(s.res, 0, sizeof(*s.res));
    jw_init(&s.w);
    s.taken = (char (*)[64])calloc(2 * META_MAXIMG, 64);
    if (!old || !nb || !s.taken) { sb_puts(err, M_EDIT_NOMEM); goto out; }
    if (!boxedit_editable(bagdir, err)) goto out;
    u8_join(data, sizeof(data), bagdir, "data");
    if (meta_load(old, data, err)) goto out;
    for (i = 0; i < want->n; i++)
        if (want->item[i].was[0] && !image_named(old, want->item[i].was)) {
            sb_printf(err, M_IMG_NOT_THERE, want->item[i].was);
            goto out;
        }
    if (!boximg_diff(old, want, &one, 1)) { rc = 0; goto out; }
    if (boxedit_whole(bagdir, &ok, err)) goto out;

    *nb = *old;
    nb->nimage = 0;
    s.bagdir = bagdir; s.old = old; s.nb = nb; s.want = want;
    cc.host = host;
    for (i = 0; i < want->n; i++) {
        const img_item *it = &want->item[i];
        if (!it->was[0]) {
            long long sz = u8_filesize(it->source);
            if (sz < 0) { sb_printf(err, M_IMG_SOURCE, it->source); goto out; }
            cc.total += sz; cc.n++;
        } else {
            /* a name that stays is never given to another image */
            meta_image t = *image_named(old, it->was);
            t.kind = it->im.kind; t.side = it->im.side; t.mkind = it->im.mkind; t.mnum = it->im.mnum;
            if (boximg_valid(old, &t, t.file, err)) goto out;
            if (boximg_same_base(&t)) take(&s, t.file);
        }
    }
    boxedit_entry_head(&s.w, "images", reason);
    jw_begin_arr(&s.w, "images");
    plan_removed(&s, want);
    staged = 1;
    for (i = 0; i < want->n; i++) {
        const img_item *it = &want->item[i];
        int r = 0;
        if (it->was[0]) plan_kept(&s, it);
        else r = plan_new(&s, it, &cc, err);
        if (r) { rc = r; goto out; }
    }
    jw_end_arr(&s.w);
    if (reordered(old, want)) jw_bool(&s.w, "reordered", 1);
    jw_end_obj(&s.w);
    /* every new file was already attached: nothing is left to change */
    if (!s.first.n && !s.last.n && !s.res->relabeled && !reordered(old, want)) { rc = 0; goto out; }
    if (meta_history_add(nb, s.w.out.s)) { sb_puts(err, M_EDIT_HISTORY_FULL); goto out; }
    for (i = 0; i < s.first.n; i++) txn_add(&all, s.first.from[i], s.first.to[i]);
    for (i = 0; i < s.last.n; i++) txn_add(&all, s.last.from[i], s.last.to[i]);
    meta_now(nb->closed);
    boxedit_keep_open(&kt, bagdir, -1, 0, boxedit_tech, NULL);
    i = boxtxn_commit(bagdir, &all, nb, boxedit_kept_tech, &kt, &ok, NULL, err);
    boxedit_keep_close(&kt);
    if (i == -2) { staged = 0; goto out; }      /* the tests' power cut: left for the recovery */
    if (i) goto out;
    staged = 0;
    tidy(bagdir, nb);
    rc = 0;
out:
    if (staged) drop_stage(bagdir);
    if (rc) { s.res->added = s.res->removed = s.res->relabeled = 0; }
    jw_free(&s.w);
    txn_free(&s.first); txn_free(&s.last); txn_free(&all);
    bag_digests_free(&ok);
    free(s.taken);
    meta_delete(old);
    meta_delete(nb);
    return rc;
}
