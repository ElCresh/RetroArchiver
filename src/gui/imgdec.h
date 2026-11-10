/* imgdec - pictures for the window: a scan decoded and scaled to fit, and
 * the thumbnails with their cache. The decoder is GDI+ (JPEG, PNG, TIFF,
 * BMP, GIF), loaded with LoadLibrary when first needed and never linked: it
 * stays out of the imports, and a Windows without it only loses the
 * pictures (the placeholder says the file's type, and Windows opens it).
 * Verified on XP SP3: a plain LoadLibraryW("gdiplus.dll") finds the copy in
 * WinSxS through the system's default activation context, with the
 * window's own manifest; no side-by-side dependency is needed.
 * Nothing here changes a file of the archive: the thumbnails are a cache
 * outside it, by the SHA-256 of the image. Windows only. */
#ifndef RA_IMGDEC_H
#define RA_IMGDEC_H

#include <windows.h>

/* 1 if GDI+ is there. The first call loads and starts it: make it from the
 * window's thread before any other thread decodes */
int imgdec_ready(void);
/* the picture scaled down to fit maxw x maxh (never enlarged), on white; w
 * and h get its size. NULL if it cannot be decoded (a PDF, a TIFF GDI+ does
 * not know, no GDI+). Any thread; the caller deletes the bitmap */
HBITMAP imgdec_fit(const wchar_t *path, int maxw, int maxh, int *w, int *h);
/* where the thumbnails are kept (%APPDATA%\RetroArchiver\thumbs), created
 * when the first one is written */
void imgdec_cache_dir(const wchar_t *dir);
/* a size x size thumbnail of the picture, centred on white: from the cache
 * if sha256 (may be "" or NULL: no cache) has one, otherwise decoded and
 * kept there. NULL if it cannot be decoded */
HBITMAP imgdec_thumb(const wchar_t *path, const char *sha256, int size);
/* a size x size grey card with the file's type on it (".PDF"), for what has
 * no thumbnail */
HBITMAP imgdec_placeholder(const wchar_t *path, int size);

#endif
