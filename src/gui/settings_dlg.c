/* The Settings window: five tabs (General, Floppy, CD and DVD, Automatic,
 * Archive.org) over a copy of the settings. The layout is in
 * retroarchiver.rc, one child dialog per tab; the texts come from tr(). OK
 * checks every value and only then hands them back: saving them is main.c's
 * job (prefs_save).
 *
 * The Archive.org keys are the one exception: they are not a pref but a
 * secret of the user's ini (settings.c), so the dialog reads them when it
 * opens and writes them itself on OK, only if they were touched. "Test the
 * keys" asks Archive.org in a thread, as About asks the programs' versions. */
#include "gui/dialogs.h"
#include "gui/resource.h"
#include "gui/lang.h"
#include "gui/winutil.h"
#include "core/iakeys.h"
#include "core/media.h"
#include "core/meta.h"
#include "core/strbuf.h"
#include "core/version.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

/* where the drives' read offsets are listed (the redump one has no fixed address) */
#define OFFSET_URL L"http://www.accuraterip.com/driveoffsets.htm"

/* the CA bundle: the setup puts Mozilla's roots here, next to the exe, and
 * a copy in the user's folder wins over it (tls.c reads the file) */
#define CACERT_REL L"certs\\cacert.pem"
#define CACERT_NAME L"cacert.pem"

#define WM_APP_IA_TEST (WM_APP + 2)      /* lParam: ia_test*, the thread's */

typedef struct {
    prefs *p;                            /* the values being edited */
    const settings *set;
    const ui_lang *langs;
    int nlangs, page;
    HWND tabs, pages[PAGE_COUNT];
    char noleadin[PREFS_LIST];           /* the list, with the drives forgotten so far */
    int keys_state;                      /* SET_SECRET_* of the keys found in the ini */
    int keys_touched;                    /* the user changed them: write them on OK */
    int testing;                         /* a test is on its way */
} sdlg;

static const int page_dlg[PAGE_COUNT] = {
    IDD_PG_GENERAL, IDD_PG_FLOPPY, IDD_PG_OPTICAL, IDD_PG_AUTO, IDD_PG_IA
};
static const int page_title[PAGE_COUNT] = {
    T_SET_TAB_GENERAL, T_SET_TAB_FLOPPY, T_SET_TAB_OPTICAL, T_SET_TAB_AUTO, T_SET_TAB_IA
};

/* every text of the tabs; the programs' names are the same in every language */
static const struct { int page, id, text; } texts[] = {
    { PAGE_GENERAL, IDC_UILANG_L,   T_SETUP_UILANG },
    { PAGE_GENERAL, IDC_UILANG_H,   T_SET_UILANG_H },
    { PAGE_GENERAL, IDC_FOLDER_L,   T_SET_FOLDER },
    { PAGE_GENERAL, IDC_MAXREADS_L, T_SETUP_MAXREAD },
    { PAGE_GENERAL, IDC_MAXREADS_H, T_SET_MAXREADS_H },
    { PAGE_GENERAL, IDC_SOUNDS,     T_SETUP_SOUNDS },
    { PAGE_GENERAL, IDC_SOUNDS_H,   T_SET_SOUNDS_H },
    { PAGE_FLOPPY,  IDC_SAMDISK_H,  T_SET_SAMDISK_H },
    { PAGE_FLOPPY,  IDC_READFROM_L, T_SETUP_READFROM },
    { PAGE_FLOPPY,  IDC_READFROM_H, T_SET_READFROM_H },
    { PAGE_FLOPPY,  IDC_FNAME_L,    T_SET_FILENAME },
    { PAGE_FLOPPY,  IDC_FNAME_H,    T_SET_FLOPPYNAME_H },
    { PAGE_OPTICAL, IDC_DRIVE_L,    T_SET_DRIVE },
    { PAGE_OPTICAL, IDC_CDSPEED_L,  T_SET_CDSPEED },
    { PAGE_OPTICAL, IDC_CDSPEED_U,  T_SET_CDSPEED_U },
    { PAGE_OPTICAL, IDC_CDNAME_L,   T_SET_CDNAME },
    { PAGE_OPTICAL, IDC_DVDNAME_L,  T_SET_DVDNAME },
    { PAGE_OPTICAL, IDC_DAT_L,      T_SET_DAT },
    { PAGE_OPTICAL, IDC_DAT_H,      T_SET_DAT_H },
    { PAGE_OPTICAL, IDC_OFFSET_L,   T_SET_OFFSET },
    { PAGE_OPTICAL, IDC_OFFSET_H,   T_SET_OFFSET_H },
    { PAGE_OPTICAL, IDC_NOLEADIN_L, T_SET_NOLEADIN },
    { PAGE_OPTICAL, IDC_FORGET,     T_SET_FORGET },
    { PAGE_AUTO,    IDC_AUTO_INTRO, T_SET_AUTO_INTRO },
    { PAGE_AUTO,    IDC_COUNT_L,    T_SET_COUNTDOWN },
    { PAGE_AUTO,    IDC_COUNT_U,    T_SET_SECONDS },
    { PAGE_AUTO,    IDC_COUNT_H,    T_SET_COUNTDOWN_H },
    { PAGE_AUTO,    IDC_POLL_L,     T_SET_POLL },
    { PAGE_AUTO,    IDC_POLL_U,     T_SET_SECONDS },
    { PAGE_AUTO,    IDC_POLL_H,     T_SET_POLL_H },
    { PAGE_AUTO,    IDC_AUTO_NOTE,  T_SET_AUTO_NOTE },
    { PAGE_IA,      IDC_IA_INTRO,   T_IA_INTRO },
    { PAGE_IA,      IDC_IA_ACCESS_L, T_IA_ACCESS },
    { PAGE_IA,      IDC_IA_SECRET_L, T_IA_SECRET },
    { PAGE_IA,      IDC_IA_TEST,    T_IA_TEST },
    { PAGE_IA,      IDC_IA_FORGET,  T_IA_FORGET },
    { PAGE_IA,      IDC_IA_COLL_L,  T_IA_COLL },
    { PAGE_IA,      IDC_IA_COLL_H,  T_IA_COLL_H },
    { -1, 0, 0 }
};

