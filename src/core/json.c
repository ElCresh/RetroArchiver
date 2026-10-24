#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

/* ---- writing ---- */

static void indent(json_writer *w)
{
    int i;
    sb_puts(&w->out, "\n");
    for (i = 0; i < w->depth; i++) sb_puts(&w->out, "  ");
}

static void put_string(strbuf *o, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    sb_puts(o, "\"");
    for (; *p; p++) {
        if (*p == '"')        sb_puts(o, "\\\"");
        else if (*p == '\\')  sb_puts(o, "\\\\");
        else if (*p == '\n')  sb_puts(o, "\\n");
        else if (*p == '\r')  sb_puts(o, "\\r");
        else if (*p == '\t')  sb_puts(o, "\\t");
        else if (*p < 0x20)   sb_printf(o, "\\u%04x", *p);
        else { char c[2]; c[0] = (char)*p; c[1] = 0; sb_puts(o, c); }
    }
    sb_puts(o, "\"");
}

/* comma, newline and key before each element */
static void member(json_writer *w, const char *key)
{
    if (w->depth > 0) {
        if (w->count[w->depth]) sb_puts(&w->out, ",");
        w->count[w->depth]++;
        indent(w);
    }
    if (key) { put_string(&w->out, key); sb_puts(&w->out, ": "); }
}

void jw_init(json_writer *w) { memset(w, 0, sizeof(*w)); sb_init(&w->out); }
void jw_free(json_writer *w) { sb_free(&w->out); }

/* beyond the limit the whole subtree is dropped: while skipped > 0
 * nothing is written (no commas, no keys, no brackets), so the JSON
 * stays valid and overflow tells the caller that something was lost */
static void open_(json_writer *w, const char *key, const char *br)
{
    if (w->skipped > 0 || w->depth >= JSON_MAXDEPTH - 1) {
        w->overflow = 1;
        w->skipped++;
        return;
    }
    member(w, key);
    sb_puts(&w->out, br);
    w->depth++;
    w->count[w->depth] = 0;
}

static void close_(json_writer *w, const char *br)
{
    int had;
    if (w->skipped > 0) { w->skipped--; return; }
    had = w->count[w->depth];
    if (w->depth > 0) w->depth--;
    if (had) indent(w);
    sb_puts(&w->out, br);
    if (w->depth == 0) sb_puts(&w->out, "\n");
}

void jw_begin_obj(json_writer *w, const char *key) { open_(w, key, "{"); }
void jw_end_obj(json_writer *w)                    { close_(w, "}"); }
void jw_begin_arr(json_writer *w, const char *key) { open_(w, key, "["); }
void jw_end_arr(json_writer *w)                    { close_(w, "]"); }

void jw_str(json_writer *w, const char *key, const char *val)
{
    if (w->skipped > 0) return;
    member(w, key);
    if (val) put_string(&w->out, val); else sb_puts(&w->out, "null");
}

void jw_int(json_writer *w, const char *key, long long v)
{
    if (w->skipped > 0) return;
    member(w, key);
    sb_printf(&w->out, "%lld", v);
}

void jw_null(json_writer *w, const char *key)
{
    if (w->skipped > 0) return;
    member(w, key);
    sb_puts(&w->out, "null");
}

void jw_bool(json_writer *w, const char *key, int v)
{
    if (w->skipped > 0) return;
    member(w, key);
    sb_puts(&w->out, v ? "true" : "false");
}

/* whole numbers as integers, the others with all their digits */
static void put_number(json_writer *w, const char *key, double n)
{
    if (w->skipped > 0) return;
    if (n == (double)(long long)n) { jw_int(w, key, (long long)n); return; }
    member(w, key);
    sb_printf(&w->out, "%.17g", n);
}

void json_write_val(json_writer *w, const char *key, const jval *v)
{
    const jval *c;
    switch (v->t) {
    case J_NULL: jw_null(w, key); break;
    case J_BOOL: jw_bool(w, key, v->b); break;
    case J_NUM:  put_number(w, key, v->n); break;
    case J_STR:  jw_str(w, key, v->s); break;
    case J_ARR:
        jw_begin_arr(w, key);
        for (c = v->child; c; c = c->next) json_write_val(w, NULL, c);
        jw_end_arr(w);
        break;
    case J_OBJ:
        jw_begin_obj(w, key);
        for (c = v->child; c; c = c->next) json_write_val(w, c->key, c);
        jw_end_obj(w);
        break;
    }
}

/* ---- reading ---- */

typedef struct { const char *p; char *err; size_t errlen; } jp;

static void fail(jp *s, const char *what)
{
    if (s->err && s->errlen && !s->err[0])
        snprintf(s->err, s->errlen, "%s near \"%.20s\"", what, s->p);
}

static void ws(jp *s)
{
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') s->p++;
}

static jval *newv(jtype t)
{
    jval *v = (jval *)calloc(1, sizeof(jval));
    if (v) v->t = t;
    return v;
}

