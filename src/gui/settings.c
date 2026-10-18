/* Settings in two ini files: see settings.h */
#include "settings.h"
#include <shlobj.h>
#include <string.h>
#include <wchar.h>

#define SET_MAXVAL 1024

static void copy_w(wchar_t *out, int n, const wchar_t *s)
{
    if (n <= 0) return;
    wcsncpy(out, s ? s : L"", n);
    out[n - 1] = 0;
}

/* "...\folder\RetroArchiver.exe" -> "...\folder\RetroArchiver.ini" */
static void defaults_path(wchar_t *out)
{
    wchar_t *p;
    out[0] = 0;
    if (!GetModuleFileNameW(NULL, out, MAX_PATH) || !(p = wcsrchr(out, L'\\'))) { out[0] = 0; return; }
    if ((p + 1 - out) + (int)wcslen(SET_FILE) >= MAX_PATH) { out[0] = 0; return; }
    wcscpy(p + 1, SET_FILE);
}

/* %APPDATA%\RetroArchiver\RetroArchiver.ini; Application Data is created if missing */
static void user_path(wchar_t *out)
{
    wchar_t dir[MAX_PATH];
    out[0] = 0;
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, dir) != S_OK) return;
    if (wcslen(dir) + wcslen(SET_USER_DIR) + wcslen(SET_FILE) + 3 > MAX_PATH) return;
    _snwprintf(out, MAX_PATH, L"%ls\\%ls\\%ls", dir, SET_USER_DIR, SET_FILE);
    out[MAX_PATH - 1] = 0;
}

void set_init(settings *s)
{
    defaults_path(s->defaults);
    user_path(s->user);
}

void set_init_paths(settings *s, const wchar_t *defaults, const wchar_t *user)
{
    copy_w(s->defaults, MAX_PATH, defaults);
    copy_w(s->user, MAX_PATH, user);
}

/* the Italian keys of 261017 and the English ones that replace them */
static const struct { const wchar_t *sec, *key, *old_sec, *old_key; } aliases[] = {
    { SEC_GENERAL, KEY_FOLDER,     L"Archiviazione", L"Cartella" },
    { SEC_GENERAL, KEY_MAXREADS,   L"Archiviazione", L"LettureMax" },
    { SEC_GENERAL, KEY_SOUNDS,     L"Archiviazione", L"Suoni" },
    { SEC_GENERAL, KEY_UILANG,     L"Finestra",      L"UILanguage" },
    { SEC_FLOPPY,  KEY_SAMDISK,    L"Archiviazione", L"SAMdisk" },
    { SEC_FLOPPY,  KEY_READFROM,   L"Archiviazione", L"LeggiDa" },
    { SEC_FLOPPY,  KEY_FLOPPYNAME, L"Archiviazione", L"NomeFloppy" },
    { SEC_OPTICAL, KEY_DIC,        L"Archiviazione", L"DiscImageCreator" },
    { SEC_OPTICAL, KEY_DRIVE,      L"Archiviazione", L"LettoreCD" },
    { SEC_OPTICAL, KEY_CDNAME,     L"Archiviazione", L"NomeCD" },
    { SEC_OPTICAL, KEY_DVDNAME,    L"Archiviazione", L"NomeDVD" },
    { SEC_AUTO,    KEY_ENABLED,    L"Archiviazione", L"AutoMode" },
    { SEC_BOX,     KEY_PLATFORM,   L"Archiviazione", L"Piattaforma" },
    { SEC_BOX,     KEY_LANGUAGE,   L"Archiviazione", L"Lingua" },
    { SEC_WINDOW,  L"X",           L"Finestra",      L"X" },
    { SEC_WINDOW,  L"Y",           L"Finestra",      L"Y" },
    { SEC_WINDOW,  L"Width",       L"Finestra",      L"Larghezza" },
    { SEC_WINDOW,  L"Height",      L"Finestra",      L"Altezza" },
    { SEC_WINDOW,  L"Maximized",   L"Finestra",      L"Massimizzata" },
    { NULL, NULL, NULL, NULL }
};
/* old keys that mean nothing any more: they go when converting */
static const struct { const wchar_t *sec, *key; } obsolete[] = {
    { L"Archiviazione", L"NomeFile" }, { NULL, NULL }
};
static const wchar_t *const old_sections[] = { L"Archiviazione", L"Finestra", NULL };

#define SET_ABSENT L"\x01"               /* no ini value can be this */

static int alias_of(const wchar_t *sec, const wchar_t *key)
{
    int i;
    for (i = 0; aliases[i].sec; i++)
        if (!_wcsicmp(aliases[i].sec, sec) && !_wcsicmp(aliases[i].key, key)) return i;
    return -1;
}

/* the key in one file: 1 found (in out), 0 absent. An empty file name must
 * not reach the APIs: they would look up win.ini */
static int read_raw(const wchar_t *file, const wchar_t *sec, const wchar_t *key, wchar_t *out, int n)
{
    if (!file[0]) return 0;
    GetPrivateProfileStringW(sec, key, SET_ABSENT, out, n, file);
    return wcscmp(out, SET_ABSENT) != 0;
}

/* the English key, else the Italian one it replaces */
static int read_file(const wchar_t *file, const wchar_t *sec, const wchar_t *key, wchar_t *out, int n)
{
    int a;
    if (read_raw(file, sec, key, out, n)) return 1;
    a = alias_of(sec, key);
    return a >= 0 && read_raw(file, aliases[a].old_sec, aliases[a].old_key, out, n);
}

/* the default: the installer's ini, else def */
static void read_default(const settings *s, const wchar_t *sec, const wchar_t *key,
                         const wchar_t *def, wchar_t *out, int n)
{
    if (!read_file(s->defaults, sec, key, out, n)) copy_w(out, n, def);
}

