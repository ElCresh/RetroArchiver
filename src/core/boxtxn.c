#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "boxtxn.h"
#include "fsu8.h"
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
#define JOURNAL    BOXTXN_DIR "/journal.txt"
#define PROGRESS   BOXTXN_DIR "/moves-done.txt"
#define OLD_DIR    BOXTXN_DIR "/old"
#define NEW_DIR    BOXTXN_DIR "/new"
#define JOURNAL_ID "retroarchiver-txn 1"

/* the tag files a commit replaces: kept in editing/old, then in set-aside */
static const char *const tag_files[] = {
    "bag-info.txt", "manifest-sha256.txt", "tagmanifest-sha256.txt", "data/metadata.json"
};
#define NTAG ((int)(sizeof(tag_files) / sizeof(tag_files[0])))

static int g_crash, g_fail;
void boxtxn_test_crash(int step) { g_crash = step; }
void boxtxn_test_fail(int step)  { g_fail = step; }
static int g_fail_undo;
void boxtxn_test_fail_undo(int on) { g_fail_undo = on; }

void txn_add(txn_moves *m, const char *from, const char *to)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->from = (char (*)[320])realloc(m->from, (size_t)m->cap * sizeof(*m->from));
        m->to = (char (*)[320])realloc(m->to, (size_t)m->cap * sizeof(*m->to));
    }
    CP(m->from[m->n], from);
    CP(m->to[m->n], to);
    m->n++;
}

void txn_free(txn_moves *m)
{
    free(m->from); free(m->to);
    memset(m, 0, sizeof(*m));
}

/* bag + "a/b" with the system's separator */
static void path_of(const char *bagdir, const char *rel, char *out, size_t n)
{
    char r[400], *q;
    CP(r, rel);
    for (q = r; *q; q++) if (*q == '/') *q = U8_SEP[0];
    u8_join(out, n, bagdir, r);
}

static int rel_exists(const char *bagdir, const char *rel)
{
    char p[1024];
    path_of(bagdir, rel, p, sizeof(p));
    return u8_exists(p);
}

static int is_dest(const txn_moves *m, const char *rel)
{
    int i;
    for (i = 0; m && i < m->n; i++) if (!strcmp(m->to[i], rel)) return 1;
    return 0;
}

void txn_aside_name(const char *bagdir, const txn_moves *m, const char *name, const char *suffix,
                    char *out, size_t n)
{
    const char *aside = session_aside_dir(bagdir);
    int j;
    snprintf(out, n, "%s/%s%s", aside, name, suffix);
    for (j = 2; (rel_exists(bagdir, out) || is_dest(m, out)) && j < 1000; j++)
        snprintf(out, n, "%s/%s%s%d", aside, name, suffix, j);
}

/* the folder that holds rel, created if missing */
static int parent_of(const char *bagdir, const char *rel)
{
    char r[400], p[1024], *s;
    CP(r, rel);
    if (!(s = strrchr(r, '/'))) return 0;
    *s = 0;
    path_of(bagdir, r, p, sizeof(p));
    return u8_mkdirs(p);
}

/* the journal, the counter and the copies must be on the disk before
 * what they protect changes: a power cut keeps a rename and can lose the
 * data written just before it */
static int sync_close(FILE *f, int ok)
{
    if (ok && (fflush(f) != 0 || SYNC_FILE(f) != 0)) ok = 0;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static int copy_file(const char *from, const char *to)
{
    FILE *a = u8_fopen(from, "rb"), *b; char buf[8192]; size_t k; int ok = 1;
    if (!a) return -1;
    if (!(b = u8_fopen(to, "wb"))) { fclose(a); return -1; }
    while ((k = fread(buf, 1, sizeof(buf), a)) > 0)
        if (fwrite(buf, 1, k, b) != k) { ok = 0; break; }
    fclose(a);
    return sync_close(b, ok) ? 0 : -1;
}

static int write_atomic(const char *path, const char *text)
{
    char tmp[1100]; FILE *f; int ok;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!(f = u8_fopen(tmp, "wb"))) return -1;
    ok = sync_close(f, fputs(text, f) >= 0);
    if (ok && u8_rename_replace(tmp, path)) ok = 0;
    if (!ok) u8_remove(tmp);
    return ok ? 0 : -1;
}