/* the text fields and the setting each one edits */
#define OFF(f) offsetof(prefs, f), (int)(sizeof(((prefs *)0)->f) / sizeof(wchar_t))
static const struct { int page, id; size_t off; int n; } fields[] = {
    { PAGE_GENERAL, IDC_FOLDER,   OFF(folder) },
    { PAGE_FLOPPY,  IDC_SAMDISK,  OFF(samdisk) },
    { PAGE_FLOPPY,  IDC_READFROM, OFF(readfrom) },
    { PAGE_FLOPPY,  IDC_FNAME,    OFF(floppy_name) },
    { PAGE_OPTICAL, IDC_DIC,      OFF(dic) },
    { PAGE_OPTICAL, IDC_DRIVE,    OFF(drive) },
    { PAGE_OPTICAL, IDC_CDNAME,   OFF(cd_name) },
    { PAGE_OPTICAL, IDC_DVDNAME,  OFF(dvd_name) },
    { PAGE_OPTICAL, IDC_DAT,      OFF(datdir) },
    { PAGE_OPTICAL, IDC_OFFSET,   OFF(offset) },
    { PAGE_IA,      IDC_IA_COLL,  OFF(ia_collection) },
    { -1, 0, 0, 0 }
};
#undef OFF

/* the number fields, with their range */
static const struct { int page, id; size_t off; int min, max, bad; } numbers[] = {
    { PAGE_GENERAL, IDC_MAXREADS, offsetof(prefs, maxreads), PREFS_MAXREADS_MIN, META_MAXREAD, T_SET_BAD_MAXREADS },
    { PAGE_AUTO,    IDC_COUNT,    offsetof(prefs, countdown), PREFS_COUNTDOWN_MIN, PREFS_COUNTDOWN_MAX, T_SET_BAD_COUNTDOWN },
    { PAGE_AUTO,    IDC_POLL,     offsetof(prefs, floppy_poll), PREFS_POLL_MIN, PREFS_POLL_MAX, T_SET_BAD_POLL },
    { PAGE_OPTICAL, IDC_CDSPEED,  offsetof(prefs, cd_speed), PREFS_CDSPEED_MIN, PREFS_CDSPEED_MAX, T_SET_BAD_CDSPEED },
    { -1, 0, 0, 0, 0, 0 }
};

static wchar_t *field_at(prefs *p, int i) { return (wchar_t *)((char *)p + fields[i].off); }
static int *number_at(prefs *p, int i) { return (int *)((char *)p + numbers[i].off); }

