#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#include "fsu8.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(sha256_ctx *c, const unsigned char *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(sha256_ctx *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(c->h, iv, sizeof(iv));
    c->bytes = 0; c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    c->bytes += len;
    while (len) {
        size_t k = 64 - c->n;
        if (k > len) k = len;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; len -= k;
        if (c->n == 64) { block(c, c->buf); c->n = 0; }
    }
}

void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    uint64_t bits = c->bytes * 8;
    unsigned char pad = 0x80, zero = 0, len[8];
    int i;

    sha256_update(c, &pad, 1);
    while (c->n != 56) sha256_update(c, &zero, 1);
    for (i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, len, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

void sha256_hex(const unsigned char d[32], char out[65])
{
    static const char x[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[i * 2] = x[d[i] >> 4]; out[i * 2 + 1] = x[d[i] & 15]; }
    out[64] = 0;
}

int sha256_file(const char *path, char hex[65], long long *bytes)
{
    /* a buffer per call: the GUI computes digests from several threads */
    enum { BUFSZ = 65536 };
    unsigned char *buf; FILE *f;
    sha256_ctx c; unsigned char d[32]; size_t n; int bad;

    f = u8_fopen(path, "rb");
    if (!f) return -1;
    buf = (unsigned char *)malloc(BUFSZ);
    if (!buf) { fclose(f); return -1; }
    sha256_init(&c);
    while ((n = fread(buf, 1, BUFSZ, f)) > 0) sha256_update(&c, buf, n);
    bad = ferror(f);
    fclose(f);
    free(buf);
    if (bad) return -1;
    if (bytes) *bytes = (long long)c.bytes;
    sha256_final(&c, d);
    sha256_hex(d, hex);
    return 0;
}

/* ---- digest of a folder ----
 * An optical medium is a folder of files (CD01/). Its digest is that of
 * the text "digest  path\n" for each file, like a BagIt manifest line,
 * in path order: it changes if a file changes, goes missing or is
 * added. */

typedef struct { char **rel; int n, cap, bad; } rel_list;

static void add_rel(void *c, const char *rel)
{
    rel_list *l = (rel_list *)c; size_t len = strlen(rel) + 1;
    if (l->n == l->cap) {
        char **p;
        int cap = l->cap ? l->cap * 2 : 32;
        p = (char **)realloc(l->rel, (size_t)cap * sizeof(char *));
        if (!p) { l->bad = 1; return; }
        l->rel = p; l->cap = cap;
    }
    if ((l->rel[l->n] = (char *)malloc(len)) != NULL) memcpy(l->rel[l->n++], rel, len);
    else l->bad = 1;
}

static int by_rel(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void sha256_tree_line(sha256_ctx *c, const char *hex, const char *rel)
{
    sha256_update(c, hex, 64);
    sha256_update(c, "  ", 2);
    sha256_update(c, rel, strlen(rel));
    sha256_update(c, "\n", 1);
}

int sha256_path(const char *path, char hex[65], long long *bytes)
{
    rel_list l = { NULL, 0, 0, 0 };
    sha256_ctx c; unsigned char d[32]; char one[65], full[1024];
    long long total = 0, n; int i, rc = 0;

    if (!u8_isdir(path)) return sha256_file(path, hex, bytes);
    if (u8_walk(path, add_rel, &l)) return -1;
    if (l.n) qsort(l.rel, (size_t)l.n, sizeof(char *), by_rel);
    sha256_init(&c);
    for (i = 0; i < l.n; i++) {
        u8_join(full, sizeof(full), path, l.rel[i]);
        if (!rc && sha256_file(full, one, &n) == 0) { total += n; sha256_tree_line(&c, one, l.rel[i]); }
        else rc = -1;
        free(l.rel[i]);
    }
    free(l.rel);
    if (rc || l.bad) return -1;
    sha256_final(&c, d);
    sha256_hex(d, hex);
    if (bytes) *bytes = total;
    return 0;
}