void set_get(const settings *s, const wchar_t *sec, const wchar_t *key,
             const wchar_t *def, wchar_t *out, int n)
{
    wchar_t v[SET_MAXVAL];
    if (!read_file(s->user, sec, key, v, SET_MAXVAL)) read_default(s, sec, key, def, v, SET_MAXVAL);
    copy_w(out, n, v);
}

int set_int(const settings *s, const wchar_t *sec, const wchar_t *key, int def)
{
    wchar_t v[64], *end;
    long k;
    set_get(s, sec, key, L"", v, 64);
    k = wcstol(v, &end, 10);
    return end == v ? def : (int)k;
}

/* the user's ini folder, and the ini with the UTF-16 BOM: this way
 * Windows writes it in Unicode and accented paths are not lost */
static int user_create(const settings *s)
{
    wchar_t dir[MAX_PATH], *p;
    HANDLE h;
    if (!s->user[0]) return 0;
    if (GetFileAttributesW(s->user) != INVALID_FILE_ATTRIBUTES) return 1;
    copy_w(dir, MAX_PATH, s->user);
    if ((p = wcsrchr(dir, L'\\')) != NULL) {
        *p = 0;
        if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    }
    h = CreateFileW(s->user, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w; static const unsigned char bom[2] = { 0xFF, 0xFE };
        WriteFile(h, bom, 2, &w, NULL);
        CloseHandle(h);
    }
    return GetFileAttributesW(s->user) != INVALID_FILE_ATTRIBUTES;
}

/* writes the English key; the Italian one it replaces goes, or it would
 * come back as soon as the English one is removed */
int set_put(const settings *s, const wchar_t *sec, const wchar_t *key, const wchar_t *v)
{
    int a = alias_of(sec, key);
    if (!v && s->user[0] && GetFileAttributesW(s->user) == INVALID_FILE_ATTRIBUTES) return 1;
    if (!user_create(s)) return 0;
    if (a >= 0) WritePrivateProfileStringW(aliases[a].old_sec, aliases[a].old_key, NULL, s->user);
    return WritePrivateProfileStringW(sec, key, v, s->user) != 0;
}

int set_save(const settings *s, const wchar_t *sec, const wchar_t *key,
             const wchar_t *def, const wchar_t *v)
{
    wchar_t d[SET_MAXVAL];
    read_default(s, sec, key, def ? def : L"", d, SET_MAXVAL);
    return set_put(s, sec, key, wcscmp(d, v) ? v : NULL);
}

/* a section with no keys left */
static int section_empty(const wchar_t *file, const wchar_t *sec)
{
    wchar_t buf[8];
    return GetPrivateProfileSectionW(sec, buf, 8, file) == 0;
}

int set_convert(const wchar_t *file)
{
    wchar_t v[SET_MAXVAL], cur[SET_MAXVAL];
    int i, n = 0;
    if (!file[0] || GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) return 0;
    for (i = 0; aliases[i].sec; i++) {
        if (!read_raw(file, aliases[i].old_sec, aliases[i].old_key, v, SET_MAXVAL)) continue;
        /* the English key, if already there, is the newer choice */
        if (!read_raw(file, aliases[i].sec, aliases[i].key, cur, SET_MAXVAL) &&
            !WritePrivateProfileStringW(aliases[i].sec, aliases[i].key, v, file)) return -1;
        if (!WritePrivateProfileStringW(aliases[i].old_sec, aliases[i].old_key, NULL, file)) return -1;
        n++;
    }
    for (i = 0; obsolete[i].sec; i++)
        if (read_raw(file, obsolete[i].sec, obsolete[i].key, v, SET_MAXVAL)) {
            WritePrivateProfileStringW(obsolete[i].sec, obsolete[i].key, NULL, file);
            n++;
        }
    for (i = 0; old_sections[i]; i++)
        if (section_empty(file, old_sections[i])) WritePrivateProfileStringW(old_sections[i], NULL, NULL, file);
    return n;
}

int set_put_int(const settings *s, const wchar_t *sec, const wchar_t *key, int v)
{
    wchar_t w[16];
    _snwprintf(w, 16, L"%d", v); w[15] = 0;
    return set_put(s, sec, key, w);
}

/* the way CreateProcess looks for it: a path, or a name in the exe's
 * folder, the current one, the system one, the PATH */
static int tool_found(const wchar_t *path)
{
    wchar_t full[MAX_PATH], *part;
    DWORD a;
    if (!path[0]) return 0;
    if (!SearchPathW(NULL, path, L".exe", MAX_PATH, full, &part)) return 0;
    a = GetFileAttributesW(full);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int set_tool_fallback(const wchar_t *configured, const wchar_t *rel, wchar_t *out, int n)
{
    wchar_t exe[MAX_PATH], *p;
    DWORD a;
    if (tool_found(configured)) return 0;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH) || !(p = wcsrchr(exe, L'\\'))) return 0;
    *p = 0;
    if ((int)(wcslen(exe) + wcslen(rel) + 2) > n) return 0;
    _snwprintf(out, n, L"%ls\\%ls", exe, rel);
    out[n - 1] = 0;
    a = GetFileAttributesW(out);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void set_default_archive(wchar_t *out, int n)
{
    wchar_t win[MAX_PATH], drive[3] = L"C:";
    UINT len = GetWindowsDirectoryW(win, MAX_PATH);
    if (len >= 2 && len < MAX_PATH && win[1] == L':') { drive[0] = win[0]; }
    _snwprintf(out, n, L"%ls\\%ls", drive, SET_ARCHIVE_DIR);
    out[n - 1] = 0;
}