/* ---- the tabs' background: themed on XP, as in a property sheet ---- */

static void theme_page(HWND page)
{
    typedef HRESULT (WINAPI *fn)(HWND, DWORD);
    static fn enable;
    static int tried;
    if (!tried) {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        tried = 1;
        if (ux) enable = (fn)(void (*)(void))GetProcAddress(ux, "EnableThemeDialogTexture");
    }
    if (enable) enable(page, 6);         /* ETDT_ENABLETAB */
}

/* ---- browsing ---- */

static void browse_folder(HWND page, int id, int title)
{
    BROWSEINFOW bi; LPITEMIDLIST pidl; wchar_t path[MAX_PATH];
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = page;
    bi.lpszTitle = tr(title);
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!(pidl = SHBrowseForFolderW(&bi))) return;
    if (SHGetPathFromIDListW(pidl, path)) SetDlgItemTextW(page, id, path);
    CoTaskMemFree(pidl);
}

/* "Programs (*.exe)" + "*.exe" + "All files" + "*.*", each ended by a 0 */
static void filter(wchar_t *out, int n, int name, const wchar_t *pattern)
{
    int k = _snwprintf(out, n - 2, L"%ls|%ls|%ls|*.*|", tr(name), pattern, tr(T_SET_FILTER_ALL));
    int i;
    if (k < 0) k = n - 3;
    out[k] = 0;
    out[k + 1] = 0;
    for (i = 0; i < k; i++) if (out[i] == L'|') out[i] = 0;
}

static void browse_file(HWND page, int id, int name, const wchar_t *pattern)
{
    OPENFILENAMEW of; wchar_t path[MAX_PATH], filt[256];
    GetDlgItemTextW(page, id, path, MAX_PATH);
    if (wcschr(path, L':') == path + 1 && !path[2]) path[0] = 0;   /* "a:" is not a file */
    filter(filt, 256, name, pattern);
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = page;
    of.lpstrFilter = filt;
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&of)) SetDlgItemTextW(page, id, path);
}

/* ---- the CD drives and the drives that cannot read the lead-in ---- */

static void drives_fill(HWND page, const wchar_t *cur)
{
    HWND cb = GetDlgItem(page, IDC_DRIVE);
    DWORD mask = GetLogicalDrives();
    wchar_t root[4] = L"A:\\", name[3] = L"A:";
    int i;
    for (i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        root[0] = name[0] = (wchar_t)(L'A' + i);
        if (GetDriveTypeW(root) == DRIVE_CDROM) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)name);
    }
    SetWindowTextW(cb, cur);
}

static void noleadin_fill(HWND page, const sdlg *d)
{
    HWND lb = GetDlgItem(page, IDC_NOLEADIN);
    char m[200]; wchar_t w[200];
    int i;
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (i = 0; optical_list_item(d->noleadin, i, m, sizeof(m)); i++) {
        to_w(m, w, 200);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)w);
    }
    if (!i) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)tr(T_SET_NOLEADIN_NONE));
    EnableWindow(lb, i > 0);
    EnableWindow(GetDlgItem(page, IDC_FORGET), 0);
}

static void noleadin_forget(HWND page, sdlg *d)
{
    HWND lb = GetDlgItem(page, IDC_NOLEADIN);
    int k = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    wchar_t w[200]; char m[200];
    if (k < 0 || SendMessageW(lb, LB_GETTEXTLEN, k, 0) >= 200) return;
    SendMessageW(lb, LB_GETTEXT, k, (LPARAM)w);
    to_u8(w, m, sizeof(m));
    optical_list_remove(d->noleadin, sizeof(d->noleadin), m);
    noleadin_fill(page, d);
}

static void trim(wchar_t *s)
{
    size_t k = wcslen(s), i = 0;
    while (k && (s[k - 1] == L' ' || s[k - 1] == L'\t')) s[--k] = 0;
    while (s[i] == L' ' || s[i] == L'\t') i++;
    if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof(wchar_t));
}

static void put_w(wchar_t *out, int n, const wchar_t *s)
{
    wcsncpy(out, s, n);
    out[n - 1] = 0;
}

/* ---- the Archive.org keys ---- */

/* the folder of the user's ini: where a cacert.pem of theirs goes, and
 * what the messages name. The literal if its place is unknown */
