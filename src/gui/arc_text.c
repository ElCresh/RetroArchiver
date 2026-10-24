/* The archive's texts about a box, from tr(): see arc_text.h */
#include "gui/arc_text.h"
#include "gui/lang.h"
#include <stdio.h>
#include <string.h>
#include <wctype.h>

#define MAXW 1200

/* a text of the language with its arguments, all given as strings: each
 * %ls or %d takes the next one (the language files keep the English
 * conversions, lang_same_format sees to it) */
static void put(wchar_t *out, size_t n, const wchar_t *f, const wchar_t *const *arg, int nargs)
{
    size_t o = wcslen(out); int k = 0;
    for (; *f && o + 1 < n; f++) {
        if (f[0] == L'%' && f[1] == L'%') { out[o++] = L'%'; f++; continue; }
        if (f[0] == L'%') {
            const wchar_t *a = k < nargs ? arg[k] : L"";
            while (f[1] && !wcschr(L"sdiuxc", f[1])) f++;
            if (f[1]) f++;
            k++;
            while (*a && o + 1 < n) out[o++] = *a++;
            continue;
        }
        out[o++] = *f;
    }
    out[o] = 0;
}

static void put0(wchar_t *out, size_t n, int id) { put(out, n, tr(id), NULL, 0); }
static void put1(wchar_t *out, size_t n, int id, const wchar_t *a) { put(out, n, tr(id), &a, 1); }
static void put2(wchar_t *out, size_t n, int id, const wchar_t *a, const wchar_t *b)
{
    const wchar_t *v[2]; v[0] = a; v[1] = b;
    put(out, n, tr(id), v, 2);
}
static void put3(wchar_t *out, size_t n, int id, const wchar_t *a, const wchar_t *b, const wchar_t *c)
{
    const wchar_t *v[3]; v[0] = a; v[1] = b; v[2] = c;
    put(out, n, tr(id), v, 3);
}

static void add(wchar_t *out, size_t n, const wchar_t *s)
{
    size_t o = wcslen(out);
    while (*s && o + 1 < n) out[o++] = *s++;
    out[o] = 0;
}

static void num(int v, wchar_t *out) { swprintf(out, 16, L"%d", v); }

static const int kind_one[MEDIA_KINDS] = { T_KIND_FLOPPY, T_KIND_CD, T_KIND_DVD };
static const int kind_many[MEDIA_KINDS] = { T_KIND_FLOPPY_PL, T_KIND_CD_PL, T_KIND_DVD_PL };

static const wchar_t *kind_word(int kind, int many)
{
    if (kind < 0 || kind >= MEDIA_KINDS) kind = MEDIA_FLOPPY;
    return tr(many ? kind_many[kind] : kind_one[kind]);
}

/* ---- the outcome ---- */

static void part(wchar_t *out, size_t n, int k, int one, int many)
{
    wchar_t v[16];
    if (!k) return;
    if (out[0]) put0(out, n, T_ARCT_SEP);
    num(k, v);
    put1(out, n, k == 1 ? one : many, v);
}

void arc_outcome_w(const arc_box *b, wchar_t *out, size_t n)
{
    wchar_t v[16];
    out[0] = 0;
    part(out, n, b->clean, T_ARCT_CLEAN_ONE, T_ARCT_CLEAN_MANY);
    if (b->redump) { num(b->redump, v); put1(out, n, b->redump == 1 ? T_ARCT_REDUMP_ONE : T_ARCT_REDUMP_MANY, v); }
    part(out, n, b->marginal, T_ARCT_MARGINAL_ONE, T_ARCT_MARGINAL_MANY);
    part(out, n, b->fragile, T_ARCT_FRAGILE_ONE, T_ARCT_FRAGILE_MANY);
    part(out, n, b->errors, T_ARCT_ERRORS_ONE, T_ARCT_ERRORS_MANY);
    if (!out[0]) put0(out, n, b->state == ARC_BROKEN ? T_ARCT_UNREADABLE : T_ARCT_NONE);
}

void arc_media_w(const arc_box *b, wchar_t *out, size_t n)
{
    wchar_t d[16], c[16]; int k;
    out[0] = 0;
    for (k = 0; k < MEDIA_KINDS; k++) {
        if (!b->count[k]) continue;
        if (out[0]) put0(out, n, T_ARCT_SEP);
        num(b->kind_archived[k], d); num(b->count[k], c);
        put3(out, n, T_ARCT_MEDIA, d, c, kind_word(k, 0));
    }
    /* without a composition (unreadable bag): just the numbers */
    if (!out[0] && b->expected) { num(b->archived, d); num(b->expected, c); put2(out, n, T_ARCT_MEDIA_N, d, c); }
}

/* ---- the state and what to do ---- */

/* "floppy 3", "floppy 3, 5" of the same kind, "floppy 3, CD 1" if not */
static int list_of(const arc_box *b, char ch, wchar_t *out, size_t n)
{
    int i, k = 0, kind, nr, first = -1, same = 1; wchar_t v[16], name[64];
    out[0] = 0;
    for (i = 0; b->disks[i]; i++) {
        if (b->disks[i] != ch) continue;
        arc_slot(b, i + 1, &kind, &nr);
        if (first < 0) first = kind;
        else if (kind != first) same = 0;
    }
    for (i = 0; b->disks[i]; i++) {
        if (b->disks[i] != ch) continue;
        arc_slot(b, i + 1, &kind, &nr);
        num(nr, v);
        if (k) put0(out, n, T_ARCT_SEP);
        if (!k || !same) {
            name[0] = 0;
            put2(name, 64, T_ARCT_MEDIUM, kind_word(kind, 0), v);
            add(out, n, name);
        } else {
            add(out, n, v);
        }
        k++;
    }
    return k;
}

