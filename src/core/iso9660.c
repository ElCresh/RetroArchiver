#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iso9660.h"
#include "fsu8.h"
#include "strbuf.h"

#define MAXDEPTH 16

typedef struct {
    FILE *f;
    int ssize, off;
    int joliet;             /* record names are UCS-2 big endian */
    iso_info *out;
} disc;

static int sector(disc *d, long lba, unsigned char buf[2048])
{
    long long pos = (long long)lba * d->ssize + d->off;
#ifdef _WIN32
    /* _fseeki64 is missing from the XP msvcrt; there fpos_t is a 64-bit integer */
    fpos_t p = (fpos_t)pos;
    if (fsetpos(d->f, &p)) return -1;
#else
    if (fseeko(d->f, (off_t)pos, SEEK_SET)) return -1;
#endif
    return fread(buf, 1, 2048, d->f) == 2048 ? 0 : -1;
}

static unsigned long le32(const unsigned char *p)
{
    return p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* a space-padded field: without trailing spaces, printable ASCII only */
static void field(char *out, size_t n, const unsigned char *p, size_t len)
{
    size_t i, k = 0;
    for (i = 0; i < len && k + 1 < n; i++) out[k++] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '_';
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static void put_utf8(char *out, size_t n, size_t *k, unsigned long cp)
{
    char b[4]; size_t len, i;
    if (cp < 0x80) { b[0] = (char)cp; len = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 63)); len = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 63));
        b[2] = (char)(0x80 | (cp & 63)); len = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 63));
        b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); len = 4;
    }
    if (*k + len >= n) return;          /* does not fit: no half character */
    for (i = 0; i < len; i++) out[(*k)++] = b[i];
}

/* a Joliet field: UCS-2 big endian (surrogate pairs too) to UTF-8,
 * without trailing spaces; control characters become '_' */
