#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "bagit.h"
#include "fsu8.h"
#include "messages.h"
#include "sha256.h"

/* SAMdisk 3.8.12 receives its arguments in the ANSI code page (cp1252 on
 * an Italian XP): a character outside it would arrive as '?' */
static int in_cp1252(unsigned long cp)
{
    static const unsigned short extra[] = {
        0x20AC, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
        0x0160, 0x2039, 0x0152, 0x017D, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x017E, 0x0178
    };
    size_t i;
    if (cp >= 0x20 && cp < 0x7F) return 1;
    if (cp >= 0xA0 && cp <= 0xFF) return 1;
    for (i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) if (extra[i] == cp) return 1;
    return 0;
}

/* decodes one UTF-8 character; returns the bytes consumed */
static int utf8_next(const unsigned char *p, unsigned long *cp)
{
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((unsigned long)(p[0] & 31) << 6) | (p[1] & 63); return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((unsigned long)(p[0] & 15) << 12) | ((unsigned long)(p[1] & 63) << 6) | (p[2] & 63);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *cp = ((unsigned long)(p[0] & 7) << 18) | ((unsigned long)(p[1] & 63) << 12) |
              ((unsigned long)(p[2] & 63) << 6) | (p[3] & 63);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

/* CON, NUL, COM1... are names reserved by Windows even with an extension */
static int reserved_stem(const char *s)
{
    static const char *names[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    size_t len = strcspn(s, "."), i, j;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) != len) continue;
        for (j = 0; j < len && toupper((unsigned char)s[j]) == names[i][j]; j++) ;
        if (j == len) return 1;
    }
    return 0;
}

void bag_dirname(const char *title, char *out, size_t n)
{
    const unsigned char *p = (const unsigned char *)title;
    size_t o = 0, start = 0;
    unsigned long cp; int k;

    while (*p && o + 4 < n) {
        k = utf8_next(p, &cp);
        /* % too: the names end up in the manifest, which is not percent-encoded */
        if ((cp < 0x80 && strchr("\\/:*?\"<>|%", (int)cp)) || !in_cp1252(cp))
            out[o++] = '_';
        else { memcpy(out + o, p, (size_t)k); o += (size_t)k; }
        p += k;
    }
    out[o] = 0;
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) out[--o] = 0;
    while (out[start] == ' ' || out[start] == '.') start++;
    if (start) memmove(out, out + start, strlen(out + start) + 1);
    if (!out[0]) str_copy(out, n, "untitled");
    if (reserved_stem(out)) {
        size_t len = strlen(out);
        if (len + 2 > n) { len = n - 2; out[len] = 0; }
        memmove(out + 1, out, len + 1);
        out[0] = '_';
    }
}

static int write_text(const char *path, const char *text)
{
    FILE *f = u8_fopen(path, "wb"); int ok;
    if (!f) return -1;
    ok = fputs(text, f) >= 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

int bag_create(const char *bagdir, strbuf *err)
{
    char p[1024];
    if (u8_mkdir(bagdir)) { sb_printf(err, M_BAG_MKDIR, bagdir); return -1; }
    u8_join(p, sizeof(p), bagdir, "data");
    if (u8_mkdir(p)) { sb_printf(err, M_BAG_MKDIR, p); return -1; }
    u8_join(p, sizeof(p), bagdir, "bagit.txt");
    if (write_text(p, "BagIt-Version: 1.0\nTag-File-Character-Encoding: UTF-8\n")) {
        sb_printf(err, M_BAG_WRITE, p); return -1;
    }
    u8_join(p, sizeof(p), bagdir, BAG_OPEN_MARK);
    if (write_text(p, "Box being archived: the manifests are not written yet.\n")) {
        sb_printf(err, M_BAG_WRITE, p); return -1;
    }
    return 0;
}

int bag_is_open(const char *bagdir)
{
    char p[1024];
    u8_join(p, sizeof(p), bagdir, BAG_OPEN_MARK);
    if (u8_exists(p)) return 1;
    u8_join(p, sizeof(p), bagdir, BAG_OPEN_MARK_OLD);
    return u8_exists(p);
}

int bag_status_complete(const char *status)
{
    return !strcmp(status, BAG_COMPLETE) || !strcmp(status, "completo");
}

void bag_digests_add(bag_digests *d, const char *rel, const char *hex)
{
    size_t len = strlen(rel) + 1;
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->rel = (char **)realloc(d->rel, (size_t)d->cap * sizeof(char *));
        d->hex = (char (*)[65])realloc(d->hex, (size_t)d->cap * sizeof(*d->hex));
    }
    d->rel[d->n] = (char *)malloc(len);
    memcpy(d->rel[d->n], rel, len);
    str_copy(d->hex[d->n], sizeof(d->hex[d->n]), hex);
    d->n++;
}

