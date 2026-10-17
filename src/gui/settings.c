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

/* an empty file name must not reach the APIs: they would look up win.ini */
static void read_one(const wchar_t *file, const wchar_t *sec, const wchar_t *key,
                     const wchar_t *def, wchar_t *out, int n)
{
    if (file[0]) GetPrivateProfileStringW(sec, key, def, out, n, file);
    else copy_w(out, n, def);
}

void set_get(const settings *s, const wchar_t *sec, const wchar_t *key,
             const wchar_t *def, wchar_t *out, int n)
{
    wchar_t mid[SET_MAXVAL];
    read_one(s->defaults, sec, key, def ? def : L"", mid, SET_MAXVAL);
    read_one(s->user, sec, key, mid, out, n < SET_MAXVAL ? n : SET_MAXVAL);
}

int set_int(const settings *s, const wchar_t *sec, const wchar_t *key, int def)
{
    if (s->defaults[0]) def = (int)GetPrivateProfileIntW(sec, key, def, s->defaults);
    if (s->user[0]) def = (int)GetPrivateProfileIntW(sec, key, def, s->user);
    return def;
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

int set_put(const settings *s, const wchar_t *sec, const wchar_t *key, const wchar_t *v)
{
    if (!v && s->user[0] && GetFileAttributesW(s->user) == INVALID_FILE_ATTRIBUTES) return 1;
    if (!user_create(s)) return 0;
    return WritePrivateProfileStringW(sec, key, v, s->user) != 0;
}

int set_save(const settings *s, const wchar_t *sec, const wchar_t *key,
             const wchar_t *def, const wchar_t *v)
{
    wchar_t d[SET_MAXVAL];
    read_one(s->defaults, sec, key, def ? def : L"", d, SET_MAXVAL);
    return set_put(s, sec, key, wcscmp(d, v) ? v : NULL);
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
