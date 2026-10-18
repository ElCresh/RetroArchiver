/* optical - CD and DVD with DiscImageCreator (DIC), like floppy.c with
 * SAMdisk.
 *
 * Each read is a folder (data/CD01/) with everything DIC writes: one .bin
 * per track and the .cue, .img/.ccd/.sub, the _*.txt logs and the .dat
 * with the fingerprints (CRC32, MD5, SHA-1) of each track; for a DVD the
 * .iso. On top of that there is dic-console.txt, DIC's screen output
 * without the progress lines: that is where the C2 check result and the
 * detected protection are.
 *
 * Verification: the track fingerprints in the .dat are looked up in the
 * redump.org DATs placed next to the program; if the disc is there, one
 * read is enough. Otherwise a second read and a comparison of the
 * fingerprints.
 *
 * DIC options (version 20260101, Doc and README.md of the package):
 *   cd  <drive> <file.bin> <speed> /c2 20 /sf /q
 *       /c2 20: rereads sectors with C2 errors up to 20 times;
 *       /sf: looks for protections in the files (SafeDisc, LaserLock...)
 *       and does not insist on their intentional errors; /q: no beeps,
 *       the window does them
 *   dvd <drive> <file.iso> <speed> /q
 *   ls  <drive>: the drive responds (0) or not (1, "Access is denied")
 * DIC launched without arguments or after an error stops on "Press any
 * key": runner gives it NUL as input and it goes on by itself. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"
#include "fsu8.h"
#include "iso9660.h"
#include "sha256.h"

#define CD_SPEED  "16"          /* slow enough for a reliable C2 check */
#define DVD_SPEED "8"
#define C2_REREAD "20"
#define CONSOLE   "dic-console.txt"
#define MAXTRK    100

/* DIC starts but Windows cannot find a DLL: on XP the Visual C++ runtimes */
#define DLL_NOT_FOUND ((int)0xC0000135)
#define ENTRY_NOT_FOUND ((int)0xC0000139)
#define INIT_FAILED ((int)0xC0000142)

static char dic[512] = "DiscImageCreator";
static char drive[2] = "D";
static char datdir[512] = "";

void optical_set_dic(const char *path) { str_copy(dic, sizeof(dic), path); }
void optical_set_datdir(const char *dir) { str_copy(datdir, sizeof(datdir), dir); }

void optical_set_drive(const char *d)
{
    while (*d && !isalpha((unsigned char)*d)) d++;
    if (*d) { drive[0] = (char)toupper((unsigned char)*d); drive[1] = 0; }
}

/* ---- where the program is: the redump DATs are looked for there ---- */

#ifdef _WIN32
#include <windows.h>
static void exe_dir(char *out, size_t n)
{
    wchar_t w[MAX_PATH], *p;
    out[0] = 0;
    if (!GetModuleFileNameW(NULL, w, MAX_PATH)) return;
    if ((p = wcsrchr(w, L'\\')) != NULL) *p = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL)) out[0] = 0;
}
#else
#include <unistd.h>
static void exe_dir(char *out, size_t n)
{
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    char *p;
    out[k > 0 ? k : 0] = 0;
    if ((p = strrchr(out, '/')) != NULL) *p = 0;
}
#endif

static void dat_dir(char *out, size_t n)
{
    if (datdir[0]) str_copy(out, n, datdir);
    else exe_dir(out, n);
}

/* ---- small helpers ---- */

static const char *base_of(const char *path)
{
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > a) a = b;
    return a ? a + 1 : path;
}

static int ends_with_ci(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail), i;
    if (a < b) return 0;
    for (i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)tail[i])) return 0;
    return 1;
}

static void trim_eol(char *s)
{
    size_t k = strlen(s);
    while (k > 0 && (s[k - 1] == '\n' || s[k - 1] == '\r' || s[k - 1] == ' ' || s[k - 1] == '\t')) s[--k] = 0;
}

/* the value of attr="..." in an XML line, with the most common entities */
static int xml_attr(const char *line, const char *attr, char *out, size_t n)
{
    char key[32]; const char *p, *e; size_t k = 0;
    snprintf(key, sizeof(key), " %s=\"", attr);
    if (!(p = strstr(line, key))) return 0;
    p += strlen(key);
    if (!(e = strchr(p, '"'))) return 0;
    while (p < e && k + 1 < n) {
        if (*p == '&') {
            static const char *ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" },
                                            { "&quot;", "\"" }, { "&apos;", "'" } };
            size_t i;
            for (i = 0; i < 5; i++)
                if (!strncmp(p, ent[i][0], strlen(ent[i][0]))) { out[k++] = ent[i][1][0]; p += strlen(ent[i][0]); break; }
            if (i < 5) continue;
        }
        out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}

/* "<tag>value</tag>" on one line */
static int xml_text(const char *line, const char *tag, char *out, size_t n)
{
    char open[32], close[32]; const char *p, *e;
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    if (!(p = strstr(line, open))) return 0;
    p += strlen(open);
    if (!(e = strstr(p, close))) return 0;
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return 1;
}

/* a DIC progress line: it ends with "number/number" ("Creating
 * .scm (LBA)  1234/300000"); it does not go into the logs, only to the bar */
int optical_progress(const char *line, long *done, long *total)
{
    const char *p = line + strlen(line), *slash;
    while (p > line && p[-1] == ' ') p--;
    if (p == line || !isdigit((unsigned char)p[-1])) return 0;
    while (p > line && isdigit((unsigned char)p[-1])) p--;
    while (p > line && p[-1] == ' ') p--;           /* "%6d/%6d": spaces after the slash too */
    if (p == line || p[-1] != '/') return 0;
    slash = p - 1;
    p = slash;
    while (p > line && p[-1] == ' ') p--;
    if (p == line || !isdigit((unsigned char)p[-1])) return 0;
    while (p > line && isdigit((unsigned char)p[-1])) p--;
    if (done) *done = atol(p);
    if (total) *total = atol(slash + 1);
    return 1;
}

/* ---- DIC output: on screen and in the read's log ---- */

typedef struct {
    FILE *f; runner_line_fn fn; void *ctx;
    int wrong;          /* "Wrong command. The disc isn't CD" */
    int denied;         /* "GetLastError: 5" */
    char last[300];     /* the last meaningful line: it goes into the error message */
} tee;

/* DIC housekeeping lines that do not explain an error */
static int boring(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    return !*p || !strncmp(p, "StartTime", 9) || !strncmp(p, "EndTime", 7) || !strncmp(p, "AppVersion", 10) ||
           !strncmp(p, "32 bit", 6) || !strncmp(p, "64 bit", 6) || optical_progress(p, NULL, NULL);
}

static void tee_line(void *c, const char *line)
{
    tee *t = (tee *)c;
    if (strstr(line, "Wrong command")) t->wrong = 1;
    if (strstr(line, "GetLastError: 5,")) t->denied = 1;
    if (!boring(line)) str_copy(t->last, sizeof(t->last), line);
    /* "Remains 3 c2 error. Rereading times: 20/ 20" is also a result */
    if (t->f && (!optical_progress(line, NULL, NULL) || strstr(line, "error"))) fprintf(t->f, "%s\n", line);
    if (t->fn) t->fn(t->ctx, line);
}

