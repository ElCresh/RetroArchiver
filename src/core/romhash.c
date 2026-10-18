#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "romhash.h"
#include "fsu8.h"

/* ---- CRC32 (IEEE 802.3, the one of zip and of the .dat) ---- */

static uint32_t crc_table[256];

static void crc_init_table(void)
{
    uint32_t i, k, c;
    if (crc_table[1]) return;
    for (i = 0; i < 256; i++) {
        for (c = i, k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

/* ---- MD5 (RFC 1321) ---- */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const unsigned char MD5_R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void md5_block(uint32_t h[4], const unsigned char *p)
{
    uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3], f, t;
    int i, g;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 |
               (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    for (i = 0; i < 64; i++) {
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
        else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }
        t = d; d = c; c = b;
        b = b + ROL(a + f + MD5_K[i] + w[g], MD5_R[i]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

/* ---- SHA-1 (FIPS 180-4) ---- */

static void sha1_block(uint32_t h[5], const unsigned char *p)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                   k = 0xca62c1d6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

/* ---- the three together ---- */

void romhash_init(romhash_ctx *c)
{
    static const uint32_t md5_iv[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    static const uint32_t sha1_iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    crc_init_table();
    c->crc = 0xFFFFFFFFu;
    memcpy(c->md5, md5_iv, sizeof(md5_iv));
    memcpy(c->sha1, sha1_iv, sizeof(sha1_iv));
    c->bytes = 0; c->n = 0;
}

static void blocks(romhash_ctx *c, const unsigned char *p)
{
    md5_block(c->md5, p);
    sha1_block(c->sha1, p);
}

void romhash_update(romhash_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < len; i++) c->crc = crc_table[(c->crc ^ p[i]) & 0xFF] ^ (c->crc >> 8);
    c->bytes += len;
    while (len) {
        size_t k = 64 - c->n;
        if (k > len) k = len;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; len -= k;
        if (c->n == 64) { blocks(c, c->buf); c->n = 0; }
    }
}

void romhash_final(romhash_ctx *c, romhash *out)
{
    uint64_t bits = c->bytes * 8;
    int i;
    out->size = (long long)c->bytes;
    sprintf(out->crc, "%08lx", (unsigned long)(c->crc ^ 0xFFFFFFFFu));
    /* the same padding for both, the length little endian for MD5 and big endian for SHA-1 */
    c->buf[c->n++] = 0x80;
    if (c->n > 56) {
        memset(c->buf + c->n, 0, 64 - c->n);
        blocks(c, c->buf);
        c->n = 0;
    }
    memset(c->buf + c->n, 0, 56 - c->n);
    {
        unsigned char tail[64];
        memcpy(tail, c->buf, 56);
        for (i = 0; i < 8; i++) tail[56 + i] = (unsigned char)(bits >> (8 * i));
        md5_block(c->md5, tail);
        for (i = 0; i < 8; i++) tail[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
        sha1_block(c->sha1, tail);
    }
    for (i = 0; i < 16; i++)
        sprintf(out->md5 + 2 * i, "%02x", (unsigned)(c->md5[i / 4] >> (8 * (i % 4))) & 0xFF);
    for (i = 0; i < 20; i++)
        sprintf(out->sha1 + 2 * i, "%02x", (unsigned)(c->sha1[i / 4] >> (24 - 8 * (i % 4))) & 0xFF);
}

int romhash_file(const char *path, romhash *out)
{
    FILE *f = u8_fopen(path, "rb");
    unsigned char *buf; size_t n; romhash_ctx c;
    if (!f) return -1;
    if (!(buf = (unsigned char *)malloc(1 << 20))) { fclose(f); return -1; }
    romhash_init(&c);
    while ((n = fread(buf, 1, 1 << 20, f)) > 0) romhash_update(&c, buf, n);
    n = ferror(f);
    fclose(f);
    free(buf);
    if (n) return -1;
    romhash_final(&c, out);
    return 0;
}