/* the last part of a path */
static const char *base_of(const char *p)
{
    const char *s = p + strlen(p);
    while (s > p && s[-1] != '/' && s[-1] != '\\') s--;
    return s;
}

/* the same folder but for the case of its letters: Windows sees it as
 * already there. '/' and '\' are the same separator ("a/b" from the
 * caller, "a\B" from u8_join) */
static int same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a == '\\' ? '/' : tolower((unsigned char)*a), y = *b == '\\' ? '/' : tolower((unsigned char)*b);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ---- the steps ---- */

/* the destination of move i is the source of an earlier one: free by then */
static int freed_before(const txn_moves *m, int i)
{
    int k;
    for (k = 0; k < i; k++) if (!strcmp(m->from[k], m->to[i])) return 1;
    return 0;
}

/* 1: nothing is touched if a move cannot be made */
static int precheck(const char *bagdir, const txn_moves *m, const char *newdir, strbuf *err)
{
    int i;
    for (i = 0; i < m->n; i++) {
        if (!rel_exists(bagdir, m->from[i])) {
            sb_printf(err, "%s is missing in %s: nothing was changed", m->from[i], bagdir);
            return -1;
        }
        if (rel_exists(bagdir, m->to[i]) && !freed_before(m, i)) {
            sb_printf(err, "%s already exists in %s: nothing was changed", m->to[i], bagdir);
            return -1;
        }
    }
    if (newdir && !same_ci(newdir, bagdir) && u8_exists(newdir)) {
        sb_printf(err, "the folder %s already exists: choose another title or edition", newdir);
        return -1;
    }
    return 0;
}

/* 2: the old tag files in editing/old, the new metadata in editing/new */
static int keep_old(const char *bagdir, const meta_box *box, strbuf *err)
{
    char from[1024], to[1024], rel[200]; int i;
    path_of(bagdir, OLD_DIR, to, sizeof(to));
    if (u8_mkdirs(to)) { sb_printf(err, "cannot create %s", to); return -1; }
    for (i = 0; i < NTAG; i++) {
        path_of(bagdir, tag_files[i], from, sizeof(from));
        snprintf(rel, sizeof(rel), OLD_DIR "/%s", base_of(tag_files[i]));
        path_of(bagdir, rel, to, sizeof(to));
        if (copy_file(from, to)) { sb_printf(err, "cannot copy %s", from); return -1; }
    }
    path_of(bagdir, NEW_DIR, to, sizeof(to));
    if (u8_mkdirs(to) || meta_save(box, to, NULL, NULL, err)) return -1;
    return 0;
}

/* 3 */
static int write_journal(const char *bagdir, const txn_moves *m, const char *newdir, strbuf *err)
{
    strbuf j; char p[1024]; int i, rc;
    sb_init(&j);
    sb_puts(&j, JOURNAL_ID "\n");
    if (newdir) sb_printf(&j, "folder\t%s\n", base_of(newdir));
    for (i = 0; i < m->n; i++) sb_printf(&j, "move\t%s\t%s\n", m->from[i], m->to[i]);
    path_of(bagdir, JOURNAL, p, sizeof(p));
    rc = write_atomic(p, j.s);
    if (rc) sb_printf(err, "cannot write %s", p);
    sb_free(&j);
    return rc;
}

/* 4: for these few seconds the bag says it is open */
static int mark_open(const char *bagdir, strbuf *err)
{
    char p[1024];
    u8_join(p, sizeof(p), bagdir, BAG_OPEN_MARK);
    if (write_atomic(p, "Box being edited: the manifests are being written again.\n")) {
        sb_printf(err, "cannot write %s", p);
        return -1;
    }
    return 0;
}

static void set_progress(const char *bagdir, int k)
{
    char p[1024], t[16];
    path_of(bagdir, PROGRESS, p, sizeof(p));
    snprintf(t, sizeof(t), "%d\n", k);
    write_atomic(p, t);
}

/* how many moves are done: the count written after each one, plus the one
 * that may have been made just before the power went. Counting matters: a
 * later move may refill the source of an earlier one (data/A.dsk) */