static int dll_missing(int code)
{
    return code == DLL_NOT_FOUND || code == ENTRY_NOT_FOUND || code == INIT_FAILED;
}

/* a missing or non-starting DIC must be reported as such, not as a damaged disc */
static int no_tool(int code, runner_line_fn on_line, void *ctx)
{
    char line[900];
    if (dll_missing(code))
        snprintf(line, sizeof(line), "%s non parte: mancano i runtime di Visual C++ 2015-2019 x86 "
                                     "(VCRUNTIME140.dll). Su Windows XP installa la versione 14.27 "
                                     "del Visual C++ Redistributable.", dic);
    else
        snprintf(line, sizeof(line), "Non trovo %s: mettilo nella stessa cartella del programma "
                                     "o nel PATH, oppure indica il percorso completo di "
                                     "DiscImageCreator.exe.", dic);
    if (on_line) on_line(ctx, line);
    return MEDIA_NOTOOL;
}

static void say(runner_line_fn on_line, void *ctx, const char *text)
{
    if (on_line) on_line(ctx, text);
}

/* ---- check: the drive responds ---- */

static int check(strbuf *msg)
{
    const char *argv[] = { dic, "ls", drive, NULL };
    tee t; int code = 0, rc;
    memset(&t, 0, sizeof(t));
    rc = runner_run(argv, tee_line, &t, &code);
    if (rc == RUNNER_NOSTART) {
        sb_printf(msg, "Non trovo %s: mettilo nella stessa cartella del programma o nel PATH.", dic);
        return -1;
    }
    if (rc != RUNNER_DONE) {
        sb_printf(msg, "Controllo del lettore %s: annullato, riprova quando vuoi.", drive);
        return -1;
    }
    if (dll_missing(code)) {
        sb_printf(msg, "%s non parte: mancano i runtime di Visual C++ 2015-2019 x86.", dic);
        return -1;
    }
    if (code != 0) {
        if (t.denied)
            sb_printf(msg, "Non riesco ad aprire il lettore %s: (accesso negato): controlla la lettera "
                           "dell'unita' e lancia il programma da un utente amministratore.", drive);
        else
            sb_printf(msg, "Il lettore %s: non risponde (DiscImageCreator ls ha dato errore %d): "
                           "controlla la lettera dell'unita'.", drive, code);
        return -1;
    }
    return 0;
}

/* ---- scan: the drive and the disc's TOC ---- */

/* the disc's sectors according to the last TOC read (-1 = unknown): the
 * estimate of the space the read needs */
static long toc_sectors = -1;

#ifdef _WIN32
/* IOCTL_CDROM_READ_TOC: TOC in MSF, as the drive's driver returns it */
#define RA_IOCTL_CDROM_READ_TOC 0x00024000
typedef struct { UCHAR reserved, control_adr, track, reserved1, addr[4]; } ra_track;
typedef struct { UCHAR length[2], first, last; ra_track t[100]; } ra_toc;

static long msf_lba(const UCHAR a[4]) { return ((long)a[1] * 60 + a[2]) * 75 + a[3] - 150; }

static void write_toc(FILE *f, runner_line_fn on_line, void *ctx)
{
    wchar_t dev[16]; HANDLE h; ra_toc toc; DWORD got = 0; char line[200]; int i, n;
    _snwprintf(dev, 16, L"\\\\.\\%c:", (wchar_t)toupper((unsigned char)drive[0]));
    dev[15] = 0;
    h = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    memset(&toc, 0, sizeof(toc));
    toc_sectors = -1;
    if (h == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(h, RA_IOCTL_CDROM_READ_TOC, NULL, 0, &toc, sizeof(toc), &got, NULL)) {
        snprintf(line, sizeof(line), "TOC: non la leggo dal lettore %c: (nessun disco, o il lettore non "
                                     "risponde; errore %lu)", toupper((unsigned char)drive[0]),
                 (unsigned long)GetLastError());
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        fprintf(f, "%s\n", line);
        say(on_line, ctx, line);
        return;
    }
    CloseHandle(h);
    n = toc.last - toc.first + 1;
    if (n < 1 || n > 99) n = 0;
    snprintf(line, sizeof(line), "TOC del disco nel lettore %c: tracce %d-%d",
             toupper((unsigned char)drive[0]), toc.first, toc.last);
    fprintf(f, "%s\n", line);
    say(on_line, ctx, line);
    for (i = 0; i < n; i++) {
        long a = msf_lba(toc.t[i].addr), b = msf_lba(toc.t[i + 1].addr);
        snprintf(line, sizeof(line), "  Traccia %2d  %-5s  LBA %7ld  %7ld settori (%02ld:%02ld)",
                 toc.t[i].track, (toc.t[i].control_adr & 4) ? "dati" : "audio", a, b - a,
                 (b - a) / 75 / 60, (b - a) / 75 % 60);
        fprintf(f, "%s\n", line);
        say(on_line, ctx, line);
    }
    if (n) {
        long end = msf_lba(toc.t[n].addr);
        toc_sectors = end;
        snprintf(line, sizeof(line), "  Totale %ld settori (%ld MB di dati utente)%s", end,
                 end * 2048L / 1048576L, end > 405000 ? ": piu' lungo di un CD, forse e' un DVD" : "");
        fprintf(f, "%s\n", line);
        say(on_line, ctx, line);
    }
}
#else
static void write_toc(FILE *f, runner_line_fn on_line, void *ctx)
{
    const char *line = "TOC: la lettura diretta dal lettore c'e' solo su Windows";
    fprintf(f, "%s\n", line);
    say(on_line, ctx, line);
}
#endif

static int scan(const char *out_txt, runner_line_fn on_line, void *ctx)
{
    const char *argv[] = { dic, "ls", drive, NULL };
    tee t; int code = 0, rc;
    toc_sectors = -1;
    memset(&t, 0, sizeof(t));
    t.f = u8_fopen(out_txt, "wb"); t.fn = on_line; t.ctx = ctx;
    if (!t.f) return -1;
    rc = runner_run(argv, tee_line, &t, &code);
    if (rc == RUNNER_NOSTART || (rc == RUNNER_DONE && dll_missing(code))) {
        fclose(t.f);
        u8_remove(out_txt);
        return no_tool(rc == RUNNER_NOSTART ? 0 : code, on_line, ctx);
    }
    if (rc == RUNNER_CANCELED) { fclose(t.f); return 1; }
    if (code != 0) {
        char line[300];
        snprintf(line, sizeof(line), t.denied
                 ? "ATTENZIONE: non riesco ad aprire il lettore %s: (accesso negato): serve un utente "
                   "amministratore, o la lettera dell'unita' e' sbagliata."
                 : "ATTENZIONE: il lettore %s: non risponde a DiscImageCreator.", drive);
        fprintf(t.f, "%s\n", line);
        say(on_line, ctx, line);
    }
    write_toc(t.f, on_line, ctx);
    fclose(t.f);
    return 0;
}

/* ---- read: the DIC read into a folder ---- */

#define MB (1024LL * 1024LL)
#define FAT_MAX 4294967295LL        /* the biggest file on FAT32 */

