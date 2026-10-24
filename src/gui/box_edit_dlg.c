/* Edit box: two tabs over a copy of a closed box. Box has its data, Media
 * its composition and the media to read again; Save hands them back,
 * Save and read with at least one medium ticked. Saving is main.c's job
 * (boxedit_save_data in a thread, then the session in edit mode). Also
 * the question "which read to keep" when two reads are as good. */
#include "gui/dialogs.h"
#include "gui/resource.h"
#include "gui/lang.h"
#include "gui/winutil.h"
#include "core/fsu8.h"
#include "core/media.h"
#include "core/session.h"
#include <commctrl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum { BE_DATA, BE_MEDIA, BE_PAGES };

typedef struct {
    const char *bagdir;
    const meta_box *cur;
    const wchar_t *const *platforms;
    box_edit_result *out;
    HWND tabs, pages[BE_PAGES], list;
    char ticked[MEDIA_KINDS][META_MAXDISK + 1];
    media_identity id[META_MAXDISK + 1];     /* how each medium of the box was read */
    int filling;                             /* the list is being rebuilt: no tick events */
} bedlg;

static const int page_dlg[BE_PAGES] = { IDD_BE_DATA, IDD_BE_MEDIA };
static const int page_title[BE_PAGES] = { T_EDIT_TAB_DATA, T_EDIT_TAB_MEDIA };
static const int kind_cap[MEDIA_KINDS] = { T_KIND_FLOPPY_CAP, T_KIND_CD_CAP, T_KIND_DVD_CAP };
static const int count_id[MEDIA_KINDS] = { IDC_BE_FLOPPY, IDC_BE_CD, IDC_BE_DVD };

static const struct { int page, id, text; } texts[] = {
    { BE_DATA,  IDC_BE_TITLE_L,  T_SETUP_TITLE },
    { BE_DATA,  IDC_BE_PLAT_L,   T_SETUP_PLATFORM },
    { BE_DATA,  IDC_BE_YEAR_L,   T_SETUP_YEAR },
    { BE_DATA,  IDC_BE_PUB_L,    T_SETUP_PUBLISHER },
    { BE_DATA,  IDC_BE_LANG_L,   T_SETUP_LANGUAGE },
    { BE_DATA,  IDC_BE_ED_L,     T_SETUP_EDITION },
    { BE_DATA,  IDC_BE_NOTES_L,  T_SETUP_NOTES },
    { BE_MEDIA, IDC_BE_FLOPPY_L, T_EDIT_COUNT_FLOPPY },
    { BE_MEDIA, IDC_BE_CD_L,     T_EDIT_COUNT_CD },
    { BE_MEDIA, IDC_BE_DVD_L,    T_EDIT_COUNT_DVD },
    { BE_MEDIA, IDC_BE_MHINT,    T_EDIT_MEDIA_HINT },
    { -1, 0, 0 }
};

/* the box's text fields and their controls */
#define FIELD(f) offsetof(meta_box, f), sizeof(((meta_box *)0)->f)
static const struct { int id; size_t off, n; } fields[] = {
    { IDC_BE_TITLE, FIELD(title) }, { IDC_BE_PLAT, FIELD(platform) }, { IDC_BE_YEAR, FIELD(year) },
    { IDC_BE_PUB, FIELD(publisher) }, { IDC_BE_LANG, FIELD(language) }, { IDC_BE_ED, FIELD(edition) },
    { IDC_BE_NOTES, FIELD(notes) }, { 0, 0, 0 }
};
#undef FIELD

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