static int moves_done(const char *bagdir, const txn_moves *m)
{
    char p[1024], t[16] = ""; FILE *f; int k = 0;
    path_of(bagdir, PROGRESS, p, sizeof(p));
    if ((f = u8_fopen(p, "rb")) != NULL) {
        if (fgets(t, sizeof(t), f)) k = atoi(t);
        fclose(f);
    }
    if (k < 0) k = 0;
    if (k > m->n) k = m->n;
    while (k < m->n && !rel_exists(bagdir, m->from[k]) && rel_exists(bagdir, m->to[k])) k++;
    return k;
}

static int do_move(const char *bagdir, const char *from, const char *to)
{
    char a[1024], b[1024];
    path_of(bagdir, from, a, sizeof(a));
    path_of(bagdir, to, b, sizeof(b));
    if (parent_of(bagdir, to)) return -1;
    return u8_rename_replace(a, b);
}

/* 5, from move *done on; -2 for the crash of the tests, after the first move */
static int do_moves(const char *bagdir, const txn_moves *m, int *done, strbuf *err)
{
    int first = *done, i;
    for (; *done < m->n; ++*done) {
        i = *done;
        if ((g_fail == 5 && i > first) || do_move(bagdir, m->from[i], m->to[i])) {
            sb_printf(err, "cannot move %s to %s: is it open in another program?", m->from[i], m->to[i]);
            return -1;
        }
        set_progress(bagdir, i + 1);
        if (g_crash == 5 && i == first) { ++*done; return -2; }
    }
    return 0;
}

/* under rel, or rel itself: "data/CD01/x.bin" is under "data/CD01" */
static int under(const char *path, const char *rel)
{
    size_t l = strlen(rel);
    return !strncmp(path, rel, l) && (!path[l] || path[l] == '/');
}

/* the known digests that no move touches */
static void untouched(const bag_digests *known, const txn_moves *m, bag_digests *out)
{
    int i, k, keep;
    for (i = 0; known && i < known->n; i++) {
        for (k = 0, keep = 1; k < m->n && keep; k++)
            if (under(known->rel[i], m->from[k]) || under(known->rel[i], m->to[k])) keep = 0;
        if (keep && strcmp(known->rel[i], "data/metadata.json")) bag_digests_add(out, known->rel[i], known->hex[i]);
    }
}

/* 6 */
static int close_new(const char *bagdir, const meta_box *box, meta_tech_fn tech, void *ctx,
                     const bag_digests *trusted, strbuf *err)
{
    char data[1024]; int bad;
    u8_join(data, sizeof(data), bagdir, "data");
    if (meta_save(box, data, tech, ctx, err)) return -1;
    if (g_fail == 6) { sb_puts(err, "cannot write the manifests: is the disk full?"); return -1; }
    return bag_close(bagdir, box, trusted, err, &bad);
}

typedef struct { const bag_digests *man; int extra; } extra_ctx;
static void count_extra(void *c, const char *name)
{
    extra_ctx *x = (extra_ctx *)c; char rel[1040];
    snprintf(rel, sizeof(rel), "data/%s", name);
    if (bag_digests_find(x->man, rel) < 0) x->extra++;
}

/* one manifest line: a trusted file only needs to be there */
static int line_ok(const char *bagdir, const char *rel, const char *want, const bag_digests *trusted)
{
    char p[1024], hex[65]; int k = bag_digests_find(trusted, rel);
    path_of(bagdir, rel, p, sizeof(p));
    if (k >= 0 && !strcmp(trusted->hex[k], want)) return u8_exists(p);
    return sha256_path(p, hex, NULL) == 0 && !strcmp(hex, want);
}

/* 7: the tag files, the files not already trusted, and nothing more or
 * less in data/ than in the manifest */
