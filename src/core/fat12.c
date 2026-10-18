#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat12.h"

#define MAXFILES 1024
#define MAXDEPTH 8

typedef struct {
    const edsk *d;
    int spt, heads, spc, reserved, nfats, fatsz, rootents, total;
    int rootsec, datasec;
    unsigned char *fat;           /* first copy of the FAT */
    unsigned char visited[4096]; /* clusters already walked as dirs */
    fat_info *out;
} fs;

/* logical sector (LBA) -> data; NULL if missing or short */
static const unsigned char *lsec(fs *f, int lba)
{
    int c, h, r, len = 0;
    const unsigned char *p;
    if (lba < 0 || lba >= f->total) return NULL;
    c = lba / (f->spt * f->heads);
    h = (lba / f->spt) % f->heads;
    r = lba % f->spt + 1;
    p = edsk_sector(f->d, c, h, r, &len);
    return (p && len >= 512) ? p : NULL;
}

static int fatnext(fs *f, int cl)
{
    int o = cl * 3 / 2, v;
    if (o + 1 >= f->fatsz * 512) return 0xFFF;
    v = f->fat[o] | (f->fat[o + 1] << 8);
    return (cl & 1) ? v >> 4 : v & 0xFFF;
}

static void datestr(const unsigned char *e, char *out)
{
    int t = e[22] | (e[23] << 8), d = e[24] | (e[25] << 8);
    int mo = (d >> 5) & 15, dd = d & 31;
    if (!d || mo < 1 || mo > 12 || dd < 1) { out[0] = 0; return; }
    sprintf(out, "%04d-%02d-%02dT%02d:%02d:%02d", (d >> 9) + 1980, mo, dd,
            (t >> 11) & 31, (t >> 5) & 63, (t & 31) * 2);
}

/* FAT names are in the OEM code page: keep only printable ASCII,
 * so metadata.json stays valid UTF-8 */
static char safe(unsigned char c) { return (c < 0x20 || c > 0x7E) ? '_' : (char)c; }

static void name83(const unsigned char *e, char *out)
{
    int i, n = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++) out[n++] = safe(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
    if (e[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++) out[n++] = safe(e[i]);
    }
    out[n] = 0;
}

/* the first note wins: the cause closest to the origin stays visible */
static void note(fs *f, const char *msg)
{
    if (!f->out->note[0]) str_copy(f->out->note, sizeof(f->out->note), msg);
}

/* 1 if the entry was added, 0 if the limit was reached */
static int add(fs *f, const char *prefix, const unsigned char *e)
{
    fat_entry *fe; char nm[13];
    unsigned long sz;
    if (f->out->nfiles >= MAXFILES) {
        note(f, "list truncated at 1024 entries");
        return 0;
    }
    name83(e, nm);
    fe = &f->out->files[f->out->nfiles++];
    if (prefix[0]) snprintf(fe->path, sizeof(fe->path), "%s/%s", prefix, nm);
    else           snprintf(fe->path, sizeof(fe->path), "%s", nm);
    fe->isdir = (e[11] & 0x10) != 0;
    sz = (unsigned long)e[28] | ((unsigned long)e[29] << 8) |
         ((unsigned long)e[30] << 16) | ((unsigned long)e[31] << 24);
    fe->size = fe->isdir ? 0 : (long)(sz > 0x7FFFFFFFUL ? 0x7FFFFFFFUL : sz);
    datestr(e, fe->date);
    return 1;
}

static void walk_dir(fs *f, int cl, const char *prefix, int depth);

/* one directory sector: 0 if it found the end of the list */
static int dirsec(fs *f, const unsigned char *s, const char *prefix, int depth)
{
    int i, k;
    for (i = 0; i < 16; i++) {
        const unsigned char *e = s + i * 32;
        if (e[0] == 0) return 0;
        if (e[0] == 0xE5 || e[0] == '.') continue;
        if ((e[11] & 0x0F) == 0x0F) continue;               /* long name */
        if (e[11] & 0x08) {                                 /* volume label */
            if (!prefix[0] && !f->out->volume_label[0]) {
                for (k = 0; k < 11; k++) f->out->volume_label[k] = safe(e[k]);
                for (k = 10; k >= 0 && f->out->volume_label[k] == ' '; k--)
                    f->out->volume_label[k] = 0;
            }
            continue;
        }
        if (!add(f, prefix, e)) return 0;                   /* limit reached */
        if ((e[11] & 0x10) && depth < MAXDEPTH) {
            int cl = e[26] | (e[27] << 8);
            /* the array is never reallocated: the pointer stays valid */
            if (cl >= 2) walk_dir(f, cl, f->out->files[f->out->nfiles - 1].path, depth + 1);
        }
    }
    return 1;
}