static void trim_u8(char *s)
{
    size_t k = strlen(s), i = 0;
    while (k && (s[k - 1] == ' ' || s[k - 1] == '\t')) s[--k] = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void get_field(HWND page, int id, char *out, size_t n)
{
    wchar_t w[600];
    GetDlgItemTextW(page, id, w, 600);
    to_u8(w, out, (int)n);
    trim_u8(out);
}

/* an outcome as the archive's table writes it */
static const wchar_t *outcome_text(const char *s)
{
    static const struct { const char *st; int t; } map[] = {
        { ST_CLEAN, T_OUT_CLEAN }, { ST_MARGINAL, T_OUT_MARGINAL }, { ST_FRAGILE, T_OUT_FRAGILE },
        { ST_ERROR, T_OUT_ERROR }, { ST_REDUMP, T_OUT_REDUMP }, { ST_READ_ONCE, T_OUT_ONCE },
        { ST_UNSTABLE, T_OUT_UNSTABLE }, { ST_ERRORS, T_OUT_ERRORS }, { ST_FAILED, T_OUT_FAILED },
        { ST_INTERRUPTED, T_OUT_INTERRUPTED }, { ST_DUPLICATE, T_OUT_DUPLICATE }, { ST_MISSING, T_OUT_SKIPPED }
    };
    size_t i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (!strcmp(s, map[i].st)) return tr(map[i].t);
    return tr(T_OUT_TOREAD);
}

static const wchar_t *method_text(const char *m)
{
    if (!strcmp(m, "dic-cd")) return tr(T_EDIT_METHOD_CD);
    if (!strcmp(m, "dic-data")) return tr(T_EDIT_METHOD_DATA);
    return L"";
}

static const meta_disk *disk_of(const meta_box *b, int kind, int num)
{
    int i;
    for (i = 0; i < b->ndisk; i++) if (b->disk[i].kind == kind && b->disk[i].num == num) return &b->disk[i];
    return NULL;
}

/* ---- the Media tab: one row per medium of the composition as typed ---- */

static int count_of(bedlg *d, int k)
{
    BOOL ok; int v = (int)GetDlgItemInt(d->pages[BE_MEDIA], count_id[k], &ok, FALSE);
    return ok ? v : 0;
}

static void cell(HWND lv, int row, int col, const wchar_t *t)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.iSubItem = col;
    it.pszText = (wchar_t *)t;
    SendMessageW(lv, LVM_SETITEMTEXTW, row, (LPARAM)&it);
}

