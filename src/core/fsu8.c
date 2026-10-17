#include <stdlib.h>
#include <string.h>
#include "fsu8.h"

static int u8_rmdir(const char *path);

typedef struct {
    const char *root; char rel[1024];
    void (*fn)(void *, const char *); void *ctx;
} walker;

static void walk_one(void *c, const char *name, int isdir)
{
    walker *w = (walker *)c; char full[1024];
    size_t len = strlen(w->rel), k = strlen(name);
    if (len + k + 2 > sizeof(w->rel)) return;      /* path too long: it is not there */
    if (len) w->rel[len] = '/';
    memcpy(w->rel + len + (len ? 1 : 0), name, k + 1);
    if (isdir) {
        u8_join(full, sizeof(full), w->root, w->rel);
        u8_listdir(full, walk_one, w);
    } else {
        w->fn(w->ctx, w->rel);
    }
    w->rel[len] = 0;
}

int u8_walk(const char *dir, void (*fn)(void *ctx, const char *rel), void *ctx)
{
    walker w;
    w.root = dir; w.rel[0] = 0; w.fn = fn; w.ctx = ctx;
    return u8_listdir(dir, walk_one, &w);
}

typedef struct { const char *dir; int bad; } rm_ctx;

static void rm_one(void *c, const char *name, int isdir)
{
    rm_ctx *r = (rm_ctx *)c; char full[1024];
    u8_join(full, sizeof(full), r->dir, name);
    if (isdir ? u8_rmtree(full) : u8_remove(full)) r->bad = 1;
}

int u8_rmtree(const char *path)
{
    rm_ctx r;
    if (!u8_isdir(path)) return u8_remove(path);
    r.dir = path; r.bad = 0;
    u8_listdir(path, rm_one, &r);
    return (u8_rmdir(path) || r.bad) ? -1 : 0;
}

typedef struct { const char *dir; long long n; } size_ctx;

static void size_one(void *c, const char *rel)
{
    size_ctx *s = (size_ctx *)c; char full[1024]; long long n;
    u8_join(full, sizeof(full), s->dir, rel);
    if ((n = u8_filesize(full)) > 0) s->n += n;
}

long long u8_treesize(const char *path)
{
    size_ctx s;
    if (!u8_isdir(path)) return u8_filesize(path);
    s.dir = path; s.n = 0;
    u8_walk(path, size_one, &s);
    return s.n;
}

void u8_join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (!la)
        snprintf(out, n, "%s", b);
    else if (a[la - 1] == '/' || a[la - 1] == '\\')
        snprintf(out, n, "%s%s", a, b);
    else
        snprintf(out, n, "%s" U8_SEP "%s", a, b);
}

int u8_mkdirs(const char *path)
{
    char p[1024]; size_t i, n = strlen(path);
    if (n == 0 || n >= sizeof(p)) return -1;
    memcpy(p, path, n + 1);
    /* one prefix at a time; intermediate errors (the drive "C:", the
     * server of a network path, folders that exist) do not count */
    for (i = 1; i < n; i++) {
        if (p[i] != '/' && p[i] != '\\') continue;
        if (p[i - 1] == '/' || p[i - 1] == '\\' || p[i - 1] == ':') continue;
        p[i] = 0;
        u8_mkdir(p);
        p[i] = path[i];
    }
    return u8_mkdir(p) == 0 && u8_isdir(p) ? 0 : -1;
}

#ifdef _WIN32
#include <windows.h>

/* the caller frees it with free() */
static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = (char *)malloc(n);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

char *u8_to_cp(const char *s, unsigned cp)
{
    wchar_t *w = wide(s); char *out = NULL; int n;
    if (!w) return NULL;
    n = WideCharToMultiByte(cp, 0, w, -1, NULL, 0, NULL, NULL);
    if (n > 0 && (out = (char *)malloc(n)) != NULL && !WideCharToMultiByte(cp, 0, w, -1, out, n, NULL, NULL)) {
        free(out);
        out = NULL;
    }
    free(w);
    return out;
}

FILE *u8_fopen(const char *path, const char *mode)
{
    wchar_t *wp = wide(path), *wm = wide(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp); free(wm);
    return f;
}

int u8_mkdir(const char *path)
{
    wchar_t *wp = wide(path); int ok;
    if (!wp) return -1;
    ok = CreateDirectoryW(wp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(wp);
    return ok ? 0 : -1;
}

int u8_remove(const char *path)
{
    wchar_t *wp = wide(path); int ok;
    if (!wp) return -1;
    ok = DeleteFileW(wp);
    free(wp);
    return ok ? 0 : -1;
}

static int u8_rmdir(const char *path)
{
    wchar_t *wp = wide(path); int ok;
    if (!wp) return -1;
    ok = RemoveDirectoryW(wp);
    free(wp);
    return ok ? 0 : -1;
}

int u8_rename_replace(const char *from, const char *to)
{
    wchar_t *wf = wide(from), *wt = wide(to); int ok = 0;
    if (wf && wt) ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING);
    free(wf); free(wt);
    return ok ? 0 : -1;
}

static DWORD attrs(const char *path)
{
    wchar_t *wp = wide(path); DWORD a;
    if (!wp) return INVALID_FILE_ATTRIBUTES;
    a = GetFileAttributesW(wp);
    free(wp);
    return a;
}

int u8_exists(const char *path) { return attrs(path) != INVALID_FILE_ATTRIBUTES; }

int u8_isdir(const char *path)
{
    DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

long long u8_filesize(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA d; wchar_t *wp = wide(path); BOOL ok;
    if (!wp) return -1;
    ok = GetFileAttributesExW(wp, GetFileExInfoStandard, &d);
    free(wp);
    if (!ok || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

int u8_listdir(const char *dir, void (*fn)(void *, const char *, int), void *ctx)
{
    char pat[1024]; wchar_t *wp; WIN32_FIND_DATAW fd; HANDLE h;
    u8_join(pat, sizeof(pat), dir, "*");
    wp = wide(pat);
    if (!wp) return -1;
    h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char *name;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        name = narrow(fd.cFileName);
        if (name) {
            fn(ctx, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
            free(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

FILE *u8_fopen(const char *path, const char *mode) { return fopen(path, mode); }

char *u8_to_cp(const char *s, unsigned cp)
{
    size_t n = strlen(s) + 1; char *out = (char *)malloc(n);
    (void)cp;
    if (out) memcpy(out, s, n);
    return out;
}

int u8_mkdir(const char *path)
{
    return (mkdir(path, 0777) == 0 || errno == EEXIST) ? 0 : -1;
}

int u8_remove(const char *path) { return remove(path) == 0 ? 0 : -1; }

static int u8_rmdir(const char *path) { return rmdir(path) == 0 ? 0 : -1; }

int u8_rename_replace(const char *from, const char *to)
{
    return rename(from, to) == 0 ? 0 : -1;
}

int u8_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

int u8_isdir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

long long u8_filesize(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) return -1;
    return (long long)st.st_size;
}

int u8_listdir(const char *dir, void (*fn)(void *, const char *, int), void *ctx)
{
    DIR *d = opendir(dir); struct dirent *e; char full[1024];
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        u8_join(full, sizeof(full), dir, e->d_name);
        fn(ctx, e->d_name, u8_isdir(full));
    }
    closedir(d);
    return 0;
}
#endif