static void user_dir(const sdlg *d, wchar_t *out, int n)
{
    wchar_t *p;
    if (!d->set || !d->set->user[0]) { put_w(out, n, L"%APPDATA%\\RetroArchiver"); return; }
    put_w(out, n, d->set->user);
    if ((p = wcsrchr(out, L'\\')) != NULL) *p = 0;
}

/* the CA bundle for tls.c: the user's copy wins over the setup's; "" if
 * neither is there, and then the window says to reinstall */
static void cacert_file(const sdlg *d, char *out, int n)
{
    wchar_t dir[MAX_PATH], w[MAX_PATH];
    out[0] = 0;
    user_dir(d, dir, MAX_PATH);
    if ((int)(wcslen(dir) + wcslen(CACERT_NAME) + 2) < MAX_PATH) {
        _snwprintf(w, MAX_PATH, L"%ls\\%ls", dir, CACERT_NAME);
        w[MAX_PATH - 1] = 0;
        if (file_exists(w)) { to_u8(w, out, n); return; }
    }
    exe_path(CACERT_REL, w, MAX_PATH);
    if (w[0] && file_exists(w)) to_u8(w, out, n);
}

/* a Unix time as the language writes dates */
static void when_text(long long t, wchar_t *out, int n)
{
    time_t tt = (time_t)t;
    struct tm *g = gmtime(&tt);
    char iso[40] = "";
    if (g) snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d", g->tm_year + 1900,
                    g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
    lang_date(iso, out, (size_t)n);
}

static void ia_status(HWND page, const wchar_t *s) { SetDlgItemTextW(page, IDC_IA_STATUS, s); }

/* what the keys found in the ini say, before any test */
static void ia_status_stored(HWND page, const sdlg *d)
{
    ia_status(page, tr(d->keys_state == SET_SECRET_BAD ? T_IA_ST_BAD :
                       d->keys_state == SET_SECRET_OK ? T_IA_ST_SAVED : T_IA_ST_NONE));
}

/* a TLS failure: its own text, with the dates when it is the clock */
static void ia_tls_text(const sdlg *d, const tls_info *ti, wchar_t *out, int n)
{
    wchar_t a[64], b[64], c[64], dir[MAX_PATH];
    switch (ti->why) {
    case TLS_ERR_CLOCK:
        when_text(ti->now, a, 64);
        when_text(ti->not_before, b, 64);
        if (!ti->not_after) {
            _snwprintf(out, n, tr(T_IA_ST_CLOCK_OLD), a, b);
        } else {
            when_text(ti->not_after, c, 64);
            _snwprintf(out, n, tr(T_IA_ST_CLOCK), a, b, c);
        }
        break;
    case TLS_ERR_UNKNOWN_ROOT:
        user_dir(d, dir, MAX_PATH);
        _snwprintf(out, n, tr(T_IA_ST_ROOT), dir);
        break;
    case TLS_ERR_BAD_NAME: put_w(out, n, tr(T_IA_ST_NAME)); break;
    case TLS_ERR_NET:      put_w(out, n, tr(T_IA_ST_NET)); break;
    default:               put_w(out, n, tr(T_IA_ST_TLS)); break;
    }
    out[n - 1] = 0;
}

static void ia_result_text(const sdlg *d, const ia_test *t, wchar_t *out, int n)
{
    wchar_t name[128], dir[MAX_PATH];
    switch (t->why) {
    case IA_TEST_OK:
        if (!t->screenname[0]) { put_w(out, n, tr(T_IA_ST_OK)); break; }
        to_w(t->screenname, name, 128);
        _snwprintf(out, n, tr(T_IA_ST_OKNAME), name);
        break;
    case IA_TEST_REFUSED: put_w(out, n, tr(T_IA_ST_REFUSED)); break;
    case IA_TEST_NO_CERTS:
        user_dir(d, dir, MAX_PATH);
        _snwprintf(out, n, tr(T_IA_ST_NOCERTS), dir);
        break;
    case IA_TEST_TLS: ia_tls_text(d, &t->tls, out, n); break;
    case IA_TEST_HTTP: _snwprintf(out, n, tr(T_IA_ST_HTTP), t->status); break;
    default: put_w(out, n, tr(T_IA_ST_NET)); break;
    }
    out[n - 1] = 0;
}

/* the test, in a thread of its own: the keys live in the job and nowhere
 * else, and go when it ends */
