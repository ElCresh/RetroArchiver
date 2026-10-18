#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "edsk.h"
#include "fsu8.h"

unsigned long edsk_fnv(const unsigned char *p, long n)
{
    unsigned long h = 2166136261UL; long i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619UL; h &= 0xFFFFFFFFUL; }
    return h;
}

int edsk_snap(int n)
{
    if (n >= 80) return 80;
    if (n >= 40) return 40;
    if (n >= 35) return 35;
    return n;
}

void fmt_thousands(long v, char *out)
{
    char tmp[32]; int n, i, j;
    n = sprintf(tmp, "%ld", v);
    for (i = 0, j = 0; i < n; i++) {
        out[j++] = tmp[i];
        if ((n - 1 - i) % 3 == 0 && i != n - 1) out[j++] = ',';
    }
    out[j] = 0;
}

/* A track is formatted if it has a valid Track-Info with sectors > 0.
 * The extra cylinders SAMdisk captures beyond the real format are not,
 * or they contain noise without a valid header. */
static void detect(edsk *d)
{
    int t, c, h, allfmt;

    d->nomcyl = 0; d->nsec0 = 0;
    if (!d->ok) return;

    for (t = 0; t < d->ntrk; t++) {
        d->fmt[t] = 0;
        if (d->siz[t] < 256) continue;
        if (d->off[t] + d->siz[t] > d->len) continue;
        if (memcmp(d->buf + d->off[t], "Track-Info\r\n", 12)) continue;
        if (d->buf[d->off[t] + 0x15] == 0) continue;
        d->fmt[t] = 1;
    }
    if (d->fmt[0]) d->nsec0 = d->buf[d->off[0] + 0x15];

    /* the format extends up to the last fully formatted cylinder */
    for (c = 0; c < d->cyls; c++) {
        allfmt = 1;
        for (h = 0; h < d->heads; h++) {
            t = c * d->heads + h;
            if (t >= d->ntrk || !d->fmt[t]) { allfmt = 0; break; }
        }
        if (!allfmt) break;
        d->nomcyl = c + 1;
    }
}

int edsk_load_as(const char *path, const char *shown, edsk *d, strbuf *msg)
{
    FILE *f; long pos; int t;

    memset(d, 0, sizeof(*d));
    f = u8_fopen(path, "rb");
    if (!f) { sb_printf(msg, "  ERROR: cannot open %s\n", shown); return 0; }
    fseek(f, 0, SEEK_END); d->len = ftell(f); fseek(f, 0, SEEK_SET);
    if (d->len < 256) {
        sb_printf(msg, "  ERROR: %s is too short\n", shown);
        fclose(f); return 0;
    }
    d->buf = (unsigned char *)malloc(d->len);
    if (!d->buf) { sb_puts(msg, "  ERROR: out of memory\n"); fclose(f); return 0; }
    if (fread(d->buf, 1, d->len, f) != (size_t)d->len) {
        sb_printf(msg, "  ERROR: could not read all of %s\n", shown);
        fclose(f); edsk_free(d); return 0;
    }
    fclose(f);

    d->cyls  = d->buf[0x30];
    d->heads = d->buf[0x31];
    if (d->heads < 1) d->heads = 1;
    d->ntrk  = d->cyls * d->heads;
    if (d->ntrk > EDSK_MAXTRK) d->ntrk = EDSK_MAXTRK;

    if (!memcmp(d->buf, "EXTENDED", 8)) {
        /* EDSK: size table, one byte per track, x256; it lives in the
         * header, so at most 256 - 0x34 = 204 tracks */
        if (0x34 + d->ntrk > 256) d->ntrk = 256 - 0x34;
        pos = 256;
        for (t = 0; t < d->ntrk; t++) {
            d->siz[t]  = (long)d->buf[0x34 + t] * 256L;
            d->off[t]  = pos;
            d->cyl[t]  = t / d->heads;
            d->head[t] = t % d->heads;
            pos += d->siz[t];
        }
        d->ok = 1;
    } else if (!memcmp(d->buf, "MV - CPC", 8)) {
        /* classic DSK: all tracks of the same size */
        long ts = (long)d->buf[0x32] | ((long)d->buf[0x33] << 8);
        pos = 256;
        for (t = 0; t < d->ntrk; t++) {
            d->siz[t]  = ts;
            d->off[t]  = pos;
            d->cyl[t]  = t / d->heads;
            d->head[t] = t % d->heads;
            pos += ts;
        }
        d->ok = 1;
    } else {
        sb_printf(msg, "  WARNING: %s has no recognizable DSK header.\n", shown);
        sb_puts(msg, "           Comparing byte by byte only, without a track map.\n\n");
        d->ok = 0;
    }
    detect(d);
    return 1;
}

int edsk_load(const char *path, edsk *d, strbuf *msg)
{
    return edsk_load_as(path, path, d, msg);
}

void edsk_free(edsk *d)
{
    free(d->buf);
    d->buf = NULL;
}

int edsk_track_of(const edsk *d, long o)
{
    int t;
    if (!d->ok || o < 256) return -1;
    for (t = 0; t < d->ntrk; t++)
        if (d->siz[t] > 0 && o >= d->off[t] && o < d->off[t] + d->siz[t]) return t;
    return -1;
}

const unsigned char *edsk_sector(const edsk *d, int c, int h, int r, int *len)
{
    int t = c * d->heads + h, i, ns, n;
    long pos, sz, nominal, lim;
    const unsigned char *tib;

    if (!d->ok || c < 0 || h < 0 || h >= d->heads || t >= d->ntrk || !d->fmt[t])
        return NULL;
    tib = d->buf + d->off[t];
    ns  = tib[0x15];
    pos = d->off[t] + 256;
    lim = d->off[t] + d->siz[t];
    /* the TIB lists the sectors in the order their data follows */
    for (i = 0; i < ns && 0x18 + i * 8 + 8 <= 256; i++) {
        const unsigned char *si = tib + 0x18 + i * 8;
        n = si[3] > 7 ? 7 : si[3];
        nominal = 128L << n;
        sz = (long)si[6] | ((long)si[7] << 8);   /* EDSK: stored length */
        if (sz == 0) sz = nominal;               /* classic DSK */
        if (si[2] == r) {
            if (pos + nominal > lim) return NULL;
            *len = (int)(sz < nominal ? sz : nominal);
            return d->buf + pos;
        }
        pos += sz;
    }
    return NULL;
}
