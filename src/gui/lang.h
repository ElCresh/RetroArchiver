/* lang - the window's texts in several languages.
 *
 * Every text has a stable key ("btn.read") and an English text compiled
 * in (lang_en.h). A language is a UTF-8 file, lang\<code>.lng next to the
 * executable, with one "key=text" per line: what it does not have, or has
 * wrong, falls back to the compiled English, never to the key's name.
 *
 * File format:
 *   - UTF-8, the BOM is allowed; lines end with LF or CR LF;
 *   - blank lines and lines starting with '#' are ignored;
 *   - key=text: blanks around the key and the text are dropped;
 *   - escapes in the text: \n newline, \t tab, \s space (for a text that
 *     starts or ends with one), \\ backslash; any other backslash stays;
 *   - a key that appears twice: the last one wins.
 *
 * Many texts are printf formats (%ls, %d, %lu...). A translation must have
 * the same conversions in the same order as the English text: if not, it
 * is not used, so that a wrong file cannot crash the window. Arguments
 * cannot be reordered: if a language needs another order, rephrase.
 *
 * Portable C (the tests run with gcc too): the text is wchar_t.
 */
#ifndef RA_LANG_H
#define RA_LANG_H

#include <stddef.h>
#include <wchar.h>

/* the ids: T_BTN_READ... from lang_en.h, then T_M_... for the core's
 * messages (core/messages_list.h), whose English is their format with
 * every conversion made %ls */
enum {
#define T(id, key, text) id,
#include "gui/lang_en.h"
#undef T
    T_GUI_COUNT,
    T_CORE_BEFORE_ = T_GUI_COUNT - 1,
#define MSG(m, key, kinds) T_##m,
#include "core/messages_list.h"
#undef MSG
    T_COUNT
};

#define LANG_EXT_W     L".lng"
#define LANG_DIR_W     L"lang"   /* next to the executable */
#define LANG_BUILTIN_W L"en"     /* the language compiled in */

/* what a load found wrong in a file: the keys it ignored and those it lacks */
typedef struct {
    int lines;                   /* key=text lines read */
    int unknown;                 /* keys the program does not have */
    int missing;                 /* program keys the file does not have */
    int badfmt;                  /* texts whose conversions differ from the English */
    int badline;                 /* lines without '=' or with an empty key */
    char first[128];             /* the first problem, for the messages */
} lang_report;

/* the text in the current language; the English one if missing */
const wchar_t *tr(int id);
/* the key of an id, and the id of a key (-1 if unknown) */
const char *lang_key(int id);
int lang_find(const char *key);
/* the compiled English text */
const wchar_t *lang_builtin(int id);

/* back to the compiled English */
void lang_reset(void);
/* loads a language from a buffer or from a file (UTF-8 path); rep may be
 * NULL. The texts missing or with wrong conversions stay English.
 * 0 ok, -1 if the file cannot be read (and nothing changes) */
int lang_load_text(const char *text, size_t len, lang_report *rep);
int lang_load_file(const char *path, lang_report *rep);
/* the value of a key in a file, without loading it: the language's own
 * name (language.name) for the menu. 1 found */
int lang_peek_file(const char *path, const char *key, wchar_t *out, size_t n);

/* A text the core wrote in English (an error, why a CD was not read): the
 * id of the message of core/messages.h it comes from, the most specific
 * one if more match; -1 if it is none. The first letter's case does not
 * count (the session makes it a capital) */
int lang_core_match(const char *text);
/* the same text in the current language, its arguments that are messages
 * themselves translated too; 1 done, 0 not a message (out gets the text as
 * it is: a fallback that should never be needed) */
int lang_core_text(const char *text, wchar_t *out, size_t n);

/* 1 if fmt has the same printf conversions as ref, in the same order */
int lang_same_format(const wchar_t *fmt, const wchar_t *ref);

/* v with the language's decimal separator (format.decimal): "58,9" */
void lang_number(double v, int decimals, wchar_t *out, size_t n);
/* "2026-10-14T21:20:11" -> the format.date pattern (DD, MM, YYYY) */
void lang_date(const char *iso, wchar_t *out, size_t n);

#endif