typedef struct { HWND page; char cacert[3 * MAX_PATH]; ia_keys k; } ia_job;

static DWORD WINAPI ia_worker(LPVOID p)
{
    ia_job *job = (ia_job *)p;
    ia_test *t = (ia_test *)calloc(1, sizeof(ia_test));
    if (t) {
        /* the fake Archive.org of the tests answers at this address */
        const char *base = getenv("RETROARCHIVER_IA_URL");
        ia_test_keys(base && base[0] ? base : NULL, job->cacert, job->k.access, job->k.secret, t);
        if (!PostMessageW(job->page, WM_APP_IA_TEST, 0, (LPARAM)t)) free(t);
    }
    SecureZeroMemory(job, sizeof(*job));
    free(job);
    return 0;
}

static void ia_test_start(HWND page, sdlg *d)
{
    wchar_t a[IA_KEY_MAX], s[IA_KEY_MAX];
    ia_job *job;
    HANDLE th;
    if (d->testing) return;
    GetDlgItemTextW(page, IDC_IA_ACCESS, a, IA_KEY_MAX);
    GetDlgItemTextW(page, IDC_IA_SECRET, s, IA_KEY_MAX);
    trim(a);
    trim(s);
    if (!a[0] || !s[0]) { ia_status(page, tr(T_IA_ST_EMPTY)); return; }
    if ((job = (ia_job *)calloc(1, sizeof(ia_job))) == NULL) return;
    job->page = page;
    to_u8(a, job->k.access, IA_KEY_MAX);
    to_u8(s, job->k.secret, IA_KEY_MAX);
    cacert_file(d, job->cacert, (int)sizeof(job->cacert));
    SecureZeroMemory(a, sizeof(a));
    SecureZeroMemory(s, sizeof(s));
    if ((th = CreateThread(NULL, 0, ia_worker, job, 0, NULL)) == NULL) {
        SecureZeroMemory(job, sizeof(*job));
        free(job);
        return;
    }
    CloseHandle(th);
    d->testing = 1;
    EnableWindow(GetDlgItem(page, IDC_IA_TEST), 0);
    ia_status(page, tr(T_IA_ST_TESTING));
}

static void ia_test_done(HWND page, sdlg *d, ia_test *t)
{
    wchar_t w[800];
    d->testing = 0;
    EnableWindow(GetDlgItem(page, IDC_IA_TEST), 1);
    ia_result_text(d, t, w, 800);
    ia_status(page, w);
    free(t);
}

static void ia_forget(HWND page, sdlg *d)
{
    SetDlgItemTextW(page, IDC_IA_ACCESS, L"");
    SetDlgItemTextW(page, IDC_IA_SECRET, L"");
    d->keys_touched = 1;
    ia_status(page, tr(T_IA_ST_NONE));
}

/* the keys the ini has, so that OK does not lose them */
static void ia_page_values(HWND page, sdlg *d)
{
    ia_keys k;
    wchar_t w[400], v[IA_KEY_MAX];
    SendDlgItemMessageW(page, IDC_IA_ACCESS, EM_LIMITTEXT, IA_KEY_MAX - 1, 0);
    SendDlgItemMessageW(page, IDC_IA_SECRET, EM_LIMITTEXT, IA_KEY_MAX - 1, 0);
    _snwprintf(w, 400, L"<a href=\"%ls\">%ls</a>", IA_KEYS_URL, tr(T_IA_KEYSLINK));
    w[399] = 0;
    SetDlgItemTextW(page, IDC_IA_LINK, w);
    d->keys_state = d->set ? set_ia_keys_get(d->set, &k) : SET_SECRET_NONE;
    if (d->keys_state == SET_SECRET_OK) {
        to_w(k.access, v, IA_KEY_MAX);
        SetDlgItemTextW(page, IDC_IA_ACCESS, v);
        to_w(k.secret, v, IA_KEY_MAX);
        SetDlgItemTextW(page, IDC_IA_SECRET, v);
    }
    ia_keys_wipe(&k);
    SecureZeroMemory(v, sizeof(v));
    ia_status_stored(page, d);
    d->keys_touched = 0;                 /* filling the fields is not the user typing */
}

/* ---- one tab ---- */

