/* Images: the scans attached to a closed box. A list of what the box has
 * and of what is being added, with what each one shows; Add takes files
 * from any folder (a network path too), Remove, Up and Down change the
 * list, and Save makes the box match it in one transaction (boximg_save,
 * in a thread: copying from a NAS and hashing take time, and can be
 * stopped). The files are copied as they are; nothing is deleted. */
#include "gui/dialogs.h"
#include "gui/resource.h"
#include "gui/lang.h"
#include "gui/winutil.h"
#include "gui/imgdec.h"
#include "core/boxedit.h"
#include "core/boximg.h"
#include "core/fsu8.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define THUMB 48
#define WM_IM_THUMB (WM_APP + 1)    /* wParam: the row's serial; lParam: HBITMAP or NULL */
#define WM_IM_PROG  (WM_APP + 2)    /* lParam: im_progress*, malloc'd */
#define WM_IM_DONE  (WM_APP + 3)    /* lParam: im_done*, malloc'd */
#define MAX_CHANGES (2 * META_MAXIMG + 2)

typedef struct { int k, n; long long done, total; wchar_t name[200]; } im_progress;
typedef struct { int rc; img_result res; char err[1200]; } im_done;

typedef struct {
    HWND dlg, list;
    const char *bagdir;
    meta_box *box;                      /* the box as it is on disk */
    img_list want;
    int serial[META_MAXIMG + 1];        /* of each item of want: the thumbnails come back by it */
    int icon[META_MAXIMG + 1];          /* its picture in the image list */
    int next_serial;
    HIMAGELIST il;
    int filling;                        /* the controls are being set: their notifications do not count */
    int saving;
    volatile LONG stop;
    int saved;
} imdlg;

static const int kind_text[IMG_KINDS] = { T_IMG_KIND_BOX, T_IMG_KIND_MEDIUM, T_IMG_KIND_INSERT, T_IMG_KIND_CASE };
static const int side_text[IMG_SIDES] = { T_IMG_SIDE_FRONT, T_IMG_SIDE_BACK, T_IMG_SIDE_SPINE, T_IMG_SIDE_TOP,
                                          T_IMG_SIDE_BOTTOM, T_IMG_SIDE_OTHER };
static const int kind_cap[MEDIA_KINDS] = { T_KIND_FLOPPY_CAP, T_KIND_CD_CAP, T_KIND_DVD_CAP };

void image_caption(const meta_image *im, wchar_t *out, int n)
{
    if (im->kind == IMG_BOX || im->kind == IMG_CASE)
        _snwprintf(out, n, tr(im->kind == IMG_BOX ? T_IMG_CAP_BOX : T_IMG_CAP_CASE), tr(side_text[im->side >= 0 && im->side < IMG_SIDES ? im->side : IMG_OTHER]));
    else if (im->kind == IMG_MEDIUM)
        _snwprintf(out, n, tr(T_IMG_CAP_MEDIUM), tr(kind_cap[im->mkind >= 0 && im->mkind < MEDIA_KINDS ? im->mkind : 0]),
                   im->mnum, tr(side_text[im->side == IMG_BACK ? IMG_BACK : IMG_FRONT]));
    else _snwprintf(out, n, L"%ls", tr(T_IMG_KIND_INSERT));
    out[n - 1] = 0;
}

void image_path(const char *bagdir, const char *file, wchar_t *out, int n)
{
    char data[1040], dir[1100], p[1200];
    u8_join(data, sizeof(data), bagdir, "data");
    u8_join(dir, sizeof(dir), data, BOXIMG_DIR);
    u8_join(p, sizeof(p), dir, file);
    to_w(p, out, n);
}

/* where the item's file is now: in the box, or where it is being taken from */
static void item_path(const imdlg *d, const img_item *it, wchar_t *out, int n)
{
    if (it->was[0]) image_path(d->bagdir, it->was, out, n);
    else to_w(it->source, out, n);
}

static const char *base_of(const char *p)
{
    const char *s = p + strlen(p);
    while (s > p && s[-1] != '/' && s[-1] != '\\') s--;
    return s;
}

/* ---- the thumbnails, in a thread ---- */

