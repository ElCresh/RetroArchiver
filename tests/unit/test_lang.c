/* The window's languages (src/gui/lang.c): the file format, the English
 * fallback, and the real files in lang/: same keys as the code, same
 * conversions as the English text, en.lng equal to the compiled one. */
#include "unit.h"
#include "gui/lang.h"
#include "core/messages.h"
#include <stdlib.h>
#include <wchar.h>

#define LANG_FILES "../lang/"

static int wsame(const wchar_t *a, const wchar_t *b) { return a && b && !wcscmp(a, b); }

static void load_str(const char *s) { lang_load_text(s, strlen(s), NULL); }

static void report_line(const char *file, const lang_report *r)
{
    if (r->unknown || r->missing || r->badfmt || r->badline)
        printf("  %s: %d unknown, %d missing, %d bad formats, %d bad lines; first: %s\n",
               file, r->unknown, r->missing, r->badfmt, r->badline, r->first);
}

static void test_table(void)
{
    int i, j, dup = 0, badref = 0;
    for (i = 0; i < T_COUNT; i++) {
        for (j = i + 1; j < T_COUNT; j++) if (!strcmp(lang_key(i), lang_key(j))) dup++;
        /* the compiled texts themselves: only allowed conversions */
        if (!lang_same_format(lang_builtin(i), lang_builtin(i))) badref++;
    }
    CHECK(dup == 0);
    CHECK(badref == 0);
    CHECK(lang_find("button.read") == T_BTN_READ);
    CHECK(lang_find("no.such.key") < 0);
    CHECK(wsame(tr(-1), L""));
    CHECK(wsame(tr(T_COUNT), L""));
}

static void test_formats(void)
{
    CHECK(lang_same_format(L"Leggo %ls fra %d secondi", L"Reading %ls in %d seconds"));
    CHECK(lang_same_format(L"%d%%, ancora %lu s", L"%d%%, %lu s left"));
    CHECK(lang_same_format(L"seriale %04lX-%04lX", L"serial %08lX-%lX"));     /* width is free */
    CHECK(lang_same_format(L"del %.10ls", L"of %ls"));
    CHECK(!lang_same_format(L"%d %ls", L"%ls %d"));             /* order */
    CHECK(!lang_same_format(L"%ls", L"%ls %d"));                /* one less */
    CHECK(!lang_same_format(L"%ls %d %d", L"%ls %d"));          /* one more */
    CHECK(!lang_same_format(L"%s", L"%ls"));                    /* narrow string */
    CHECK(!lang_same_format(L"%n%ls", L"%ls"));
    CHECK(!lang_same_format(L"%*d", L"%d"));
    CHECK(!lang_same_format(L"100%", L"100%%"));                /* a lone % */
    CHECK(!lang_same_format(L"%1$ls", L"%ls"));                 /* no positional arguments */
}

static void test_parse(void)
{
    static const char text[] =
        "\xEF\xBB\xBF# a comment\r\n"
        "\r\n"
        "   # an indented comment\n"
        "button.read = Leggi\r\n"
        "button.skip=Salta\\sora\\s\n"
        "setup.files=\\s\xC2\xB7 file\\s\n"
        "samdisk.fix=%ls.\\n\\nC:\\\\SAMdisk\\\\samdisk.exe \\q\n"
        "auto.count.many=Leggo %d fra %ls\n"             /* swapped: refused */
        "bar.cylinder=Cilindro %d di %d\n"
        "bar.cylinder=Cil. %d/%d\n"                      /* the last one wins */
        "no.such.key=x\n"
        "a line without equals\n"
        "=empty key\n"
        "language.name=Fran\xC3\xA7" "ais \xF0\x9F\x92\xBE\n"
        "kind.mixed=bad \xFF byte";                     /* no final newline */
    lang_report r;
    CHECK(lang_load_text(text, sizeof(text) - 1, &r) == 0);
    CHECK(wsame(tr(T_BTN_READ), L"Leggi"));              /* blanks around key and text go */
    CHECK(wsame(tr(T_BTN_SKIP), L"Salta ora "));
    CHECK(wsame(tr(T_FILES_PREFIX), L" \x00b7 file "));
    CHECK(wsame(tr(T_SD_FIX), L"%ls.\n\nC:\\SAMdisk\\samdisk.exe \\q"));
    CHECK(wsame(tr(T_AUTO_COUNT_MANY), lang_builtin(T_AUTO_COUNT_MANY)));
    CHECK(wsame(tr(T_BAR_CYL), L"Cil. %d/%d"));
    CHECK(wsame(tr(T_BTN_STOP), L"Stop"));               /* missing: English */
    CHECK(wcslen(tr(T_LANG_NAME)) == (sizeof(wchar_t) == 2 ? 11 : 10));   /* the floppy emoji */
    CHECK(wsame(tr(T_KIND_MIXED), L"bad \xFFFD byte"));
    CHECK(r.unknown == 1);
    CHECK(r.badfmt == 1);
    CHECK(r.badline == 2);
    CHECK(r.lines == 10);
    CHECK(r.missing == T_COUNT - 8);
    CHECK(r.first[0] != 0);

    lang_reset();
    CHECK(wsame(tr(T_BTN_READ), L"Read"));
    /* a file that cannot be read changes nothing */
    load_str("button.read=Leggi\n");
    CHECK(lang_load_file(LANG_FILES "no-such-language.lng", &r) == -1);
    CHECK(wsame(tr(T_BTN_READ), L"Leggi"));
    lang_reset();
}