static void page_texts(HWND page, int pg)
{
    wchar_t link[300];
    int i;
    for (i = 0; texts[i].page >= 0; i++)
        if (texts[i].page == pg) SetDlgItemTextW(page, texts[i].id, tr(texts[i].text));
    if (pg == PAGE_FLOPPY) SetDlgItemTextW(page, IDC_SAMDISK_L, L"SAMdisk");
    if (pg == PAGE_OPTICAL) {
        SetDlgItemTextW(page, IDC_DIC_L, L"DiscImageCreator");
        _snwprintf(link, 300, L"<a href=\"%ls\">%ls</a>", OFFSET_URL, tr(T_SET_OFFSET_LINK));
        link[299] = 0;
        SetDlgItemTextW(page, IDC_OFFSET_LINK, link);
    }
}

static void lang_fill(HWND page, const sdlg *d)
{
    HWND cb = GetDlgItem(page, IDC_UILANG);
    int i, sel = 0;
    for (i = 0; i < d->nlangs; i++) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)d->langs[i].name);
        if (!_wcsicmp(d->langs[i].code, d->p->uilang)) sel = i;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

static void page_values(HWND page, int pg, sdlg *d)
{
    int i;
    for (i = 0; fields[i].page >= 0; i++)
        if (fields[i].page == pg && fields[i].id != IDC_DRIVE) {
            SendDlgItemMessageW(page, fields[i].id, EM_LIMITTEXT, fields[i].n - 1, 0);
            SetDlgItemTextW(page, fields[i].id, field_at(d->p, i));
        }
    for (i = 0; numbers[i].page >= 0; i++)
        if (numbers[i].page == pg) {
            SendDlgItemMessageW(page, numbers[i].id, EM_LIMITTEXT, 2, 0);
            SetDlgItemInt(page, numbers[i].id, (UINT)*number_at(d->p, i), FALSE);
        }
    if (pg == PAGE_GENERAL) {
        lang_fill(page, d);
        CheckDlgButton(page, IDC_SOUNDS, d->p->sounds ? BST_CHECKED : BST_UNCHECKED);
    }
    if (pg == PAGE_OPTICAL) {
        drives_fill(page, d->p->drive);
        noleadin_fill(page, d);
    }
    if (pg == PAGE_IA) ia_page_values(page, d);
}

static void link_open(const NMHDR *h)
{
    const NMLINK *l = (const NMLINK *)h;
    if ((h->code == NM_CLICK || h->code == NM_RETURN) && l->item.szUrl[0])
        ShellExecuteW(h->hwndFrom, L"open", l->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
}

static int page_command(HWND page, sdlg *d, WPARAM wp)
{
    switch (LOWORD(wp)) {
    case IDC_FOLDER_B:   browse_folder(page, IDC_FOLDER, T_BROWSE_DIR); return 1;
    case IDC_DAT_B:      browse_folder(page, IDC_DAT, T_SET_DAT_BROWSE); return 1;
    case IDC_SAMDISK_B:  browse_file(page, IDC_SAMDISK, T_SET_FILTER_EXE, L"*.exe"); return 1;
    case IDC_DIC_B:      browse_file(page, IDC_DIC, T_SET_FILTER_EXE, L"*.exe"); return 1;
    case IDC_READFROM_B: browse_file(page, IDC_READFROM, T_SET_FILTER_DSK, L"*.dsk"); return 1;
    case IDC_FORGET:     noleadin_forget(page, d); return 1;
    case IDC_IA_TEST:    ia_test_start(page, d); return 1;
    case IDC_IA_FORGET:  ia_forget(page, d); return 1;
    case IDC_IA_ACCESS:
    case IDC_IA_SECRET:
        if (HIWORD(wp) == EN_CHANGE) d->keys_touched = 1;
        return 1;
    case IDC_NOLEADIN:
        if (HIWORD(wp) == LBN_SELCHANGE)
            EnableWindow(GetDlgItem(page, IDC_FORGET),
                         SendDlgItemMessageW(page, IDC_NOLEADIN, LB_GETCURSEL, 0, 0) >= 0);
        return 1;
    }
    return 0;
}

static INT_PTR CALLBACK page_proc(HWND page, UINT m, WPARAM wp, LPARAM lp)
{
    sdlg *d = (sdlg *)GetWindowLongPtrW(page, GWLP_USERDATA);
    switch (m) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(page, GWLP_USERDATA, lp);
        return FALSE;
    case WM_COMMAND:
        return d && page_command(page, d, wp);
    case WM_APP_IA_TEST:
        if (d) ia_test_done(page, d, (ia_test *)lp); else free((void *)lp);
        return TRUE;
    case WM_NOTIFY: {
        UINT_PTR id = ((NMHDR *)lp)->idFrom;
        if (id == IDC_OFFSET_LINK || id == IDC_IA_LINK) link_open((NMHDR *)lp);
        return TRUE;
    }
    }
    return FALSE;
}