static int check_new(const char *bagdir, const bag_digests *trusted, strbuf *err)
{
    bag_digests man, tag; char data[1024]; extra_ctx x; int i, rc = -1;
    memset(&man, 0, sizeof(man)); memset(&tag, 0, sizeof(tag));
    if (g_fail == 7) { sb_puts(err, "the new package does not match its manifest"); return -1; }
    if (bag_manifest_load(bagdir, "manifest-sha256.txt", &man) ||
        bag_manifest_load(bagdir, "tagmanifest-sha256.txt", &tag)) {
        sb_printf(err, "the manifests of %s cannot be read", bagdir);
        goto out;
    }
    for (i = 0; i < tag.n; i++)
        if (!line_ok(bagdir, tag.rel[i], tag.hex[i], NULL)) {
            sb_printf(err, "%s does not match the new manifest", tag.rel[i]);
            goto out;
        }
    for (i = 0; i < man.n; i++)
        if (!line_ok(bagdir, man.rel[i], man.hex[i], trusted)) {
            sb_printf(err, "%s does not match the new manifest", man.rel[i]);
            goto out;
        }
    x.man = &man; x.extra = 0;
    u8_join(data, sizeof(data), bagdir, "data");
    u8_walk(data, count_extra, &x);
    if (x.extra) { sb_printf(err, "data/ in %s has files the new manifest does not list", bagdir); goto out; }
    rc = 0;
out:
    bag_digests_free(&man); bag_digests_free(&tag);
    return rc;
}

/* 8: through a temporary name when only the case of the letters changes */
static int rename_bag(const char *bagdir, const char *newdir, strbuf *err)
{
    char tmp[1100];
    if (!newdir || !strcmp(newdir, bagdir)) return 0;
    if (g_fail != 8) {
        if (same_ci(newdir, bagdir)) {
            snprintf(tmp, sizeof(tmp), "%s.tmp-rename", newdir);
            if (u8_rename_replace(bagdir, tmp) == 0) {
                if (u8_rename_replace(tmp, newdir) == 0) return 0;
                u8_rename_replace(tmp, bagdir);
            }
        } else if (u8_rename_replace(bagdir, newdir) == 0) {
            return 0;
        }
    }
    sb_printf(err, "cannot rename %s: close the folder if it is open in Explorer and try again", bagdir);
    return -1;
}

/* 9: the old tag files in set-aside/previous-<date>, editing/ removed */
static int cleanup(const char *bagdir)
{
    char from[1024], to[1024], now[20], rel[200], *q; int j;
    meta_now(now);
    for (q = now; (q = strchr(q, ':')) != NULL; ) memmove(q, q + 1, strlen(q + 1) + 1);
    snprintf(rel, sizeof(rel), "%s/previous-%s", session_aside_dir(bagdir), now);
    path_of(bagdir, rel, to, sizeof(to));
    for (j = 2; u8_exists(to) && j < 100; j++) {
        char alt[220];
        snprintf(alt, sizeof(alt), "%s-%d", rel, j);
        path_of(bagdir, alt, to, sizeof(to));
    }
    path_of(bagdir, OLD_DIR, from, sizeof(from));
    if (u8_isdir(from)) {
        parent_of(bagdir, rel);
        u8_rename_replace(from, to);
    }
    if (g_crash == 10) return -2;
    /* the journal goes first: without it nothing can be undone, and the
     * rest of editing/ is no longer needed */
    path_of(bagdir, JOURNAL, from, sizeof(from));
    u8_remove(from);
    path_of(bagdir, PROGRESS, from, sizeof(from));
    u8_remove(from);
    if (g_crash == 11) return -2;
    path_of(bagdir, BOXTXN_DIR, from, sizeof(from));
    u8_rmtree(from);
    return 0;
}

/* ---- back to the old bag ---- */

/* moved back, not copied: a rename needs no space on a full disk; -1 if
 * one did not come back */
static int restore_old(const char *bagdir)
{
    char from[1024], to[1024], rel[200]; int i, rc = 0;
    for (i = 0; i < NTAG; i++) {
        snprintf(rel, sizeof(rel), OLD_DIR "/%s", base_of(tag_files[i]));
        path_of(bagdir, rel, from, sizeof(from));
        path_of(bagdir, tag_files[i], to, sizeof(to));
        if (u8_exists(from) && u8_rename_replace(from, to)) rc = -1;
    }
    return rc;
}

/* the done moves undone, the old tag files back (restore: they may have
 * been rewritten), no marker; editing/ keeps only the new reads */
static int roll_back(const char *bagdir, const txn_moves *m, int done, int restore)
{
    static const char *const work[] = { OLD_DIR, NEW_DIR, JOURNAL, PROGRESS };
    char p[1024]; int i, rc = 0;
    for (i = done - 1; i >= 0; i--) {
        if (g_fail_undo || do_move(bagdir, m->to[i], m->from[i])) { rc = -1; break; }
        set_progress(bagdir, i);
    }
    if (!rc && restore) rc = restore_old(bagdir);
    /* something could not be put back: the journal, the copies and the open
     * marker stay, and a later recovery tries again */
    if (rc) return -1;
    u8_join(p, sizeof(p), bagdir, BAG_OPEN_MARK);
    if (u8_exists(p)) u8_remove(p);
    for (i = 0; i < 4; i++) {
        path_of(bagdir, work[i], p, sizeof(p));
        u8_rmtree(p);
    }
    return rc;
}

