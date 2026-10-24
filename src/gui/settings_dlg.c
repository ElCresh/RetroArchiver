/* The Settings window: four tabs (General, Floppy, CD and DVD, Automatic)
 * over a copy of the settings. The layout is in retroarchiver.rc, one child
 * dialog per tab; the texts come from tr(). OK checks every value and only
 * then hands them back: saving them is main.c's job (prefs_save). */
#include "gui/dialogs.h"
#include "gui/resource.h"
#include "gui/lang.h"
#include "gui/winutil.h"
#include "core/media.h"
#include "core/meta.h"
#include "core/strbuf.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* where the drives' read offsets are listed (the redump one has no fixed address) */
#define OFFSET_URL L"http://www.accuraterip.com/driveoffsets.htm"

typedef struct {
    prefs *p;                            /* the values being edited */
    const ui_lang *langs;
    int nlangs, page;
    HWND tabs, pages[PAGE_COUNT];
    char noleadin[PREFS_LIST];           /* the list, with the drives forgotten so far */
} sdlg;

static const int page_dlg[PAGE_COUNT] = { IDD_PG_GENERAL, IDD_PG_FLOPPY, IDD_PG_OPTICAL, IDD_PG_AUTO };
static const int page_title[PAGE_COUNT] = { T_SET_TAB_GENERAL, T_SET_TAB_FLOPPY, T_SET_TAB_OPTICAL, T_SET_TAB_AUTO };

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
    { -1, 0, 0, 0 }
};
#undef OFF

/* the number fields, with their range */
static const struct { int page, id; size_t off; int min, max, bad; } numbers[] = {
    { PAGE_GENERAL, IDC_MAXREADS, offsetof(prefs, maxreads), PREFS_MAXREADS_MIN, META_MAXREAD, T_SET_BAD_MAXREADS },
    { PAGE_AUTO,    IDC_COUNT,    offsetof(prefs, countdown), PREFS_COUNTDOWN_MIN, PREFS_COUNTDOWN_MAX, T_SET_BAD_COUNTDOWN },
    { PAGE_AUTO,    IDC_POLL,     offsetof(prefs, floppy_poll), PREFS_POLL_MIN, PREFS_POLL_MAX, T_SET_BAD_POLL },
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
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == IDC_OFFSET_LINK) link_open((NMHDR *)lp);
        return TRUE;
    }
    return FALSE;
}

/* ---- reading the values back, with the checks ---- */

static void trim(wchar_t *s)
{
    size_t k = wcslen(s), i = 0;
    while (k && (s[k - 1] == L' ' || s[k - 1] == L'\t')) s[--k] = 0;
    while (s[i] == L' ' || s[i] == L'\t') i++;
    if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof(wchar_t));
}

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
        if (LOWORD(wp) == IDOK && d && read_all(d, &out)) {
            *d->p = out;
            EndDialog(dlg, 1);
        } else if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, 0);
        }
        return TRUE;
    }
    return FALSE;
}

int settings_dialog(HWND owner, prefs *p, const ui_lang *langs, int nlangs, int page)
{
    sdlg d;
    memset(&d, 0, sizeof(d));
    d.p = p;
    d.langs = langs;
    d.nlangs = nlangs;
    d.page = page >= 0 && page < PAGE_COUNT ? page : PAGE_GENERAL;
    return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                           dlg_proc, (LPARAM)&d) == 1;
}