/* ---- reading the values back, with the checks ---- */

static void show_page(sdlg *d, int pg)
{
    int i;
    for (i = 0; i < PAGE_COUNT; i++) ShowWindow(d->pages[i], i == pg ? SW_SHOW : SW_HIDE);
}

/* a refused value: its tab in front, the focus on it, what to write instead */
static int refuse(sdlg *d, int pg, int id, const wchar_t *why)
{
    TabCtrl_SetCurSel(d->tabs, pg);
    show_page(d, pg);
    MessageBoxW(GetParent(d->pages[pg]), why, L"RetroArchiver", MB_ICONEXCLAMATION);
    SetFocus(GetDlgItem(d->pages[pg], id));
    SendDlgItemMessageW(d->pages[pg], id, EM_SETSEL, 0, -1);
    return 0;
}

static int read_numbers(sdlg *d, prefs *p)
{
    wchar_t w[300];
    BOOL ok;
    int i, v;
    for (i = 0; numbers[i].page >= 0; i++) {
        v = (int)GetDlgItemInt(d->pages[numbers[i].page], numbers[i].id, &ok, FALSE);
        if (!ok || v < numbers[i].min || v > numbers[i].max) {
            _snwprintf(w, 300, tr(numbers[i].bad), numbers[i].min, numbers[i].max);
            w[299] = 0;
            return refuse(d, numbers[i].page, numbers[i].id, w);
        }
        *number_at(p, i) = v;
    }
    return 1;
}

/* "d", "d:", " D:\ " -> "D:"; "" is the default D: */
static int drive_ok(wchar_t *drive)
{
    wchar_t c = drive[0];
    if (!c) { wcscpy(drive, L"D:"); return 1; }
    if (!iswalpha(c) || (drive[1] && wcscmp(drive + 1, L":") && wcscmp(drive + 1, L":\\"))) return 0;
    drive[0] = (wchar_t)towupper(c);
    wcscpy(drive + 1, L":");
    return 1;
}