#define CRASH(k) do { if (g_crash == (k)) return -2; } while (0)

int boxtxn_commit(const char *bagdir, const txn_moves *m, const meta_box *box, meta_tech_fn tech,
                  void *ctx, const bag_digests *known, const char *newdir, strbuf *err)
{
    bag_digests trusted; int rc, done = 0;
    /* tests only: RETROARCHIVER_TXN_CRASH=N stops a program (dskrun) at step N */
    if (!g_crash && getenv("RETROARCHIVER_TXN_CRASH")) g_crash = atoi(getenv("RETROARCHIVER_TXN_CRASH"));
    CRASH(1);
    if (precheck(bagdir, m, newdir, err)) return -1;
    CRASH(2);
    if (keep_old(bagdir, box, err)) { roll_back(bagdir, m, 0, 0); return -1; }
    CRASH(3);
    if (write_journal(bagdir, m, newdir, err)) { roll_back(bagdir, m, 0, 0); return -1; }
    CRASH(4);
    if (mark_open(bagdir, err)) { roll_back(bagdir, m, 0, 0); return -1; }
    if ((rc = do_moves(bagdir, m, &done, err)) == -2) return -2;
    if (rc) { roll_back(bagdir, m, done, 0); return -1; }
    CRASH(6);
    memset(&trusted, 0, sizeof(trusted));
    untouched(known, m, &trusted);
    if (close_new(bagdir, box, tech, ctx, &trusted, err)) goto back;
    CRASH(7);
    if (check_new(bagdir, &trusted, err)) goto back;
    CRASH(8);
    if (rename_bag(bagdir, newdir, err)) goto back;
    bag_digests_free(&trusted);
    CRASH(9);
    return cleanup(newdir ? newdir : bagdir) == -2 ? -2 : 0;
back:
    bag_digests_free(&trusted);
    roll_back(bagdir, m, done, 1);
    return -1;
}

/* ---- after an interruption ---- */