typedef struct { int serial; wchar_t path[1024]; char sha[65]; } thumb_one;
typedef struct { HWND dlg; int n; thumb_one one[1]; } thumb_job;

static DWORD WINAPI thumb_worker(LPVOID p)
{
    thumb_job *job = (thumb_job *)p; int i;
    for (i = 0; i < job->n && IsWindow(job->dlg); i++) {
        HBITMAP b = imgdec_thumb(job->one[i].path, job->one[i].sha, THUMB);
        if (b && !PostMessageW(job->dlg, WM_IM_THUMB, (WPARAM)job->one[i].serial, (LPARAM)b)) DeleteObject(b);
    }
    free(job);
    return 0;
}

/* asks for the thumbnails of the items from first on */
static void thumbs_ask(imdlg *d, int first)
{
    int n = d->want.n - first, i; thumb_job *job; HANDLE th;
    if (n < 1 || !imgdec_ready()) return;
    if (!(job = (thumb_job *)calloc(1, sizeof(thumb_job) + (size_t)n * sizeof(thumb_one)))) return;
    job->dlg = d->dlg;
    for (i = 0; i < n; i++) {
        const img_item *it = &d->want.item[first + i];
        if (boximg_file_type(it->was[0] ? it->was : it->source) != 1) continue;
        job->one[job->n].serial = d->serial[first + i];
        item_path(d, it, job->one[job->n].path, 1024);
        if (it->was[0]) strcpy(job->one[job->n].sha, it->im.sha256);
        job->n++;
    }
    th = CreateThread(NULL, 0, thumb_worker, job, 0, NULL);
    if (th) CloseHandle(th); else free(job);
}

static int add_icon(imdlg *d, HBITMAP b)
{
    int i = b ? ImageList_Add(d->il, b, NULL) : -1;
    if (b) DeleteObject(b);
    return i < 0 ? 0 : i;
}

/* a new row's serial and its placeholder, until the thumbnail arrives */
static void row_init(imdlg *d, int i)
{
    wchar_t p[1024];
    item_path(d, &d->want.item[i], p, 1024);
    d->serial[i] = ++d->next_serial;
    d->icon[i] = add_icon(d, imgdec_placeholder(p, THUMB));
}

/* ---- the list ---- */

static void cell(HWND lv, int row, int col, const wchar_t *t)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.iSubItem = col;
    it.pszText = (wchar_t *)t;
    SendMessageW(lv, LVM_SETITEMTEXTW, row, (LPARAM)&it);
}

static void row_texts(imdlg *d, int i)
{
    const img_item *it = &d->want.item[i]; wchar_t w[400], f[300]; LVITEMW lv;
    image_caption(&it->im, w, 400);
    cell(d->list, i, 0, w);
    if (it->was[0]) to_w(it->was, w, 400);
    else { to_w(base_of(it->source), f, 300); _snwprintf(w, 400, tr(T_IMG_FILE_NEW), f); w[399] = 0; }
    cell(d->list, i, 1, w);
    if (it->was[0]) { lang_number((double)it->im.bytes / 1024.0, 0, f, 300); _snwprintf(w, 400, L"%ls KB", f); w[399] = 0; }
    else w[0] = 0;
    cell(d->list, i, 2, w);
    to_w(it->im.note, w, 400);
    cell(d->list, i, 3, w);
    memset(&lv, 0, sizeof(lv));
    lv.mask = LVIF_IMAGE; lv.iItem = i; lv.iImage = d->icon[i];
    SendMessageW(d->list, LVM_SETITEMW, 0, (LPARAM)&lv);
}

