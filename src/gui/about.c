/* The About window: RetroArchiver's version, its page and its license,
 * and the third-party programs with the version found on this PC. The
 * versions are asked in a thread (SAMdisk and DiscImageCreator are run,
 * fdrawcmd.sys answers an IOCTL, the Visual C++ runtime is in the registry
 * and in its files), which posts them to the window: the window opens at
 * once. "Copy info" puts all of it on the clipboard as plain text, for a
 * bug report. */
#include "gui/dialogs.h"
#include "gui/resource.h"
#include "gui/lang.h"
#include "gui/winutil.h"
#include "core/media.h"
#include "core/version.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_APP_VERSIONS (WM_APP + 1)     /* lParam: ab_result* */

/* fdrawcmd.sys: its control device and the version question (fdrawcmd.h) */
#define FDRAW_CTL_CODE(i, m) CTL_CODE(FILE_DEVICE_UNKNOWN, i, m, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_FDRAWCMD_GET_VERSION FDRAW_CTL_CODE(0x888, METHOD_BUFFERED)

#define VCRT_KEY L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x86"

enum { AB_SAMDISK, AB_FDRAW, AB_DIC, AB_VCRT, AB_DAT, AB_ROWS };

/* the rows: name (a text id if 0), license (literal, or a text id), the
 * license file in licenses\ next to the exe */
static const struct { const wchar_t *name; int name_id; const wchar_t *lic; int lic_id; const wchar_t *file; } rows[AB_ROWS] = {
    { L"SAMdisk",          0,             L"MIT",        0,                L"samdisk-LICENSE.txt" },
    { L"fdrawcmd.sys",     0,             L"MIT",        0,                L"fdrawcmd-LICENSE.txt" },
    { L"DiscImageCreator", 0,             L"Apache 2.0", 0,                L"DiscImageCreator-LICENSE.txt" },
    { NULL,                T_AB_VCRT,     NULL,          T_AB_LIC_MS,      NULL },
    { NULL,                T_AB_DAT,      NULL,          T_AB_LIC_REDUMP,  NULL },
};

typedef struct { wchar_t ver[200], where[MAX_PATH]; } ab_row;
typedef struct { ab_row row[AB_ROWS]; } ab_result;
typedef struct { HWND dlg; about_tools t; } ab_job;

typedef struct {
    HFONT bold;
    ab_result found;
    int have;                            /* the versions have arrived */
} ab_state;

static const wchar_t *row_name(int i) { return rows[i].name ? rows[i].name : tr(rows[i].name_id); }
static const wchar_t *row_lic(int i) { return rows[i].lic ? rows[i].lic : tr(rows[i].lic_id); }

static void put(wchar_t *out, int n, const wchar_t *s) { wcsncpy(out, s, n); out[n - 1] = 0; }

/* ---- the questions, in the thread ---- */

/* where CreateProcess would find it: "samdisk" -> the full path */
static void resolve(const wchar_t *prog, wchar_t *out)
{
    wchar_t *part;
    if (!SearchPathW(NULL, prog, L".exe", MAX_PATH, out, &part)) put(out, MAX_PATH, prog);
}

static void probe_samdisk(const wchar_t *path, ab_row *r)
{
    wchar_t cmd[MAX_PATH + 4]; char out[4096], ver[32];
    _snwprintf(cmd, MAX_PATH + 4, L"\"%ls\"", path); cmd[MAX_PATH + 3] = 0;
    resolve(path, r->where);
    if (!run_capture(cmd, out, sizeof(out), NULL)) put(r->ver, 200, tr(T_AB_NOTFOUND));
    else if (!floppy_samdisk_series(out, ver, sizeof(ver))) put(r->ver, 200, tr(T_AB_UNKNOWN));
    else to_w(ver, r->ver, 200);
}

