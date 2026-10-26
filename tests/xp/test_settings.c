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
    WritePrivateProfileStringW(SEC_GENERAL, KEY_FOLDER, L"D:\\Cofanetti già fatti", path);
    WritePrivateProfileStringW(SEC_GENERAL, KEY_MAXREADS, L"5", path);
    WritePrivateProfileStringW(SEC_WINDOW, L"X", L"40", path);
}

/* an ini of version 261017, with the Italian keys, as the setup and the window wrote it */
static void write_old(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w; static const unsigned char bom[2] = { 0xFF, 0xFE };
    WriteFile(h, bom, 2, &w, NULL);
    CloseHandle(h);
    WritePrivateProfileStringW(L"Archiviazione", L"Cartella", L"D:\\Cofanetti già fatti", path);
    WritePrivateProfileStringW(L"Archiviazione", L"LettureMax", L"5", path);
    WritePrivateProfileStringW(L"Archiviazione", L"LeggiDa", L"b:", path);
    WritePrivateProfileStringW(L"Archiviazione", L"NomeFloppy", L"DISK{nnn}", path);
    WritePrivateProfileStringW(L"Archiviazione", L"Lingua", L"Italiano", path);
    WritePrivateProfileStringW(L"Archiviazione", L"NomeFile", L"DISK{nn}", path);
    WritePrivateProfileStringW(L"Finestra", L"X", L"-12", path);
    WritePrivateProfileStringW(L"Finestra", L"Massimizzata", L"1", path);
}

static int has_key(const wchar_t *file, const wchar_t *sec, const wchar_t *key)
{
    wchar_t v[64];
    GetPrivateProfileStringW(sec, key, L"(manca)", v, 64, file);
    return wcscmp(v, L"(manca)") != 0;
}

static int has_section(const wchar_t *file, const wchar_t *sec)
{
    wchar_t buf[64];
    return GetPrivateProfileSectionW(sec, buf, 64, file) != 0;
}

/* the old Italian ini: read as it is, then converted once */
static void test_old_ini(const wchar_t *root)
{
    wchar_t def[MAX_PATH], user[MAX_PATH], v[512];
    settings s;
    _snwprintf(def, MAX_PATH, L"%ls\\tmp_settings_old_def.ini", root);
    _snwprintf(user, MAX_PATH, L"%ls\\tmp_settings_old_user.ini", root);
    DeleteFileW(def); DeleteFileW(user);
    write_old(def);
    set_init_paths(&s, def, user);

    /* defaults in Italian and no user ini: read through the old keys */
    set_get(&s, SEC_GENERAL, KEY_FOLDER, L"C:\\RetroArchive", v, 512);
    check_str(v, L"D:\\Cofanetti già fatti", "old defaults ini: Cartella read as ArchiveFolder");
    check(set_int(&s, SEC_GENERAL, KEY_MAXREADS, 3) == 5, "old defaults ini: LettureMax read as MaxReads");
    check(set_int(&s, SEC_WINDOW, L"X", -32000) == -12, "old ini: a negative integer");
    set_get(&s, SEC_FLOPPY, KEY_FLOPPYNAME, L"FLOPPY{nn}", v, 512);
    check_str(v, L"DISK{nnn}", "old defaults ini: NomeFloppy read as FileName");
    check(set_save(&s, SEC_GENERAL, KEY_MAXREADS, L"3", L"5"), "a choice equal to the old default");
    check(GetFileAttributesW(user) == INVALID_FILE_ATTRIBUTES, "equal to the old default: nothing written");

    /* a user ini in Italian: it wins over the defaults, the English key over it */
    write_old(user);
    WritePrivateProfileStringW(L"Archiviazione", L"LettureMax", L"7", user);
    check(set_int(&s, SEC_GENERAL, KEY_MAXREADS, 3) == 7, "old user ini wins over the defaults");
    WritePrivateProfileStringW(SEC_GENERAL, KEY_MAXREADS, L"9", user);
    check(set_int(&s, SEC_GENERAL, KEY_MAXREADS, 3) == 9, "the English key wins over the Italian one");
    /* writing a key removes the Italian one it replaces */
    check(set_put(&s, SEC_FLOPPY, KEY_READFROM, L"a:"), "writes ReadFrom");
    check(!has_key(user, L"Archiviazione", L"LeggiDa"), "writing ReadFrom removes LeggiDa");

    /* conversion: values kept, the English one already there wins, nothing Italian left */
    check(set_convert(user) > 0, "the old user ini is converted");
    check(!has_section(user, L"Archiviazione") && !has_section(user, L"Finestra"), "no Italian sections left");
    GetPrivateProfileStringW(SEC_GENERAL, KEY_FOLDER, L"", v, 512, user);
    check_str(v, L"D:\\Cofanetti già fatti", "converted: Cartella -> [General] ArchiveFolder");
    GetPrivateProfileStringW(SEC_GENERAL, KEY_MAXREADS, L"", v, 512, user);
    check_str(v, L"9", "converted: the English value already there stays");
    GetPrivateProfileStringW(SEC_BOX, KEY_LANGUAGE, L"", v, 512, user);
    check_str(v, L"Italiano", "converted: Lingua -> [Box] Language");
    GetPrivateProfileStringW(SEC_WINDOW, L"Maximized", L"", v, 512, user);
    check_str(v, L"1", "converted: Massimizzata -> [Window] Maximized");
    check(!has_key(user, SEC_FLOPPY, L"NomeFile") && !has_key(user, L"Archiviazione", L"NomeFile"),
          "the obsolete NomeFile is gone");
    check(set_convert(user) == 0, "a second conversion changes nothing");
    check(set_convert(def) > 0 && !has_section(def, L"Archiviazione"), "the defaults ini converts too");
    set_get(&s, SEC_FLOPPY, KEY_FLOPPYNAME, L"FLOPPY{nn}", v, 512);
    check_str(v, L"DISK{nnn}", "after converting, the same values");
    DeleteFileW(def); DeleteFileW(user);
}