/* the space needed: a CD keeps the .bin tracks and also the .img with
 * the same sectors, plus .sub and .c2; a DVD is the .iso. Without a TOC
 * the estimate is an 80-minute CD or a single-layer DVD */
static long long need_bytes(int dvd)
{
    long long s = toc_sectors > 0 ? toc_sectors : (dvd ? 2295104L : 360000L);
    return dvd ? s * 2048 + 64 * MB : s * (2352LL * 2 + 96 + 294) + 64 * MB;
}

/* the folder's file system: an .iso over 4 GB does not fit on FAT32 */
int optical_fs_ok(const char *fsname, long long biggest, strbuf *why)
{
    if (fsname && !strncmp(fsname, "FAT", 3) && biggest > FAT_MAX) {
        sb_printf(why, "la cartella e' su un disco %s, che non regge file oltre 4 GB, e l'immagine di "
                       "questo disco sara' di %lld MB: scegli una cartella su un disco NTFS", fsname, biggest / MB);
        return -1;
    }
    return 0;
}

#ifdef _WIN32
static wchar_t *wide_of(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

long long optical_free_bytes(const char *dir)
{
    ULARGE_INTEGER avail; wchar_t *w = wide_of(dir); long long r = -1;
    if (w && GetDiskFreeSpaceExW(w, &avail, NULL, NULL)) r = (long long)avail.QuadPart;
    free(w);
    return r;
}

static void fs_name(const char *dir, char *out, size_t n)
{
    wchar_t *w = wide_of(dir), root[MAX_PATH], fs[32];
    out[0] = 0;
    if (w && GetVolumePathNameW(w, root, MAX_PATH) &&
        GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, 32))
        WideCharToMultiByte(CP_UTF8, 0, fs, -1, out, (int)n, NULL, NULL);
    free(w);
}

/* DIC is an ANSI program: it receives its arguments in the system code
 * page. A path with characters missing there would reach it with '?'s:
 * the folder's 8.3 short name is used, if there is one */
static int ansi_ok(const wchar_t *w)
{
    BOOL used = FALSE;
    return WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, NULL, 0, NULL, &used) > 0 && !used;
}

int optical_dic_path(const char *dir, const char *file, char *out, size_t n, strbuf *why)
{
    char full[1100]; wchar_t *w, shortp[MAX_PATH + 100]; int ok;
    u8_join(full, sizeof(full), dir, file);
    if (!(w = wide_of(full))) return -1;
    ok = ansi_ok(w);
    free(w);
    if (ok) { str_copy(out, n, full); return 0; }
    w = wide_of(dir);
    if (w && GetShortPathNameW(w, shortp, MAX_PATH) > 0) {
        wchar_t *wf = wide_of(file);
        if (wf && wcslen(shortp) + wcslen(wf) + 2 < MAX_PATH + 100) {
            wcscat(shortp, L"\\");
            wcscat(shortp, wf);
            if (ansi_ok(shortp) && WideCharToMultiByte(CP_UTF8, 0, shortp, -1, out, (int)n, NULL, NULL) > 0) {
                free(wf); free(w);
                return 0;
            }
        }
        free(wf);
    }
    free(w);
    sb_printf(why, "la cartella %s ha nel nome caratteri che DiscImageCreator (un programma ANSI) non sa "
                   "leggere, e non ha un nome corto 8.3: scegli una cartella dell'archivio con un nome "
                   "senza lettere straniere", dir);
    return -1;
}

/* the names DIC writes in the .cue and the .dat are in the ANSI code page */
static void acp_to_utf8(const char *in, char *out, size_t n)
{
    wchar_t w[600];
    if (MultiByteToWideChar(CP_ACP, 0, in, -1, w, 600) <= 0 ||
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) <= 0)
        str_copy(out, n, in);
}
#else
#include <sys/statvfs.h>
long long optical_free_bytes(const char *dir)
{
    struct statvfs s;
    if (statvfs(dir, &s)) return -1;
    return (long long)s.f_bavail * (long long)s.f_frsize;
}

static void fs_name(const char *dir, char *out, size_t n) { (void)dir; if (n) out[0] = 0; }

int optical_dic_path(const char *dir, const char *file, char *out, size_t n, strbuf *why)
{
    (void)why;
    u8_join(out, n, dir, file);
    return 0;
}

/* outside Windows the ANSI code page is taken to be Latin-1 (the tests) */
static void acp_to_utf8(const char *in, char *out, size_t n)
{
    size_t k = 0;
    const unsigned char *p = (const unsigned char *)in;
    for (; *p && k + 3 < n; p++) {
        if (*p < 0x80) out[k++] = (char)*p;
        else { out[k++] = (char)(0xC0 | (*p >> 6)); out[k++] = (char)(0x80 | (*p & 0x3F)); }
    }
    out[k] = 0;
}
#endif

