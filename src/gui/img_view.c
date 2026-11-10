/* Looking at the images of a box: the strip of thumbnails in the Archive's
 * detail pane, and the window that shows one image at a time. Decoding
 * happens in threads (a 600 dpi scan takes seconds): the window never
 * waits for a picture. What cannot be decoded (a PDF, a TIFF GDI+ does not
 * know) gets a card with its type, and Open with Windows shows it. */
#include "gui/dialogs.h"
#include "gui/lang.h"
#include "gui/winutil.h"
#include "gui/imgdec.h"
#include "core/boximg.h"
#include "core/fsu8.h"
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WM_IV_PICTURE (WM_APP + 1)  /* wParam: generation; lParam: iv_picture*, malloc'd */
#define WM_TS_LIST    (WM_APP + 2)  /* wParam: generation; lParam: ts_list*, malloc'd */
#define WM_TS_THUMB   (WM_APP + 3)  /* wParam: generation << 12 | index; lParam: HBITMAP */

static HFONT ui_font(void) { return (HFONT)GetStockObject(DEFAULT_GUI_FONT); }

/* ---- the viewer ---- */

#define IV_CLASS L"RetroArchiverImage"
#define IV_BAR 40                   /* the row of buttons under the picture */
#define IV_TIMER 1
enum { IV_PREV = 100, IV_NEXT, IV_OPEN, IV_CLOSE };

typedef struct { HBITMAP bmp; int w, h; } iv_picture;
typedef struct {
    HWND wnd; const view_item *item; int n, cur;
    LONG gen;                       /* of the picture asked for: an older answer is dropped */
    HBITMAP bmp; int bw, bh;
    int loading, failed, done;
} viewer;

typedef struct { HWND wnd; LONG gen; int w, h; wchar_t path[1024]; } iv_job;

static DWORD WINAPI iv_worker(LPVOID p)
{
    iv_job *job = (iv_job *)p; iv_picture *pic = (iv_picture *)calloc(1, sizeof(iv_picture));
    if (pic) {
        pic->bmp = imgdec_fit(job->path, job->w, job->h, &pic->w, &pic->h);
        if (!PostMessageW(job->wnd, WM_IV_PICTURE, (WPARAM)job->gen, (LPARAM)pic)) {
            if (pic->bmp) DeleteObject(pic->bmp);
            free(pic);
        }
    }
    free(job);
    return 0;
}

static void iv_title(viewer *v)
{
    wchar_t t[700]; const wchar_t *name = wcsrchr(v->item[v->cur].path, L'\\');
    _snwprintf(t, 700, tr(T_VIEW_TITLE), v->item[v->cur].caption, v->cur + 1, v->n, name ? name + 1 : v->item[v->cur].path);
    t[699] = 0;
    SetWindowTextW(v->wnd, t);
}

/* asks for the current image at the size of the window */
static void iv_load(viewer *v)
{
    RECT r; iv_job *job; HANDLE th;
    GetClientRect(v->wnd, &r);
    v->gen++;
    v->loading = 1; v->failed = 0;
    iv_title(v);
    EnableWindow(GetDlgItem(v->wnd, IV_PREV), v->cur > 0);
    EnableWindow(GetDlgItem(v->wnd, IV_NEXT), v->cur < v->n - 1);
    InvalidateRect(v->wnd, NULL, TRUE);
    if (!imgdec_ready() || !(job = (iv_job *)calloc(1, sizeof(iv_job)))) { v->loading = 0; v->failed = 1; return; }
    job->wnd = v->wnd; job->gen = v->gen;
    job->w = r.right - 16 > 16 ? r.right - 16 : 16;
    job->h = r.bottom - IV_BAR - 16 > 16 ? r.bottom - IV_BAR - 16 : 16;
    wcsncpy(job->path, v->item[v->cur].path, 1023);
    th = CreateThread(NULL, 0, iv_worker, job, 0, NULL);
    if (th) CloseHandle(th); else { free(job); v->loading = 0; v->failed = 1; }
}

static void iv_go(viewer *v, int to)
{
    if (to < 0 || to >= v->n || to == v->cur) return;
    v->cur = to;
    if (v->bmp) { DeleteObject(v->bmp); v->bmp = NULL; }
    iv_load(v);
}