/* the Settings window: everything at once, only the differences written */
static void test_prefs(const wchar_t *root)
{
    wchar_t def[MAX_PATH], user[MAX_PATH], v[512];
    settings s;
    prefs p, d;
    _snwprintf(def, MAX_PATH, L"%ls\\tmp_prefs_def.ini", root);
    _snwprintf(user, MAX_PATH, L"%ls\\tmp_prefs_user.ini", root);
    DeleteFileW(def); DeleteFileW(user);
    set_init_paths(&s, def, user);

    /* no ini: the code's values */
    prefs_load(&s, &p);
    check(p.maxreads == 3 && p.sounds == 0 && p.countdown == 5 && p.floppy_poll == 2, "prefs: code numbers");
    check_str(p.samdisk, L"samdisk", "prefs: code SAMdisk");
    check_str(p.readfrom, L"a:", "prefs: code ReadFrom");
    check_str(p.drive, L"D:", "prefs: code Drive");
    check_str(p.dvd_name, L"DVD{nn}", "prefs: code DVDFileName");
    check(p.folder[1] == L':' && !wcscmp(p.folder + 2, L"\\RetroArchive"), "prefs: code archive folder");
    check(!p.offset[0] && !p.noleadin[0] && !p.datdir[0] && !p.uilang[0], "prefs: empty by default");
    check(prefs_save(&s, &p) && !exists(user), "prefs: nothing changed, no user ini");

    /* the installer's defaults, one out of range */
    write_defaults(def);
    WritePrivateProfileStringW(SEC_AUTO, KEY_COUNTDOWN, L"500", def);
    WritePrivateProfileStringW(SEC_OPTICAL, KEY_DRIVE, L"E:", def);
    prefs_load(&s, &p);
    check(p.maxreads == 5, "prefs: MaxReads from the defaults");
    check(p.countdown == 5, "prefs: a default out of range is the code's");
    check_str(p.folder, L"D:\\Cofanetti già fatti", "prefs: folder from the defaults");

    /* the user changes three values: only those are written */
    p.countdown = 10;
    wcscpy(p.offset, L"+6");
    wcscpy(p.noleadin, L"ASUS DRW-08D6MT A101");
    check(prefs_save(&s, &p), "prefs: saved");
    check(has_key(user, SEC_AUTO, KEY_COUNTDOWN) && has_key(user, SEC_OPTICAL, KEY_DRIVEOFFSET) &&
          has_key(user, SEC_OPTICAL, KEY_NOLEADIN), "prefs: the changed keys are written");
    check(!has_key(user, SEC_GENERAL, KEY_MAXREADS) && !has_key(user, SEC_OPTICAL, KEY_DRIVE) &&
          !has_key(user, SEC_GENERAL, KEY_FOLDER) && !has_key(user, SEC_FLOPPY, KEY_SAMDISK),
          "prefs: the unchanged keys are not");
    prefs_load(&s, &d);
    check(d.countdown == 10 && d.maxreads == 5, "prefs: read back");
    check_str(d.offset, L"+6", "prefs: offset read back");

    /* a hand-written user value out of range: the default */
    WritePrivateProfileStringW(SEC_AUTO, KEY_FLOPPYPOLL, L"0", user);
    WritePrivateProfileStringW(SEC_GENERAL, KEY_MAXREADS, L"tre", user);
    prefs_load(&s, &d);
    check(d.floppy_poll == 2 && d.maxreads == 5, "prefs: user values out of range are the default");

    /* back to the defaults: the keys go */
    prefs_defaults(&s, &d);
    check(d.countdown == 5 && !d.offset[0], "prefs: prefs_defaults ignores the user");
    check(prefs_save(&s, &d), "prefs: defaults saved");
    check(!has_key(user, SEC_AUTO, KEY_COUNTDOWN) && !has_key(user, SEC_OPTICAL, KEY_DRIVEOFFSET) &&
          !has_key(user, SEC_AUTO, KEY_FLOPPYPOLL), "prefs: equal to the defaults, the keys are removed");
    GetPrivateProfileStringW(SEC_WINDOW, L"X", L"(manca)", v, 512, user);
    check_str(v, L"(manca)", "prefs: other sections untouched");
    DeleteFileW(def); DeleteFileW(user);
}