static int valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0) return 0;
        for (p++; k > 0; k--, p++) if ((*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}

/* a name from the .cue or the .dat in UTF-8: as is if it already is,
 * otherwise from the ANSI code page DIC wrote it in */
void optical_name_utf8(char *s, size_t n)
{
    char tmp[600];
    if (valid_utf8(s)) return;
    acp_to_utf8(s, tmp, sizeof(tmp));
    str_copy(s, n, tmp);
}

/* is there room, on a suitable file system? -1 with the reason */
int optical_room(const char *dir, long long need, long long biggest, strbuf *why)
{
    char fs[32]; long long avail = optical_free_bytes(dir);
    fs_name(dir, fs, sizeof(fs));
    if (optical_fs_ok(fs, biggest, why)) return -1;
    if (avail >= 0 && avail < need) {
        sb_printf(why, "in %s ci sono %lld MB liberi e la lettura ne richiede circa %lld: libera spazio o "
                       "scegli un'altra cartella dell'archivio", dir, avail / MB, need / MB);
        return -1;
    }
    return 0;
}

/* the message when DIC stops: it is not always the disc's fault */
static void stop_message(char *p, size_t n, int code, const tee *t, const char *dir)
{
    char last[320]; long long avail = optical_free_bytes(dir);
    if (t->last[0]) snprintf(last, sizeof(last), " Ultima riga: \"%.280s\".", t->last);
    else last[0] = 0;
    snprintf(p, n, "DiscImageCreator si e' fermato con l'errore %d.%s Controlla che il disco sia nel "
                   "lettore %s: e pulito, e lo spazio nella cartella %s (%lld MB liberi), poi riprova.",
             code, last, drive, dir, avail >= 0 ? avail / MB : -1LL);
}

static int read_media(int dvd, const char *out_dir, runner_line_fn on_line, void *ctx)
{
    char base[96], img[1100], cue[1100], dat[1100], con[1100], arg[1100], parent[1100], *sep;
    const char *cd_argv[] = { dic, "cd", drive, arg, CD_SPEED, "/c2", C2_REREAD, "/sf", "/q", NULL };
    const char *dvd_argv[] = { dic, "dvd", drive, arg, DVD_SPEED, "/q", NULL };
    tee t; int code = 0, rc, have;
    char p[1600];
    strbuf why;
    long long need = need_bytes(dvd);

    str_copy(base, sizeof(base), base_of(out_dir));
    /* before 30 minutes of reading: is there room, and is the file system fine? */
    str_copy(parent, sizeof(parent), out_dir);
    sep = strrchr(parent, U8_SEP[0]);
    if (sep) *sep = 0;
    sb_init(&why);
    if (optical_room(u8_isdir(parent) ? parent : out_dir, need, dvd ? need : 0, &why)) {
        snprintf(p, sizeof(p), "Non leggo il %s: %s.", dvd ? "DVD" : "CD", why.s);
        say(on_line, ctx, p);
        sb_free(&why);
        return -1;
    }
    if (u8_mkdirs(out_dir)) {
        snprintf(p, sizeof(p), "Non riesco a creare la cartella %s: controlla il disco di destinazione.", out_dir);
        say(on_line, ctx, p);
        sb_free(&why);
        return -1;
    }
    snprintf(p, sizeof(p), "%s%s", base, dvd ? ".iso" : ".bin");
    u8_join(img, sizeof(img), out_dir, p);
    if (optical_dic_path(out_dir, p, arg, sizeof(arg), &why)) {
        snprintf(p, sizeof(p), "Non leggo il %s: %s.", dvd ? "DVD" : "CD", why.s);
        say(on_line, ctx, p);
        sb_free(&why);
        u8_rmtree(out_dir);
        return -1;
    }
    sb_free(&why);
    snprintf(p, sizeof(p), "%s.cue", base);
    u8_join(cue, sizeof(cue), out_dir, p);
    snprintf(p, sizeof(p), "%s.dat", base);
    u8_join(dat, sizeof(dat), out_dir, p);
    u8_join(con, sizeof(con), out_dir, CONSOLE);

    memset(&t, 0, sizeof(t));
    t.f = u8_fopen(con, "wb"); t.fn = on_line; t.ctx = ctx;
    /* in a hidden console: on a pipe DIC's C runtime buffers its output in
     * blocks, so the progress lines ("Creating .scm (LBA) 1234/300000")
     * arrived only in bursts or at the end and the bar stood still
     * (seen on the XP VM: 0 bytes in 60 s on a pipe, at once on a console) */
    rc = runner_run_tty(dvd ? dvd_argv : cd_argv, tee_line, &t, &code);
    if (rc == RUNNER_NOSTART || (rc == RUNNER_DONE && dll_missing(code))) {
        if (t.f) fclose(t.f);
        u8_rmtree(out_dir);
        return no_tool(rc == RUNNER_NOSTART ? 0 : code, on_line, ctx);
    }
    if (rc == RUNNER_CANCELED) { if (t.f) fclose(t.f); u8_rmtree(out_dir); return 1; }
    /* the .dat is the last thing DIC writes: if it is there with the .cue
     * (or the .iso) the read is complete, even if DIC exits with an error.
     * On a dying disc it may be the only good one: it is kept, and the
     * verification will tell whether it has errors */
    have = u8_filesize(dat) > 0 && (dvd ? u8_filesize(img) > 0 : u8_filesize(cue) > 0);
    if (have && code != 0 && t.f) fprintf(t.f, "RetroArchiver: DiscImageCreator e' uscito con il codice %d\n", code);
    if (t.f) fclose(t.f);
    if (have) {
        if (code != 0) {
            snprintf(p, sizeof(p), "DiscImageCreator e' uscito con l'errore %d, ma la lettura e' completa: "
                                   "la tengo e la verifica dira' se e' buona.", code);
            say(on_line, ctx, p);
        }
        return 0;
    }
    if (t.wrong)
        snprintf(p, sizeof(p), "Nel lettore non c'e' un %s: DiscImageCreator dice che il disco e' di un "
                               "altro tipo. Controlla il disco, o la composizione del cofanetto.",
                 dvd ? "DVD" : "CD");
    else if (t.denied)
        snprintf(p, sizeof(p), "DiscImageCreator non apre il lettore %s: (accesso negato): serve un "
                               "utente amministratore.", drive);
    else if (code != 0)
        stop_message(p, sizeof(p), code, &t, out_dir);
    else
        snprintf(p, sizeof(p), "DiscImageCreator ha finito ma manca %.200s: lettura incompleta.",
                 u8_filesize(dat) > 0 ? base_of(dvd ? img : cue) : base_of(dat));
    say(on_line, ctx, p);
    u8_rmtree(out_dir);         /* the .dat or the image is missing: it is not a read */
    return -1;
}

static int read_cd(const char *out, runner_line_fn fn, void *ctx) { return read_media(0, out, fn, ctx); }
static int read_dvd(const char *out, runner_line_fn fn, void *ctx) { return read_media(1, out, fn, ctx); }

/* ---- a read already done: tracks, fingerprints, errors ---- */

typedef struct {
    char name[200], mode[16];
    long long size;
    char crc[12], md5[36], sha1[44];
} track;

typedef struct {
    char base[96];
    track t[MAXTRK];
    int n;
    int c2, edc, readerr;           /* -1: unknown */
    int no_c2;                      /* the drive does not report C2 errors: DIC dropped /c2 */
    int dic_exit;                   /* DIC exited with an error but the read is complete (0 = no) */
    char protection[160];
    char tool[64], command[300], drive[120];
} dump;

typedef struct { char name[200], mode[16]; } cue_file;

/* the .cue FILEs with the mode of their first track */
static int load_cue(const char *path, cue_file *f, int max)
{
    FILE *in = u8_fopen(path, "rb"); char line[600]; int n = 0;
    if (!in) return -1;
    while (fgets(line, sizeof(line), in)) {
        char *p = line, *q;
        trim_eol(line);
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "FILE \"", 6) && n < max) {
            p += 6;
            if (!(q = strrchr(p, '"'))) continue;
            *q = 0;
            str_copy(f[n].name, sizeof(f[n].name), p);
            optical_name_utf8(f[n].name, sizeof(f[n].name));
            f[n].mode[0] = 0;
            n++;
        } else if (!strncmp(p, "TRACK ", 6) && n > 0 && !f[n - 1].mode[0]) {
            p += 6;
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
            str_copy(f[n - 1].mode, sizeof(f[n - 1].mode), p);
        }
    }
    fclose(in);
    return n;
}

/* files that are not disc tracks: service areas DIC saves separately */
static int extra_area(const char *name)
{
    return strstr(name, "(Lead-in)") || strstr(name, "(Lead-out)") || strstr(name, "(-LBA)") ||
           strstr(name, "(Pregap)") || strstr(name, "(Track all)") || strstr(name, "(Track 0)") ||
           strstr(name, "(Track 00)") || strstr(name, "(Track AA)") || strstr(name, "(Subs desync)");
}

static int last_number(const char *s)
{
    while (*s && !isdigit((unsigned char)*s)) s++;
    return atoi(s);
}