static int selected(const imdlg *d)
{
    return (int)SendMessageW(d->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void select_row(imdlg *d, int i)
{
    LVITEMW it;
    if (i < 0 || i >= d->want.n) return;
    memset(&it, 0, sizeof(it));
    it.stateMask = it.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(d->list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&it);
    SendMessageW(d->list, LVM_ENSUREVISIBLE, (WPARAM)i, FALSE);
}

static void list_fill(imdlg *d, int sel)
{
    LVITEMW it; int i;
    d->filling = 1;
    SendMessageW(d->list, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < d->want.n; i++) {
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = i; it.pszText = (wchar_t *)L""; it.iImage = d->icon[i];
        SendMessageW(d->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        row_texts(d, i);
    }
    d->filling = 0;
    select_row(d, sel);
}

/* ---- what the selected image shows ---- */

static void combo_add(HWND dlg, int id, const wchar_t *text, LPARAM data)
{
    LRESULT i = SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, (LPARAM)text);
    if (i >= 0) SendDlgItemMessageW(dlg, id, CB_SETITEMDATA, (WPARAM)i, data);
}

static void combo_pick(HWND dlg, int id, LPARAM data)
{
    int i, n = (int)SendDlgItemMessageW(dlg, id, CB_GETCOUNT, 0, 0);
    for (i = 0; i < n; i++)
        if (SendDlgItemMessageW(dlg, id, CB_GETITEMDATA, (WPARAM)i, 0) == data) {
            SendDlgItemMessageW(dlg, id, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    SendDlgItemMessageW(dlg, id, CB_SETCURSEL, (WPARAM)-1, 0);
}

static LPARAM combo_data(HWND dlg, int id, LPARAM def)
{
    LRESULT i = SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0);
    return i < 0 ? def : (LPARAM)SendDlgItemMessageW(dlg, id, CB_GETITEMDATA, (WPARAM)i, 0);
}

/* the sides a kind has: all six for the box and the case, front and back for a medium */
static void sides_fill(HWND dlg, int kind)
{
    int s, n = kind == IMG_BOX || kind == IMG_CASE ? IMG_SIDES : kind == IMG_MEDIUM ? 2 : 0;
    SendDlgItemMessageW(dlg, IDC_IM_SIDE, CB_RESETCONTENT, 0, 0);
    for (s = 0; s < n; s++) combo_add(dlg, IDC_IM_SIDE, tr(side_text[s]), s);
}

static void media_fill(HWND dlg, const meta_box *b)
{
    wchar_t w[80]; int k, num;
    for (k = 0; k < MEDIA_KINDS; k++)
        for (num = 1; num <= meta_count(b, k); num++) {
            _snwprintf(w, 80, L"%ls %d", tr(kind_cap[k]), num); w[79] = 0;
            combo_add(dlg, IDC_IM_MEDIUM, w, k * 1000 + num);
        }
}

static void buttons(imdlg *d)
{
    static const int edit[] = { IDC_IM_KIND, IDC_IM_SIDE, IDC_IM_MEDIUM, IDC_IM_NOTE, IDC_IM_REMOVE, IDC_IM_UP,
                                IDC_IM_DOWN, IDC_IM_VIEW, IDC_IM_ADD, IDC_IM_REASON, IDOK, IDC_IM_LIST, 0 };
    int i, sel = selected(d), on = !d->saving, pdf = 0, kind = -1;
    if (sel >= 0) {
        const img_item *it = &d->want.item[sel];
        kind = it->im.kind;
        pdf = boximg_file_type(it->was[0] ? it->was : it->source) == 2;
    }
    for (i = 0; edit[i]; i++) EnableWindow(GetDlgItem(d->dlg, edit[i]), on);
    if (!on) return;
    EnableWindow(GetDlgItem(d->dlg, IDC_IM_KIND), sel >= 0 && !pdf);
    EnableWindow(GetDlgItem(d->dlg, IDC_IM_SIDE), sel >= 0 && kind != IMG_INSERT);
    EnableWindow(GetDlgItem(d->dlg, IDC_IM_MEDIUM), sel >= 0 && kind == IMG_MEDIUM);
    EnableWindow(GetDlgItem(d->dlg, IDC_IM_NOTE), sel >= 0);
    EnableWindow(GetDlgItem(d->dlg, IDC_IM_REMOVE), sel >= 0);
    EnableWindow(GetDlgItem(d->dlg, IDC_IM_VIEW), sel >= 0);
    EnableWindow(GetDlgItem(d->dlg, IDC_IM_UP), sel > 0);
    EnableWindow(GetDlgItem(d->dlg, IDC_IM_DOWN), sel >= 0 && sel < d->want.n - 1);
}

/* the controls under the list show the selected image */
static void show_selected(imdlg *d)
{
    int sel = selected(d); wchar_t w[300];
    d->filling = 1;
    if (sel < 0) {
        SendDlgItemMessageW(d->dlg, IDC_IM_KIND, CB_SETCURSEL, (WPARAM)-1, 0);
        sides_fill(d->dlg, IMG_INSERT);
        SendDlgItemMessageW(d->dlg, IDC_IM_MEDIUM, CB_SETCURSEL, (WPARAM)-1, 0);
        SetDlgItemTextW(d->dlg, IDC_IM_NOTE, L"");
    } else {
        const meta_image *im = &d->want.item[sel].im;
        combo_pick(d->dlg, IDC_IM_KIND, im->kind);
        sides_fill(d->dlg, im->kind);
        combo_pick(d->dlg, IDC_IM_SIDE, im->side);
        combo_pick(d->dlg, IDC_IM_MEDIUM, im->kind == IMG_MEDIUM ? im->mkind * 1000 + im->mnum : -1);
        to_w(im->note, w, 300);
        SetDlgItemTextW(d->dlg, IDC_IM_NOTE, w);
    }
    d->filling = 0;
    buttons(d);
}

/* the controls changed: into the selected image */
static void read_selected(imdlg *d, int from)
{
    int sel = selected(d); meta_image *im; wchar_t w[300]; LPARAM m;
    if (sel < 0 || d->filling) return;
    im = &d->want.item[sel].im;
    if (from == IDC_IM_KIND) {
        im->kind = (int)combo_data(d->dlg, IDC_IM_KIND, im->kind);
        if (im->kind == IMG_INSERT) im->side = IMG_OTHER;
        if (im->kind == IMG_MEDIUM) {
            if (im->side != IMG_BACK) im->side = IMG_FRONT;
            if (im->mnum < 1 || im->mnum > meta_count(d->box, im->mkind)) {
                /* the first medium the box has */
                for (im->mkind = 0; im->mkind < MEDIA_KINDS - 1 && !meta_count(d->box, im->mkind); im->mkind++) ;
                im->mnum = 1;
            }
        }
        if ((im->kind == IMG_BOX || im->kind == IMG_CASE) && (im->side < 0 || im->side >= IMG_SIDES)) im->side = IMG_FRONT;
    } else if (from == IDC_IM_SIDE) {
        im->side = (int)combo_data(d->dlg, IDC_IM_SIDE, im->side);
    } else if (from == IDC_IM_MEDIUM) {
        m = combo_data(d->dlg, IDC_IM_MEDIUM, im->mkind * 1000 + im->mnum);
        im->mkind = (int)(m / 1000); im->mnum = (int)(m % 1000);
    } else {
        GetDlgItemTextW(d->dlg, IDC_IM_NOTE, w, 300);
        to_u8(w, im->note, (int)sizeof(im->note));
    }
    row_texts(d, sel);
    if (from != IDC_IM_NOTE) show_selected(d);
}

/* ---- Add, Remove, Up, Down, View ---- */

static void core_msg(HWND dlg, int fmt, const char *core, UINT icon)
{
    wchar_t e[900], w[1200];
    lang_core_text(core ? core : "", e, 900);
    if (e[0]) e[0] = (wchar_t)towupper(e[0]);
    if (fmt >= 0) { _snwprintf(w, 1200, tr(fmt), e); w[1199] = 0; }
    else { wcsncpy(w, e, 1200); w[1199] = 0; }
    MessageBoxW(dlg, w, L"RetroArchiver", icon);
}

static int add_one(imdlg *d, const wchar_t *path, strbuf *err)
{
    char u8[1024]; meta_image im;
    to_u8(path, u8, (int)sizeof(u8));
    boximg_guess(u8, d->box, &im);          /* what its name suggests; an insert if nothing */
    if (boximg_list_add(&d->want, d->box, u8, &im, err)) return -1;
    row_init(d, d->want.n - 1);
    return 0;
}

static void add_files(imdlg *d)
{
    static wchar_t buf[32768];
    OPENFILENAMEW of; wchar_t filter[200], path[1100], *p; strbuf err; int first = d->want.n, k;
    /* "Pictures and PDF\0*.jpg;...\0\0" */
    k = _snwprintf(filter, 150, L"%ls", tr(T_IMG_FILTER));
    if (k < 0) k = 0;
    memcpy(filter + k + 1, L"*.jpg;*.jpeg;*.png;*.tif;*.tiff;*.bmp;*.gif;*.pdf\0", 51 * sizeof(wchar_t));
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = d->dlg;
    of.lpstrFilter = filter;
    of.lpstrFile = buf; of.nMaxFile = 32768;
    of.lpstrTitle = tr(T_IMG_PICK);
    of.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    buf[0] = 0;
    if (!GetOpenFileNameW(&of)) return;
    sb_init(&err);
    p = buf + wcslen(buf) + 1;
    if (!*p) {
        add_one(d, buf, &err);              /* one file: its whole path */
    } else {
        for (; *p; p += wcslen(p) + 1) {    /* several: the folder, then the names */
            _snwprintf(path, 1100, L"%ls%ls%ls", buf, buf[wcslen(buf) - 1] == L'\\' ? L"" : L"\\", p);
            path[1099] = 0;
            if (add_one(d, path, &err)) break;
        }
    }
    if (err.len) core_msg(d->dlg, -1, err.s, MB_ICONEXCLAMATION);
    sb_free(&err);
    if (d->want.n > first) {
        list_fill(d, first);
        thumbs_ask(d, first);
    }
    show_selected(d);
}

static void remove_selected(imdlg *d)
{
    int sel = selected(d), i;
    if (sel < 0) return;
    boximg_list_remove(&d->want, sel);
    for (i = sel; i < d->want.n; i++) { d->serial[i] = d->serial[i + 1]; d->icon[i] = d->icon[i + 1]; }
    list_fill(d, sel < d->want.n ? sel : d->want.n - 1);
    show_selected(d);
}

static void move_selected(imdlg *d, int dir)
{
    int sel = selected(d), to, t;
    if (sel < 0 || (to = boximg_list_move(&d->want, sel, dir)) == sel) return;
    t = d->serial[sel]; d->serial[sel] = d->serial[to]; d->serial[to] = t;
    t = d->icon[sel]; d->icon[sel] = d->icon[to]; d->icon[to] = t;
    list_fill(d, to);
    show_selected(d);
}

static void view_selected(imdlg *d)
{
    view_item *v; int i, sel = selected(d);
    if (sel < 0 || !(v = (view_item *)calloc((size_t)d->want.n, sizeof(view_item)))) return;
    for (i = 0; i < d->want.n; i++) {
        item_path(d, &d->want.item[i], v[i].path, 1024);
        image_caption(&d->want.item[i].im, v[i].caption, 200);
    }
    image_viewer(d->dlg, v, d->want.n, sel);
    free(v);
}

/* ---- Save ---- */

typedef struct { imdlg *d; img_list want; char bagdir[1024], reason[256]; DWORD last; int last_k; } save_job;

static int save_progress(void *ctx, int k, int n, const char *name, long long done, long long total)
{
    save_job *job = (save_job *)ctx; DWORD now = GetTickCount(); im_progress *p;
    if (job->d->stop) return 1;
    if (k == job->last_k && now - job->last < 150 && done < total) return 0;
    job->last = now; job->last_k = k;
    if ((p = (im_progress *)calloc(1, sizeof(*p))) != NULL) {
        p->k = k; p->n = n; p->done = done; p->total = total;
        to_w(name, p->name, 200);
        if (!PostMessageW(job->d->dlg, WM_IM_PROG, 0, (LPARAM)p)) free(p);
    }
    return 0;
}

static DWORD WINAPI save_worker(LPVOID p)
{
    save_job *job = (save_job *)p; im_done *r = (im_done *)calloc(1, sizeof(im_done)); strbuf err; img_host host;
    if (r) {
        sb_init(&err);
        host.ctx = job; host.progress = save_progress;
        r->rc = boximg_save(job->bagdir, &job->want, job->reason, &host, &r->res, &err);
        str_copy(r->err, sizeof(r->err), err.s ? err.s : "");
        sb_free(&err);
        if (!PostMessageW(job->d->dlg, WM_IM_DONE, 0, (LPARAM)r)) free(r);
    }
    boximg_list_free(&job->want);
    free(job);
    return 0;
}

static void change_line(const img_change *c, wchar_t *out, int n)
{
    wchar_t a[300], b[300], f[300];
    out[0] = 0;
    switch (c->what) {
    case IMGCH_ADDED:
        to_w(base_of(c->source), f, 300); image_caption(&c->to, a, 300);
        _snwprintf(out, n, tr(T_IMG_CH_ADD), f, a);
        break;
    case IMGCH_REMOVED:
        to_w(c->from.file, f, 300); image_caption(&c->from, a, 300);
        _snwprintf(out, n, tr(T_IMG_CH_REMOVE), f, a);
        break;
    case IMGCH_RELABELED:
        to_w(c->from.file, f, 300); image_caption(&c->from, a, 300); image_caption(&c->to, b, 300);
        if (!wcscmp(a, b)) _snwprintf(out, n, tr(T_IMG_CH_NOTE), f);
        else _snwprintf(out, n, tr(T_IMG_CH_RELABEL), f, a, b);
        break;
    default:
        _snwprintf(out, n, L"%ls", tr(T_IMG_CH_ORDER));
    }
    out[n - 1] = 0;
}

/* "Save these changes?" with the list of them; 1 yes */
static int confirm(imdlg *d, const img_change *ch, int n)
{
    static wchar_t list[6000], all[7000];
    wchar_t line[1000], t[300]; int i, shown = 0;
    list[0] = 0;
    for (i = 0; i < n && i < MAX_CHANGES; i++) {
        if (shown == 14 && n > 15) {        /* a long list ends with how many more */
            _snwprintf(line, 1000, tr(T_IMG_CH_MORE), n - shown); line[999] = 0;
            wcscat(list, L"\x2022 "); wcscat(list, line); wcscat(list, L"\n");
            break;
        }
        change_line(&ch[i], line, 1000);
        if (wcslen(list) + wcslen(line) + 4 < 5000) { wcscat(list, L"\x2022 "); wcscat(list, line); wcscat(list, L"\n"); shown++; }
    }
    to_w(d->box->title, t, 300);
    _snwprintf(all, 7000, tr(T_IMG_CONFIRM), t, list); all[6999] = 0;
    return MessageBoxW(d->dlg, all, L"RetroArchiver", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) == IDYES;
}

static void progress_show(imdlg *d, int on)
{
    ShowWindow(GetDlgItem(d->dlg, IDC_IM_PROG), on ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(d->dlg, IDC_IM_BAR), on ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(d->dlg, IDC_IM_HINT), on ? SW_HIDE : SW_SHOW);
    SetDlgItemTextW(d->dlg, IDCANCEL, tr(on ? T_IMG_STOP : T_BTN_CANCEL));
}

static void save(imdlg *d)
{
    img_change *ch; save_job *job; strbuf err; wchar_t w[300]; HANDLE th; int i, n;
    sb_init(&err);
    for (i = 0; i < d->want.n; i++) {
        const img_item *it = &d->want.item[i];
        if (boximg_valid(d->box, &it->im, it->was[0] ? it->was : it->source, &err)) {
            select_row(d, i);
            core_msg(d->dlg, -1, err.s, MB_ICONEXCLAMATION);
            sb_free(&err);
            return;
        }
    }
    sb_free(&err);
    if (!(ch = (img_change *)calloc(MAX_CHANGES, sizeof(img_change)))) return;
    n = boximg_diff(d->box, &d->want, ch, MAX_CHANGES);
    if (n == 0) { free(ch); EndDialog(d->dlg, 0); return; }
    i = confirm(d, ch, n);
    free(ch);
    if (!i || !(job = (save_job *)calloc(1, sizeof(save_job)))) return;
    job->d = d;
    job->want.item = (img_item *)malloc((size_t)(d->want.n ? d->want.n : 1) * sizeof(img_item));
    if (!job->want.item) { free(job); return; }
    memcpy(job->want.item, d->want.item, (size_t)d->want.n * sizeof(img_item));
    job->want.n = job->want.cap = d->want.n;
    str_copy(job->bagdir, sizeof(job->bagdir), d->bagdir);
    GetDlgItemTextW(d->dlg, IDC_IM_REASON, w, 300);
    to_u8(w, job->reason, (int)sizeof(job->reason));
    d->saving = 1; d->stop = 0;
    buttons(d);
    progress_show(d, 1);
    SetDlgItemTextW(d->dlg, IDC_IM_PROG, tr(T_IMG_CHECKING));
    SendDlgItemMessageW(d->dlg, IDC_IM_BAR, PBM_SETPOS, 0, 0);
    th = CreateThread(NULL, 0, save_worker, job, 0, NULL);
    if (th) { CloseHandle(th); return; }
    boximg_list_free(&job->want);
    free(job);
    d->saving = 0;
    progress_show(d, 0);
    buttons(d);
}

static void on_progress(imdlg *d, im_progress *p)
{
    wchar_t w[400];
    if (p->total > 0 && p->done >= p->total) wcsncpy(w, tr(T_IMG_CLOSING), 400);
    else _snwprintf(w, 400, tr(T_IMG_COPYING), p->k, p->n, p->name);
    w[399] = 0;
    SetDlgItemTextW(d->dlg, IDC_IM_PROG, w);
    SendDlgItemMessageW(d->dlg, IDC_IM_BAR, PBM_SETPOS,
                        (WPARAM)(p->total > 0 ? (int)(p->done * 1000 / p->total) : 0), 0);
    free(p);
}

static void on_done(imdlg *d, im_done *r)
{
    wchar_t w[1400], names[1100];
    d->saving = 0;
    progress_show(d, 0);
    buttons(d);
    if (r->rc < 0) {
        core_msg(d->dlg, T_IMG_FAILED, r->err, MB_ICONEXCLAMATION);
    } else if (r->rc > 0) {
        MessageBoxW(d->dlg, tr(T_IMG_STOPPED), L"RetroArchiver", MB_ICONINFORMATION);
    } else {
        if (r->res.skipped) {
            to_w(r->res.skipped_names, names, 1100);
            _snwprintf(w, 1400, tr(T_IMG_SKIPPED), names); w[1399] = 0;
            MessageBoxW(d->dlg, w, L"RetroArchiver", MB_ICONINFORMATION);
        }
        d->saved = r->res.added || r->res.removed || r->res.relabeled || !r->res.skipped;
        free(r);
        EndDialog(d->dlg, d->saved);
        return;
    }
    free(r);
}

/* ---- the dialog ---- */

static void column(HWND lv, int i, int text, int width)
{
    LVCOLUMNW col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (wchar_t *)tr(text);
    col.cx = width;
    SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
}

static void dlg_init(HWND dlg, imdlg *d)
{
    static const struct { int id, text; } texts[] = {
        { IDC_IM_ADD, T_IMG_ADD }, { IDC_IM_REMOVE, T_IMG_REMOVE }, { IDC_IM_UP, T_IMG_UP },
        { IDC_IM_DOWN, T_IMG_DOWN }, { IDC_IM_VIEW, T_IMG_VIEW }, { IDC_IM_KIND_L, T_IMG_KIND_L },
        { IDC_IM_SIDE_L, T_IMG_SIDE_L }, { IDC_IM_MEDIUM_L, T_IMG_MEDIUM_L }, { IDC_IM_NOTE_L, T_IMG_NOTE_L },
        { IDC_IM_REASON_L, T_EDIT_REASON }, { IDC_IM_HINT, T_IMG_HINT }, { IDOK, T_EDIT_SAVE },
        { IDCANCEL, T_BTN_CANCEL }, { 0, 0 }
    };
    wchar_t w[400], t[300]; int i;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
    d->dlg = dlg;
    d->list = GetDlgItem(dlg, IDC_IM_LIST);
    to_w(d->box->title, t, 300);
    _snwprintf(w, 400, tr(T_IMG_TITLE), t); w[399] = 0;
    SetWindowTextW(dlg, w);
    for (i = 0; texts[i].id; i++) SetDlgItemTextW(dlg, texts[i].id, tr(texts[i].text));
    SendMessageW(d->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    d->il = ImageList_Create(THUMB, THUMB, ILC_COLOR24, 16, 16);
    SendMessageW(d->list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)d->il);
    column(d->list, 0, T_IMG_COL_SHOWS, 170);
    column(d->list, 1, T_IMG_COL_FILE, 160);
    column(d->list, 2, T_IMG_COL_SIZE, 76);
    column(d->list, 3, T_IMG_NOTE_L, 140);
    {   /* the case right after the box, not in the order of the numbers */
        static const int order[IMG_KINDS] = { IMG_BOX, IMG_CASE, IMG_MEDIUM, IMG_INSERT };
        for (i = 0; i < IMG_KINDS; i++) combo_add(dlg, IDC_IM_KIND, tr(kind_text[order[i]]), order[i]);
    }
    media_fill(dlg, d->box);
    SendDlgItemMessageW(dlg, IDC_IM_NOTE, EM_LIMITTEXT, 120, 0);
    SendDlgItemMessageW(dlg, IDC_IM_REASON, EM_LIMITTEXT, 120, 0);
    SendDlgItemMessageW(dlg, IDC_IM_BAR, PBM_SETRANGE32, 0, 1000);
    imgdec_ready();
    boximg_list_load(d->box, &d->want);
    for (i = 0; i < d->want.n; i++) row_init(d, i);
    list_fill(d, d->want.n ? 0 : -1);
    thumbs_ask(d, 0);
    progress_show(d, 0);
    show_selected(d);
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    imdlg *d = (imdlg *)GetWindowLongPtrW(dlg, GWLP_USERDATA); int i;
    switch (m) {
    case WM_INITDIALOG:
        dlg_init(dlg, (imdlg *)lp);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (!d || h->idFrom != IDC_IM_LIST) return FALSE;
        if (h->code == LVN_ITEMCHANGED && !d->filling && (((NMLISTVIEW *)lp)->uChanged & LVIF_STATE)) show_selected(d);
        if (h->code == NM_DBLCLK && !d->saving) view_selected(d);
        return TRUE;
    }
    case WM_IM_THUMB:
        for (i = 0; d && i < d->want.n; i++)
            if (d->serial[i] == (int)wp) {
                d->icon[i] = add_icon(d, (HBITMAP)lp);
                row_texts(d, i);
                return TRUE;
            }
        if (lp) DeleteObject((HBITMAP)lp);
        return TRUE;
    case WM_IM_PROG:
        if (d) on_progress(d, (im_progress *)lp); else free((void *)lp);
        return TRUE;
    case WM_IM_DONE:
        if (d) on_done(d, (im_done *)lp); else free((void *)lp);
        return TRUE;
    case WM_COMMAND:
        if (!d) return TRUE;
        if (d->saving) {                    /* only Stop answers while the box is being saved */
            if (LOWORD(wp) == IDCANCEL) { d->stop = 1; SetDlgItemTextW(dlg, IDC_IM_PROG, tr(T_IMG_STOPPING)); }
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_IM_KIND: case IDC_IM_SIDE: case IDC_IM_MEDIUM:
            if (HIWORD(wp) == CBN_SELCHANGE) read_selected(d, LOWORD(wp));
            break;
        case IDC_IM_NOTE:
            if (HIWORD(wp) == EN_CHANGE) read_selected(d, IDC_IM_NOTE);
            break;
        case IDC_IM_ADD: add_files(d); break;
        case IDC_IM_REMOVE: remove_selected(d); break;
        case IDC_IM_UP: move_selected(d, -1); break;
        case IDC_IM_DOWN: move_selected(d, 1); break;
        case IDC_IM_VIEW: view_selected(d); break;
        case IDOK: save(d); break;
        case IDCANCEL: EndDialog(dlg, 0); break;
        }
        return TRUE;
    }
    return FALSE;
}

int images_dialog(HWND owner, const char *bagdir)
{
    imdlg *d = (imdlg *)calloc(1, sizeof(imdlg)); strbuf err; char data[1040]; int rc = 0;
    if (!d) return 0;
    sb_init(&err);
    u8_join(data, sizeof(data), bagdir, "data");
    d->bagdir = bagdir;
    d->box = meta_new();
    if (d->box && meta_load(d->box, data, &err) == 0)
        rc = (int)DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_IMAGES), owner, dlg_proc, (LPARAM)d);
    else core_msg(owner, -1, err.s, MB_ICONEXCLAMATION);
    if (d->il) ImageList_Destroy(d->il);
    boximg_list_free(&d->want);
    meta_delete(d->box);
    sb_free(&err);
    free(d);
    return rc == 1;
}