int bag_digests_find(const bag_digests *d, const char *rel)
{
    int i;
    for (i = 0; d && i < d->n; i++) if (!strcmp(d->rel[i], rel)) return i;
    return -1;
}

void bag_digests_free(bag_digests *d)
{
    int i;
    for (i = 0; i < d->n; i++) free(d->rel[i]);
    free(d->rel); free(d->hex);
    memset(d, 0, sizeof(*d));
}

int bag_manifest_load(const char *bagdir, const char *name, bag_digests *out)
{
    char p[1024], line[1200], *sp, *e; FILE *f;
    u8_join(p, sizeof(p), bagdir, name);
    if (!(f = u8_fopen(p, "rb"))) return -1;
    while (fgets(line, sizeof(line), f)) {
        e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (!(sp = strchr(line, ' '))) continue;
        *sp++ = 0;
        while (*sp == ' ' || *sp == '*') sp++;
        bag_digests_add(out, sp, line);
    }
    fclose(f);
    return 0;
}

typedef struct { char **names; int n, cap; } namelist;

/* the files inside the optical media folders too (data/CD01/...) */
static void collect(void *ctx, const char *name)
{
    namelist *l = (namelist *)ctx; size_t len;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->names = (char **)realloc(l->names, (size_t)l->cap * sizeof(char *));
    }
    len = strlen(name) + 1;
    l->names[l->n] = (char *)malloc(len);
    memcpy(l->names[l->n++], name, len);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* "Key: value" on one line; newlines and tabs become spaces */
static void field(strbuf *o, const char *key, const char *val)
{
    const char *p;
    if (!val || !val[0]) return;
    sb_printf(o, "%s: ", key);
    for (p = val; *p; p++) {
        char c[2];
        c[0] = (*p == '\r' || *p == '\n' || *p == '\t') ? ' ' : *p; c[1] = 0;
        sb_puts(o, c);
    }
    sb_puts(o, "\n");
}

static const meta_disk *disk_n(const meta_box *b, int n)
{
    int i;
    for (i = 0; i < b->ndisk; i++) if (b->disk[i].n == n) return &b->disk[i];
    return NULL;
}

/* the digest of a read that is a folder (CD01), from the digests of its
 * files already computed for the manifest: like sha256_path. 0 if the
 * folder has no files */
static int tree_hash(const namelist *l, char **hashes, const char *dir, char hex[65])
{
    sha256_ctx c; unsigned char d[32]; size_t len = strlen(dir); int i, n = 0;
    sha256_init(&c);
    for (i = 0; i < l->n; i++) {
        if (strncmp(l->names[i], dir, len) || l->names[i][len] != '/') continue;
        sha256_tree_line(&c, hashes[i], l->names[i] + len + 1);
        n++;
    }
    if (!n) return 0;
    sha256_final(&c, d);
    sha256_hex(d, hex);
    return 1;
}

/* writes a tag file and adds its digest to the tagmanifest */
static int tag_file(const char *bagdir, const char *name, const char *text,
                    strbuf *tag, strbuf *err)
{
    char p[1024], hex[65];
    u8_join(p, sizeof(p), bagdir, name);
    if (text && write_text(p, text)) { sb_printf(err, M_BAG_WRITE, p); return -1; }
    if (sha256_file(p, hex, NULL)) { sb_printf(err, M_BAG_READ, p); return -1; }
    sb_printf(tag, "%s  %s\n", hex, name);
    return 0;
}