/* the lines that matter in DIC's logs */
static void scan_logs(const char *dir, dump *d)
{
    char p[1100], name[200], line[1000]; FILE *f;
    const char *logs[] = { CONSOLE, "_disc.txt", NULL };
    int i;
    d->c2 = d->edc = d->readerr = -1;
    for (i = 0; logs[i]; i++) {
        if (logs[i][0] == '_') snprintf(name, sizeof(name), "%s%s", d->base, logs[i]);
        else str_copy(name, sizeof(name), logs[i]);
        u8_join(p, sizeof(p), dir, name);
        if (!(f = u8_fopen(p, "rb"))) continue;
        while (fgets(line, sizeof(line), f)) {
            char *q;
            trim_eol(line);
            if (strstr(line, "No C2 errors") || strstr(line, "No unintentional C2 errors") ||
                strstr(line, "Correction complete (c2)")) d->c2 = 0;
            else if ((q = strstr(line, "Remains ")) && strstr(q, "c2 error")) d->c2 = last_number(q + 8);
            /* "[WARNING] This drive doesn't support reporting C2 error. Disabled /c2" */
            if (strstr(line, "doesn't support reporting C2 error")) d->no_c2 = 1;
            if ((q = strstr(line, "RetroArchiver: DiscImageCreator e' uscito con il codice ")) != NULL)
                d->dic_exit = atoi(q + 56);
            /* "Detected protection [SafeDisc]", "Detected a protected [x]. LBA..." */
            if ((q = strstr(line, "Detected protection [")) || (q = strstr(line, "Detected a protected ["))) {
                char *a = strchr(q, '['), *b = a ? strchr(a, ']') : NULL;
                if (a && b && !d->protection[0]) snprintf(d->protection, sizeof(d->protection), "%.*s", (int)(b - a - 1), a + 1);
            }
            if (strstr(line, "=> SecuROM") && !d->protection[0])
                str_copy(d->protection, sizeof(d->protection), strstr(line, "=> SecuROM") + 3);
        }
        fclose(f);
    }
    /* unreadable sectors */
    snprintf(name, sizeof(name), "%s_mainError.txt", d->base);
    u8_join(p, sizeof(p), dir, name);
    if ((f = u8_fopen(p, "rb")) != NULL) {
        d->readerr = 0;
        while (fgets(line, sizeof(line), f)) if (strstr(line, "Read error")) d->readerr++;
        fclose(f);
    }
    /* the program and the command: "CD01_20260101T233129.txt" holds the command */
    snprintf(name, sizeof(name), "%s_drive.txt", d->base);
    u8_join(p, sizeof(p), dir, name);
    if ((f = u8_fopen(p, "rb")) != NULL) {
        char vendor[40] = "", product[40] = "", rev[40] = "";
        while (fgets(line, sizeof(line), f)) {
            char *q = line, *v;
            trim_eol(line);
            while (*q == ' ' || *q == '\t') q++;
            if (!(v = strchr(q, ':'))) continue;
            *v++ = 0;
            while (*v == ' ') v++;
            if (!strcmp(q, "VendorId") && !vendor[0]) str_copy(vendor, sizeof(vendor), v);
            else if (!strcmp(q, "ProductId") && !product[0]) str_copy(product, sizeof(product), v);
            else if (!strcmp(q, "ProductRevisionLevel") && !rev[0]) str_copy(rev, sizeof(rev), v);
        }
        fclose(f);
        snprintf(d->drive, sizeof(d->drive), "%s%s%s%s%s", vendor, vendor[0] ? " " : "", product,
                 rev[0] ? " " : "", rev);
    }
}

typedef struct { dump *d; const char *dir; } edc_ctx;

/* the result of EccEdc, which DIC runs at the end of a CD: "Total errors: N" */
static void find_edc(void *c, const char *name, int isdir)
{
    edc_ctx *e = (edc_ctx *)c; char p[1100], line[400]; FILE *f;
    if (isdir || !(ends_with_ci(name, "EdcEcc.txt") || ends_with_ci(name, "EccEdc.txt"))) return;
    u8_join(p, sizeof(p), e->dir, name);
    if (!(f = u8_fopen(p, "rb"))) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "Total errors:", 13)) e->d->edc = atoi(line + 13);
        else if (strstr(line, "[NO ERROR]") && e->d->edc < 0) e->d->edc = 0;
    }
    fclose(f);
}

/* "CD01_20260101T233129.txt": the DIC version and the command */
static void find_cmd(void *c, const char *name, int isdir)
{
    edc_ctx *e = (edc_ctx *)c; size_t bl = strlen(e->d->base);
    char p[1100]; FILE *f;
    if (isdir || strncmp(name, e->d->base, bl) || name[bl] != '_' || !ends_with_ci(name, ".txt")) return;
    if (strlen(name + bl + 1) != 19 || name[bl + 9] != 'T' || !isdigit((unsigned char)name[bl + 1])) return;
    snprintf(e->d->tool, sizeof(e->d->tool), "DiscImageCreator %.15s", name + bl + 1);
    u8_join(p, sizeof(p), e->dir, name);
    if ((f = u8_fopen(p, "rb")) != NULL) {
        if (fgets(e->d->command, sizeof(e->d->command), f)) trim_eol(e->d->command);
        fclose(f);
    }
}

/* 0 ok; -1 with the reason in why */
static int load_dump(const char *dir, dump *d, strbuf *why)
{
    char p[1100], name[200], line[1200]; FILE *f;
    cue_file cf[MAXTRK]; int ncue, i;
    edc_ctx e;

    memset(d, 0, sizeof(*d));
    str_copy(d->base, sizeof(d->base), base_of(dir));
    snprintf(name, sizeof(name), "%s.cue", d->base);
    u8_join(p, sizeof(p), dir, name);
    ncue = load_cue(p, cf, MAXTRK);
    snprintf(name, sizeof(name), "%s.dat", d->base);
    u8_join(p, sizeof(p), dir, name);
    if (!(f = u8_fopen(p, "rb"))) {
        sb_printf(why, "manca %s, le impronte che DiscImageCreator scrive alla fine", name);
        return -1;
    }
    while (fgets(line, sizeof(line), f) && d->n < MAXTRK) {
        track *t = &d->t[d->n]; char size[32];
        if (!strstr(line, "<rom ") || !xml_attr(line, "name", t->name, sizeof(t->name))) continue;
        optical_name_utf8(t->name, sizeof(t->name));
        if (!(ends_with_ci(t->name, ".bin") || ends_with_ci(t->name, ".iso")) || extra_area(t->name)) continue;
        if (ncue > 0) {         /* with a .cue only its tracks count */
            for (i = 0; i < ncue && strcmp(cf[i].name, t->name); i++) ;
            if (i == ncue) continue;
            str_copy(t->mode, sizeof(t->mode), cf[i].mode);
        } else {
            str_copy(t->mode, sizeof(t->mode), ends_with_ci(t->name, ".iso") ? "DVD" : "");
        }
        xml_attr(line, "size", size, sizeof(size));
        t->size = atoll(size);
        xml_attr(line, "crc", t->crc, sizeof(t->crc));
        xml_attr(line, "md5", t->md5, sizeof(t->md5));
        xml_attr(line, "sha1", t->sha1, sizeof(t->sha1));
        d->n++;
    }
    fclose(f);
    if (!d->n) { sb_printf(why, "%s non elenca nessuna traccia", name); return -1; }
    scan_logs(dir, d);
    e.d = d; e.dir = dir;
    u8_listdir(dir, find_edc, &e);
    u8_listdir(dir, find_cmd, &e);
    return 0;
}