static void field_ucs2(char *out, size_t n, const unsigned char *p, size_t len)
{
    size_t i, k = 0;
    for (i = 0; i + 1 < len; i += 2) {
        unsigned long c = ((unsigned long)p[i] << 8) | p[i + 1];
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < len) {
            unsigned long lo = ((unsigned long)p[i + 2] << 8) | p[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) c = '_';
        put_utf8(out, n, &k, c);
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* a supplementary descriptor is Joliet if it declares UCS-2 level 1, 2 or 3 */
static int is_joliet(const unsigned char *svd)
{
    return svd[0] == 2 && svd[88] == '%' && svd[89] == '/' &&
           (svd[90] == '@' || svd[90] == 'C' || svd[90] == 'E');
}

/* descriptor date: "YYYYMMDDHHMMSScc" + time zone; all zeros = none */
static void desc_date(char out[20], const unsigned char *p)
{
    int i;
    out[0] = 0;
    for (i = 0; i < 14; i++) if (p[i] < '0' || p[i] > '9') return;
    if (!memcmp(p, "0000", 4)) return;
    snprintf(out, 20, "%.4s-%.2s-%.2sT%.2s:%.2s:%.2s", p, p + 4, p + 6, p + 8, p + 10, p + 12);
}

/* date of a directory record: years since 1900, month, day, hour, minutes, seconds */
static void rec_date(char out[20], const unsigned char *p)
{
    out[0] = 0;
    if (!p[1] || p[1] > 12 || !p[2] || p[2] > 31 || p[3] > 23 || p[4] > 59 || p[5] > 59) return;
    snprintf(out, 20, "%04d-%02d-%02dT%02d:%02d:%02d", 1900 + p[0], p[1], p[2], p[3], p[4], p[5]);
}

static void add(disc *d, const char *path, long long size, const char *date, int isdir)
{
    iso_info *o = d->out; iso_file *f;
    if (o->nfiles >= ISO_MAXFILES) { o->truncated = 1; return; }
    f = &o->files[o->nfiles++];
    str_copy(f->path, sizeof(f->path), path);
    f->size = size;
    str_copy(f->date, sizeof(f->date), date);
    f->isdir = isdir;
}

static void walk(disc *d, unsigned long lba, unsigned long len, const char *prefix, int depth)
{
    unsigned char buf[2048];
    unsigned long done;
    if (depth > MAXDEPTH || len > 16UL * 1024 * 1024) return;
    for (done = 0; done < len; done += 2048) {
        int pos = 0;
        if (sector(d, (long)(lba + done / 2048), buf)) {
            str_copy(d->out->note, sizeof(d->out->note), "a folder of the CD cannot be read");
            return;
        }
        while (pos < 2048 && buf[pos]) {
            const unsigned char *r = buf + pos;
            int rlen = r[0], nlen = r[32], isdir = (r[25] & 2) != 0;
            char name[400], path[512], date[20];
            if (rlen < 34 || pos + rlen > 2048 || 33 + nlen > rlen) break;
            pos += rlen;
            if (nlen == 1 && (r[33] == 0 || r[33] == 1)) continue;     /* . and .. */
            if (d->joliet) field_ucs2(name, sizeof(name), r + 33, (size_t)nlen);
            else field(name, sizeof(name), r + 33, (size_t)nlen);
            if (!isdir) {                                                /* "README.TXT;1" */
                char *semi = strchr(name, ';');
                if (semi) *semi = 0;
                if (name[0] && name[strlen(name) - 1] == '.') name[strlen(name) - 1] = 0;
            }
            if (prefix[0]) snprintf(path, sizeof(path), "%s/%s", prefix, name);
            else snprintf(path, sizeof(path), "%s", name);
            rec_date(date, r + 18);
            add(d, path, isdir ? 0 : (long long)le32(r + 10), date, isdir);
            if (isdir) walk(d, le32(r + 2), le32(r + 10), path, depth + 1);
        }
    }
}

static int by_path(const void *a, const void *b)
{
    return strcmp(((const iso_file *)a)->path, ((const iso_file *)b)->path);
}

int iso_read(const char *path, int sector_size, int data_offset, iso_info *out)
{
    disc d; unsigned char pvd[2048], vd[2048], svd[2048]; long lba; int has_pvd = 0, has_svd = 0;
    char label[sizeof(out->volume_label)];
    memset(out, 0, sizeof(*out));
    d.f = u8_fopen(path, "rb");
    d.ssize = sector_size; d.off = data_offset; d.out = out; d.joliet = 0;
    if (!d.f) { str_copy(out->note, sizeof(out->note), "the image cannot be read"); return 0; }
    /* the descriptors go from sector 16 up to the terminator: the primary
     * and, if present, the Joliet supplementary one with the long names */
    for (lba = 16; lba < 32; lba++) {
        if (sector(&d, lba, vd) || memcmp(vd + 1, "CD001", 5) || vd[0] == 255) break;
        if (vd[0] == 1 && !has_pvd) { memcpy(pvd, vd, 2048); has_pvd = 1; }
        else if (is_joliet(vd) && !has_svd) { memcpy(svd, vd, 2048); has_svd = 1; }
    }
    if (!has_pvd) {
        fclose(d.f);
        str_copy(out->note, sizeof(out->note), "no ISO 9660 file system (an audio disc, or another file system)");
        return 0;
    }
    field(out->system_id, sizeof(out->system_id), pvd + 8, 32);
    field(out->volume_label, sizeof(out->volume_label), pvd + 40, 32);
    field(out->publisher, sizeof(out->publisher), pvd + 318, 128);
    field(out->application, sizeof(out->application), pvd + 574, 128);
    desc_date(out->created, pvd + 813);
    desc_date(out->modified, pvd + 830);
    out->volume_bytes = (long long)le32(pvd + 80) * (pvd[128] | (pvd[129] << 8));
    out->files = (iso_file *)calloc(ISO_MAXFILES, sizeof(iso_file));
    if (!out->files) { fclose(d.f); str_copy(out->note, sizeof(out->note), "out of memory"); return 0; }
    if (has_svd) {
        d.joliet = 1;
        walk(&d, le32(svd + 156 + 2), le32(svd + 156 + 10), "", 0);
        /* a Joliet that cannot be read: fall back to the 8.3 names */
        if (!out->nfiles && out->note[0]) {
            out->note[0] = 0; out->truncated = 0; d.joliet = 0;
        } else {
            out->joliet = 1;
            field_ucs2(label, sizeof(label), svd + 40, 32);
            if (label[0]) str_copy(out->volume_label, sizeof(out->volume_label), label);
        }
    }
    if (!out->joliet) walk(&d, le32(pvd + 156 + 2), le32(pvd + 156 + 10), "", 0);
    fclose(d.f);
    if (out->nfiles) qsort(out->files, (size_t)out->nfiles, sizeof(iso_file), by_path);
    if (out->truncated)
        snprintf(out->note, sizeof(out->note), "file list truncated at %d entries", ISO_MAXFILES);
    return 1;
}

void iso_free(iso_info *i)
{
    free(i->files);
    i->files = NULL;
    i->nfiles = 0;
}