static void iv_paint(viewer *v, HDC dc)
{
    RECT r; HDC mem; HGDIOBJ o;
    GetClientRect(v->wnd, &r);
    r.bottom -= IV_BAR;
    FillRect(dc, &r, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xe0, 0xe0, 0xe0));
    SelectObject(dc, ui_font());
    if (v->bmp) {
        mem = CreateCompatibleDC(dc);
        o = SelectObject(mem, v->bmp);
        BitBlt(dc, (r.right - v->bw) / 2, (r.bottom - v->bh) / 2, v->bw, v->bh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, o);
        DeleteDC(mem);
        if (!v->loading) return;
        r.top = r.bottom - 22;          /* over the old picture while the new size comes */
    }
    if (v->loading) DrawTextW(dc, tr(T_VIEW_LOADING), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    else if (v->failed) DrawTextW(dc, tr(T_VIEW_NONE), -1, &r, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
}

static void iv_layout(viewer *v)
{
    RECT r; int y;
    GetClientRect(v->wnd, &r);
    y = r.bottom - IV_BAR + 8;
    MoveWindow(GetDlgItem(v->wnd, IV_PREV), 8, y, 96, 24, TRUE);
    MoveWindow(GetDlgItem(v->wnd, IV_NEXT), 110, y, 96, 24, TRUE);
    MoveWindow(GetDlgItem(v->wnd, IV_OPEN), 220, y, 150, 24, TRUE);
    MoveWindow(GetDlgItem(v->wnd, IV_CLOSE), r.right - 104, y, 96, 24, TRUE);
}

static HWND iv_button(HWND w, int id, int text)
{
    HWND b = CreateWindowExW(0, L"BUTTON", tr(text), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, w,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)ui_font(), TRUE);
    return b;
}