static int read_journal(const char *bagdir, txn_moves *m, char *folder, size_t fn)
{
    char p[1024], line[800], *t1, *t2, *e; FILE *f; int ok = 0;
    folder[0] = 0;
    path_of(bagdir, JOURNAL, p, sizeof(p));
    if (!(f = u8_fopen(p, "rb"))) return -1;
    while (fgets(line, sizeof(line), f)) {
        e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (!strcmp(line, JOURNAL_ID)) { ok = 1; continue; }
        if (!(t1 = strchr(line, '\t'))) continue;
        *t1++ = 0;
        if (!strcmp(line, "folder")) { str_copy(folder, fn, t1); continue; }
        if (strcmp(line, "move") || !(t2 = strchr(t1, '\t'))) continue;
        *t2++ = 0;
        txn_add(m, t1, t2);
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* the reads the stage's metadata.json lists are finished: set aside as
 * .unused; the rest is what an interruption left */
/* -1 if a finished read could not be set aside: editing/ stays, nothing
 * is deleted, and the next recovery tries again */
static int clean_stage(const char *bagdir, strbuf *log)
{
    char stage[1024], from[1024], to[400], p[1024]; meta_box *b = meta_new(); strbuf err; int i, k, stuck = 0;
    sb_init(&err);
    path_of(bagdir, BOXTXN_STAGE, stage, sizeof(stage));
    if (b && meta_load(b, stage, &err) == 0)
        for (i = 0; i < b->ndisk; i++)
            for (k = 0; k < b->disk[i].nreads; k++) {
                u8_join(from, sizeof(from), stage, b->disk[i].reads[k].file);
                if (!u8_exists(from)) continue;
                txn_aside_name(bagdir, NULL, b->disk[i].reads[k].file, ".unused", to, sizeof(to));
                parent_of(bagdir, to);
                path_of(bagdir, to, p, sizeof(p));
                if (u8_rename_replace(from, p) == 0)
                    sb_printf(log, "%s set aside as %s: it was never used.\n", b->disk[i].reads[k].file, to);
                else stuck = 1;
            }
    meta_delete(b);
    sb_free(&err);
    if (stuck) {
        sb_printf(log, "Some reads in %s cannot be set aside yet: they stay in " BOXTXN_DIR ".\n", bagdir);
        return -1;
    }
    path_of(bagdir, BOXTXN_DIR, p, sizeof(p));
    u8_rmtree(p);
    return 0;
}

/* the moves still to make can be made, and the new metadata is there */
static int forward_possible(const char *bagdir, const txn_moves *m, int done, meta_box *box)
{
    char p[1024]; strbuf err; int i, ok;
    for (i = done; i < m->n; i++)
        if (!rel_exists(bagdir, m->from[i])) return 0;
    sb_init(&err);
    path_of(bagdir, NEW_DIR, p, sizeof(p));
    ok = meta_load(box, p, &err) == 0;
    sb_free(&err);
    return ok;
}

static int finish(const char *bagdir, const txn_moves *m, int *done, const meta_box *box,
                  meta_tech_fn tech, void *ctx, const char *newdir, strbuf *log)
{
    strbuf err; int rc = -1;
    sb_init(&err);
    if (do_moves(bagdir, m, done, &err) == 0 && mark_open(bagdir, &err) == 0 &&
        close_new(bagdir, box, tech, ctx, NULL, &err) == 0 && check_new(bagdir, NULL, &err) == 0 &&
        rename_bag(bagdir, newdir, &err) == 0)
        rc = 0;
    if (rc) sb_printf(log, "%s\n", err.s ? err.s : "");
    sb_free(&err);
    return rc;
}

int boxtxn_recover(const char *bagdir, meta_tech_fn tech, void *ctx, char *finaldir, size_t n,
                   strbuf *log)
{
    char p[1024], folder[300], newdir[1024]; txn_moves m; meta_box *box; int rc, done;
    str_copy(finaldir, n, bagdir);
    path_of(bagdir, BOXTXN_DIR, p, sizeof(p));
    if (!u8_isdir(p)) return BOXTXN_NONE;
    memset(&m, 0, sizeof(m));
    if (read_journal(bagdir, &m, folder, sizeof(folder))) {
        txn_free(&m);
        return clean_stage(bagdir, log) ? -1 : BOXTXN_CLEANED;
    }
    newdir[0] = 0;
    if (folder[0]) {
        /* the parent folder of the bag, then the new name */
        char parent[1024];
        CP(parent, bagdir);
        parent[base_of(parent) - parent] = 0;
        snprintf(newdir, sizeof(newdir), "%s%s", parent, folder);
    }
    if (!(box = meta_new())) { txn_free(&m); return -1; }
    done = moves_done(bagdir, &m);
    if (folder[0] && !strcmp(base_of(bagdir), folder) && moves_done(bagdir, &m) == m.n) {
        /* already renamed: the change was complete, only forward from here */
        if (forward_possible(bagdir, &m, done, box) && finish(bagdir, &m, &done, box, tech, ctx, NULL, log) == 0) {
            cleanup(bagdir);
            sb_printf(log, "The interrupted change of %s was finished.\n", bagdir);
            rc = BOXTXN_FORWARD;
        } else {
            sb_printf(log, "The interrupted change of %s could not be finished yet: it stays in editing/.\n", bagdir);
            rc = -1;
        }
    } else if (forward_possible(bagdir, &m, done, box) &&
        finish(bagdir, &m, &done, box, tech, ctx, newdir[0] ? newdir : NULL, log) == 0) {
        str_copy(finaldir, n, newdir[0] ? newdir : bagdir);
        cleanup(finaldir);
        sb_printf(log, "The interrupted change of %s was finished.\n", finaldir);
        rc = BOXTXN_FORWARD;
    } else {
        rc = roll_back(bagdir, &m, done, 1) ? -1 : BOXTXN_BACK;
        if (rc == BOXTXN_BACK) {
            clean_stage(bagdir, log);
            sb_printf(log, "The interrupted change of %s was undone: the box is as it was.\n", bagdir);
        }
    }
    meta_delete(box);
    txn_free(&m);
    return rc;
}