static void probe_dic(const wchar_t *path, ab_row *r)
{
    wchar_t cmd[MAX_PATH + 8]; char out[8192], ver[40]; DWORD code = 0;
    _snwprintf(cmd, MAX_PATH + 8, L"\"%ls\" /v", path); cmd[MAX_PATH + 7] = 0;
    resolve(path, r->where);
    if (!run_capture(cmd, out, sizeof(out), &code)) put(r->ver, 200, tr(T_AB_NOTFOUND));
    else if (exit_missing_dll(code)) put(r->ver, 200, tr(T_AB_DIC_NODLL));
    else if (!parse_dic_version(out, ver, sizeof(ver))) put(r->ver, 200, tr(T_AB_UNKNOWN));
    else to_w(ver, r->ver, 200);
}

/* 0x0100010b -> "1.0.1.11" */
static void probe_fdraw(ab_row *r)
{
    HANDLE h = CreateFileW(L"\\\\.\\fdrawcmd", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DWORD v = 0, got = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE) { put(r->ver, 200, tr(T_AB_NOTINSTALLED)); return; }
    ok = DeviceIoControl(h, IOCTL_FDRAWCMD_GET_VERSION, NULL, 0, &v, sizeof(v), &got, NULL) && got == sizeof(v);
    CloseHandle(h);
    if (!ok) { put(r->ver, 200, tr(T_AB_UNKNOWN)); return; }
    _snwprintf(r->ver, 200, L"%lu.%lu.%lu.%lu", v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    r->ver[199] = 0;
    put(r->where, MAX_PATH, L"\\\\.\\fdrawcmd");
}

/* a DLL's file version, through version.dll loaded here: it is not among
 * the DLLs the window links to. 0 if unknown */
static int file_version(const wchar_t *path, wchar_t *out, int n)
{
    typedef DWORD (WINAPI *size_fn)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *info_fn)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL (WINAPI *query_fn)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
    HMODULE m = LoadLibraryW(L"version.dll");
    size_fn size; info_fn info; query_fn query;
    VS_FIXEDFILEINFO *fi; UINT len; DWORD dummy, k; void *buf; int ok = 0;
    if (!m) return 0;
    size = (size_fn)(void (*)(void))GetProcAddress(m, "GetFileVersionInfoSizeW");
    info = (info_fn)(void (*)(void))GetProcAddress(m, "GetFileVersionInfoW");
    query = (query_fn)(void (*)(void))GetProcAddress(m, "VerQueryValueW");
    if (size && info && query && (k = size(path, &dummy)) > 0 && (buf = malloc(k)) != NULL) {
        if (info(path, 0, k, buf) && query(buf, L"\\", (void **)&fi, &len) && len >= sizeof(*fi)) {
            _snwprintf(out, n, L"%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                       HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            out[n - 1] = 0;
            ok = 1;
        }
        free(buf);
    }
    FreeLibrary(m);
    return ok;
}

/* the registry says which runtime the installer put; the Universal CRT is a
 * file of its own (on 7 and 8.1 it comes from Windows Update) */
static void probe_vcrt(ab_row *r)
{
    HKEY k; wchar_t reg[64] = L"", ucrt[40] = L"", path[MAX_PATH];
    DWORD type, len = sizeof(reg) - sizeof(wchar_t);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VCRT_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"Version", NULL, &type, (BYTE *)reg, &len) != ERROR_SUCCESS || type != REG_SZ)
            reg[0] = 0;
        RegCloseKey(k);
    }
    if (GetSystemDirectoryW(path, MAX_PATH - 16)) {
        wcscat(path, L"\\ucrtbase.dll");
        if (!file_version(path, ucrt, 40)) ucrt[0] = 0;
        put(r->where, MAX_PATH, path);
    }
    if (!reg[0] && !ucrt[0]) { put(r->ver, 200, tr(T_AB_NOTINSTALLED)); return; }
    _snwprintf(r->ver, 200, tr(T_AB_VCRT_VER), reg[0] == L'v' ? reg + 1 : reg[0] ? reg : L"?",
               ucrt[0] ? ucrt : tr(T_AB_UCRT_MISSING));
    r->ver[199] = 0;
}

static void probe_dat(ab_row *r)
{
    char path[1100], date[40]; wchar_t d[40];
    if (!optical_find_dat(path, sizeof(path), date, sizeof(date))) { put(r->ver, 200, tr(T_AB_DAT_NONE)); return; }
    to_w(date, d, 40);
    put(r->ver, 200, d[0] ? d : L"?");
    to_w(path, r->where, MAX_PATH);
}

