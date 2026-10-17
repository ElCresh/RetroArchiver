/* Test of src/gui/settings.c: settings in two ini files, the user's first,
 * then the defaults, then the code. Windows only: built with
 * `make xp-tests` (build/settings_test.exe) and run on the Windows host
 * or the XP VM, from a writable folder. Exits with 0 if all is well. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "gui/settings.h"

static int fails, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fails++; printf("FAIL: %s\n", what); }
}

static void check_str(const wchar_t *got, const wchar_t *want, const char *what)
{
    checks++;
    if (wcscmp(got, want)) { fails++; printf("FAIL: %s: \"%ls\" instead of \"%ls\"\n", what, got, want); }
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* a default ini as the installer writes it: UTF-16 LE with BOM */
static void write_defaults(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w; static const unsigned char bom[2] = { 0xFF, 0xFE };
    WriteFile(h, bom, 2, &w, NULL);
    CloseHandle(h);
    WritePrivateProfileStringW(L"Archiviazione", L"Cartella", L"D:\\Cofanetti già fatti", path);
    WritePrivateProfileStringW(L"Archiviazione", L"LettureMax", L"5", path);
    WritePrivateProfileStringW(L"Finestra", L"X", L"40", path);
}

static void file_time(const wchar_t *p, FILETIME *t)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    memset(t, 0, sizeof(*t));
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &d)) *t = d.ftLastWriteTime;
}