static LRESULT CALLBACK iv_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    viewer *v = (viewer *)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE:
        v = (viewer *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)v);
        v->wnd = w;
        iv_button(w, IV_PREV, T_VIEW_PREV); iv_button(w, IV_NEXT, T_VIEW_NEXT);
        iv_button(w, IV_OPEN, T_VIEW_OPEN); iv_button(w, IV_CLOSE, T_VIEW_CLOSE);
        iv_layout(v);
        iv_load(v);
        return 0;
    case WM_SIZE:
        if (!v) break;
        iv_layout(v);
        InvalidateRect(w, NULL, TRUE);
        SetTimer(w, IV_TIMER, 200, NULL);       /* decoded again once the resizing stops */
        return 0;
    case WM_TIMER:
        KillTimer(w, IV_TIMER);
        if (v) iv_load(v);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 500;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 300;
        return 0;
    case WM_IV_PICTURE: {
        iv_picture *pic = (iv_picture *)lp;
        if (v && (LONG)wp == v->gen) {
            if (v->bmp) DeleteObject(v->bmp);
            v->bmp = pic->bmp; v->bw = pic->w; v->bh = pic->h;
            v->loading = 0; v->failed = !pic->bmp;
            InvalidateRect(w, NULL, TRUE);
        } else if (pic->bmp) DeleteObject(pic->bmp);
        free(pic);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(w, &r);
        r.top = r.bottom - IV_BAR;
        FillRect((HDC)wp, &r, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
        if (v) iv_paint(v, dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (!v) break;
        switch (LOWORD(wp)) {
        case IV_PREV: iv_go(v, v->cur - 1); break;
        case IV_NEXT: iv_go(v, v->cur + 1); break;
        case IV_OPEN: ShellExecuteW(w, L"open", v->item[v->cur].path, NULL, NULL, SW_SHOWNORMAL); break;
        case IV_CLOSE: case IDCANCEL: DestroyWindow(w); break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        if (v) { v->done = 1; if (v->bmp) DeleteObject(v->bmp); v->bmp = NULL; }
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* the arrows, Home, End and Esc, wherever the focus is in the viewer */
static int iv_key(viewer *v, const MSG *msg)
{
    if (msg->message != WM_KEYDOWN || (msg->hwnd != v->wnd && !IsChild(v->wnd, msg->hwnd))) return 0;
    switch (msg->wParam) {
    case VK_LEFT: case VK_PRIOR: iv_go(v, v->cur - 1); return 1;
    case VK_RIGHT: case VK_NEXT: case VK_SPACE: iv_go(v, v->cur + 1); return 1;
    case VK_HOME: iv_go(v, 0); return 1;
    case VK_END: iv_go(v, v->n - 1); return 1;
    case VK_ESCAPE: DestroyWindow(v->wnd); return 1;
    }
    return 0;
}

void image_viewer(HWND owner, const view_item *items, int n, int first)
{
    static int registered;
    viewer v; MSG msg; RECT r; HWND w; int cw, ch, got = 1;
    if (n < 1) return;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = iv_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
        wc.lpszClassName = IV_CLASS;
        registered = RegisterClassW(&wc) != 0;
    }
    memset(&v, 0, sizeof(v));
    v.item = items; v.n = n; v.cur = first >= 0 && first < n ? first : 0;
    /* most of the work area: a scan is looked at large */
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    cw = (r.right - r.left) * 4 / 5; ch = (r.bottom - r.top) * 9 / 10;
    w = CreateWindowExW(0, IV_CLASS, L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        r.left + (r.right - r.left - cw) / 2, r.top + (r.bottom - r.top - ch) / 2, cw, ch,
                        owner, NULL, GetModuleHandleW(NULL), &v);
    if (!w) return;
    EnableWindow(owner, FALSE);         /* modal over its owner, with a loop of its own */
    ShowWindow(w, SW_SHOW);
    SetFocus(GetDlgItem(w, IV_NEXT));
    while (!v.done && (got = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        if (iv_key(&v, &msg)) continue;
        if (!v.done && IsDialogMessageW(w, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    if (got == 0) {                     /* the program is closing: the quit goes on to the main loop */
        if (!v.done) DestroyWindow(w);
        PostQuitMessage((int)msg.wParam);
        return;
    }
    SetForegroundWindow(owner);
}

/* ---- the strip of thumbnails ---- */

#define TS_CLASS L"RetroArchiverThumbs"
#define TS_SIZE 48
#define TS_GAP 6

typedef struct { int n; view_item item[1]; } ts_list;
typedef struct {
    HWND wnd; int id;
    LONG gen;
    ts_list *list;                  /* in display order; NULL: no box */
    HBITMAP thumb[META_MAXIMG];
    RECT link;                      /* "Images...": where a click opens the dialog */
    int shown;                      /* how many thumbnails fit */
    int has_box;
} strip;

typedef struct { HWND wnd; LONG gen; char bagdir[1024]; } ts_job;

static DWORD WINAPI ts_worker(LPVOID p)
{
    ts_job *job = (ts_job *)p; meta_box *b = meta_new(); strbuf err; char data[1100];
    int idx[META_MAXIMG], i, n; ts_list *l; char (*sha)[65] = NULL;
    sb_init(&err);
    u8_join(data, sizeof(data), job->bagdir, "data");
    n = b && meta_load(b, data, &err) == 0 ? b->nimage : 0;
    sb_free(&err);
    l = (ts_list *)calloc(1, sizeof(ts_list) + (size_t)n * sizeof(view_item));
    if (n) sha = (char (*)[65])calloc((size_t)n, 65);
    if (l && (sha || !n)) {
        boximg_order(b, idx);
        for (i = 0; i < n; i++) {
            const meta_image *im = &b->image[idx[i]];
            image_path(job->bagdir, im->file, l->item[i].path, 1024);
            image_caption(im, l->item[i].caption, 200);
            if (im->note[0]) {      /* "Insert: the registration card" */
                wchar_t note[260]; size_t k = wcslen(l->item[i].caption);
                to_w(im->note, note, 260);
                _snwprintf(l->item[i].caption + k, 200 - k, L": %ls", note);
                l->item[i].caption[199] = 0;
            }
            strcpy(sha[i], im->sha256);
        }
        l->n = n;
        /* the list first (and its copy for the thumbnails), then the pictures one by one */
        if (!PostMessageW(job->wnd, WM_TS_LIST, (WPARAM)job->gen, (LPARAM)l)) { free(l); n = 0; }
        else {
            ts_list *copy = (ts_list *)malloc(sizeof(ts_list) + (size_t)n * sizeof(view_item));
            if (copy) memcpy(copy, l, sizeof(ts_list) + (size_t)n * sizeof(view_item));
            l = copy;
        }
        for (i = 0; l && i < n && IsWindow(job->wnd); i++) {
            HBITMAP t = imgdec_thumb(l->item[i].path, sha[i], TS_SIZE);    /* NULL for a PDF, or without GDI+ */
            if (!t) t = imgdec_placeholder(l->item[i].path, TS_SIZE);
            if (t && !PostMessageW(job->wnd, WM_TS_THUMB, (WPARAM)((job->gen << 12) | i), (LPARAM)t)) DeleteObject(t);
        }
        free(l);
    } else free(l);
    free(sha);
    meta_delete(b);
    free(job);
    return 0;
}

static void ts_clear(strip *s)
{
    int i;
    for (i = 0; i < META_MAXIMG; i++) if (s->thumb[i]) { DeleteObject(s->thumb[i]); s->thumb[i] = NULL; }
    free(s->list);
    s->list = NULL;
}

static void ts_paint(strip *s, HDC dc)
{
    RECT r, t; wchar_t w[200]; SIZE sz; HDC mem; HFONT f, under; LOGFONTW lf; int i, x = 0, n, y, room;
    GetClientRect(s->wnd, &r);
    FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));     /* the detail pane's own background */
    SetRectEmpty(&s->link);
    s->shown = 0;
    if (!s->has_box) return;
    n = s->list ? s->list->n : 0;
    f = (HFONT)SendMessageW(s->wnd, WM_GETFONT, 0, 0);
    if (!f) f = ui_font();
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x44, 0x44, 0x44));
    if (n) _snwprintf(w, 200, tr(T_STRIP_COUNT), n); else wcsncpy(w, tr(T_STRIP_NONE), 200);
    w[199] = 0;
    GetTextExtentPoint32W(dc, w, (int)wcslen(w), &sz);
    SetRect(&t, 0, 0, sz.cx, r.bottom);
    DrawTextW(dc, w, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    x = sz.cx + 8;
    /* the link's room on the right, then as many thumbnails as fit */
    GetObjectW(f, sizeof(lf), &lf);
    lf.lfUnderline = TRUE;
    under = CreateFontIndirectW(&lf);
    SelectObject(dc, under);
    wcsncpy(w, tr(n ? T_STRIP_LINK : T_STRIP_LINK_ADD), 200); w[199] = 0;
    GetTextExtentPoint32W(dc, w, (int)wcslen(w), &sz);
    room = r.right - sz.cx - 12;
    y = (r.bottom - TS_SIZE) / 2;
    mem = CreateCompatibleDC(dc);
    for (i = 0; i < n && x + TS_SIZE <= room - (i < n - 1 ? 34 : 0); i++, x += TS_SIZE + TS_GAP) {
        if (s->thumb[i]) {
            HGDIOBJ o = SelectObject(mem, s->thumb[i]);
            BitBlt(dc, x, y, TS_SIZE, TS_SIZE, mem, 0, 0, SRCCOPY);
            SelectObject(mem, o);
        }
        SetRect(&t, x - 1, y - 1, x + TS_SIZE + 1, y + TS_SIZE + 1);
        FrameRect(dc, &t, (HBRUSH)GetStockObject(GRAY_BRUSH));
        s->shown++;
    }
    DeleteDC(mem);
    if (s->shown < n) {                 /* "+5": the others are in the viewer */
        wchar_t more[16];
        _snwprintf(more, 16, L"+%d", n - s->shown); more[15] = 0;
        SelectObject(dc, f);
        SetRect(&t, x, 0, x + 34, r.bottom);
        DrawTextW(dc, more, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        x += 34;
        SelectObject(dc, under);
    }
    SetTextColor(dc, RGB(0x00, 0x55, 0xcc));
    SetRect(&s->link, x + 4, (r.bottom - sz.cy) / 2, x + 4 + sz.cx, (r.bottom + sz.cy) / 2);
    DrawTextW(dc, w, -1, &s->link, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, f);
    DeleteObject(under);
}

/* the thumbnail at that point, -1 if none */
static int ts_hit(const strip *s, int px, int py)
{
    RECT r; HDC dc; SIZE sz; wchar_t w[200]; HFONT f; int x, i;
    if (!s->list || !s->list->n) return -1;
    GetClientRect(s->wnd, &r);
    dc = GetDC(s->wnd);
    f = (HFONT)SendMessageW(s->wnd, WM_GETFONT, 0, 0);
    SelectObject(dc, f ? f : ui_font());
    _snwprintf(w, 200, tr(T_STRIP_COUNT), s->list->n); w[199] = 0;
    GetTextExtentPoint32W(dc, w, (int)wcslen(w), &sz);
    ReleaseDC(s->wnd, dc);
    x = sz.cx + 8;
    if (py < (r.bottom - TS_SIZE) / 2 || py >= (r.bottom + TS_SIZE) / 2) return -1;
    for (i = 0; i < s->shown; i++, x += TS_SIZE + TS_GAP)
        if (px >= x && px < x + TS_SIZE) return i;
    return -1;
}

static LRESULT CALLBACK ts_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    strip *s = (strip *)GetWindowLongPtrW(w, GWLP_USERDATA); POINT pt; int i;
    switch (m) {
    case WM_CREATE:
        s = (strip *)calloc(1, sizeof(strip));
        if (!s) return -1;
        s->wnd = w;
        s->id = (int)(INT_PTR)((CREATESTRUCTW *)lp)->hMenu;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)s);
        return 0;
    case WM_DESTROY:
        if (s) { ts_clear(s); free(s); }
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        return 0;
    case WM_SETFONT:
        SetPropW(w, L"font", (HANDLE)wp);
        return 0;
    case WM_GETFONT:
        return (LRESULT)GetPropW(w, L"font");
    case WM_NCDESTROY:
        RemovePropW(w, L"font");
        break;
    case WM_TS_LIST:
        if (s && (LONG)wp == s->gen) {
            ts_clear(s);
            s->list = (ts_list *)lp;
            InvalidateRect(w, NULL, TRUE);
        } else free((void *)lp);
        return 0;
    case WM_TS_THUMB:
        i = (int)(wp & 0xFFF);
        if (s && (LONG)(wp >> 12) == (s->gen & 0xFFFFF) && s->list && i < s->list->n && i < META_MAXIMG) {
            if (s->thumb[i]) DeleteObject(s->thumb[i]);
            s->thumb[i] = (HBITMAP)lp;
            InvalidateRect(w, NULL, FALSE);
        } else if (lp) DeleteObject((HBITMAP)lp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
        if (s) ts_paint(s, dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (s && LOWORD(lp) == HTCLIENT) {
            GetCursorPos(&pt);
            ScreenToClient(w, &pt);
            if (PtInRect(&s->link, pt) || ts_hit(s, pt.x, pt.y) >= 0) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONUP:
        if (!s) break;
        pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
        if (PtInRect(&s->link, pt))
            PostMessageW(GetParent(w), WM_COMMAND, MAKEWPARAM(s->id, THUMBS_LINK), (LPARAM)w);
        else if ((i = ts_hit(s, pt.x, pt.y)) >= 0)
            PostMessageW(GetParent(w), WM_COMMAND, MAKEWPARAM(s->id, THUMBS_VIEW + i), (LPARAM)w);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

HWND thumbs_create(HWND parent, int id)
{
    static int registered;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = ts_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = TS_CLASS;
        registered = RegisterClassW(&wc) != 0;
    }
    return CreateWindowExW(0, TS_CLASS, L"", WS_CHILD, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(NULL), NULL);
}

void thumbs_show(HWND w, const char *bagdir)
{
    strip *s = (strip *)GetWindowLongPtrW(w, GWLP_USERDATA); ts_job *job; HANDLE th;
    if (!s) return;
    s->gen = (s->gen + 1) & 0xFFFFF;
    s->has_box = bagdir != NULL;
    if (!bagdir) { ts_clear(s); InvalidateRect(w, NULL, TRUE); return; }
    imgdec_ready();
    if (!(job = (ts_job *)calloc(1, sizeof(ts_job)))) return;
    job->wnd = w; job->gen = s->gen;
    str_copy(job->bagdir, sizeof(job->bagdir), bagdir);
    th = CreateThread(NULL, 0, ts_worker, job, 0, NULL);
    if (th) CloseHandle(th); else free(job);
}

void thumbs_view(HWND w, HWND owner, int first)
{
    strip *s = (strip *)GetWindowLongPtrW(w, GWLP_USERDATA); ts_list *copy; size_t bytes;
    if (!s || !s->list || !s->list->n) return;
    /* a copy: the strip may be filled again while the viewer is open */
    bytes = sizeof(ts_list) + (size_t)s->list->n * sizeof(view_item);
    if (!(copy = (ts_list *)malloc(bytes))) return;
    memcpy(copy, s->list, bytes);
    image_viewer(owner, copy->item, copy->n, first);
    free(copy);
}
