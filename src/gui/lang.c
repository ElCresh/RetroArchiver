/* The window's texts in several languages: see lang.h */
#include "gui/lang.h"
#include "core/fsu8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LANG_MAXFILE (1024 * 1024)   /* a language file is a few dozen KB */
#define LANG_MAXKEY  128

static const char *const keys[T_COUNT] = {
#define T(id, key, text) key,
#include "gui/lang_en.h"
#undef T
};

static const wchar_t *const builtin[T_COUNT] = {
#define T(id, key, text) text,
#include "gui/lang_en.h"
#undef T
};

static wchar_t *loaded[T_COUNT];     /* NULL: the English one */

const wchar_t *tr(int id)
{
    if (id < 0 || id >= T_COUNT) return L"";
    return loaded[id] ? loaded[id] : builtin[id];
}

const char *lang_key(int id)
{
    return id >= 0 && id < T_COUNT ? keys[id] : "";
}

const wchar_t *lang_builtin(int id)
{
    return id >= 0 && id < T_COUNT ? builtin[id] : L"";
}

int lang_find(const char *key)
{
    int i;
    for (i = 0; i < T_COUNT; i++) if (!strcmp(keys[i], key)) return i;
    return -1;
}

void lang_reset(void)
{
    int i;
    for (i = 0; i < T_COUNT; i++) { free(loaded[i]); loaded[i] = NULL; }
}

/* ---- printf conversions ---- */

/* the next conversion after *p, its type ("ls", "d", "04lX" -> "lX") in sig:
 * 1 found, 0 none left, -1 one that is not allowed (%n, %*d, a lone %) */
static int next_conv(const wchar_t **p, char sig[8])
{
    const wchar_t *s = *p;
    int k = 0;
    for (;;) {
        while (*s && *s != L'%') s++;
        if (!*s) { *p = s; return 0; }
        s++;
        if (*s == L'%') { s++; continue; }
        break;
    }
    while (*s && wcschr(L"-+ #0", *s)) s++;
    while (*s >= L'0' && *s <= L'9') s++;
    if (*s == L'.') { s++; while (*s >= L'0' && *s <= L'9') s++; }
    while (*s && wcschr(L"hlLjzt", *s) && k < 3) sig[k++] = (char)*s++;
    if (!*s || !wcschr(L"diouxXcsfFeEgGaAp", *s)) { *p = s; return -1; }
    sig[k++] = (char)*s++;
    sig[k] = 0;
    *p = s;
    return 1;
}

int lang_same_format(const wchar_t *fmt, const wchar_t *ref)
{
    char a[8], b[8];
    int ra, rb;
    for (;;) {
        ra = next_conv(&fmt, a);
        rb = next_conv(&ref, b);
        if (ra < 0 || rb < 0 || ra != rb) return 0;
        if (!ra) return 1;
        if (strcmp(a, b)) return 0;
    }
}

/* ---- UTF-8 ---- */

/* one code point from s (at most n bytes); U+FFFD for a broken sequence */
static unsigned long u8_next(const unsigned char **s, const unsigned char *end)
{
    const unsigned char *p = *s;
    unsigned long c = *p++;
    int more = c < 0x80 ? 0 : c < 0xC2 ? -1 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : c < 0xF5 ? 3 : -1;
    if (more < 0) { *s = p; return 0xFFFD; }
    if (more) c &= 0x3F >> more;
    while (more--) {
        if (p >= end || (*p & 0xC0) != 0x80) { *s = p; return 0xFFFD; }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *s = p;
    return c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) ? 0xFFFD : c;
}

/* UTF-8 bytes to wchar_t, into out of n: UTF-16 on Windows (surrogate
 * pairs), UTF-32 elsewhere. Returns the characters written */
static size_t u8_to_wide(const char *s, size_t len, wchar_t *out, size_t n)
{
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    size_t k = 0;
    if (!n) return 0;
    while (p < end) {
        unsigned long c = u8_next(&p, end);
        if (sizeof(wchar_t) == 2 && c > 0xFFFF) {
            if (k + 2 >= n) break;
            c -= 0x10000;
            out[k++] = (wchar_t)(0xD800 + (c >> 10));
            out[k++] = (wchar_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (k + 1 >= n) break;
            out[k++] = (wchar_t)c;
        }
    }
    out[k] = 0;
    return k;
}

/* ---- the file ---- */

/* \n \t \s \\ in place; any other backslash stays. Returns the new length */
static size_t unescape(char *s, size_t len)
{
    size_t i, k = 0;
    for (i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            char c = s[i + 1];
            if (c == 'n' || c == 't' || c == 's' || c == '\\') {
                s[k++] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 's' ? ' ' : '\\';
                i++;
                continue;
            }
        }
        s[k++] = s[i];
    }
    return k;
}

/* the text, unescaped and as wchar_t: malloc'd */
static wchar_t *value_w(const char *v, size_t len)
{
    char *t = (char *)malloc(len + 1);
    wchar_t *w;
    if (!t) return NULL;
    memcpy(t, v, len);
    len = unescape(t, len);
    if ((w = (wchar_t *)malloc((len + 1) * sizeof(wchar_t))) != NULL) u8_to_wide(t, len, w, len + 1);
    free(t);
    return w;
}

typedef int (*line_fn)(void *ctx, const char *key, const char *val, size_t vlen);

/* calls fn for each key=text line; a line without '=' or with an empty
 * key gets key NULL. Stops if fn returns nonzero */