/* the whole user ini as bytes, to look for a key in clear text in it */
static int ini_has_text(const wchar_t *file, const char *needle)
{
    static char buf[65536];
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD got = 0;
    size_t i, k = strlen(needle);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    /* the ini is UTF-16 LE: the ASCII of a key is there every other byte */
    for (i = 0; i + 2 * k <= got; i++) {
        size_t j;
        for (j = 0; j < k && buf[i + 2 * j] == needle[j] && !buf[i + 2 * j + 1]; j++) { }
        if (j == k) return 1;
    }
    return 0;
}

/* the secrets: DPAPI and base64, nothing in clear text in the ini */
static void test_secrets(const wchar_t *root)
{
    wchar_t def[MAX_PATH], user[MAX_PATH], v[512];
    settings s;
    ia_keys k, back;
    char got[600], longv[600];
    int i;
    _snwprintf(def, MAX_PATH, L"%ls\\tmp_secret_def.ini", root);
    _snwprintf(user, MAX_PATH, L"%ls\\tmp_secret_user.ini", root);
    DeleteFileW(def); DeleteFileW(user);
    set_init_paths(&s, def, user);

    /* no ini at all: no keys, and nothing was created to say so */
    check(set_ia_keys_get(&s, &k) == SET_SECRET_NONE, "secret: no ini, no keys");
    check(!k.access[0] && !k.secret[0], "secret: nothing read, nothing left behind");
    check(!exists(user), "secret: reading does not create the user ini");

    /* a value there and back, whatever is in it */
    check(set_put_secret(&s, SEC_IA, L"Probe", "a value with spaces, accents (e') and = signs"),
          "secret: written");
    check(set_get_secret(&s, SEC_IA, L"Probe", got, sizeof(got)) == SET_SECRET_OK, "secret: read back");
    check(!strcmp(got, "a value with spaces, accents (e') and = signs"), "secret: the same value");
    check(!ini_has_text(user, "a value with spaces"), "secret: not in clear text in the ini");

    /* a value too long is refused, the longest one allowed is not */
    for (i = 0; i < 599; i++) longv[i] = 'x';
    longv[599] = 0;
    check(!set_put_secret(&s, SEC_IA, L"Probe2", longv), "secret: over the limit is refused");
    longv[SET_SECRET_MAX] = 0;
    check(set_put_secret(&s, SEC_IA, L"Probe2", longv), "secret: the longest value allowed");
    check(set_get_secret(&s, SEC_IA, L"Probe2", got, sizeof(got)) == SET_SECRET_OK &&
          strlen(got) == SET_SECRET_MAX, "secret: the longest value read back");
    /* a buffer too small is not a half value */
    check(set_get_secret(&s, SEC_IA, L"Probe2", got, 10) == SET_SECRET_BAD && !got[0],
          "secret: it does not fit, nothing written");

    /* the two keys together */
    memset(&k, 0, sizeof(k));
    strcpy(k.access, "AbCdEfGh12345678");
    strcpy(k.secret, "ZyXwVuTs87654321");
    check(set_ia_keys_put(&s, &k), "keys: written");
    check(set_ia_keys_get(&s, &back) == SET_SECRET_OK, "keys: read back");
    check(!strcmp(back.access, k.access) && !strcmp(back.secret, k.secret), "keys: the same two keys");
    check(!ini_has_text(user, "AbCdEfGh12345678") && !ini_has_text(user, "ZyXwVuTs87654321"),
          "keys: neither key is in the ini in clear text");
    check(has_key(user, SEC_IA, KEY_IAKEYS), "keys: the key is in [ArchiveOrg]");
    check(!has_key(def, SEC_IA, KEY_IAKEYS), "keys: nothing in the defaults ini");

    /* a value written by hand, in clear text: refused, not read as a key */
    WritePrivateProfileStringW(SEC_IA, KEY_IAKEYS, L"myaccess:mysecret", user);
    check(set_ia_keys_get(&s, &back) == SET_SECRET_BAD, "keys: a hand-written value is refused");
    check(!back.access[0] && !back.secret[0], "keys: refused, nothing read");
    /* valid base64 that DPAPI cannot decrypt (an ini from another PC) */
    WritePrivateProfileStringW(SEC_IA, KEY_IAKEYS, L"AAAAAAAAAAAAAAAAAAAAAA==", user);
    check(set_ia_keys_get(&s, &back) == SET_SECRET_BAD, "keys: a blob of another user is refused");
    /* an empty value counts as no keys */
    WritePrivateProfileStringW(SEC_IA, KEY_IAKEYS, L"", user);
    check(set_ia_keys_get(&s, &back) == SET_SECRET_NONE, "keys: an empty value means none");

    /* Forget the keys: the key goes */
    check(set_ia_keys_put(&s, &k), "keys: written again");
    ia_keys_wipe(&k);
    check(set_ia_keys_put(&s, &k), "keys: forgotten");
    check(!has_key(user, SEC_IA, KEY_IAKEYS), "keys: forgetting removes the key");
    check(set_ia_keys_get(&s, &back) == SET_SECRET_NONE, "keys: none after forgetting");

    /* the collection and the prefix are plain settings */
    {
        prefs p;
        prefs_load(&s, &p);
        check_str(p.ia_collection, IA_COLLECTION_DEF, "prefs: the collection by default");
        check(!p.ia_prefix[0], "prefs: no identifier prefix by default");
        wcscpy(p.ia_collection, L"test_collection");
        check(prefs_save(&s, &p), "prefs: collection saved");
        GetPrivateProfileStringW(SEC_IA, KEY_IACOLL, L"", v, 512, user);
        check_str(v, L"test_collection", "prefs: the collection in [ArchiveOrg]");
        prefs_load(&s, &p);
        check_str(p.ia_collection, L"test_collection", "prefs: the collection read back");
    }
    DeleteFileW(def); DeleteFileW(user);
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
    set_get(&s, SEC_GENERAL, KEY_FOLDER, L"C:\\RetroArchive", v, 512);
    check_str(v, L"C:\\RetroArchive", "no ini: the code value");
    check(set_int(&s, SEC_WINDOW, L"X", -32000) == -32000, "no ini: the code integer");

    /* empty paths: no win.ini, no writes */
    set_init_paths(&none, L"", L"");
    set_get(&none, L"windows", L"load", L"x", v, 512);
    check_str(v, L"x", "empty paths do not read win.ini");
    check(!set_put(&none, SEC_GENERAL, KEY_FOLDER, L"C:\\x"), "no user ini: nothing is saved");

    /* the installer defaults */
    write_defaults(def);
    set_get(&s, SEC_GENERAL, KEY_FOLDER, L"C:\\RetroArchive", v, 512);
    check_str(v, L"D:\\Cofanetti già fatti", "the default wins, with accents");
    check(set_int(&s, SEC_GENERAL, KEY_MAXREADS, 3) == 5, "integer from the defaults");
    check(set_int(&s, SEC_GENERAL, KEY_SOUNDS, 0) == 0, "key missing from the defaults");
    check(!exists(dir), "reading does not create the user folder");

    /* the user writes: only in their ini, folder created, UTF-16 BOM */
    file_time(def, &t0);
    Sleep(20);
    check(set_put(&s, SEC_GENERAL, KEY_FOLDER, L"E:\\Mio archivio è qui"), "writes the user ini");
    check(exists(user), "the user ini and its folder exist");
    h = CreateFileW(user, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, head, 2, &got, NULL); CloseHandle(h); }
    check(head[0] == 0xFF && head[1] == 0xFE, "the user ini starts with the UTF-16 LE BOM");
    set_get(&s, SEC_GENERAL, KEY_FOLDER, L"C:\\RetroArchive", v, 512);
    check_str(v, L"E:\\Mio archivio è qui", "the user wins over the default");
    set_get(&s, SEC_GENERAL, KEY_MAXREADS, L"3", v, 512);
    check_str(v, L"5", "the other keys keep the defaults");
    file_time(def, &t1);
    check(CompareFileTime(&t0, &t1) == 0, "the defaults are not touched");

    /* integers and an empty value chosen by the user */
    check(set_put_int(&s, SEC_WINDOW, L"X", 7), "writes an integer");
    check(set_int(&s, SEC_WINDOW, L"X", -32000) == 7, "user integer over the default");
    set_put(&s, SEC_OPTICAL, KEY_DRIVE, L"");
    WritePrivateProfileStringW(SEC_OPTICAL, KEY_DRIVE, L"F:", def);
    set_get(&s, SEC_OPTICAL, KEY_DRIVE, L"D:", v, 512);
    check_str(v, L"", "an empty user value stays empty");

    /* removing a key: back to the default */
    set_put(&s, SEC_GENERAL, KEY_FOLDER, NULL);
    set_get(&s, SEC_GENERAL, KEY_FOLDER, L"C:\\RetroArchive", v, 512);
    check_str(v, L"D:\\Cofanetti già fatti", "key removed: back to the default");

    /* set_save: equal to the default it is removed, different it is written */
    check(set_save(&s, SEC_GENERAL, KEY_MAXREADS, L"3", L"7"), "set_save different: writes");
    check(set_int(&s, SEC_GENERAL, KEY_MAXREADS, 3) == 7, "set_save different: the user wins");
    check(set_save(&s, SEC_GENERAL, KEY_MAXREADS, L"3", L"5"), "set_save equal to the installer default");
    GetPrivateProfileStringW(SEC_GENERAL, KEY_MAXREADS, L"(manca)", v, 512, user);
    check_str(v, L"(manca)", "set_save equal: the user key is removed");
    check(set_save(&s, SEC_OPTICAL, KEY_CDNAME, L"CD{nn}", L"CD{nn}"), "set_save equal to the code");
    GetPrivateProfileStringW(SEC_OPTICAL, KEY_CDNAME, L"(manca)", v, 512, user);
    check_str(v, L"(manca)", "set_save equal to the code: not written");
    WritePrivateProfileStringW(SEC_GENERAL, KEY_MAXREADS, L"4", def);
    check(set_int(&s, SEC_GENERAL, KEY_MAXREADS, 3) == 4, "a new default reaches users who did not change it");
    {   /* no user ini and an equal choice: the file is not created */
        settings t;
        wchar_t u2[MAX_PATH];
        _snwprintf(u2, MAX_PATH, L"%ls\\tmp_settings_mai\\RetroArchiver.ini", root);
        set_init_paths(&t, def, u2);
        set_save(&t, SEC_GENERAL, KEY_MAXREADS, L"3", L"4");
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
    test_old_ini(root);
    test_prefs(root);
    test_secrets(root);
    printf("settings: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
