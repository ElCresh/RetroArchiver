#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boxclean.h"
#include "bagit.h"
#include "boxedit.h"
#include "fsu8.h"
#include "session.h"
#include "strbuf.h"

enum { K_IDENTICAL, K_DUPLICATE, K_FAILED, K_OTHER };

/* "CD01_b.failed2" -> K_FAILED and the stem "CD01_b"; K_OTHER for a name
 * that is not one of the three, which nothing here ever removes */
static int entry_kind(const char *name, char *stem, size_t n)
{
    static const char *const suffix[] = { EDIT_IDENTICAL, SESSION_DUPLICATE, SESSION_FAILED };
    size_t e = strlen(name), l; int k;
    while (e > 0 && isdigit((unsigned char)name[e - 1])) e--;      /* the number of a second one */
    for (k = 0; k < K_OTHER; k++) {
        l = strlen(suffix[k]);
        if (e <= l || strncmp(name + e - l, suffix[k], l)) continue;
        snprintf(stem, n, "%.*s", (int)(e - l), name);
        return k;
    }
    return K_OTHER;
}

static int ended_well(const meta_disk *d)
{
    return d && (!strcmp(d->status, ST_CLEAN) || !strcmp(d->status, ST_REDUMP));
}

static const meta_disk *disk_at(const meta_box *b, int n)
{
    int i;
    for (i = 0; i < b->ndisk; i++) if (b->disk[i].n == n) return &b->disk[i];
    return NULL;
}

/* 1 if the stem is a read of a medium of the box, and that medium ended
 * well. The stem starts with the medium's file name, then ends or goes on
 * with "_b" or an extension: "CD1" is not "CD10" */
static int stem_well(const meta_box *b, const char *stem)
{
    char base[96]; int n, kind, num; size_t l;
    for (n = 1; n <= b->media_expected && n <= META_MAXDISK; n++) {
        meta_slot(b, n, &kind, &num);
        session_disk_base(b, NULL, kind, num, base, sizeof(base));
        l = strlen(base);
        if (!l || strncmp(stem, base, l) || (stem[l] && stem[l] != '.' && stem[l] != '_')) continue;
        return ended_well(disk_at(b, n));
    }
    return 0;
}

static int is_log(const char *rel)
{
    size_t l = strlen(rel);
    return l >= 4 && rel[l - 4] == '.' && tolower((unsigned char)rel[l - 3]) == 't'
        && tolower((unsigned char)rel[l - 2]) == 'x' && tolower((unsigned char)rel[l - 1]) == 't';
}

/* the files of a failed read's folder: the logs stay, the rest goes */
typedef struct {
    const char *dir; long long bytes; int logs, others;
    boxclean_rm rm; void *ctx; int failed;
} logs_ctx;

static void log_or_not(void *c, const char *rel)
{
    logs_ctx *x = (logs_ctx *)c; char p[1300], r[512], *q;
    if (is_log(rel)) { x->logs++; return; }
    x->others++;
    str_copy(r, sizeof(r), rel);
    for (q = r; *q; q++) if (*q == '/') *q = U8_SEP[0];
    u8_join(p, sizeof(p), x->dir, r);
    if (x->rm) { if (x->rm(x->ctx, p, 0)) x->failed++; }
    else { long long sz = u8_filesize(p); if (sz > 0) x->bytes += sz; }
}

typedef struct { const char *aside; const meta_box *box; clean_plan *p; } plan_ctx;

static clean_item *add_item(clean_plan *p)
{
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        clean_item *ni = (clean_item *)realloc(p->item, (size_t)cap * sizeof(clean_item));
        if (!ni) return NULL;
        p->item = ni; p->cap = cap;
    }
    memset(&p->item[p->n], 0, sizeof(clean_item));
    return &p->item[p->n++];
}