int main(void)
{
    wchar_t root[MAX_PATH], def[MAX_PATH], dir[MAX_PATH], user[MAX_PATH], v[512];
    settings s, none;
    FILETIME t0, t1;
    unsigned char head[2] = { 0, 0 };
    HANDLE h; DWORD got;

    GetCurrentDirectoryW(MAX_PATH, root);
    _snwprintf(def, MAX_PATH, L"%ls\\tmp_settings_def.ini", root);
    _snwprintf(dir, MAX_PATH, L"%ls\\tmp_settings_utente", root);
    _snwprintf(user, MAX_PATH, L"%ls\\RetroArchiver.ini", dir);
    DeleteFileW(def); DeleteFileW(user); RemoveDirectoryW(dir);

    /* no ini: the value from the code */
    set_init_paths(&s, def, user);
    set_get(&s, L"Archiviazione", L"Cartella", L"C:\\RetroArchive", v, 512);
    check_str(v, L"C:\\RetroArchive", "no ini: the code value");
    check(set_int(&s, L"Finestra", L"X", -32000) == -32000, "no ini: the code integer");

    /* empty paths: no win.ini, no writes */
    set_init_paths(&none, L"", L"");
    set_get(&none, L"windows", L"load", L"x", v, 512);
    check_str(v, L"x", "empty paths do not read win.ini");
    check(!set_put(&none, L"Archiviazione", L"Cartella", L"C:\\x"), "no user ini: nothing is saved");

    /* the installer defaults */
    write_defaults(def);
    set_get(&s, L"Archiviazione", L"Cartella", L"C:\\RetroArchive", v, 512);
    check_str(v, L"D:\\Cofanetti già fatti", "the default wins, with accents");
    check(set_int(&s, L"Archiviazione", L"LettureMax", 3) == 5, "integer from the defaults");
    check(set_int(&s, L"Archiviazione", L"Suoni", 0) == 0, "key missing from the defaults");
    check(!exists(dir), "reading does not create the user folder");

    /* the user writes: only in their ini, folder created, UTF-16 BOM */
    file_time(def, &t0);
    Sleep(20);
    check(set_put(&s, L"Archiviazione", L"Cartella", L"E:\\Mio archivio è qui"), "writes the user ini");
    check(exists(user), "the user ini and its folder exist");
    h = CreateFileW(user, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, head, 2, &got, NULL); CloseHandle(h); }
    check(head[0] == 0xFF && head[1] == 0xFE, "the user ini starts with the UTF-16 LE BOM");
    set_get(&s, L"Archiviazione", L"Cartella", L"C:\\RetroArchive", v, 512);
    check_str(v, L"E:\\Mio archivio è qui", "the user wins over the default");
    set_get(&s, L"Archiviazione", L"LettureMax", L"3", v, 512);
    check_str(v, L"5", "the other keys keep the defaults");
    file_time(def, &t1);
    check(CompareFileTime(&t0, &t1) == 0, "the defaults are not touched");

    /* integers and an empty value chosen by the user */
    check(set_put_int(&s, L"Finestra", L"X", 7), "writes an integer");
    check(set_int(&s, L"Finestra", L"X", -32000) == 7, "user integer over the default");
    set_put(&s, L"Archiviazione", L"LettoreCD", L"");
    WritePrivateProfileStringW(L"Archiviazione", L"LettoreCD", L"F:", def);
    set_get(&s, L"Archiviazione", L"LettoreCD", L"D:", v, 512);
    check_str(v, L"", "an empty user value stays empty");

    /* removing a key: back to the default */
    set_put(&s, L"Archiviazione", L"Cartella", NULL);
    set_get(&s, L"Archiviazione", L"Cartella", L"C:\\RetroArchive", v, 512);
    check_str(v, L"D:\\Cofanetti già fatti", "key removed: back to the default");

    /* set_save: equal to the default it is removed, different it is written */
    check(set_save(&s, L"Archiviazione", L"LettureMax", L"3", L"7"), "set_save different: writes");
    check(set_int(&s, L"Archiviazione", L"LettureMax", 3) == 7, "set_save different: the user wins");
    check(set_save(&s, L"Archiviazione", L"LettureMax", L"3", L"5"), "set_save equal to the installer default");
    GetPrivateProfileStringW(L"Archiviazione", L"LettureMax", L"(manca)", v, 512, user);
    check_str(v, L"(manca)", "set_save equal: the user key is removed");
    check(set_save(&s, L"Archiviazione", L"NomeCD", L"CD{nn}", L"CD{nn}"), "set_save equal to the code");
    GetPrivateProfileStringW(L"Archiviazione", L"NomeCD", L"(manca)", v, 512, user);
    check_str(v, L"(manca)", "set_save equal to the code: not written");
    WritePrivateProfileStringW(L"Archiviazione", L"LettureMax", L"4", def);
    check(set_int(&s, L"Archiviazione", L"LettureMax", 3) == 4, "a new default reaches users who did not change it");
    {   /* no user ini and an equal choice: the file is not created */
        settings t;
        wchar_t u2[MAX_PATH];
        _snwprintf(u2, MAX_PATH, L"%ls\\tmp_settings_mai\\RetroArchiver.ini", root);
        set_init_paths(&t, def, u2);
        set_save(&t, L"Archiviazione", L"LettureMax", L"3", L"4");
        check(!exists(u2), "choice equal to the default: the user ini is not created");
    }

    /* the default archive on the system disk */
    set_default_archive(v, 512);
    check(v[1] == L':' && !wcscmp(v + 2, L"\\RetroArchive"), "default archive X:\\RetroArchive");

    /* the fallback for programs: a name that is found stays */
    check(!set_tool_fallback(L"cmd.exe", L"tools\\samdisk\\samdisk.exe", v, 512), "cmd.exe is found: no fallback");
    check(!set_tool_fallback(L"C:\\non\\c'e'\\samdisk.exe", L"tools\\non-esiste.exe", v, 512),
          "no tools\\: no fallback");
    {   /* a program in <exe folder>\tools_prova\: it finds it */
        wchar_t self[MAX_PATH], tdir[MAX_PATH], tool[MAX_PATH], *p;
        GetModuleFileNameW(NULL, self, MAX_PATH);
        wcscpy(tdir, self);
        if ((p = wcsrchr(tdir, L'\\')) != NULL) *p = 0;
        _snwprintf(tool, MAX_PATH, L"%ls\\tools_prova\\finto.exe", tdir);
        wcscat(tdir, L"\\tools_prova");
        CreateDirectoryW(tdir, NULL);
        CopyFileW(self, tool, FALSE);
        check(set_tool_fallback(L"C:\\samdisk\\samdisk.exe", L"tools_prova\\finto.exe", v, 512),
              "old path that does not exist: fallback in tools");
        check_str(v, tool, "the fallback is next to the exe");
        check(set_tool_fallback(L"", L"tools_prova\\finto.exe", v, 512), "empty field: fallback in tools");
        DeleteFileW(tool); RemoveDirectoryW(tdir);
    }

    DeleteFileW(def); DeleteFileW(user); RemoveDirectoryW(dir);
    printf("settings: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
