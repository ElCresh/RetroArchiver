#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strbuf.h"

static void oom(void)
{
    fputs("out of memory\n", stderr);
    exit(3);
}

void sb_init(strbuf *b)
{
    b->cap = 256; b->len = 0;
    b->s = (char *)malloc(b->cap);
    if (!b->s) oom();
    b->s[0] = 0;
}

void sb_free(strbuf *b)
{
    free(b->s);
    b->s = NULL; b->len = b->cap = 0;
}

void sb_clear(strbuf *b) { b->len = 0; b->s[0] = 0; }

static void grow(strbuf *b, size_t need)
{
    char *p;
    if (b->len + need + 1 <= b->cap) return;
    while (b->len + need + 1 > b->cap) b->cap *= 2;
    p = (char *)realloc(b->s, b->cap);
    if (!p) oom();
    b->s = p;
}

void sb_vprintf(strbuf *b, const char *fmt, va_list ap)
{
    va_list cp; int n;
    va_copy(cp, ap);
    n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) return;
    grow(b, (size_t)n);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void sb_printf(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

void sb_puts(strbuf *b, const char *s)
{
    size_t n = strlen(s);
    grow(b, n);
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

void str_copy(char *dst, size_t n, const char *src)
{
    size_t k;
    if (!n) return;
    if (!src) src = "";
    k = strlen(src);
    if (k >= n) {
        k = n - 1;
        while (k > 0 && ((unsigned char)src[k] & 0xC0) == 0x80) k--;
    }
    memcpy(dst, src, k);
    dst[k] = 0;
}
