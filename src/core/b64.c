/* base64: see b64.h */
#include "b64.h"

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const void *data, size_t n, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i, k = 0;
    if (cap < B64_ENCODED_SIZE(n)) return 0;
    for (i = 0; i < n; i += 3) {
        unsigned long v = (unsigned long)p[i] << 16;
        if (i + 1 < n) v |= (unsigned long)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out[k++] = alphabet[(v >> 18) & 0x3f];
        out[k++] = alphabet[(v >> 12) & 0x3f];
        out[k++] = i + 1 < n ? alphabet[(v >> 6) & 0x3f] : '=';
        out[k++] = i + 2 < n ? alphabet[v & 0x3f] : '=';
    }
    out[k] = 0;
    return k;
}

/* the character's value, -1 if it is not one of the alphabet */
static int value_of(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

long b64_decode(const char *s, void *out, size_t cap)
{
    unsigned char *o = (unsigned char *)out;
    unsigned long v = 0;
    size_t k = 0;
    int have = 0, pad = 0;
    for (; *s; s++) {
        int d;
        if (is_blank(*s)) continue;
        if (*s == '=') { pad++; continue; }
        if (pad) return -1;                             /* nothing after the padding */
        if ((d = value_of(*s)) < 0) return -1;
        v = (v << 6) | (unsigned long)d;
        if (++have < 4) continue;
        if (k + 3 > cap) return -1;
        o[k++] = (unsigned char)(v >> 16);
        o[k++] = (unsigned char)(v >> 8);
        o[k++] = (unsigned char)v;
        v = 0;
        have = 0;
    }
    /* the last group: 2 characters are one byte, 3 are two; 1 is impossible */
    if (have == 1 || pad > 2 || (pad && (have + pad) != 4)) return -1;
    if (have == 2) {
        if (k + 1 > cap) return -1;
        o[k++] = (unsigned char)(v >> 4);
    } else if (have == 3) {
        if (k + 2 > cap) return -1;
        o[k++] = (unsigned char)(v >> 10);
        o[k++] = (unsigned char)(v >> 2);
    }
    return (long)k;
}