/* the remaining C2 errors and the bad EDC/ECC are often the same sectors:
 * count the larger of the two, plus those that cannot be read at all */
static int errors_of(const dump *d)
{
    int c2 = d->c2 > 0 ? d->c2 : 0, edc = d->edc > 0 ? d->edc : 0;
    /* DIC exiting with an error counts as one error even if the logs say nothing */
    return (c2 > edc ? c2 : edc) + (d->readerr > 0 ? d->readerr : 0) + (d->dic_exit != 0);
}

/* ---- redump: the fingerprints in the DATs next to the program ---- */

typedef struct {
    char game[256], dat[256], date[40];
} redump_hit;

typedef struct { char names[32][256]; int n; } dat_list;

static void add_dat(void *c, const char *name, int isdir)
{
    dat_list *l = (dat_list *)c;
    if (!isdir && ends_with_ci(name, ".dat") && l->n < 32) str_copy(l->names[l->n++], sizeof(l->names[0]), name);
}

/* a redump DAT: a <datafile> with a real name (DIC's default.dat and the
 * reads' .dat have "-insert name-"); date from the header */
static int dat_header(const char *path, char *date, size_t dn)
{
    FILE *f = u8_fopen(path, "rb"); char line[1000], v[200]; int k, datafile = 0, named = 0;
    if (date && dn) date[0] = 0;
    if (!f) return 0;
    for (k = 0; k < 40 && fgets(line, sizeof(line), f); k++) {
        if (strstr(line, "<datafile")) datafile = 1;
        if (xml_text(line, "name", v, sizeof(v)) && strcmp(v, "-insert name-")) named = 1;
        if (date && xml_text(line, "date", v, sizeof(v)) && strcmp(v, "-insert date-")) str_copy(date, dn, v);
        else if (date && !date[0] && xml_text(line, "version", v, sizeof(v)) && strcmp(v, "-insert version-"))
            str_copy(date, dn, v);
        if (strstr(line, "</header>")) break;
    }
    fclose(f);
    return datafile && named;
}

int optical_find_dat(char *path, size_t n, char *date, size_t dn)
{
    char dir[600], p[1100], d[40], best[40] = ""; dat_list l; int i, found = 0;
    dat_dir(dir, sizeof(dir));
    l.n = 0;
    if (path && n) path[0] = 0;
    if (date && dn) date[0] = 0;
    if (!dir[0] || u8_listdir(dir, add_dat, &l)) return 0;
    for (i = 0; i < l.n; i++) {
        u8_join(p, sizeof(p), dir, l.names[i]);
        if (!dat_header(p, d, sizeof(d))) continue;
        if (!found || strcmp(d, best) > 0) {
            str_copy(best, sizeof(best), d);
            if (path) str_copy(path, n, p);
            if (date) str_copy(date, dn, d);
        }
        found = 1;
    }
    return found;
}

static int same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* all the disc's tracks, and no others, in a single game of the DAT */
static int game_matches(const dump *d, track *g, int ng)
{
    int used[MAXTRK], i, k;
    if (ng != d->n) return 0;
    memset(used, 0, sizeof(used));
    for (i = 0; i < d->n; i++) {
        for (k = 0; k < ng; k++)
            if (!used[k] && g[k].size == d->t[i].size && same_ci(g[k].sha1, d->t[i].sha1) &&
                (!g[k].crc[0] || same_ci(g[k].crc, d->t[i].crc))) break;
        if (k == ng) return 0;
        used[k] = 1;
    }
    return 1;
}

static int match_in(const char *path, const dump *d, redump_hit *hit)
{
    FILE *f = u8_fopen(path, "rb");
    char line[1200], game[256] = "";
    track *g = (track *)malloc(MAXTRK * sizeof(track));
    int ng = 0, found = 0;
    if (!f || !g) { if (f) fclose(f); free(g); return 0; }
    while (!found && fgets(line, sizeof(line), f)) {
        if (strstr(line, "<game ") || strstr(line, "<machine ")) {
            xml_attr(line, "name", game, sizeof(game));
            ng = 0;
        } else if (strstr(line, "<rom ") && ng < MAXTRK) {
            track *t = &g[ng]; char size[32];
            if (!xml_attr(line, "name", t->name, sizeof(t->name))) continue;
            if (!(ends_with_ci(t->name, ".bin") || ends_with_ci(t->name, ".iso"))) continue;
            size[0] = 0; t->crc[0] = t->sha1[0] = 0;
            xml_attr(line, "size", size, sizeof(size));
            t->size = atoll(size);
            xml_attr(line, "crc", t->crc, sizeof(t->crc));
            xml_attr(line, "sha1", t->sha1, sizeof(t->sha1));
            ng++;
        } else if (strstr(line, "</game>") || strstr(line, "</machine>")) {
            if (game_matches(d, g, ng)) {
                found = 1;
                str_copy(hit->game, sizeof(hit->game), game);
            }
            ng = 0;
        }
    }
    fclose(f);
    free(g);
    return found;
}

/* 1 found, 0 not found, -1 no DAT; dats: how many DATs were checked */
static int redump_match(const dump *d, redump_hit *hit, char *dir, size_t dirn, int *dats)
{
    char p[1100], date[40]; dat_list l; int i;
    memset(hit, 0, sizeof(*hit));
    dat_dir(dir, dirn);
    l.n = 0;
    *dats = 0;
    if (!dir[0] || u8_listdir(dir, add_dat, &l)) return -1;
    for (i = 0; i < l.n; i++) {
        u8_join(p, sizeof(p), dir, l.names[i]);
        if (!dat_header(p, date, sizeof(date))) continue;
        (*dats)++;
        if (match_in(p, d, hit)) {
            str_copy(hit->dat, sizeof(hit->dat), l.names[i]);
            str_copy(hit->date, sizeof(hit->date), date);
            return 1;
        }
    }
    return *dats ? 0 : -1;
}

/* tech redoes the lookup at every save: the last one is kept per folder
 * and per read (the fingerprints of all tracks: the same folder may later
 * hold another read, perhaps differing only in the audio track) */
static struct { char dir[1024], key[65]; int rc; redump_hit hit; } cache[8];
static int cache_next;

static void tracks_key(const dump *d, char key[65])
{
    sha256_ctx c; unsigned char h[32]; int i;
    sha256_init(&c);
    for (i = 0; i < d->n; i++) sha256_update(&c, d->t[i].sha1, strlen(d->t[i].sha1));
    sha256_final(&c, h);
    sha256_hex(h, key);
}

static int redump_cached(const char *path, const dump *d, redump_hit *hit)
{
    char dir[600], key[65]; int i, dats;
    tracks_key(d, key);
    for (i = 0; i < 8; i++)
        if (cache[i].dir[0] && !strcmp(cache[i].dir, path) && !strcmp(cache[i].key, key)) {
            *hit = cache[i].hit;
            return cache[i].rc;
        }
    i = cache_next++ % 8;
    cache[i].rc = redump_match(d, &cache[i].hit, dir, sizeof(dir), &dats);
    str_copy(cache[i].dir, sizeof(cache[i].dir), path);
    str_copy(cache[i].key, sizeof(cache[i].key), key);
    *hit = cache[i].hit;
    return cache[i].rc;
}