static void plan_entry(void *c, const char *name, int isdir)
{
    plan_ctx *x = (plan_ctx *)c; char path[1024], stem[300]; clean_item *it;
    long long all, goes; int kind = entry_kind(name, stem, sizeof(stem)), partial = 0;
    u8_join(path, sizeof(path), x->aside, name);
    if ((all = u8_treesize(path)) < 0) all = 0;
    goes = all;
    if (kind != K_OTHER && kind != K_IDENTICAL && !stem_well(x->box, stem)) kind = K_OTHER;
    if (kind == K_FAILED && isdir) {
        logs_ctx l;
        memset(&l, 0, sizeof(l));
        l.dir = path;
        u8_walk(path, log_or_not, &l);
        /* only logs: there is nothing to take, the entry stays as it is */
        if (l.logs && !l.others) kind = K_OTHER;
        else if (l.logs) { partial = 1; goes = l.bytes; }
    }
    if (kind == K_OTHER || !(it = add_item(x->p))) {
        x->p->kept++;
        x->p->kept_bytes += all;
        return;
    }
    str_copy(it->name, sizeof(it->name), name);
    it->bytes = goes; it->isdir = isdir; it->partial = partial;
    x->p->bytes += goes;
}

void boxclean_plan_box(const char *bagdir, const meta_box *box, clean_plan *p)
{
    char aside[1024]; plan_ctx x;
    memset(p, 0, sizeof(*p));
    u8_join(aside, sizeof(aside), bagdir, SESSION_ASIDE_DIR);
    if (!u8_isdir(aside)) return;
    x.aside = aside; x.box = box; x.p = p;
    u8_listdir(aside, plan_entry, &x);
}

/* closed, of this version, nothing of an edit around */
static int cleanable(const char *bagdir)
{
    static const char *const no[] = { BAG_EDIT_DIR, SESSION_ASIDE_DIR_OLD, BAG_OPEN_MARK_OLD, NULL };
    char p[1024]; int i;
    u8_join(p, sizeof(p), bagdir, "bagit.txt");
    if (!u8_exists(p) || bag_is_open(bagdir)) return 0;
    for (i = 0; no[i]; i++) {
        u8_join(p, sizeof(p), bagdir, no[i]);
        if (u8_exists(p)) return 0;
    }
    return 1;
}

int boxclean_plan(const char *bagdir, clean_plan *p)
{
    char data[1024]; meta_box *m; strbuf err; int rc = -1;
    memset(p, 0, sizeof(*p));
    if (!cleanable(bagdir) || !(m = meta_new())) return -1;
    sb_init(&err);
    u8_join(data, sizeof(data), bagdir, "data");
    if (meta_load(m, data, &err) == 0 && !strcmp(m->schema, META_SCHEMA)) {
        boxclean_plan_box(bagdir, m, p);
        rc = 0;
    }
    sb_free(&err);
    meta_delete(m);
    return rc;
}

void boxclean_free(clean_plan *p)
{
    free(p->item);
    memset(p, 0, sizeof(*p));
}

int boxclean_rm_delete(void *ctx, const char *path, int isdir)
{
    (void)ctx; (void)isdir;
    return u8_rmtree(path) || u8_exists(path) ? -1 : 0;
}

int boxclean_run(const char *bagdir, const clean_plan *p, boxclean_rm rm, void *ctx)
{
    char aside[1024], path[1024]; int i, failed = 0;
    u8_join(aside, sizeof(aside), bagdir, SESSION_ASIDE_DIR);
    for (i = 0; i < p->n; i++) {
        const clean_item *it = &p->item[i];
        u8_join(path, sizeof(path), aside, it->name);
        if (it->partial) {
            logs_ctx l;
            memset(&l, 0, sizeof(l));
            l.dir = path; l.rm = rm; l.ctx = ctx;
            u8_walk(path, log_or_not, &l);
            failed += l.failed;
        } else if (rm(ctx, path, it->isdir)) failed++;
    }
    boxclean_tidy(bagdir);
    return failed;
}

static void count_entry(void *c, const char *name, int isdir)
{
    (void)name; (void)isdir;
    (*(int *)c)++;
}

void boxclean_tidy(const char *bagdir)
{
    char aside[1024]; int n = 0;
    u8_join(aside, sizeof(aside), bagdir, SESSION_ASIDE_DIR);
    if (!u8_isdir(aside) || u8_listdir(aside, count_entry, &n) || n) return;
    u8_rmtree(aside);
}

int boxclean_all_well(const meta_box *box)
{
    int n;
    if (box->media_expected < 1) return 0;
    for (n = 1; n <= box->media_expected; n++)
        if (!ended_well(disk_at(box, n))) return 0;
    return 1;
}