int bag_close(const char *bagdir, const meta_box *box, const bag_digests *known,
              strbuf *err, int *bad_disk)
{
    char data[1024], p[1024], hex[65], date[11], now[20], comp[80], name[32];
    namelist l = { NULL, 0, 0 };
    strbuf man, info, tag;
    char **hashes;
    long long total = 0, bytes;
    int i, k, j, rc = -1, archived = 0, complete = 1, first = 1;

    if (bad_disk) *bad_disk = 0;
    if (!box->title[0] || !box->platform[0] || box->media_expected < 1) {
        sb_printf(err, M_BAG_NO_DATA);
        return -1;
    }
    u8_join(data, sizeof(data), bagdir, "data");
    if (u8_walk(data, collect, &l)) { sb_printf(err, M_BAG_READ, data); return -1; }
    if (l.n) qsort(l.names, (size_t)l.n, sizeof(char *), by_name);
    hashes = (char **)calloc((size_t)(l.n ? l.n : 1), sizeof(char *));
    sb_init(&man); sb_init(&info); sb_init(&tag);

    for (i = 0; i < l.n; i++) {
        char rel[1040]; int kn;
        u8_join(p, sizeof(p), data, l.names[i]);
        snprintf(rel, sizeof(rel), "data/%s", l.names[i]);
        if ((kn = bag_digests_find(known, rel)) >= 0 && (bytes = u8_filesize(p)) >= 0)
            memcpy(hex, known->hex[kn], 65);
        else if (sha256_file(p, hex, &bytes)) { sb_printf(err, M_BAG_READ, p); goto out; }
        hashes[i] = (char *)malloc(65);
        memcpy(hashes[i], hex, 65);
        total += bytes;
        sb_printf(&man, "%s  data/%s\n", hex, l.names[i]);
    }

    /* the digests taken right after each read must match */
    for (i = 0; i < box->ndisk; i++) {
        const meta_disk *d = &box->disk[i];
        for (k = 0; k < d->nreads; k++) {
            const char *got = NULL;
            for (j = 0; j < l.n && strcmp(l.names[j], d->reads[k].file); j++) ;
            if (j < l.n) got = hashes[j];
            else if (tree_hash(&l, hashes, d->reads[k].file, hex)) got = hex;
            if (!got) {
                meta_disk_name(d->kind, d->num, name, sizeof(name));
                sb_printf(err, M_BAG_MISSING, d->reads[k].file, name);
                if (bad_disk) *bad_disk = d->n;
                goto out;
            }
            if (strcmp(got, d->reads[k].sha256)) {
                meta_disk_name(d->kind, d->num, name, sizeof(name));
                sb_printf(err, M_BAG_CHANGED, d->reads[k].file, name);
                if (bad_disk) *bad_disk = d->n;
                goto out;
            }
        }
    }

    for (i = 1; i <= box->media_expected; i++) {
        const meta_disk *d = disk_n(box, i);
        if (d && d->master[0]) archived++;
        if (!d || !meta_disk_done(d)) complete = 0;
    }
    if (strlen(box->closed) >= 10) memcpy(date, box->closed, 10);
    else { meta_now(now); memcpy(date, now, 10); }
    date[10] = 0;

    field(&info, "Bag-Software-Agent", "RetroArchiver " RA_VERSION);
    field(&info, "Bagging-Date", date);
    field(&info, "BagIt-Profile-Identifier", BAG_PROFILE);
    field(&info, "Title", box->title);
    field(&info, "Platform", box->platform);
    meta_composition(box, comp, sizeof(comp));
    field(&info, "Media-Type", comp);
    sb_printf(&info, "Media-Expected: %d\nMedia-Archived: %d\n", box->media_expected, archived);
    field(&info, "Box-Status", complete ? BAG_COMPLETE : BAG_INCOMPLETE);
    if (!complete) {
        sb_puts(&info, "Missing-Media: ");
        for (i = 1; i <= box->media_expected; i++) {
            const meta_disk *d = disk_n(box, i);
            int kind, num;
            if (d && meta_disk_done(d)) continue;
            meta_slot(box, i, &kind, &num);
            meta_disk_name(kind, num, name, sizeof(name));
            sb_printf(&info, first ? "%s" : ", %s", name);
            first = 0;
        }
        sb_puts(&info, "\n");
    }
    field(&info, "Publisher", box->publisher);
    field(&info, "Year", box->year);
    field(&info, "Language", box->language);
    field(&info, "Edition", box->edition);
    field(&info, "Notes", box->notes);
    sb_printf(&info, "Payload-Oxum: %lld.%d\n", total, l.n);

    if (tag_file(bagdir, "bagit.txt", NULL, &tag, err) ||
        tag_file(bagdir, "bag-info.txt", info.s, &tag, err) ||
        tag_file(bagdir, "manifest-sha256.txt", man.s, &tag, err)) goto out;
    u8_join(p, sizeof(p), bagdir, "tagmanifest-sha256.txt");
    if (write_text(p, tag.s)) { sb_printf(err, M_BAG_WRITE, p); goto out; }
    /* the marker, new or old; neither left means closed */
    u8_join(p, sizeof(p), bagdir, BAG_OPEN_MARK);
    if (u8_exists(p) && u8_remove(p)) {
        sb_printf(err, M_BAG_REMOVE_MARK, BAG_OPEN_MARK, bagdir);
        goto out;
    }
    u8_join(p, sizeof(p), bagdir, BAG_OPEN_MARK_OLD);
    if (u8_exists(p) && u8_remove(p)) {
        sb_printf(err, M_BAG_REMOVE_MARK, BAG_OPEN_MARK_OLD, bagdir);
        goto out;
    }
    rc = 0;
out:
    for (i = 0; i < l.n; i++) { free(l.names[i]); free(hashes[i]); }
    free(l.names); free(hashes);
    sb_free(&man); sb_free(&info); sb_free(&tag);
    return rc;
}