/* ---- compare: verdict on one or more reads ---- */

static void describe(strbuf *o, const char *what, const dump *d)
{
    int i;
    sb_printf(o, "  %s: %d tracc%s\n", d->base, d->n, d->n == 1 ? "ia" : "e");
    for (i = 0; i < d->n; i++)
        sb_printf(o, "    %-11s %10lld byte  crc %s  sha1 %s  %s\n", d->t[i].mode[0] ? d->t[i].mode : what,
                  d->t[i].size, d->t[i].crc, d->t[i].sha1, d->t[i].name);
    if (d->c2 >= 0 || d->edc >= 0 || d->readerr >= 0)
        sb_printf(o, "    errori: C2 non risolti %d, settori con EDC/ECC sbagliati %d, settori illeggibili %d\n",
                  d->c2 > 0 ? d->c2 : 0, d->edc > 0 ? d->edc : 0, d->readerr > 0 ? d->readerr : 0);
    if (d->protection[0]) sb_printf(o, "    protezione rilevata da DiscImageCreator: %s\n", d->protection);
    if (d->no_c2) sb_puts(o, "    controllo C2 non disponibile su questo lettore: gli errori C2 non sono stati cercati\n");
    if (d->dic_exit) sb_printf(o, "    DiscImageCreator e' uscito con l'errore %d, ma la lettura e' completa\n", d->dic_exit);
}

static int same_tracks(const dump *a, const dump *b)
{
    int i;
    if (a->n != b->n) return 0;
    for (i = 0; i < a->n; i++)
        if (a->t[i].size != b->t[i].size || !same_ci(a->t[i].sha1, b->t[i].sha1)) return 0;
    return 1;
}

static int finish(compare_result *r, int code, const char *outcome, const char *verdict)
{
    r->exit_code = code;
    r->v = code == 0 ? V_IDENTICAL : code == 1 ? V_DATA_DIFFER : V_ERROR;
    str_copy(r->outcome, sizeof(r->outcome), outcome);
    str_copy(r->verdict_key, sizeof(r->verdict_key), verdict);
    return code;
}

static int compare_media(const char *what, const char *const *imgs, int n, compare_result *r)
{
    dump *d = (dump *)calloc((size_t)(n > 0 ? n : 1), sizeof(dump));
    strbuf *o = &r->detail, why;
    int i, k, best = 0, agree, rc;

    if (!d) { sb_puts(o, "Memoria insufficiente per il confronto.\n"); return finish(r, 2, "errori", "error"); }
    sb_init(&why);
    sb_printf(o, "Verifica del %s con DiscImageCreator e redump\n\n", what);
    for (i = 0; i < n; i++) {
        if (load_dump(imgs[i], &d[i], &why)) {
            sb_printf(o, "  %s: %s.\n\n  La lettura non e' completa: rileggi il %s.\n", base_of(imgs[i]), why.s, what);
            sb_free(&why);
            free(d);
            return finish(r, 2, "errori", "error");
        }
        describe(o, what, &d[i]);
    }
    sb_free(&why);
    sb_puts(o, "\n");

    if (n == 1) {
        redump_hit hit; char dir[600]; int dats, m = redump_match(&d[0], &hit, dir, sizeof(dir), &dats);
        r->agree = 1;
        if (m == 1) {
            sb_printf(o, "  Redump: coincide con \"%s\"\n  (DAT %s, del %s).\n\n", hit.game, hit.dat,
                      hit.date[0] ? hit.date : "data non indicata");
            sb_printf(o, "  Il %s e' identico a quello verificato da altri archivisti: una lettura basta.\n", what);
            free(d);
            return finish(r, 0, "verificato redump", "redump");
        }
        if (m < 0) sb_printf(o, "  Redump: nessun DAT di redump in %s.\n", dir[0] ? dir : "(cartella del programma)");
        else sb_printf(o, "  Redump: non trovato in %d DAT di %s.\n", dats, dir);
        if (errors_of(&d[0]))
            sb_printf(o, "  La lettura ha %d errori: una seconda lettura dira' se sono del disco.\n", errors_of(&d[0]));
        sb_printf(o, "  Una lettura sola non e' verificata: serve una seconda lettura da confrontare.\n");
        free(d);
        return finish(r, 1, "letto una volta", "single");
    }

    /* two or more reads: one that redump knows is good even if the others
     * are not, and it becomes the master */
    {
        redump_hit hit; char dir[600]; int dats;
        for (i = n - 1; i >= 0; i--) {
            for (k = i + 1; k < n && !same_tracks(&d[i], &d[k]); k++) ;
            if (k < n) continue;                /* already looked up with the same tracks */
            if (redump_match(&d[i], &hit, dir, sizeof(dir), &dats) != 1) continue;
            sb_printf(o, "  Redump: la lettura %s coincide con \"%s\"\n  (DAT %s, del %s).\n\n", d[i].base,
                      hit.game, hit.dat, hit.date[0] ? hit.date : "data non indicata");
            sb_printf(o, "  Il %s e' identico a quello verificato da altri archivisti: tengo %s.\n", what, d[i].base);
            r->master = i + 1;
            for (k = 0; k < n; k++) r->agree += same_tracks(&d[i], &d[k]);
            free(d);
            return finish(r, 0, "verificato redump", "redump");
        }
    }

    /* how many match each one */
    for (i = 0; i < n; i++) {
        int same = 0;
        for (k = 0; k < n; k++) if (same_tracks(&d[i], &d[k])) same++;
        if (same > best) best = same;
    }
    agree = best;
    r->agree = agree;
    for (i = 0; i < d[0].n; i++)
        for (k = 1; k < n; k++)
            if (i >= d[k].n || d[k].t[i].size != d[0].t[i].size || !same_ci(d[k].t[i].sha1, d[0].t[i].sha1)) {
                r->unstable_tracks++;
                break;
            }
    if (agree == n) {
        if (errors_of(&d[0])) {
            sb_printf(o, "  Le %d letture coincidono, ma con %d errori non risolti: il disco e' rovinato\n"
                         "  (o ha una protezione che redump non conosce). Prova un altro lettore e\n"
                         "  catalogalo con errori.\n", n, errors_of(&d[0]));
            rc = finish(r, 2, "errori", "identical");
        } else {
            sb_printf(o, "  Le %d letture coincidono traccia per traccia: catalogalo come pulito.\n", n);
            rc = finish(r, 0, "pulito", "identical");
        }
    } else {
        sb_printf(o, "  Le letture non coincidono (%d tracce diverse, %d letture su %d uguali):\n"
                     "  il disco non si legge in modo stabile. Puliscilo e rileggilo.\n",
                  r->unstable_tracks, agree, n);
        rc = finish(r, 1, "instabile", "data_differ");
    }
    free(d);
    return rc;
}

static int compare_cd(const char *const *imgs, int n, int forced, compare_result *r)
{
    (void)forced;
    return compare_media("CD", imgs, n, r);
}