static DWORD WINAPI probe_worker(LPVOID p)
{
    ab_job *job = (ab_job *)p;
    ab_result *r = (ab_result *)calloc(1, sizeof(ab_result));
    if (r) {
        probe_samdisk(job->t.samdisk, &r->row[AB_SAMDISK]);
        probe_fdraw(&r->row[AB_FDRAW]);
        probe_dic(job->t.dic, &r->row[AB_DIC]);
        probe_vcrt(&r->row[AB_VCRT]);
        probe_dat(&r->row[AB_DAT]);
        /* the window may have been closed in the meantime */
        if (!PostMessageW(job->dlg, WM_APP_VERSIONS, 0, (LPARAM)r)) free(r);
    }
    free(job);
    return 0;
}

static void probe_start(HWND dlg, const about_tools *t)
{
    ab_job *job = (ab_job *)calloc(1, sizeof(ab_job));
    HANDLE th;
    if (!job) return;
    job->dlg = dlg;
    job->t = *t;
    th = CreateThread(NULL, 0, probe_worker, job, 0, NULL);
    if (th) CloseHandle(th); else free(job);
}

/* ---- the window ---- */

static void license_text(HWND dlg)
{
    wchar_t w[300], lic[100], file[MAX_PATH];
    to_w(RA_LICENSE, lic, 100);
    if (lic[0]) _snwprintf(w, 300, tr(T_AB_LICENSE), lic);
    else put(w, 300, tr(T_AB_LICENSE_NONE));
    w[299] = 0;
    SetDlgItemTextW(dlg, IDC_AB_LICENSE, w);
    exe_path(RA_LICENSE_FILE, file, MAX_PATH);
    EnableWindow(GetDlgItem(dlg, IDC_AB_LICVIEW), file_exists(file));
}