static void test_numbers(void)
{
    wchar_t w[64];
    lang_reset();
    lang_number(58.94, 1, w, 64);
    CHECK(wsame(w, L"58.9"));
    lang_date("2026-10-14T21:20:11", w, 64);
    CHECK(wsame(w, L"2026-10-14"));
    lang_date("2026", w, 64);
    CHECK(wsame(w, L""));
    load_str("format.decimal=,\nformat.date=DD/MM/YYYY\n");
    lang_number(58.94, 1, w, 64);
    CHECK(wsame(w, L"58,9"));
    lang_date("2026-10-14T21:20:11", w, 64);
    CHECK(wsame(w, L"14/10/2026"));
    lang_reset();
}

/* every file in lang/: all the keys, no others, the right conversions */
static void test_files(void)
{
    static const char *const files[] = { "en.lng", "it.lng", NULL };
    char path[256];
    lang_report r;
    wchar_t name[64];
    int i, k, diff = 0;
    for (k = 0; files[k]; k++) {
        snprintf(path, sizeof(path), "%s%s", LANG_FILES, files[k]);
        CHECK(lang_load_file(path, &r) == 0);
        report_line(files[k], &r);
        CHECK(r.unknown == 0);
        CHECK(r.missing == 0);
        CHECK(r.badfmt == 0);
        CHECK(r.badline == 0);
        CHECK(r.lines == T_COUNT);                   /* no key twice */
        if (k == 0)                                  /* en.lng is the compiled English */
            for (i = 0; i < T_COUNT; i++)
                if (!wsame(tr(i), lang_builtin(i))) {
                    if (!diff++) printf("  en.lng differs from lang_en.h at %s\n", lang_key(i));
                }
    }
    CHECK(diff == 0);
    CHECK(wsame(tr(T_LANG_NAME), L"Italiano"));
    CHECK(wsame(tr(T_BTN_READ), L"Leggi"));
    CHECK(wsame(tr(T_SETUP_HINT), L"Per continuare un cofanetto lasciato a met\x00e0: Riprendi..., "
                                  L"oppure lo stesso titolo e la stessa versione."));
    lang_reset();
    CHECK(lang_peek_file(LANG_FILES "it.lng", "language.name", name, 64) == 1);
    CHECK(wsame(name, L"Italiano"));
    CHECK(lang_peek_file(LANG_FILES "it.lng", "no.such.key", name, 64) == 0);
    CHECK(wsame(tr(T_LANG_NAME), L"English"));      /* peeking loads nothing */
}

/* ---- the core's messages ---- */

static const char *const core_fmts[] = {
#define MSG(m, key, kinds) m,
#include "core/messages_list.h"
#undef MSG
};

/* fmt with sample arguments: strings "a<k>z" (k from 1), numbers 7<k> */
static void sample(const char *fmt, char *out, size_t n)
{
    size_t o = 0; int k = 0; const char *p = fmt;
    while (*p && o + 16 < n) {
        if (*p != '%') { out[o++] = *p++; continue; }
        p++;
        if (*p == '%') { out[o++] = '%'; p++; continue; }
        while (*p && !strchr("sdiuxc", *p)) p++;
        k++;
        if (*p == 's') o += (size_t)snprintf(out + o, n - o, "a%dz", k);
        else o += (size_t)snprintf(out + o, n - o, "7%d", k);
        if (*p) p++;
    }
    out[o] = 0;
}