static void each_line(const char *text, size_t len, line_fn fn, void *ctx)
{
    const char *p = text, *end = text + len, *nl, *eq, *a, *b;
    char key[LANG_MAXKEY];
    if (len >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
    for (; p < end; p = nl + 1) {
        size_t n = 0;
        if (!(nl = memchr(p, '\n', (size_t)(end - p)))) nl = end;
        b = nl;
        if (b > p && b[-1] == '\r') b--;
        for (a = p; a < b && (*a == ' ' || *a == '\t'); a++) ;
        if (a == b || *a == '#') continue;
        eq = memchr(a, '=', (size_t)(b - a));
        if (eq) {
            const char *k = eq;
            while (k > a && (k[-1] == ' ' || k[-1] == '\t')) k--;
            n = (size_t)(k - a);
        }
        if (!eq || !n || n >= sizeof(key)) {
            if (fn(ctx, NULL, a, (size_t)(b - a))) return;
            continue;
        }
        memcpy(key, a, n);
        key[n] = 0;
        /* blanks around the text go: \s keeps a wanted one */
        for (a = eq + 1; a < b && (*a == ' ' || *a == '\t'); a++) ;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if (fn(ctx, key, a, (size_t)(b - a))) return;
    }
}

typedef struct { lang_report r; char seen[T_COUNT]; } load_ctx;

static void problem(lang_report *r, const char *what, const char *key)
{
    if (!r->first[0]) snprintf(r->first, sizeof(r->first), "%s: %s", what, key);
}

static int load_line(void *ctx, const char *key, const char *val, size_t vlen)
{
    load_ctx *c = (load_ctx *)ctx;
    wchar_t *w;
    int id;
    if (!key) {
        c->r.badline++;
        if (!c->r.first[0]) snprintf(c->r.first, sizeof(c->r.first), "line without key=text: %.*s",
                                     (int)(vlen > 60 ? 60 : vlen), val);
        return 0;
    }
    c->r.lines++;
    if ((id = lang_find(key)) < 0) { c->r.unknown++; problem(&c->r, "unknown key", key); return 0; }
    c->seen[id] = 1;
    if (!(w = value_w(val, vlen))) return 0;
    /* a wrong conversion could crash the window: the English text stays */
    if (!lang_same_format(w, builtin[id])) {
        c->r.badfmt++;
        problem(&c->r, "conversions differ from the English text", key);
        free(w);
        return 0;
    }
    free(loaded[id]);
    loaded[id] = w;
    return 0;
}

int lang_load_text(const char *text, size_t len, lang_report *rep)
{
    load_ctx *c = (load_ctx *)calloc(1, sizeof(load_ctx));
    int i;
    if (!c) return -1;
    lang_reset();
    each_line(text, len, load_line, c);
    for (i = 0; i < T_COUNT; i++)
        if (!c->seen[i]) { c->r.missing++; problem(&c->r, "missing key", keys[i]); }
    if (rep) *rep = c->r;
    free(c);
    return 0;
}

/* the whole file in memory: malloc'd, NULL if it cannot be read */
static char *read_all(const char *path, size_t *len)
{
    FILE *f = u8_fopen(path, "rb");
    char *buf = NULL;
    long sz;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0 && sz <= LANG_MAXFILE &&
        fseek(f, 0, SEEK_SET) == 0 && (buf = (char *)malloc((size_t)sz + 1)) != NULL) {
        *len = fread(buf, 1, (size_t)sz, f);
        buf[*len] = 0;
    }
    fclose(f);
    return buf;
}

int lang_load_file(const char *path, lang_report *rep)
{
    size_t len = 0;
    char *buf = read_all(path, &len);
    int rc;
    if (!buf) return -1;
    rc = lang_load_text(buf, len, rep);
    free(buf);
    return rc;
}

typedef struct { const char *key; wchar_t *out; size_t n; int found; } peek_ctx;

static int peek_line(void *ctx, const char *key, const char *val, size_t vlen)
{
    peek_ctx *c = (peek_ctx *)ctx;
    wchar_t *w;
    if (!key || strcmp(key, c->key) || !(w = value_w(val, vlen))) return 0;
    wcsncpy(c->out, w, c->n - 1);
    c->out[c->n - 1] = 0;
    free(w);
    c->found = 1;
    return 0;                       /* on: the last one wins, as when loading */
}

int lang_peek_file(const char *path, const char *key, wchar_t *out, size_t n)
{
    peek_ctx c;
    size_t len = 0;
    char *buf;
    if (!n) return 0;
    out[0] = 0;
    if (!(buf = read_all(path, &len))) return 0;
    c.key = key; c.out = out; c.n = n; c.found = 0;
    each_line(buf, len, peek_line, &c);
    free(buf);
    return c.found;
}

/* ---- numbers and dates ---- */

void lang_number(double v, int decimals, wchar_t *out, size_t n)
{
    const wchar_t *sep = tr(T_FMT_DECIMAL);
    wchar_t *p;
    if (!n) return;
    swprintf(out, n, L"%.*f", decimals, v);
    out[n - 1] = 0;
    if (sep[0] && (p = wcschr(out, L'.')) != NULL) *p = sep[0];
}

void lang_date(const char *iso, wchar_t *out, size_t n)
{
    const wchar_t *f = tr(T_FMT_DATE);
    size_t k = 0;
    if (!n) return;
    out[0] = 0;
    if (strlen(iso) < 10) return;
    while (*f && k + 4 < n) {
        if (!wcsncmp(f, L"YYYY", 4)) {
            int i;
            for (i = 0; i < 4; i++) out[k++] = (wchar_t)(unsigned char)iso[i];
            f += 4;
        } else if (!wcsncmp(f, L"MM", 2) || !wcsncmp(f, L"DD", 2)) {
            int at = *f == L'M' ? 5 : 8;
            out[k++] = (wchar_t)(unsigned char)iso[at];
            out[k++] = (wchar_t)(unsigned char)iso[at + 1];
            f += 2;
        } else {
            out[k++] = *f++;
        }
    }
    out[k] = 0;
}