static void put_utf8(strbuf *b, unsigned long cp)
{
    char c[5] = { 0, 0, 0, 0, 0 };
    if (cp == 0) return;
    if (cp < 0x80) c[0] = (char)cp;
    else if (cp < 0x800) {
        c[0] = (char)(0xC0 | (cp >> 6)); c[1] = (char)(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        c[0] = (char)(0xE0 | (cp >> 12)); c[1] = (char)(0x80 | ((cp >> 6) & 63));
        c[2] = (char)(0x80 | (cp & 63));
    } else {
        c[0] = (char)(0xF0 | (cp >> 18)); c[1] = (char)(0x80 | ((cp >> 12) & 63));
        c[2] = (char)(0x80 | ((cp >> 6) & 63)); c[3] = (char)(0x80 | (cp & 63));
    }
    sb_puts(b, c);
}

static int hex4(const char *p, unsigned long *out)
{
    int i; unsigned long v = 0;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* s->p on the opening quote; the caller frees the result */
static char *pstring(jp *s)
{
    strbuf b;
    sb_init(&b);
    s->p++;
    while (*s->p && *s->p != '"') {
        if (*s->p != '\\') {
            char c[2]; c[0] = *s->p++; c[1] = 0;
            sb_puts(&b, c);
            continue;
        }
        s->p++;
        switch (*s->p) {
        case '"':  sb_puts(&b, "\""); break;
        case '\\': sb_puts(&b, "\\"); break;
        case '/':  sb_puts(&b, "/");  break;
        case 'b':  sb_puts(&b, "\b"); break;
        case 'f':  sb_puts(&b, "\f"); break;
        case 'n':  sb_puts(&b, "\n"); break;
        case 'r':  sb_puts(&b, "\r"); break;
        case 't':  sb_puts(&b, "\t"); break;
        case 'u': {
            unsigned long cp, lo;
            if (!hex4(s->p + 1, &cp)) { fail(s, "invalid \\u"); sb_free(&b); return NULL; }
            s->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && s->p[1] == '\\' && s->p[2] == 'u' &&
                hex4(s->p + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                s->p += 6;
            }
            put_utf8(&b, cp);
            break;
        }
        default:
            fail(s, "invalid escape"); sb_free(&b); return NULL;
        }
        s->p++;
    }
    if (*s->p != '"') { fail(s, "unterminated string"); sb_free(&b); return NULL; }
    s->p++;
    return b.s;
}

static jval *pvalue(jp *s, int depth);

static jval *pcontainer(jp *s, int depth, int obj)
{
    jval *v = newv(obj ? J_OBJ : J_ARR), *last = NULL, *e;
    char close = obj ? '}' : ']';

    if (!v) return NULL;
    s->p++;
    ws(s);
    if (*s->p == close) { s->p++; return v; }
    for (;;) {
        char *key = NULL;
        ws(s);
        if (obj) {
            if (*s->p != '"') { fail(s, "missing member name"); json_free(v); return NULL; }
            key = pstring(s);
            if (!key) { json_free(v); return NULL; }
            ws(s);
            if (*s->p != ':') { fail(s, "missing colon"); free(key); json_free(v); return NULL; }
            s->p++;
        }
        e = pvalue(s, depth + 1);
        if (!e) { free(key); json_free(v); return NULL; }
        e->key = key;
        if (last) last->next = e; else v->child = e;
        last = e;
        ws(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == close) { s->p++; return v; }
        fail(s, obj ? "missing , or }" : "missing , or ]");
        json_free(v);
        return NULL;
    }
}

static jval *pvalue(jp *s, int depth)
{
    jval *v;
    ws(s);
    if (depth > JSON_MAXDEPTH) { fail(s, "nesting too deep"); return NULL; }
    switch (*s->p) {
    case '{': return pcontainer(s, depth, 1);
    case '[': return pcontainer(s, depth, 0);
    case '"': {
        char *str = pstring(s);
        if (!str) return NULL;
        v = newv(J_STR);
        if (v) v->s = str; else free(str);
        return v;
    }
    case 't':
        if (!strncmp(s->p, "true", 4)) { s->p += 4; v = newv(J_BOOL); if (v) v->b = 1; return v; }
        break;
    case 'f':
        if (!strncmp(s->p, "false", 5)) { s->p += 5; return newv(J_BOOL); }
        break;
    case 'n':
        if (!strncmp(s->p, "null", 4)) { s->p += 4; return newv(J_NULL); }
        break;
    default:
        if (*s->p == '-' || (*s->p >= '0' && *s->p <= '9')) {
            char *end; double d = strtod(s->p, &end);
            if (end == s->p) break;
            s->p = end;
            v = newv(J_NUM);
            if (v) v->n = d;
            return v;
        }
    }
    fail(s, "invalid value");
    return NULL;
}

jval *json_parse(const char *text, char *err, size_t errlen)
{
    jp s; jval *v;
    s.p = text; s.err = err; s.errlen = errlen;
    if (err && errlen) err[0] = 0;
    v = pvalue(&s, 0);
    if (!v) return NULL;
    ws(&s);
    if (*s.p) { fail(&s, "extra text after the JSON"); json_free(v); return NULL; }
    return v;
}

void json_free(jval *v)
{
    while (v) {
        jval *n = v->next;
        json_free(v->child);
        free(v->key); free(v->s); free(v);
        v = n;
    }
}

const jval *json_get(const jval *obj, const char *key)
{
    const jval *e;
    if (!obj || obj->t != J_OBJ) return NULL;
    for (e = obj->child; e; e = e->next)
        if (e->key && !strcmp(e->key, key)) return e;
    return NULL;
}

const char *json_str(const jval *obj, const char *key)
{
    const jval *e = json_get(obj, key);
    return (e && e->t == J_STR) ? e->s : NULL;
}

long long json_int(const jval *obj, const char *key, long long def)
{
    const jval *e = json_get(obj, key);
    if (!e || e->t != J_NUM) return def;
    /* the cast is undefined out of range: saturate at the exact limits */
    if (e->n != e->n) return def;                       /* NaN */
    if (e->n >= 9223372036854775808.0) return LLONG_MAX;
    if (e->n < -9223372036854775808.0) return LLONG_MIN;
    return (long long)e->n;
}