static int dir_exists(const wchar_t *dir)
{
    DWORD a = GetFileAttributesW(dir);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* the values that must have a given shape */
static int check_fields(sdlg *d, prefs *p)
{
    wchar_t w[400];
    char u8[64], off[16];
    if (!p->folder[0]) return refuse(d, PAGE_GENERAL, IDC_FOLDER, tr(T_MISS_FOLDER));
    if (!drive_ok(p->drive)) return refuse(d, PAGE_OPTICAL, IDC_DRIVE, tr(T_SET_BAD_DRIVE));
    if (p->datdir[0] && !dir_exists(p->datdir)) {
        _snwprintf(w, 400, tr(T_SET_BAD_DAT), p->datdir);
        w[399] = 0;
        return refuse(d, PAGE_OPTICAL, IDC_DAT, w);
    }
    to_u8(p->offset, u8, sizeof(u8));
    if (optical_parse_offset(u8, off, sizeof(off))) {
        _snwprintf(w, 400, tr(T_SET_BAD_OFFSET), p->offset);
        w[399] = 0;
        return refuse(d, PAGE_OPTICAL, IDC_OFFSET, w);
    }
    to_w(off, p->offset, 16);           /* as DIC wants it: "+6" */
    return 1;
}

static int read_all(sdlg *d, prefs *p)
{
    int i, k;
    *p = *d->p;
    for (i = 0; fields[i].page >= 0; i++) {
        GetDlgItemTextW(d->pages[fields[i].page], fields[i].id, field_at(p, i), fields[i].n);
        trim(field_at(p, i));
    }
    if (!read_numbers(d, p) || !check_fields(d, p)) return 0;
    p->sounds = IsDlgButtonChecked(d->pages[PAGE_GENERAL], IDC_SOUNDS) == BST_CHECKED;
    k = (int)SendDlgItemMessageW(d->pages[PAGE_GENERAL], IDC_UILANG, CB_GETCURSEL, 0, 0);
    if (k >= 0 && k < d->nlangs) wcscpy(p->uilang, d->langs[k].code);
    to_w(d->noleadin, p->noleadin, PREFS_LIST);
    return 1;
}

/* On OK: the keys, only if the user touched them. One of the two alone is
 * no use, so it is refused like a bad value */
static int ia_commit(sdlg *d)
{
    ia_keys k;
    wchar_t a[IA_KEY_MAX], s[IA_KEY_MAX];
    if (!d->keys_touched || !d->set || !d->pages[PAGE_IA]) return 1;
    GetDlgItemTextW(d->pages[PAGE_IA], IDC_IA_ACCESS, a, IA_KEY_MAX);
    GetDlgItemTextW(d->pages[PAGE_IA], IDC_IA_SECRET, s, IA_KEY_MAX);
    trim(a);
    trim(s);
    if (!a[0] != !s[0]) {
        SecureZeroMemory(s, sizeof(s));
        return refuse(d, PAGE_IA, a[0] ? IDC_IA_SECRET : IDC_IA_ACCESS, tr(T_IA_ST_EMPTY));
    }
    memset(&k, 0, sizeof(k));
    to_u8(a, k.access, IA_KEY_MAX);
    to_u8(s, k.secret, IA_KEY_MAX);
    /* an ini that cannot be written is said once, by main.c after prefs_save */
    set_ia_keys_put(d->set, &k);
    ia_keys_wipe(&k);
    SecureZeroMemory(a, sizeof(a));
    SecureZeroMemory(s, sizeof(s));
    return 1;
}

/* ---- the dialog with the tabs ---- */

/* the tabs as child dialogs over the tab control's display area, right
 * after it in the Tab order */
static void make_pages(HWND dlg, sdlg *d)
{
    RECT r; TCITEMW it; int i;
    d->tabs = GetDlgItem(dlg, IDC_TABS);
    for (i = 0; i < PAGE_COUNT; i++) {
        memset(&it, 0, sizeof(it));
        it.mask = TCIF_TEXT;
        it.pszText = (wchar_t *)tr(page_title[i]);
        SendMessageW(d->tabs, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    GetWindowRect(d->tabs, &r);
    MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
    TabCtrl_AdjustRect(d->tabs, FALSE, &r);
    for (i = PAGE_COUNT - 1; i >= 0; i--) {
        d->pages[i] = CreateDialogParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(page_dlg[i]), dlg,
                                         page_proc, (LPARAM)d);
        if (!d->pages[i]) continue;
        theme_page(d->pages[i]);
        page_texts(d->pages[i], i);
        page_values(d->pages[i], i, d);
        SetWindowPos(d->pages[i], d->tabs, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    }
}

static void dlg_init(HWND dlg, sdlg *d)
{
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
    SetWindowTextW(dlg, tr(T_SET_TITLE));
    SetDlgItemTextW(dlg, IDOK, tr(T_BTN_OK));
    SetDlgItemTextW(dlg, IDCANCEL, tr(T_BTN_CANCEL));
    to_u8(d->p->noleadin, d->noleadin, sizeof(d->noleadin));
    make_pages(dlg, d);
    TabCtrl_SetCurSel(d->tabs, d->page);
    show_page(d, d->page);
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    static prefs out;                    /* a few KB: not on the stack */
    sdlg *d = (sdlg *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (m) {
    case WM_INITDIALOG:
        dlg_init(dlg, (sdlg *)lp);
        return TRUE;
    case WM_NOTIFY:
        if (d && ((NMHDR *)lp)->hwndFrom == d->tabs && ((NMHDR *)lp)->code == TCN_SELCHANGE)
            show_page(d, TabCtrl_GetCurSel(d->tabs));
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && d && read_all(d, &out) && ia_commit(d)) {
            *d->p = out;
            EndDialog(dlg, 1);
        } else if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, 0);
        }
        return TRUE;
    }
    return FALSE;
}

int settings_dialog(HWND owner, const settings *s, prefs *p,
                    const ui_lang *langs, int nlangs, int page)
{
    sdlg d;
    memset(&d, 0, sizeof(d));
    d.p = p;
    d.set = s;
    d.langs = langs;
    d.nlangs = nlangs;
    d.page = page >= 0 && page < PAGE_COUNT ? page : PAGE_GENERAL;
    return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                           dlg_proc, (LPARAM)&d) == 1;
}
