#include "gui/imgdec.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* GDI+'s flat API, only what is used here */
typedef struct { UINT32 version; void *debug_cb; BOOL no_thread, no_codecs; } gdip_input;
typedef int (WINAPI *startup_fn)(ULONG_PTR *, const gdip_input *, void *);
typedef int (WINAPI *load_fn)(const WCHAR *, void **);
typedef int (WINAPI *dim_fn)(void *, UINT *);
typedef int (WINAPI *dispose_fn)(void *);
typedef int (WINAPI *scan0_fn)(int, int, int, int, BYTE *, void **);
typedef int (WINAPI *context_fn)(void *, void **);
typedef int (WINAPI *clear_fn)(void *, DWORD);
typedef int (WINAPI *mode_fn)(void *, int);
typedef int (WINAPI *draw_fn)(void *, void *, int, int, int, int);
typedef int (WINAPI *hbitmap_fn)(void *, HBITMAP *, DWORD);

#define PIXEL_24BPP_RGB 0x00021808
#define INTERPOLATION_HQ_BICUBIC 7      /* averages the source: a 600 dpi scan as a thumbnail */
#define WHITE 0xFFFFFFFF

static struct {
    int tried, ok;
    startup_fn startup; load_fn load; dim_fn width, height; dispose_fn dispose_image, delete_graphics;
    scan0_fn scan0; context_fn context; clear_fn clear; mode_fn mode; draw_fn draw; hbitmap_fn hbitmap;
} g;
static wchar_t g_cache[MAX_PATH];

static FARPROC sym(HMODULE m, const char *name, int *missing)
{
    FARPROC p = GetProcAddress(m, name);
    if (!p) *missing = 1;
    return p;
}

#define SYM(field, type, name) g.field = (type)(void (*)(void))sym(m, name, &missing)

int imgdec_ready(void)
{
    HMODULE m; ULONG_PTR token; gdip_input in; int missing = 0;
    if (g.tried) return g.ok;
    g.tried = 1;
    if (!(m = LoadLibraryW(L"gdiplus.dll"))) return 0;
    SYM(startup, startup_fn, "GdiplusStartup");
    SYM(load, load_fn, "GdipLoadImageFromFile");
    SYM(width, dim_fn, "GdipGetImageWidth");
    SYM(height, dim_fn, "GdipGetImageHeight");
    SYM(dispose_image, dispose_fn, "GdipDisposeImage");
    SYM(delete_graphics, dispose_fn, "GdipDeleteGraphics");
    SYM(scan0, scan0_fn, "GdipCreateBitmapFromScan0");
    SYM(context, context_fn, "GdipGetImageGraphicsContext");
    SYM(clear, clear_fn, "GdipGraphicsClear");
    SYM(mode, mode_fn, "GdipSetInterpolationMode");
    SYM(draw, draw_fn, "GdipDrawImageRectI");
    SYM(hbitmap, hbitmap_fn, "GdipCreateHBITMAPFromBitmap");
    if (missing) return 0;
    memset(&in, 0, sizeof(in));
    in.version = 1;
    /* started once and never shut down: the process ends with it */
    g.ok = g.startup(&token, &in, NULL) == 0;
    return g.ok;
}

HBITMAP imgdec_fit(const wchar_t *path, int maxw, int maxh, int *w, int *h)
{
    void *img = NULL, *bmp = NULL, *gr = NULL; UINT iw = 0, ih = 0; HBITMAP out = NULL; int tw, th;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!g.ok || maxw < 1 || maxh < 1) return NULL;
    if (g.load(path, &img) || !img) return NULL;
    g.width(img, &iw); g.height(img, &ih);
    if (iw && ih) {
        tw = (int)iw; th = (int)ih;
        if (tw > maxw) { th = (int)((double)ih * maxw / iw); tw = maxw; }
        if (th > maxh) { tw = (int)((double)iw * maxh / ih); th = maxh; }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        if (g.scan0(tw, th, 0, PIXEL_24BPP_RGB, NULL, &bmp) == 0 && g.context(bmp, &gr) == 0) {
            g.clear(gr, WHITE);
            g.mode(gr, INTERPOLATION_HQ_BICUBIC);
            if (g.draw(gr, img, 0, 0, tw, th) == 0 && g.hbitmap(bmp, &out, WHITE) == 0 && out) {
                if (w) *w = tw;
                if (h) *h = th;
            } else out = NULL;
        }
    }
    if (gr) g.delete_graphics(gr);
    if (bmp) g.dispose_image(bmp);
    g.dispose_image(img);
    return out;
}