static void list_fill(bedlg *d)
{
    LVITEMW it; wchar_t w[200]; int k, num, row = 0;
    d->filling = 1;
    SendMessageW(d->list, LVM_DELETEALLITEMS, 0, 0);
    for (k = 0; k < MEDIA_KINDS; k++)
        for (num = 1; num <= count_of(d, k) && row < META_MAXDISK; num++, row++) {
            const meta_disk *m = num <= meta_count(d->cur, k) ? disk_of(d->cur, k, num) : NULL;
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = row;
            _snwprintf(w, 200, L"%ls %d", tr(kind_cap[k]), num); w[199] = 0;
            it.pszText = w;
            it.lParam = k * 1000 + num;
            SendMessageW(d->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
            cell(d->list, row, 1, num > meta_count(d->cur, k) ? tr(T_EDIT_ADDED) : outcome_text(m ? m->status : ""));
            if (m && m->n >= 1 && m->n <= META_MAXDISK) {
                cell(d->list, row, 2, method_text(d->id[m->n].method));
                to_w(d->id[m->n].drive, w, 200);
                cell(d->list, row, 3, w);
            }
            ListView_SetItemState(d->list, row, INDEXTOSTATEIMAGEMASK(d->ticked[k][num] ? 2 : 1), LVIS_STATEIMAGEMASK);
        }
    d->filling = 0;
}

static void list_columns(HWND lv)
{
    static const int head[] = { T_COL_MEDIUM, T_COL_OUTCOME, T_EDIT_COL_METHOD, T_EDIT_COL_DRIVE };
    static const int width[] = { 70, 90, 70, 150 };
    LVCOLUMNW c; int i;
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    for (i = 0; i < 4; i++) {
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (wchar_t *)tr(head[i]);
        c.cx = width[i];
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
}

/* ---- the Box tab: the folder the box will have ---- */

static void folder_update(bedlg *d)
{
    char title[256], ed[128], root[1024], nd[1024], *s; wchar_t a[300], b[300], w[700];
    const char *base = d->bagdir + strlen(d->bagdir);
    while (base > d->bagdir && base[-1] != '\\' && base[-1] != '/') base--;
    get_field(d->pages[BE_DATA], IDC_BE_TITLE, title, sizeof(title));
    get_field(d->pages[BE_DATA], IDC_BE_ED, ed, sizeof(ed));
    to_w(base, a, 300);
    nd[0] = 0;
    if (title[0] && (strcmp(title, d->cur->title) || strcmp(ed, d->cur->edition))) {
        str_copy(root, sizeof(root), d->bagdir);
        if ((s = root + (base - d->bagdir)) > root) s[-1] = 0;
        session_bagdir_fresh(nd, sizeof(nd), root, title, ed);
    }
    if (nd[0] && strcmp(nd, d->bagdir)) {
        const char *nb = nd + strlen(nd);
        while (nb > nd && nb[-1] != '\\' && nb[-1] != '/') nb--;
        to_w(nb, b, 300);
        _snwprintf(w, 700, tr(T_EDIT_FOLDER_NEW), a, b);
    } else {
        _snwprintf(w, 700, tr(T_EDIT_FOLDER), a);
    }
    w[699] = 0;
    SetDlgItemTextW(d->pages[BE_DATA], IDC_BE_FOLDER, w);
}

static void page_values(bedlg *d, int pg)
{
    HWND p = d->pages[pg]; wchar_t w[600]; int i, k;
    if (pg == BE_DATA) {
        for (i = 0; d->platforms && d->platforms[i]; i++)
            SendDlgItemMessageW(p, IDC_BE_PLAT, CB_ADDSTRING, 0, (LPARAM)d->platforms[i]);
        for (i = 0; fields[i].id; i++) {
            to_w((const char *)d->cur + fields[i].off, w, 600);
            SetDlgItemTextW(p, fields[i].id, w);
            SendDlgItemMessageW(p, fields[i].id, fields[i].id == IDC_BE_PLAT ? CB_LIMITTEXT : EM_LIMITTEXT,
                                (WPARAM)fields[i].n / 2, 0);
        }
        folder_update(d);
    } else {
        for (k = 0; k < MEDIA_KINDS; k++) SetDlgItemInt(p, count_id[k], meta_count(d->cur, k), FALSE);
        d->list = GetDlgItem(p, IDC_BE_LIST);
        list_columns(d->list);
        list_fill(d);
    }
}

static INT_PTR CALLBACK page_proc(HWND page, UINT m, WPARAM wp, LPARAM lp)
{
    bedlg *d = (bedlg *)GetWindowLongPtrW(page, GWLP_USERDATA);
    switch (m) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(page, GWLP_USERDATA, lp);
        return FALSE;
    case WM_COMMAND:
        if (!d || HIWORD(wp) != EN_CHANGE) return FALSE;
        if (LOWORD(wp) == IDC_BE_TITLE || LOWORD(wp) == IDC_BE_ED) folder_update(d);
        else if (LOWORD(wp) == IDC_BE_FLOPPY || LOWORD(wp) == IDC_BE_CD || LOWORD(wp) == IDC_BE_DVD) list_fill(d);
        return TRUE;
    case WM_NOTIFY:
        if (d && !d->filling && ((NMHDR *)lp)->idFrom == IDC_BE_LIST && ((NMHDR *)lp)->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *n = (NMLISTVIEW *)lp;
            if (n->uChanged & LVIF_STATE) {
                int k = (int)(n->lParam / 1000), num = (int)(n->lParam % 1000);
                if (k >= 0 && k < MEDIA_KINDS && num >= 1 && num <= META_MAXDISK)
                    d->ticked[k][num] = ((n->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            }
        }
        return TRUE;
    }
    return FALSE;
}

static void show_page(bedlg *d, int pg)
{
    int i;
    for (i = 0; i < BE_PAGES; i++) ShowWindow(d->pages[i], i == pg ? SW_SHOW : SW_HIDE);
}

/* ---- reading back ---- */

static int refuse(bedlg *d, int pg, int id, const wchar_t *why)
{
    TabCtrl_SetCurSel(d->tabs, pg);
    show_page(d, pg);
    MessageBoxW(GetParent(d->pages[pg]), why, L"RetroArchiver", MB_ICONEXCLAMATION);
    SetFocus(GetDlgItem(d->pages[pg], id));
    return 0;
}

static int read_all(HWND dlg, bedlg *d, int reading)
{
    meta_box *b = d->out->box; wchar_t w[300]; int i, k, num, c[MEDIA_KINDS], pos = 0, any = 0;
    *b = *d->cur;
    for (i = 0; fields[i].id; i++) get_field(d->pages[BE_DATA], fields[i].id, (char *)b + fields[i].off, fields[i].n);
    if (!b->title[0]) return refuse(d, BE_DATA, IDC_BE_TITLE, tr(T_MISS_TITLE));
    if (!b->platform[0]) return refuse(d, BE_DATA, IDC_BE_PLAT, tr(T_MISS_PLATFORM));
    for (k = 0; k < MEDIA_KINDS; k++) c[k] = count_of(d, k);
    if (c[0] + c[1] + c[2] < 1 || c[0] + c[1] + c[2] > META_MAXDISK) {
        _snwprintf(w, 300, tr(T_EDIT_BAD_COUNT), META_MAXDISK); w[299] = 0;
        return refuse(d, BE_MEDIA, IDC_BE_FLOPPY, w);
    }
    meta_set_counts(b, c[0], c[1], c[2]);
    memset(d->out->reread, 0, sizeof(d->out->reread));
    for (k = 0; k < MEDIA_KINDS; k++)
        for (num = 1; num <= c[k]; num++) {
            pos++;
            if (d->ticked[k][num]) { d->out->reread[pos] = 1; any = 1; }
        }
    if (reading && !any) return refuse(d, BE_MEDIA, IDC_BE_LIST, tr(T_EDIT_NONE_TICKED));
    GetDlgItemTextW(dlg, IDC_BE_REASON, w, 256);
    to_u8(w, d->out->reason, sizeof(d->out->reason));
    trim_u8(d->out->reason);
    return 1;
}

/* ---- the dialog ---- */

static void dlg_init(HWND dlg, bedlg *d)
{
    RECT r; TCITEMW it; wchar_t t[300], cap[400]; int i, k, num;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
    to_w(d->cur->title, t, 300);
    _snwprintf(cap, 400, tr(T_EDIT_TITLE), t); cap[399] = 0;
    SetWindowTextW(dlg, cap);
    SetDlgItemTextW(dlg, IDC_BE_REASON_L, tr(T_EDIT_REASON));
    SetDlgItemTextW(dlg, IDC_BE_READ, tr(T_EDIT_SAVE_READ));
    SetDlgItemTextW(dlg, IDOK, tr(T_EDIT_SAVE));
    SetDlgItemTextW(dlg, IDCANCEL, tr(T_BTN_CANCEL));
    SendDlgItemMessageW(dlg, IDC_BE_REASON, EM_LIMITTEXT, 200, 0);
    /* how each CD and DVD was read: the method and the drive, from its logs */
    for (i = 0; i < d->cur->ndisk; i++) {
        const meta_disk *m = &d->cur->disk[i];
        char p[1100], data[1040];
        if (m->kind == MEDIA_FLOPPY || !m->master[0] || m->n < 1 || m->n > META_MAXDISK) continue;
        u8_join(data, sizeof(data), d->bagdir, "data");
        u8_join(p, sizeof(p), data, m->master);
        (m->kind == MEDIA_CD ? &cd_media : &dvd_media)->identity(p, &d->id[m->n]);
    }
    /* the media added by typing a bigger number are ticked: they are to be read */
    for (k = 0; k < MEDIA_KINDS; k++)
        for (num = meta_count(d->cur, k) + 1; num <= META_MAXDISK; num++) d->ticked[k][num] = 1;
    d->tabs = GetDlgItem(dlg, IDC_TABS);
    for (i = 0; i < BE_PAGES; i++) {
        memset(&it, 0, sizeof(it));
        it.mask = TCIF_TEXT;
        it.pszText = (wchar_t *)tr(page_title[i]);
        SendMessageW(d->tabs, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    GetWindowRect(d->tabs, &r);
    MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
    TabCtrl_AdjustRect(d->tabs, FALSE, &r);
    for (i = BE_PAGES - 1; i >= 0; i--) {
        int j;
        d->pages[i] = CreateDialogParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(page_dlg[i]), dlg,
                                         page_proc, (LPARAM)d);
        if (!d->pages[i]) continue;
        theme_page(d->pages[i]);
        for (j = 0; texts[j].page >= 0; j++)
            if (texts[j].page == i) SetDlgItemTextW(d->pages[i], texts[j].id, tr(texts[j].text));
        SetWindowPos(d->pages[i], d->tabs, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    }
    for (i = 0; i < BE_PAGES; i++) if (d->pages[i]) page_values(d, i);
    show_page(d, BE_DATA);
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    bedlg *d = (bedlg *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (m) {
    case WM_INITDIALOG:
        dlg_init(dlg, (bedlg *)lp);
        return TRUE;
    case WM_NOTIFY:
        if (d && ((NMHDR *)lp)->hwndFrom == d->tabs && ((NMHDR *)lp)->code == TCN_SELCHANGE)
            show_page(d, TabCtrl_GetCurSel(d->tabs));
        return TRUE;
    case WM_COMMAND:
        if ((LOWORD(wp) == IDOK || LOWORD(wp) == IDC_BE_READ) && d && read_all(dlg, d, LOWORD(wp) == IDC_BE_READ))
            EndDialog(dlg, LOWORD(wp));
        else if (LOWORD(wp) == IDCANCEL)
            EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

int box_edit_dialog(HWND owner, const char *bagdir, const meta_box *current,
                    const wchar_t *const *platforms, box_edit_result *out)
{
    bedlg *d = (bedlg *)calloc(1, sizeof(bedlg)); int rc;
    if (!d) return IDCANCEL;
    d->bagdir = bagdir;
    d->cur = current;
    d->platforms = platforms;
    d->out = out;
    rc = (int)DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_BOXEDIT), owner, dlg_proc, (LPARAM)d);
    free(d);
    return rc == IDOK || rc == IDC_BE_READ ? rc : IDCANCEL;
}

/* ---- which read to keep ---- */

static void choose_init(HWND dlg, const edit_choice *c)
{
    static const int rows[] = { T_CHOOSE_ROW_OUTCOME, T_CHOOSE_ROW_METHOD, T_CHOOSE_ROW_DRIVE, T_CHOOSE_ROW_DATE };
    static const int heads[] = { T_CHOOSE_ROW_OUTCOME, T_CHOOSE_OLD, T_CHOOSE_NEW };
    HWND lv = GetDlgItem(dlg, IDC_CH_LIST);
    wchar_t w[300]; LVCOLUMNW col; LVITEMW it; int i;
    _snwprintf(w, 300, tr(T_CHOOSE_TITLE), tr(kind_cap[c->kind]), c->num); w[299] = 0;
    SetWindowTextW(dlg, w);
    SetDlgItemTextW(dlg, IDC_CH_TEXT, tr(T_CHOOSE_TEXT));
    SetDlgItemTextW(dlg, IDOK, tr(T_CHOOSE_KEEP_NEW));
    SetDlgItemTextW(dlg, IDCANCEL, tr(T_CHOOSE_KEEP_OLD));
    for (i = 0; i < 3; i++) {
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (wchar_t *)(i ? tr(heads[i]) : L"");
        col.cx = i ? 175 : 70;
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }
    for (i = 0; i < 4; i++) {
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (wchar_t *)tr(rows[i]);
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    }
    cell(lv, 0, 1, outcome_text(c->old_status));
    cell(lv, 0, 2, outcome_text(c->new_status));
    cell(lv, 1, 1, method_text(c->old_method));
    cell(lv, 1, 2, method_text(c->new_method));
    to_w(c->old_drive, w, 300); cell(lv, 2, 1, w);
    to_w(c->new_drive, w, 300); cell(lv, 2, 2, w);
    /* "2026-10-24T13:43:23" as "2026-10-24 13:43" */
    to_w(c->old_at, w, 17); if (w[0]) w[10] = L' '; cell(lv, 3, 1, w);
    to_w(c->new_at, w, 17); if (w[0]) w[10] = L' '; cell(lv, 3, 2, w);
}

static INT_PTR CALLBACK choose_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_INITDIALOG:
        choose_init(dlg, (const edit_choice *)lp);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) EndDialog(dlg, LOWORD(wp));
        return TRUE;
    }
    return FALSE;
}

int choose_dialog(HWND owner, const edit_choice *c)
{
    return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_CHOOSE), owner, choose_proc,
                           (LPARAM)c) == IDOK;
}