static void test_core(void)
{
    char text[1200]; wchar_t w[1200], expect[1200];
    int i, n = (int)(sizeof(core_fmts) / sizeof(core_fmts[0])), bad = 0;
    CHECK(T_COUNT - T_GUI_COUNT == n);
    lang_reset();
    /* every message, formatted and recognized again: the same message */
    for (i = 0; i < n; i++) {
        sample(core_fmts[i], text, sizeof(text));
        if (lang_core_match(text) != T_GUI_COUNT + i) {
            bad++;
            printf("  core message %s not recognized as itself: %s\n", lang_key(T_GUI_COUNT + i), text);
        }
    }
    CHECK(bad == 0);
    /* in English the text comes back as it was */
    sample(M_SES_CLOSED, text, sizeof(text));
    CHECK(lang_core_text(text, w, 1200) == 1);
    mbstowcs(expect, text, 1200);
    CHECK(wsame(w, expect));
    /* a title with ": " and ". " inside does not break it */
    CHECK(lang_core_match("The box \"A: b. c\" is already closed in X: y: choose another title, "
                          "or tell it apart with the edition.") == T_M_SES_CLOSED);
    /* the first letter made a capital by the session still matches */
    CHECK(lang_core_match("Cannot read back x.dsk, just written: check the destination disk.") == T_M_SES_READ_BACK);
    /* the most specific format wins over a shorter one that also matches */
    CHECK(lang_core_match("cannot write /a/b: is the destination disk full or write-protected?") == T_M_META_WRITE);
    /* a reason with ": " inside "Cannot read the CD: %s.": the reason is the message */
    lang_load_file("../lang/it.lng", NULL);
    CHECK(lang_core_text("Cannot read the CD: C:/RA has 10 MB free and the read needs about 800: free some "
                         "space or choose another archive folder.", w, 1200) == 1);
    CHECK(wsame(w, L"Non riesco a leggere il CD: C:/RA ha 10 MB liberi e la lettura ne chiede circa 800: "
                   L"libera dello spazio o scegli un'altra cartella dell'archivio."));
    /* a title is the user's: never translated, even if it looks like a message */
    CHECK(lang_core_text("The box \"Model Railway\" is already closed in C:/RA/Model Railway: choose another "
                         "title, or tell it apart with the edition.", w, 1200) == 1);
    CHECK(wcsstr(w, L"“Model Railway”") != NULL);
    /* the parse that explains more of the text wins: a message inside a message */
    CHECK(lang_core_match("the box cannot be read: X cannot be read (Y): if the box is damaged, move its "
                          "folder away and start over") == T_M_EDIT_UNREADABLE);
    lang_reset();
    /* only the conversions the matcher knows, in the core's formats */
    for (i = 0; i < n; i++) {
        const char *p = core_fmts[i];
        while ((p = strchr(p, '%')) != NULL) {
            p++;
            if (*p == '%') { p++; continue; }
            while (*p && strchr("0123456789.l", *p)) p++;
            if (*p != 's' && *p != 'd') { printf("  %s: conversion %%%c not allowed\n", lang_key(T_GUI_COUNT + i), *p); bad++; }
        }
    }
    CHECK(bad == 0);
    /* not a message: copied as it is, 0 */
    CHECK(lang_core_text("Something else entirely", w, 1200) == 0 && wsame(w, L"Something else entirely"));
    /* translated, with the arguments that are messages translated too */
    load_str("core.ses.stays.err=%ls. %ls resta com'era.\n"
             "core.bag.missing=manca data/%ls: rileggi %ls\n"
             "core.medium.floppy=dischetto %ls\n");
    CHECK(lang_core_text("data/x.dsk is missing: read floppy 2 again. floppy 2 stays as it was.", w, 1200) == 1);
    CHECK(wsame(w, L"manca data/x.dsk: rileggi dischetto 2. dischetto 2 resta com'era."));
    lang_reset();
}

void test_lang(void)
{
    test_core();
    test_table();
    test_formats();
    test_parse();
    test_numbers();
    test_files();
}