void imgdec_cache_dir(const wchar_t *dir)
{
    wcsncpy(g_cache, dir, MAX_PATH);
    g_cache[MAX_PATH - 1] = 0;
}

/* a size x size 24-bit DIB section, white; bits gets its pixels */
static HBITMAP canvas(int size, void **bits)
{
    BITMAPINFO bi; HBITMAP b;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size; bi.bmiHeader.biHeight = size;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
    if (b && *bits) memset(*bits, 0xFF, (size_t)(((size * 3 + 3) & ~3) * size));
    return b;
}

static int cache_path(const char *sha256, int size, wchar_t *out, int n)
{
    wchar_t w[80]; int i;
    if (!g_cache[0] || !sha256 || strlen(sha256) != 64) return 0;
    for (i = 0; i < 64; i++) w[i] = (wchar_t)sha256[i];
    w[64] = 0;
    return _snwprintf(out, n, L"%ls\\%ls-%d.bmp", g_cache, w, size) > 0;
}

/* the cache is only a convenience: a thumbnail that cannot be written is
 * decoded again the next time */
static void cache_write(const wchar_t *file, int size, const void *bits)
{
    BITMAPFILEHEADER fh; BITMAPINFOHEADER ih; HANDLE f; DWORD n, bytes = (DWORD)(((size * 3 + 3) & ~3) * size);
    wchar_t tmp[MAX_PATH + 8];
    CreateDirectoryW(g_cache, NULL);
    _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", file); tmp[MAX_PATH + 7] = 0;
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + bytes;
    ih.biSize = sizeof(ih); ih.biWidth = size; ih.biHeight = size; ih.biPlanes = 1; ih.biBitCount = 24;
    ih.biSizeImage = bytes;
    if (WriteFile(f, &fh, sizeof(fh), &n, NULL) && WriteFile(f, &ih, sizeof(ih), &n, NULL)
        && WriteFile(f, bits, bytes, &n, NULL) && n == bytes) {
        CloseHandle(f);
        if (!MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
        return;
    }
    CloseHandle(f);
    DeleteFileW(tmp);
}

HBITMAP imgdec_thumb(const wchar_t *path, const char *sha256, int size)
{
    wchar_t file[MAX_PATH]; HBITMAP pic, out; void *bits = NULL; HDC a, b; HGDIOBJ oa, ob; int w, h;
    int cached = cache_path(sha256, size, file, MAX_PATH);
    if (cached) {
        out = (HBITMAP)LoadImageW(NULL, file, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (out) return out;
    }
    if (!(pic = imgdec_fit(path, size, size, &w, &h))) return NULL;
    if (!(out = canvas(size, &bits))) { DeleteObject(pic); return NULL; }
    a = CreateCompatibleDC(NULL); b = CreateCompatibleDC(NULL);
    oa = SelectObject(a, out); ob = SelectObject(b, pic);
    BitBlt(a, (size - w) / 2, (size - h) / 2, w, h, b, 0, 0, SRCCOPY);
    GdiFlush();
    SelectObject(a, oa); SelectObject(b, ob);
    DeleteDC(a); DeleteDC(b);
    DeleteObject(pic);
    if (cached) cache_write(file, size, bits);
    return out;
}

HBITMAP imgdec_placeholder(const wchar_t *path, int size)
{
    void *bits = NULL; HBITMAP out = canvas(size, &bits); HDC dc; HGDIOBJ o, of; RECT r; HBRUSH br; HFONT f;
    const wchar_t *dot = wcsrchr(path, L'.'); wchar_t ext[12]; int i;
    if (!out) return NULL;
    for (i = 0; dot && dot[i + 1] && i < 10; i++) ext[i] = (wchar_t)towupper(dot[i + 1]);
    ext[dot ? i : 0] = 0;
    dc = CreateCompatibleDC(NULL);
    o = SelectObject(dc, out);
    SetRect(&r, 0, 0, size, size);
    br = CreateSolidBrush(RGB(0xe4, 0xe4, 0xe4));
    FillRect(dc, &r, br);
    DeleteObject(br);
    FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    f = CreateFontW(-(size / 4 > 9 ? size / 4 : 9), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Tahoma");
    of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x60, 0x60, 0x60));
    DrawTextW(dc, ext, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(f);
    SelectObject(dc, o);
    DeleteDC(dc);
    return out;
}
