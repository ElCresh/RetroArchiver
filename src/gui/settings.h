/* RetroArchiver settings, in two ini files.
 *
 * - the defaults: RetroArchiver.ini next to the executable, written by
 *   the installer. Read only: a normal user cannot write in Program Files
 *   (and from Vista on it would end up in the VirtualStore).
 * - the user's: %APPDATA%\RetroArchiver\RetroArchiver.ini, the only file
 *   the program writes.
 *
 * For each key the user's value wins, else the default, else the value
 * hard-coded here. Whoever upgrades from C:\RetroArchiver keeps their
 * choices: the old ini next to the exe becomes their defaults.
 * Windows only: goes through GetPrivateProfileString, which reads
 * UTF-16 LE files with a BOM.
 */
#ifndef RA_SETTINGS_H
#define RA_SETTINGS_H

#include <windows.h>

#define SET_FILE        L"RetroArchiver.ini"
#define SET_USER_DIR    L"RetroArchiver"     /* inside Application Data */
#define SET_ARCHIVE_DIR L"RetroArchive"      /* the default archive, at the root of the system drive */

typedef struct {
    wchar_t defaults[MAX_PATH];   /* "" if its location is unknown */
    wchar_t user[MAX_PATH];       /* "" if its location is unknown: then nothing is saved */
} settings;

/* the two real paths: next to the executable and in Application Data */
void set_init(settings *s);
/* two chosen paths (the tests) */
void set_init_paths(settings *s, const wchar_t *defaults, const wchar_t *user);

void set_get(const settings *s, const wchar_t *sec, const wchar_t *key,
             const wchar_t *def, wchar_t *out, int n);
int  set_int(const settings *s, const wchar_t *sec, const wchar_t *key, int def);
/* v NULL removes the key. 0 if the user's ini cannot be written */
int  set_put(const settings *s, const wchar_t *sec, const wchar_t *key, const wchar_t *v);
/* A user choice: written only if it differs from the default (the
 * installer's, else def); if equal, the key is removed. This way a new
 * default brought by an upgrade also reaches those who never changed it. */
int  set_save(const settings *s, const wchar_t *sec, const wchar_t *key,
              const wchar_t *def, const wchar_t *v);
int  set_put_int(const settings *s, const wchar_t *sec, const wchar_t *key, int v);

/* "C:\RetroArchive": the archive folder on the system drive */
void set_default_archive(wchar_t *out, int n);

/* the third-party programs the installer brings, in the exe's folder */
#define SET_TOOL_SAMDISK L"tools\\samdisk\\samdisk.exe"
#define SET_TOOL_DIC     L"tools\\DIC\\DiscImageCreator.exe"

/* The configured program is not found (an older ini that said
 * C:\samdisk): if <exe folder>\rel exists, 1 and its path in out. */
int set_tool_fallback(const wchar_t *configured, const wchar_t *rel, wchar_t *out, int n);

#endif