/* the media of the composition: one kind's name, or "media" */
static const wchar_t *noun(const arc_box *b, int many)
{
    int k, kinds = 0, last = MEDIA_FLOPPY;
    for (k = 0; k < MEDIA_KINDS; k++) if (b->count[k]) { kinds++; last = k; }
    if (kinds > 1) return tr(many ? T_KIND_MIXED_PL : T_KIND_MIXED);
    return kind_word(last, many);
}

/* "3 floppies out of 3": the kinds with at least one medium archived */
static int done_parts(const arc_box *b, wchar_t *out, size_t n)
{
    int k, i, kind, nr, done[MEDIA_KINDS] = { 0 }, tot = 0, count[MEDIA_KINDS] = { 0 };
    wchar_t d[16], c[16];
    out[0] = 0;
    for (i = 0; b->disks[i]; i++) {
        arc_slot(b, i + 1, &kind, &nr);
        count[kind]++;
        if (b->disks[i] != '-') { done[kind]++; tot++; }
    }
    for (k = 0; k < MEDIA_KINDS; k++) {
        if (!done[k]) continue;
        if (out[0]) put0(out, n, T_ARCT_SEP);
        num(done[k], d); num(count[k], c);
        put3(out, n, T_ARCT_DONE_PART, d, kind_word(k, done[k] != 1), c);
    }
    return tot;
}

static void advice_open(const arc_box *b, wchar_t *out, size_t n)
{
    wchar_t p[MAXW], v[16], name[64]; int kind, nr;
    if (!b->first_todo) {
        num(b->expected, v);
        if (b->expected == 1) put0(out, n, T_ARCT_OPEN_ALL_ONE);
        else put2(out, n, T_ARCT_OPEN_ALL, v, noun(b, 1));
        return;
    }
    /* CDs and DVDs are read too: resume from the first one to do */
    if (!done_parts(b, p, MAXW)) put1(out, n, T_ARCT_OPEN_NONE, noun(b, 0));
    else put1(out, n, T_ARCT_OPEN_DONE, p);
    arc_slot(b, b->first_todo, &kind, &nr);
    num(nr, v);
    name[0] = 0;
    put2(name, 64, T_ARCT_MEDIUM, kind_word(kind, 0), v);
    put1(out, n, T_ARCT_RESUME_FROM, name);
}

/* the media with that outcome, as the subject of a sentence */
static int subject(const arc_box *b, char ch, wchar_t *out, size_t n)
{
    wchar_t l[MAXW]; int k = list_of(b, ch, l, MAXW);
    if (k) {
        l[0] = (wchar_t)towupper(l[0]);
        put1(out, n, T_ARCT_SUBJECT, l);
    }
    return k;
}

static int optical_once(const arc_box *b)
{
    int i;
    for (i = 0; b->disks[i]; i++) if (b->disks[i] == 'f' && (b->kinds[i] == 'c' || b->kinds[i] == 'd')) return 1;
    return 0;
}

static void advice_damage(const arc_box *b, wchar_t *out, size_t n)
{
    int k;
    if ((k = subject(b, 'e', out, n)) > 0) put0(out, n, k == 1 ? T_ARCT_DAMAGED_ONE : T_ARCT_DAMAGED_MANY);
    if ((k = subject(b, 'm', out, n)) > 0) put0(out, n, k == 1 ? T_ARCT_MARGINAL_IS : T_ARCT_MARGINAL_ARE);
    if ((k = subject(b, 'f', out, n)) > 0) {
        if (!optical_once(b)) put0(out, n, k == 1 ? T_ARCT_FRAGILE_IS : T_ARCT_FRAGILE_ARE);
        else put0(out, n, k == 1 ? T_ARCT_ONCE_ONE : T_ARCT_ONCE_MANY);
    }
}

void arc_advice_w(const arc_box *b, wchar_t *out, size_t n)
{
    wchar_t l[MAXW], e[MAXW]; int k;
    out[0] = 0;
    switch (b->state) {
    case ARC_OPEN:
        advice_open(b, out, n);
        break;
    case ARC_BROKEN:
        lang_core_text(b->error, e, MAXW);
        put1(out, n, T_ARCT_BROKEN, e);
        break;
    case ARC_INCOMPLETE:
        k = list_of(b, '-', l, MAXW);
        if (k) put1(out, n, k == 1 ? T_ARCT_INCOMPLETE_ONE : T_ARCT_INCOMPLETE_MANY, l);
        else put0(out, n, T_ARCT_INCOMPLETE);
        advice_damage(b, out, n);
        break;
    default:
        if (b->errors || b->marginal || b->fragile) {
            put0(out, n, T_ARCT_COMPLETE);
            advice_damage(b, out, n);
        } else {
            put1(out, n, T_ARCT_COMPLETE_ALL, noun(b, 1));
        }
    }
}