static int compare_dvd(const char *const *imgs, int n, int forced, compare_result *r)
{
    (void)forced;
    return compare_media("DVD", imgs, n, r);
}

/* ---- tech: the technical data for metadata.json ---- */

/* the first data track (index, -1 if it is an audio CD) with the sector
 * size and the offset of the user data inside the sector */
static int data_track(const dump *d, int *ssize, int *off)
{
    int k;
    for (k = 0; k < d->n; k++) {
        if (ends_with_ci(d->t[k].name, ".iso")) { *ssize = 2048; *off = 0; return k; }
        if (!strncmp(d->t[k].mode, "MODE1", 5)) { *ssize = 2352; *off = 16; return k; }
        if (!strncmp(d->t[k].mode, "MODE2", 5)) { *ssize = 2352; *off = 24; return k; }
    }
    return -1;
}

static void tech_iso(const char *dir, const dump *d, json_writer *w)
{
    char p[1100]; iso_info fi; int i, ssize = 2048, off = 0, found;
    found = data_track(d, &ssize, &off);
    if (found < 0) {
        jw_null(w, "filesystem");
        jw_str(w, "filesystem_note", "nessuna traccia dati: e' un CD audio");
        return;
    }
    u8_join(p, sizeof(p), dir, d->t[found].name);
    if (!iso_read(p, ssize, off, &fi)) {
        jw_null(w, "filesystem");
        jw_str(w, "filesystem_note", fi.note);
        return;
    }
    jw_begin_obj(w, "filesystem");
    jw_str(w, "type", "ISO 9660");
    /* where names and label come from: the Joliet long names or the 8.3 ones */
    jw_str(w, "names", fi.joliet ? "Joliet" : "ISO 9660");
    jw_str(w, "volume_label", fi.volume_label[0] ? fi.volume_label : NULL);
    jw_str(w, "created", fi.created[0] ? fi.created : NULL);
    jw_str(w, "modified", fi.modified[0] ? fi.modified : NULL);
    jw_str(w, "publisher", fi.publisher[0] ? fi.publisher : NULL);
    jw_str(w, "application", fi.application[0] ? fi.application : NULL);
    jw_begin_arr(w, "files");
    for (i = 0; i < fi.nfiles; i++) {
        jw_begin_obj(w, NULL);
        jw_str(w, "name", fi.files[i].path);
        if (fi.files[i].isdir) jw_str(w, "type", "dir");
        else jw_int(w, "bytes", fi.files[i].size);
        jw_str(w, "date", fi.files[i].date[0] ? fi.files[i].date : NULL);
        jw_end_obj(w);
    }
    jw_end_arr(w);
    jw_end_obj(w);
    if (fi.note[0]) jw_str(w, "filesystem_note", fi.note);
    else if (!fi.joliet) jw_str(w, "filesystem_note", "solo nomi ISO 9660: il disco non ha Joliet");
    iso_free(&fi);
}

static void count_or_null(json_writer *w, const char *key, int v)
{
    if (v >= 0) jw_int(w, key, v); else jw_null(w, key);
}

static void tech(void *ctx, const char *img, json_writer *w)
{
    dump *d = (dump *)calloc(1, sizeof(dump));
    strbuf why; redump_hit hit; int i;
    long long sectors = 0;
    (void)ctx;
    sb_init(&why);
    if (!d || load_dump(img, d, &why)) {
        jw_null(w, "disc");
        jw_null(w, "filesystem");
        jw_str(w, "filesystem_note", why.s ? why.s : "lettura non leggibile");
        sb_free(&why);
        free(d);
        return;
    }
    sb_free(&why);
    jw_begin_obj(w, "disc");
    jw_str(w, "tool", d->tool[0] ? d->tool : "DiscImageCreator");
    jw_str(w, "command", d->command[0] ? d->command : NULL);
    jw_str(w, "drive", d->drive[0] ? d->drive : NULL);
    jw_begin_arr(w, "tracks");
    for (i = 0; i < d->n; i++) {
        int ss = ends_with_ci(d->t[i].name, ".iso") ? 2048 : 2352;
        jw_begin_obj(w, NULL);
        jw_int(w, "number", i + 1);
        jw_str(w, "mode", d->t[i].mode[0] ? d->t[i].mode : NULL);
        jw_str(w, "file", d->t[i].name);
        jw_int(w, "bytes", d->t[i].size);
        jw_int(w, "sectors", d->t[i].size / ss);
        jw_str(w, "crc32", d->t[i].crc);
        jw_str(w, "md5", d->t[i].md5);
        jw_str(w, "sha1", d->t[i].sha1);
        jw_end_obj(w);
        sectors += d->t[i].size / ss;
    }
    jw_end_arr(w);
    jw_int(w, "sectors", sectors);
    count_or_null(w, "c2_errors", d->c2);
    count_or_null(w, "edc_errors", d->edc);
    count_or_null(w, "read_errors", d->readerr);
    jw_int(w, "c2_check", !d->no_c2);           /* 0: the drive does not report C2 errors */
    if (d->dic_exit) jw_int(w, "dic_exit_code", d->dic_exit);
    jw_str(w, "protection", d->protection[0] ? d->protection : NULL);
    if (redump_cached(img, d, &hit) == 1) {
        jw_begin_obj(w, "redump");
        jw_str(w, "game", hit.game);
        jw_str(w, "dat", hit.dat);
        jw_str(w, "dat_date", hit.date[0] ? hit.date : NULL);
        jw_end_obj(w);
    } else {
        jw_null(w, "redump");
    }
    jw_end_obj(w);
    tech_iso(img, d, w);
    free(d);
}

/* ---- the read's phases from DIC's lines ---- */

static const char *phase_of(const char *line)
{
    if (strstr(line, "C2 error") || strstr(line, "c2 error") || strstr(line, "(c2)") ||
        strstr(line, "Reread") || strstr(line, "reread sector") || strstr(line, "EccEdc") ||
        strstr(line, "Checking sectors"))
        return "Controllo C2";
    return NULL;
}

/* the file system label of the first data track, like tech_iso */
static int volume_label(const char *img, char *out, size_t n)
{
    dump *d = (dump *)calloc(1, sizeof(dump));
    strbuf why; iso_info fi; char p[1100]; int ssize = 2048, off = 0, found, ok = 0;
    sb_init(&why);
    if (d && !load_dump(img, d, &why)) {
        found = data_track(d, &ssize, &off);
        if (found >= 0) {
            u8_join(p, sizeof(p), img, d->t[found].name);
            if (iso_read(p, ssize, off, &fi)) {
                if (fi.volume_label[0]) { str_copy(out, n, fi.volume_label); ok = 1; }
                iso_free(&fi);
            }
        }
    }
    sb_free(&why);
    free(d);
    return ok;
}

const media_ops cd_media = {
    "cd", "", check, scan, read_cd, compare_cd, tech, NULL, NULL,
    "DiscImageCreator", "TOC", 1, phase_of, volume_label
};

const media_ops dvd_media = {
    "dvd", "", check, scan, read_dvd, compare_dvd, tech, NULL, NULL,
    "DiscImageCreator", "TOC", 1, phase_of, volume_label
};