static void walk_dir(fs *f, int cl, const char *prefix, int depth)
{
    int guard = 0, k;
    while (cl >= 2 && cl < 0xFF0 && guard++ < 4096) {
        if (f->visited[cl]) {
            note(f, "directory with circular references: partial list");
            return;
        }
        f->visited[cl] = 1;
        for (k = 0; k < f->spc; k++) {
            const unsigned char *s = lsec(f, f->datasec + (cl - 2) * f->spc + k);
            if (!s) { note(f, "incomplete directory (unreadable sector)"); return; }
            if (!dirsec(f, s, prefix, depth)) return;
        }
        cl = fatnext(f, cl);
    }
    /* the chain ends with EOC (0xFF8 and above) or it is broken */
    if (cl < 0xFF8) note(f, "broken directory chain");
}

int fat12_read(const edsk *d, fat_info *out)
{
    fs f; const unsigned char *b; int i, len = 0, rootsecs;

    memset(out, 0, sizeof(*out));
    memset(&f, 0, sizeof(f));
    f.d = d; f.out = out;
    if (!d->ok) { str_copy(out->note, sizeof(out->note), "image without an EDSK header"); return 0; }
    b = edsk_sector(d, 0, 0, 1, &len);
    if (!b || len < 512) { str_copy(out->note, sizeof(out->note), "unreadable boot sector"); return 0; }
    if ((b[11] | (b[12] << 8)) != 512 || b[13] == 0 || (b[13] & (b[13] - 1)) ||
        b[16] < 1 || b[16] > 2 || b[21] < 0xF0) {
        str_copy(out->note, sizeof(out->note), "no recognizable FAT12 file system");
        return 0;
    }
    f.spc = b[13];
    f.reserved = b[14] | (b[15] << 8);
    f.nfats = b[16];
    f.rootents = b[17] | (b[18] << 8);
    f.total = b[19] | (b[20] << 8);
    f.fatsz = b[22] | (b[23] << 8);
    f.spt = b[24] | (b[25] << 8);
    f.heads = b[26] | (b[27] << 8);
    if (f.reserved < 1 || f.fatsz < 1 || f.spt < 1 || f.heads < 1 || f.heads > 2 ||
        f.total < 1 || f.rootents < 1) {
        str_copy(out->note, sizeof(out->note), "invalid FAT12 parameters");
        return 0;
    }
    f.rootsec = f.reserved + f.nfats * f.fatsz;
    rootsecs = (f.rootents * 32 + 511) / 512;
    f.datasec = f.rootsec + rootsecs;

    f.fat = (unsigned char *)malloc((size_t)f.fatsz * 512);
    out->files = (fat_entry *)calloc(MAXFILES, sizeof(fat_entry));
    if (!f.fat || !out->files) {
        free(f.fat); fat12_free(out);
        str_copy(out->note, sizeof(out->note), "out of memory");
        return 0;
    }
    for (i = 0; i < f.fatsz; i++) {
        const unsigned char *s = lsec(&f, f.reserved + i);
        if (!s) {
            free(f.fat); fat12_free(out);
            str_copy(out->note, sizeof(out->note), "unreadable FAT (missing or damaged sector)");
            return 0;
        }
        memcpy(f.fat + i * 512, s, 512);
    }
    for (i = 0; i < rootsecs; i++) {
        const unsigned char *s = lsec(&f, f.rootsec + i);
        if (!s) { note(&f, "incomplete root directory"); break; }
        if (!dirsec(&f, s, "", 0)) break;
    }
    free(f.fat);
    return 1;
}

void fat12_free(fat_info *fi)
{
    free(fi->files);
    fi->files = NULL;
    fi->nfiles = 0;
}