static void list_init(HWND dlg)
{
    static const int titles[3] = { T_AB_COL_PROGRAM, T_AB_COL_VERSION, T_AB_COL_LICENSE };
    static const int widths[3] = { 160, 190, 150 };
    HWND lv = GetDlgItem(dlg, IDC_AB_LIST);
    LVCOLUMNW c; LVITEMW it; int i;
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
    for (i = 0; i < 3; i++) {
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (wchar_t *)tr(titles[i]);
        c.cx = widths[i];
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
    for (i = 0; i < AB_ROWS; i++) {
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (wchar_t *)row_name(i);
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = (wchar_t *)tr(T_AB_ASKING);
        SendMessageW(lv, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = (wchar_t *)row_lic(i);
        SendMessageW(lv, LVM_SETITEMTEXTW, i, (LPARAM)&it);
    }
}

static HFONT bold_font(HWND dlg)
{
    LOGFONTW lf;
    HFONT f = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
    if (!f || !GetObjectW(f, sizeof(lf), &lf)) return NULL;
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = lf.lfHeight * 3 / 2;
    return CreateFontIndirectW(&lf);
}

static void dlg_texts(HWND dlg, ab_state *st)
{
    static const struct { int id, text; } t[] = {
        { IDC_AB_TAGLINE, T_AB_TAGLINE }, { IDC_AB_LICVIEW, T_AB_LICVIEW }, { IDC_AB_TPTITLE, T_AB_TPTITLE },
        { IDC_AB_STATUS, T_AB_ASKING_LONG }, { IDC_AB_OPENLIC, T_AB_OPENLIC }, { IDC_AB_NOTICES, T_AB_NOTICES },
        { IDC_AB_COPY, T_AB_COPY }, { IDOK, T_BTN_CLOSE }, { 0, 0 }
    };
    wchar_t w[300];
    int i;
    _snwprintf(w, 300, tr(T_AB_TITLE), L"RetroArchiver"); w[299] = 0;
    SetWindowTextW(dlg, w);
    {
        wchar_t v[32];
        to_w(RA_VERSION, v, 32);
        _snwprintf(w, 300, L"RetroArchiver %ls", v); w[299] = 0;
    }
    SetDlgItemTextW(dlg, IDC_AB_NAME, w);
    if ((st->bold = bold_font(dlg)) != NULL) SendDlgItemMessageW(dlg, IDC_AB_NAME, WM_SETFONT, (WPARAM)st->bold, FALSE);
    for (i = 0; t[i].id; i++) SetDlgItemTextW(dlg, t[i].id, tr(t[i].text));
    _snwprintf(w, 300, L"<a href=\"%ls\">%ls</a>", RA_REPO_URL, RA_REPO_URL); w[299] = 0;
    SetDlgItemTextW(dlg, IDC_AB_LINK, w);
    license_text(dlg);
    EnableWindow(GetDlgItem(dlg, IDC_AB_OPENLIC), 0);
    EnableWindow(GetDlgItem(dlg, IDC_AB_COPY), 0);
}

static void on_versions(HWND dlg, ab_state *st, ab_result *r)
{
    HWND lv = GetDlgItem(dlg, IDC_AB_LIST);
    LVITEMW it; int i;
    st->found = *r;
    st->have = 1;
    free(r);
    for (i = 0; i < AB_ROWS; i++) {
        memset(&it, 0, sizeof(it));
        it.iSubItem = 1;
        it.pszText = st->found.row[i].ver;
        SendMessageW(lv, LVM_SETITEMTEXTW, i, (LPARAM)&it);
    }
    /* as wide as the longest text: a long one gets a scroll bar, not "..." */
    for (i = 0; i < 3; i++) SendMessageW(lv, LVM_SETCOLUMNWIDTH, i, LVSCW_AUTOSIZE_USEHEADER);
    SetDlgItemTextW(dlg, IDC_AB_STATUS, tr(T_AB_FOUND));
    EnableWindow(GetDlgItem(dlg, IDC_AB_COPY), 1);
}

/* the selected row's license file, "" if it has none or it is not installed */
static void row_license(HWND dlg, wchar_t *out)
{
    wchar_t rel[MAX_PATH];
    int i = (int)SendDlgItemMessageW(dlg, IDC_AB_LIST, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    out[0] = 0;
    if (i < 0 || i >= AB_ROWS || !rows[i].file) return;
    _snwprintf(rel, MAX_PATH, L"licenses\\%ls", rows[i].file); rel[MAX_PATH - 1] = 0;
    exe_path(rel, out, MAX_PATH);
    if (!file_exists(out)) out[0] = 0;
}

static void open_file(HWND dlg, const wchar_t *rel)
{
    wchar_t p[MAX_PATH], w[600];
    exe_path(rel, p, MAX_PATH);
    if (file_exists(p) && (INT_PTR)ShellExecuteW(dlg, L"open", p, NULL, NULL, SW_SHOWNORMAL) > 32) return;
    _snwprintf(w, 600, tr(T_OPEN_FAILED), p[0] ? p : rel); w[599] = 0;
    MessageBoxW(dlg, w, L"RetroArchiver", MB_ICONEXCLAMATION);
}

/* ---- Copy info ---- */

static void add(wchar_t *out, int n, const wchar_t *fmt, const wchar_t *a, const wchar_t *b, const wchar_t *c)
{
    size_t k = wcslen(out);
    if ((int)k < n - 1) _snwprintf(out + k, n - k, fmt, a, b, c);
    out[n - 1] = 0;
}

static void windows_text(wchar_t *out, int n)
{
    typedef BOOL (WINAPI *wow_fn)(HANDLE, PBOOL);
    OSVERSIONINFOW v; BOOL wow = FALSE;
    wow_fn f = (wow_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    GetVersionExW(&v);
    if (f) f(GetCurrentProcess(), &wow);
    _snwprintf(out, n, L"Windows %lu.%lu.%lu %ls (%ls)", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
               v.szCSDVersion, wow ? L"64 bit" : L"32 bit");
    out[n - 1] = 0;
}

static void info_text(HWND dlg, const ab_state *st, wchar_t *out, int n)
{
    wchar_t w[300], exe[MAX_PATH];
    int i;
    out[0] = 0;
    GetDlgItemTextW(dlg, IDC_AB_NAME, w, 300);
    add(out, n, L"%ls\r\n%ls\r\n", w, RA_REPO_URL, NULL);
    GetDlgItemTextW(dlg, IDC_AB_LICENSE, w, 300);
    add(out, n, L"%ls\r\n", w, NULL, NULL);
    windows_text(w, 300);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    add(out, n, L"%ls\r\n%ls\r\n\r\n", w, exe, NULL);
    add(out, n, L"%ls\r\n", tr(T_AB_TPTITLE), NULL, NULL);
    for (i = 0; i < AB_ROWS; i++) {
        add(out, n, L"%ls: %ls - %ls", row_name(i), st->found.row[i].ver, row_lic(i));
        if (st->found.row[i].where[0]) add(out, n, L" - %ls", st->found.row[i].where, NULL, NULL);
        add(out, n, L"\r\n", NULL, NULL, NULL);
    }
}

static void copy_info(HWND dlg, const ab_state *st)
{
    enum { N = 8192 };
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, N * sizeof(wchar_t));
    wchar_t *w;
    if (!g || !(w = (wchar_t *)GlobalLock(g))) { if (g) GlobalFree(g); return; }
    info_text(dlg, st, w, N);
    GlobalUnlock(g);
    if (OpenClipboard(dlg)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, g)) g = NULL;     /* now the clipboard's */
        CloseClipboard();
    }
    if (g) GlobalFree(g);
    else SetDlgItemTextW(dlg, IDC_AB_STATUS, tr(T_AB_COPIED));
}

/* ---- messages ---- */

static int on_notify(HWND dlg, const NMHDR *h)
{
    if (h->idFrom == IDC_AB_LINK && (h->code == NM_CLICK || h->code == NM_RETURN)) {
        ShellExecuteW(dlg, L"open", RA_REPO_URL, NULL, NULL, SW_SHOWNORMAL);
        return 1;
    }
    if (h->idFrom == IDC_AB_LIST && h->code == LVN_ITEMCHANGED) {
        wchar_t p[MAX_PATH];
        row_license(dlg, p);
        EnableWindow(GetDlgItem(dlg, IDC_AB_OPENLIC), p[0] != 0);
        return 1;
    }
    return 0;
}

static void on_command(HWND dlg, ab_state *st, int id)
{
    wchar_t p[MAX_PATH];
    switch (id) {
    case IDC_AB_LICVIEW: open_file(dlg, RA_LICENSE_FILE); break;
    case IDC_AB_NOTICES: open_file(dlg, L"licenses\\THIRD-PARTY-NOTICES.txt"); break;
    case IDC_AB_OPENLIC:
        row_license(dlg, p);
        if (p[0]) ShellExecuteW(dlg, L"open", p, NULL, NULL, SW_SHOWNORMAL);
        break;
    case IDC_AB_COPY: if (st->have) copy_info(dlg, st); break;
    case IDOK: case IDCANCEL: EndDialog(dlg, 0); break;
    }
}

static INT_PTR CALLBACK about_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    ab_state *st = (ab_state *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (m) {
    case WM_INITDIALOG: {
        const about_tools *t = (const about_tools *)lp;
        if (!(st = (ab_state *)calloc(1, sizeof(ab_state)))) { EndDialog(dlg, 0); return TRUE; }
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)st);
        dlg_texts(dlg, st);
        list_init(dlg);
        probe_start(dlg, t);
        return TRUE;
    }
    case WM_APP_VERSIONS:
        if (st) on_versions(dlg, st, (ab_result *)lp); else free((void *)lp);
        return TRUE;
    case WM_NOTIFY:
        return on_notify(dlg, (const NMHDR *)lp);
    case WM_COMMAND:
        if (st) on_command(dlg, st, LOWORD(wp));
        return TRUE;
    case WM_DESTROY:
        if (st) {
            if (st->bold) DeleteObject(st->bold);
            free(st);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
        }
        return FALSE;
    }
    return FALSE;
}

void about_dialog(HWND owner, const about_tools *t)
{
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_ABOUT), owner, about_proc, (LPARAM)t);
}
