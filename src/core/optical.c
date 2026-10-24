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
 *   cd  <drive> <file.bin> <speed> /c2 20 [/sf] /q
 *       /c2 20: rereads sectors with C2 errors up to 20 times;
 *       /sf: looks for protections in the files (SafeDisc, LaserLock...)
 *       and does not insist on their intentional errors; /q: no beeps,
 *       the window does them
 *   dvd <drive> <file.iso> <speed> /q
 *   ls  <drive>: the drive responds (0) or not (1, "Access is denied")
 * DIC launched without arguments or after an error stops on "Press any
 * key": runner gives it NUL as input and it goes on by itself.
 *
 * /sf only when needed: to look at the files DIC reads 64 sectors at a
 * time, and on the IDE controller of the XP PC that failed ("GetLastError:
 * 87" in ReadDirectoryRecordDetail) on a disc without protections. The box
 * chooses (auto, on, off); with auto the files on the mounted disc decide:
 * a signature file of a known protection (protection-signatures.txt next
 * to the program) turns /sf on.
 *
 * A drive missing from DIC's driveOffset.txt makes it ask for the read
 * offset on its input ("Please input drive offset(Samples): ", read with
 * scanf): with [Optical] DriveOffset set the answer comes from a file
 * instead of NUL, otherwise the question fails and the read with it.
 *
 * Drives that cannot read the lead-in or the lead-out (seen on the PC:
 * ASUS DRW-08D6MT, LBA -1 refused) make cd fail. A data CD of a single
 * track is then read with
 *   data <drive> <file.bin> <speed> 0 <lead-out LBA> /c2 20 [/sf] /q
 * which writes only the .bin, its .c2/.sub and the logs: RetroArchiver
 * writes the .cue and the .dat. The read is checked by EDC/ECC (every
 * sector carries its own); it is not a redump-grade dump. The drive's model
 * is remembered ([Optical] NoLeadInDrives in the window's ini): its next
 * data CDs go straight to data. Audio and mixed CDs need cd. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"
#include "meta.h"
#include "fsu8.h"
#include "iso9660.h"
#include "romhash.h"
#include "sha256.h"

#define CD_SPEED  "16"          /* slow enough for a reliable C2 check; a damaged CD wants less */
#define DVD_SPEED "8"
#define C2_REREAD "20"
#define CONSOLE   "dic-console.txt"
#define MAXTRK    100

/* the line RetroArchiver adds to dic-console.txt when DIC exits with an
 * error on a complete read; reads made by 261017 have the Italian one */
#define EXIT_MARK        "RetroArchiver: DiscImageCreator exited with code "
#define EXIT_MARK_261017 "RetroArchiver: DiscImageCreator e' uscito con il codice "

/* DIC starts but Windows cannot find a DLL: on XP the Visual C++ runtimes */
#define DLL_NOT_FOUND ((int)0xC0000135)
#define ENTRY_NOT_FOUND ((int)0xC0000139)
#define INIT_FAILED ((int)0xC0000142)

/* the lines RetroArchiver adds to dic-console.txt before DIC's: how the read
 * was made, for the technical data of metadata.json */
#define PROT_MARK   "RetroArchiver: copy protection scan: "
#define FILES_MARK  "RetroArchiver: copy protection files: "
#define OFFSET_MARK "RetroArchiver: drive offset given: "
/* "data 0-208949 (why)": read with the data command, not with cd */
#define METHOD_MARK "RetroArchiver: read method: "
#define SPEED_MARK  "RetroArchiver: read speed: "
/* DIC asks for the offset of a drive it does not know */
#define ASK_OFFSET  "This drive doesn't define in driveOffset.txt"
#define INPUT_FILE  "dic-input.txt"
#define RESET_STOP  3            /* drive resets during a read with /sf before stopping it */
#define SIG_FILE    "protection-signatures.txt"

static char dic[512] = "DiscImageCreator";
static char drive[2] = "D";
static char datdir[512] = "";
static char offset[16] = "";            /* "" = not set */
static char cd_speed[8] = CD_SPEED;
/* a CD that DIC stopped on with C2 errors left is read again slower: half
 * the speed each time, down to SLOW_MIN. Only that disc: the next ones
 * start again from cd_speed */
#define SLOW_MIN 4
static char slow_disc[96] = "";         /* "CD01", "" none */
static int slow_x = 0, slowed_to = 0;
static char cur_speed[8] = CD_SPEED;    /* of the read in progress */
static char sigpath[512] = "", disc_root[512] = "";
static int prot_mode = META_PROT_AUTO;  /* for the next read */
static int prot_used = 0;               /* of the last read */
static char noleadin[2048] = "";        /* the drives that cannot read the lead-in, ";" between them */
static void (*learned_fn)(void *, const char *);
static void *learned_ctx;

void optical_set_dic(const char *path) { str_copy(dic, sizeof(dic), path); }
void optical_set_noleadin_drives(const char *list) { str_copy(noleadin, sizeof(noleadin), list ? list : ""); }
void optical_on_drive_learned(void (*fn)(void *ctx, const char *model), void *ctx)
{
    learned_fn = fn;
    learned_ctx = ctx;
}
void optical_set_datdir(const char *dir) { str_copy(datdir, sizeof(datdir), dir); }
void optical_set_signatures(const char *path) { str_copy(sigpath, sizeof(sigpath), path); }
void optical_set_disc_root(const char *dir) { str_copy(disc_root, sizeof(disc_root), dir); }
static void set_protection(int mode) { prot_mode = mode; }
static int protection_used(void) { return prot_used; }

int optical_parse_offset(const char *s, char *out, size_t n)
{
    const char *p = s; long v;
    if (n) out[0] = 0;
    while (*p == ' ') p++;
    if (!*p) return 0;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) p++;
    while (*p == ' ') p++;
    if (*p) return -1;
    v = atol(s);
    /* DIC reads at most 6 characters */
    if (v < -99999 || v > 99999) return -1;
    if (v) snprintf(out, n, "%+ld", v);
    else str_copy(out, n, "0");
    return 0;
}

int optical_set_cd_speed(int x)
{
    if (x < OPTICAL_SPEED_MIN || x > OPTICAL_SPEED_MAX) return -1;
    snprintf(cd_speed, sizeof(cd_speed), "%d", x);
    slow_disc[0] = 0;
    return 0;
}

/* the disc of a read: "CD01" for CD01 and CD01_b */
static void disc_key(const char *base, char *out, size_t n)
{
    size_t k;
    str_copy(out, n, base);
    k = strlen(out);
    if (k > 2 && out[k - 2] == '_' && islower((unsigned char)out[k - 1])) out[k - 2] = 0;
}

static const char *speed_for(const char *base)
{
    char key[96];
    disc_key(base, key, sizeof(key));
    if (slow_disc[0] && !strcmp(key, slow_disc)) snprintf(cur_speed, sizeof(cur_speed), "%d", slow_x);
    else str_copy(cur_speed, sizeof(cur_speed), cd_speed);
    return cur_speed;
}

/* DIC stopped with C2 errors left: the next read of this disc goes slower */
static void slow_down(const char *base)
{
    int cur = atoi(cur_speed);
    slowed_to = 0;
    if (cur <= SLOW_MIN) return;
    slow_x = cur / 2 < SLOW_MIN ? SLOW_MIN : cur / 2;
    disc_key(base, slow_disc, sizeof(slow_disc));
    slowed_to = slow_x;
}

int optical_set_drive_offset(const char *s)
{
    char v[16];
    if (optical_parse_offset(s, v, sizeof(v))) return -1;
    str_copy(offset, sizeof(offset), v);
    return 0;
}

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

static int same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
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
    const char *p = line + strlen(line), *slash, *from;
    long a, b, n;
    /* the data command counts the LBA: "Creating .bin from 0 to 208949 (LBA)   1234" */
    if ((from = strstr(line, " from ")) != NULL && strstr(from, "(LBA)") &&
        sscanf(from, " from %ld to %ld (LBA) %ld", &a, &b, &n) == 3 && b > a) {
        if (done) *done = n - a;
        if (total) *total = b - a;
        return 1;
    }
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
    int firmware;       /* "This drive isn't latest firmware" */
    int c2_left;        /* the last "Remains N c2 error" of the rereads */
    int ask_offset;     /* DIC asked for the drive's read offset */
    int sf;             /* the read looks for copy protections */
    int resets;         /* "UNIT_ATTENTION" (the drive was reset) and "GetLastError: 121" (timeout) */
    int unit_att;       /* the resets alone: they stop a read with /sf */
    int stopped;        /* stopped by RetroArchiver after RESET_STOP of them, with /sf */
    char last[300];     /* the last meaningful line: it goes into the error message */
    int leadin;         /* the drive cannot read the lead-in (1, LBA -1) or the lead-out (2) */
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
    tee *t = (tee *)c; const char *r;
    if (strstr(line, "Wrong command")) t->wrong = 1;
    if (strstr(line, "GetLastError: 5,")) t->denied = 1;
    /* seen on the PC with the PLEXTOR PX-760A 1.06: DIC wants the last
     * firmware of a drive it knows (1.07) and stops before reading */
    if (strstr(line, "isn't latest firmware")) t->firmware = 1;
    /* seen on the PC with a scratched CD (PLEXTOR PX-760A): "Remains 929 c2
     * error. Rereading times: 20/ 20", "Done. See _c2Error.txt", and DIC
     * ends there: no .img, no .cue, no EDC/ECC check */
    if ((r = strstr(line, "Remains ")) != NULL && strstr(r, "c2 error")) t->c2_left = atoi(r + 8);
    if (strstr(line, ASK_OFFSET)) t->ask_offset = 1;
    /* seen on the PC (ASUS DRW-08D6MT): "This drive can't read the lead-in",
     * then a raw read of LBA -1 refused with ILLEGAL_REQUEST, and DIC stops
     * with error 1. The cd command needs it; /sf has nothing to do with it.
     * Which of the two DIC checks depends on the sign of the disc's
     * combined offset: the same drive may say lead-out on another disc.
     * The Sony AD-7200S (offset +48) stops at LBA -2 with "LOGICAL BLOCK
     * ADDRESS OUT OF RANGE" and no such sentence: any error on a negative
     * LBA during cd is the lead-in */
    if (strstr(line, "can't read the lead-in") || strstr(line, "LBA[-0")) t->leadin = 1;
    else if (strstr(line, "can't read the lead-out") && !t->leadin) t->leadin = 2;
    /* seen on the PC with /sf: "GetLastError: 121" (semaphore timeout), then
     * "SenseData 06-29-00 = UNIT_ATTENTION - POWER ON, RESET..." and "Please
     * wait for 40000 milliseconds": the scan resets the drive over and over,
     * for an hour. After RESET_STOP of them it is stopped */
    if (strstr(line, "UNIT_ATTENTION") || strstr(line, "GetLastError: 121,")) {
        t->resets++;
        if (strstr(line, "UNIT_ATTENTION")) t->unit_att++;
        if (t->sf && t->unit_att >= RESET_STOP && !t->stopped) { t->stopped = 1; runner_cancel(); }
    }
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
        snprintf(line, sizeof(line), "%s does not start: the Visual C++ 2015-2019 x86 runtimes are missing "
                                     "(VCRUNTIME140.dll). On Windows XP install version 14.27 "
                                     "of the Visual C++ Redistributable.", dic);
    else
        snprintf(line, sizeof(line), "Cannot find %s: put it in the same folder as the program "
                                     "or in the PATH, or give the full path of "
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
        sb_printf(msg, "Cannot find %s: put it in the same folder as the program or in the PATH.", dic);
        return -1;
    }
    if (rc != RUNNER_DONE) {
        sb_printf(msg, "Check of drive %s: canceled, try again when you like.", drive);
        return -1;
    }
    if (dll_missing(code)) {
        sb_printf(msg, "%s does not start: the Visual C++ 2015-2019 x86 runtimes are missing.", dic);
        return -1;
    }
    if (code != 0) {
        if (t.denied)
            sb_printf(msg, "Cannot open drive %s: (access denied): check the drive letter "
                           "and run the program as an administrator.", drive);
        else
            sb_printf(msg, "Drive %s: does not respond (DiscImageCreator ls gave error %d): "
                           "check the drive letter.", drive, code);
        return -1;
    }
    return 0;
}

/* ---- scan: the drive and the disc's TOC ---- */

/* the disc's sectors according to the last TOC read (-1 = unknown): the
 * estimate of the space the read needs */
static long toc_sectors = -1;
/* the TOC of the disc in the drive, from the scan; n 0 if unknown */
static optical_toc g_toc;

/* DIC's _disc.txt: "\t  Data Track  1, LBA        0 -   208948, Length   208949"
 * up to "Total", once in the TOC and again in the FULL TOC (the last one
 * wins: it has the last session's lead-out), "Pregap Track   , LBA" when
 * track 1 does not start at 0, "LastCompleteSession: 2" */
int optical_toc_file(const char *path, optical_toc *t)
{
    FILE *f = u8_fopen(path, "rb"); char line[400], kind[16]; optical_toc cur;
    int num; long a, b, len; const char *q;
    memset(t, 0, sizeof(*t));
    memset(&cur, 0, sizeof(cur));
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "==========")) { cur.n = 0; cur.pregap = 0; continue; }
        if ((q = strstr(line, "LastCompleteSession:")) != NULL) t->sessions = atoi(q + 20);
        if (strstr(line, "Pregap Track")) cur.pregap = 1;
        else if (sscanf(line, " %15s Track %d, LBA %ld - %ld, Length %ld", kind, &num, &a, &b, &len) == 5) {
            if (cur.n < MAXTRK) {
                cur.data[cur.n] = !strcmp(kind, "Data");
                cur.start[cur.n++] = a;
                cur.end = b + 1;
            }
        } else if (strstr(line, "Total") && cur.n) {
            int s = t->sessions;
            *t = cur;
            t->sessions = s;
        }
    }
    fclose(f);
    return t->n ? 0 : -1;
}

/* 1 if the data command can read the whole disc: a single data track from
 * LBA 0, in one session. 0 with the reason, for the messages */
static int data_only(const optical_toc *t, char *why, size_t n)
{
    int i, audio = 0;
    for (i = 0; i < t->n; i++) audio += !t->data[i];
    if (!t->n)
        str_copy(why, n, "the disc's TOC is not known, so it cannot be told whether it is a data CD");
    else if (audio)
        snprintf(why, n, "this CD has %d audio track%s of %d, which only the cd command reads as they are",
                 audio, audio == 1 ? "" : "s", t->n);
    else if (t->n > 1 || t->sessions > 1)
        snprintf(why, n, "this data CD has %d tracks in %d session%s, and the data command reads only a CD "
                         "of a single track", t->n, t->sessions > 1 ? t->sessions : 1, t->sessions > 1 ? "s" : "");
    else if (t->pregap || t->start[0] != 0 || t->end <= 0)
        str_copy(why, n, "its data track does not start at the beginning of the disc");
    else
        return 1;
    return 0;
}

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
        snprintf(line, sizeof(line), "TOC: cannot read it from drive %c: (no disc, or the drive does not "
                                     "respond; error %lu)", toupper((unsigned char)drive[0]),
                 (unsigned long)GetLastError());
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        fprintf(f, "%s\n", line);
        say(on_line, ctx, line);
        return;
    }
    CloseHandle(h);
    n = toc.last - toc.first + 1;
    if (n < 1 || n > 99) n = 0;
    snprintf(line, sizeof(line), "TOC of the disc in drive %c: tracks %d-%d",
             toupper((unsigned char)drive[0]), toc.first, toc.last);
    fprintf(f, "%s\n", line);
    say(on_line, ctx, line);
    for (i = 0; i < n; i++) {
        long a = msf_lba(toc.t[i].addr), b = msf_lba(toc.t[i + 1].addr);
        g_toc.data[i] = (toc.t[i].control_adr & 4) != 0;
        g_toc.start[i] = a;
        snprintf(line, sizeof(line), "  Track %2d  %-5s  LBA %7ld  %7ld sectors (%02ld:%02ld)",
                 toc.t[i].track, (toc.t[i].control_adr & 4) ? "data" : "audio", a, b - a,
                 (b - a) / 75 / 60, (b - a) / 75 % 60);
        fprintf(f, "%s\n", line);
        say(on_line, ctx, line);
    }
    if (n) {
        long end = msf_lba(toc.t[n].addr);
        toc_sectors = end;
        /* the plain TOC does not tell the sessions: with more than one there
         * is more than one track, and the data command is not used anyway */
        g_toc.n = n;
        g_toc.end = end;
        snprintf(line, sizeof(line), "  Total %ld sectors (%ld MB of user data)%s", end,
                 end * 2048L / 1048576L, end > 405000 ? ": longer than a CD, maybe it is a DVD" : "");
        fprintf(f, "%s\n", line);
        say(on_line, ctx, line);
    }
}
#else
/* tests only: RETROARCHIVER_TOC is a _disc.txt of DIC with the disc's TOC */
static void write_toc(FILE *f, runner_line_fn on_line, void *ctx)
{
    const char *line = "TOC: reading it directly from the drive works only on Windows";
    const char *env = getenv("RETROARCHIVER_TOC");
    char text[200];
    if (env && *env && !optical_toc_file(env, &g_toc)) {
        snprintf(text, sizeof(text), "TOC of the disc: %d track%s, lead-out at LBA %ld", g_toc.n,
                 g_toc.n == 1 ? "" : "s", g_toc.end);
        line = text;
        toc_sectors = g_toc.end;
    }
    fprintf(f, "%s\n", line);
    say(on_line, ctx, line);
}
#endif

static int scan(const char *out_txt, runner_line_fn on_line, void *ctx)
{
    const char *argv[] = { dic, "ls", drive, NULL };
    tee t; int code = 0, rc;
    toc_sectors = -1;
    memset(&g_toc, 0, sizeof(g_toc));
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
                 ? "WARNING: cannot open drive %s: (access denied): run the program as an "
                   "administrator, or check the drive letter."
                 : "WARNING: drive %s: does not respond to DiscImageCreator.", drive);
        fprintf(t.f, "%s\n", line);
        say(on_line, ctx, line);
    }
    write_toc(t.f, on_line, ctx);
    fclose(t.f);
    return 0;
}

/* ---- copy protections: the signature files on the disc ---- */

/* The names come from BinaryObjectScanner (SabreTools, the path checks in
 * Protection/Macrovision.SafeDisc.cs, SecuROM.cs, LaserLok.cs,
 * CDDVDCops.cs). Used when protection-signatures.txt is missing; the file
 * has the same format: the name, "*" at the start for an ending, "/" at
 * the end for a folder, then the protection */
static const char *const builtin_sigs[] = {
    "00000001.TMP SafeDisc", "00000002.TMP SafeDisc", "CLCD16.DLL SafeDisc", "CLCD32.DLL SafeDisc",
    "CLOKSPL.EXE SafeDisc", "DPLAYERX.DLL SafeDisc", "DRVMGT.DLL SafeDisc", "SECDRV.SYS SafeDisc",
    "*.ICD SafeDisc",
    "CMS16.DLL SecuROM", "CMS_95.DLL SecuROM", "CMS_NT.DLL SecuROM", "CMS32_95.DLL SecuROM",
    "CMS32_NT.DLL SecuROM",
    "LASERLOK.IN LaserLock", "LASERLOK/ LaserLock",
    "CDCOPS.DLL CD-Cops", "*.GZ_ CD-Cops", "*.QZ_ CD-Cops", "*.W_X CD-Cops",
    NULL
};

#define MAXSIG    128
#define WALK_DEPTH 8            /* the disc's folders: deeper ones are not looked at */
#define WALK_FILES 20000        /* nor more files than these (a CD of clip art) */
#define MAXHIT    24            /* the names written in the log */

typedef struct { char pat[64], prot[48]; } sig;

typedef struct {
    sig s[MAXSIG]; int ns;
    char hit[MAXHIT][64]; int hprot[MAXHIT], nhit, count;   /* names kept, files found */
    int files, depth;
    char dir[1024];
} sig_walk;

/* "NAME   Protection" (or "# comment"); 0 if the line has none */
static int parse_sig(const char *line, sig *s)
{
    const char *p = line, *e;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#' || *p == ';') return 0;
    for (e = p; *e && *e != ' ' && *e != '\t'; e++) ;
    if (e == p || (size_t)(e - p) >= sizeof(s->pat)) return 0;
    snprintf(s->pat, sizeof(s->pat), "%.*s", (int)(e - p), p);
    while (*e == ' ' || *e == '\t') e++;
    str_copy(s->prot, sizeof(s->prot), *e ? e : "unknown protection");
    trim_eol(s->prot);
    return 1;
}

static void load_sigs(sig_walk *w)
{
    char path[700], dir[600], line[200]; FILE *f; int i;
    if (sigpath[0]) str_copy(path, sizeof(path), sigpath);
    else { exe_dir(dir, sizeof(dir)); u8_join(path, sizeof(path), dir, SIG_FILE); }
    w->ns = 0;
    if ((f = u8_fopen(path, "rb")) != NULL) {
        while (fgets(line, sizeof(line), f) && w->ns < MAXSIG) {
            trim_eol(line);
            if (parse_sig(line, &w->s[w->ns])) w->ns++;
        }
        fclose(f);
    }
    /* no file, or nothing in it: the built-in list */
    if (!w->ns)
        for (i = 0; builtin_sigs[i] && w->ns < MAXSIG; i++)
            if (parse_sig(builtin_sigs[i], &w->s[w->ns])) w->ns++;
}

static int sig_match(const sig *s, const char *name, int isdir)
{
    size_t k = strlen(s->pat);
    if (s->pat[k - 1] == '/') {
        char d[64];
        snprintf(d, sizeof(d), "%.*s", (int)(k - 1), s->pat);
        return isdir && same_ci(name, d);
    }
    if (isdir) return 0;
    if (s->pat[0] == '*') return ends_with_ci(name, s->pat + 1);
    return same_ci(name, s->pat);
}

static void walk_one(void *c, const char *name, int isdir)
{
    sig_walk *w = (sig_walk *)c; int i; size_t len; char sub[1100];
    if (w->files >= WALK_FILES) return;
    w->files++;
    for (i = 0; i < w->ns; i++)
        if (sig_match(&w->s[i], name, isdir)) {
            w->count++;
            if (w->nhit < MAXHIT) {
                str_copy(w->hit[w->nhit], sizeof(w->hit[0]), name);
                w->hprot[w->nhit++] = i;
            }
            break;
        }
    if (!isdir || w->depth >= WALK_DEPTH) return;
    len = strlen(w->dir);
    u8_join(sub, sizeof(sub), w->dir, name);
    if (strlen(sub) + 1 >= sizeof(w->dir)) return;
    str_copy(w->dir, sizeof(w->dir), sub);
    w->depth++;
    u8_listdir(w->dir, walk_one, w);
    w->depth--;
    w->dir[len] = 0;
}

int optical_protection_files(const char *root, char *out, size_t n)
{
    sig_walk *w = (sig_walk *)calloc(1, sizeof(sig_walk));
    strbuf b; int i, k, rc;
#ifdef _WIN32
    UINT old = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);  /* no "insert a disk" box */
#endif
    if (n) out[0] = 0;
    if (!w) return -1;
    load_sigs(w);
    str_copy(w->dir, sizeof(w->dir), root);
    rc = u8_listdir(root, walk_one, w);
#ifdef _WIN32
    SetErrorMode(old);
#endif
    if (rc) { free(w); return -1; }
    /* "A, B (SafeDisc), C (SecuROM)": the names grouped by protection */
    sb_init(&b);
    for (i = 0; i < w->nhit; i++) {
        const char *p = w->s[w->hprot[i]].prot;
        for (k = 0; k < i && strcmp(w->s[w->hprot[k]].prot, p); k++) ;
        if (k < i) continue;                /* already written with its group */
        for (k = i; k < w->nhit; k++) {
            if (strcmp(w->s[w->hprot[k]].prot, p)) continue;
            sb_printf(&b, "%s%s", b.len ? ", " : "", w->hit[k]);
        }
        sb_printf(&b, " (%s)", p);
    }
    if (b.s) str_copy(out, n, b.s);
    sb_free(&b);
    rc = w->count;
    free(w);
    return rc;
}

/* auto: the files on the mounted disc decide. Says what it found, on screen
 * and in the read's log; 1 if the read uses /sf */
static int choose_sf(FILE *con, runner_line_fn on_line, void *ctx)
{
    char root[600], found[600], line[900]; int k;
    const char *env = getenv("RETROARCHIVER_DISC_ROOT");    /* tests only */
    if (prot_mode != META_PROT_AUTO) {
        int on = prot_mode == META_PROT_ON;
        if (con) fprintf(con, PROT_MARK "%s, %s\n", on ? "on" : "off", meta_prot_key(prot_mode));
        say(on_line, ctx, on ? "Copy protection scan on, as chosen: DiscImageCreator looks for protections (/sf)."
                             : "Copy protection scan off, as chosen.");
        return on;
    }
    if (disc_root[0]) str_copy(root, sizeof(root), disc_root);
    else if (env && *env) str_copy(root, sizeof(root), env);
    else snprintf(root, sizeof(root), "%c:\\", drive[0]);
    k = optical_protection_files(root, found, sizeof(found));
    if (k > 0) snprintf(line, sizeof(line), "Found %s: enabling the protection scan (DiscImageCreator /sf).", found);
    else if (k == 0) str_copy(line, sizeof(line), "No copy protection files on the disc: reading without the "
                                                  "protection scan.");
    else snprintf(line, sizeof(line), "Cannot list the files on %s (is the disc mounted?): reading without "
                                      "the protection scan.", root);
    if (con) {
        fprintf(con, PROT_MARK "%s, auto\n", k > 0 ? "on" : "off");
        if (k >= 0) fprintf(con, FILES_MARK "%s\n", k > 0 ? found : "none");
    }
    say(on_line, ctx, line);
    return k > 0;
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
        sb_printf(why, "the folder is on a %s drive, which cannot hold files over 4 GB, and the image of "
                       "this disc will be %lld MB: choose a folder on an NTFS drive", fsname, biggest / MB);
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
    sb_printf(why, "the name of folder %s has characters that DiscImageCreator (an ANSI program) cannot "
                   "read, and it has no 8.3 short name: choose an archive folder whose name has "
                   "no foreign letters", dir);
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
        sb_printf(why, "%s has %lld MB free and the read needs about %lld: free some space or "
                       "choose another archive folder", dir, avail / MB, need / MB);
        return -1;
    }
    return 0;
}

/* the message when DIC stops: it is not always the disc's fault */
static void stop_message(char *p, size_t n, int code, const tee *t, const char *dir)
{
    char last[320]; long long avail = optical_free_bytes(dir);
    if (t->last[0]) snprintf(last, sizeof(last), " Last line: \"%.280s\".", t->last);
    else last[0] = 0;
    snprintf(p, n, "DiscImageCreator stopped with error %d.%s Check that the disc is in "
                   "drive %s: and clean, and the space in folder %s (%lld MB free), then try again.",
             code, last, drive, dir, avail >= 0 ? avail / MB : -1LL);
}

/* the drive's model from DIC's BASE_drive.txt, "ASUS DRW-08D6MT", with_rev
 * with the firmware revision too ("ASUS DRW-08D6MT 1.00"); "" if not there */
static void drive_model(const char *dir, const char *base, int with_rev, char *out, size_t n)
{
    char p[1100], name[200], line[1000], vendor[40] = "", product[40] = "", rev[40] = ""; FILE *f;
    out[0] = 0;
    snprintf(name, sizeof(name), "%s_drive.txt", base);
    u8_join(p, sizeof(p), dir, name);
    if (!(f = u8_fopen(p, "rb"))) return;
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
    if (!with_rev) rev[0] = 0;
    snprintf(out, n, "%s%s%s%s%s", vendor, vendor[0] ? " " : "", product, rev[0] ? " " : "", rev);
}

/* ---- the drives that cannot read the lead-in ---- */

/* the words of a model with single spaces: "DVDR   PX-716A " -> "DVDR PX-716A" */
static void model_words(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 1 < n; in++) {
        if (*in == ' ' || *in == '\t') {
            if (k && out[k - 1] != ' ') out[k++] = ' ';
        } else if (*in != ';') {
            out[k++] = *in;
        }
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* the same model whatever the spaces between vendor, product and firmware:
 * DIC's _drive.txt and Windows split the inquiry data each their way */
static int same_model(const char *a, const char *b, size_t blen)
{
    const char *e = b + blen;
    for (;;) {
        while (*a == ' ' || *a == '\t') a++;
        while (b < e && (*b == ' ' || *b == '\t')) b++;
        if (!*a || b == e) return !*a && b == e;
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++; b++;
    }
}

int optical_drive_listed(const char *list, const char *model)
{
    const char *p = list, *e;
    if (!model || !*model || !list) return 0;
    while (*p) {
        for (e = p; *e && *e != ';'; e++) ;
        if (e > p && same_model(model, p, (size_t)(e - p))) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

int optical_list_add(char *list, size_t n, const char *model)
{
    char m[200]; size_t k = strlen(list);
    model_words(model, m, sizeof(m));
    if (!m[0] || optical_drive_listed(list, m)) return 0;
    if (k + strlen(m) + 3 > n) return 0;
    snprintf(list + k, n - k, "%s%s", k ? "; " : "", m);
    return 1;
}

int optical_list_item(const char *list, int i, char *out, size_t n)
{
    const char *p = list, *e;
    char raw[200];
    size_t k;
    while (list && *p) {
        for (e = p; *e && *e != ';'; e++) ;
        k = (size_t)(e - p) < sizeof(raw) ? (size_t)(e - p) : sizeof(raw) - 1;
        memcpy(raw, p, k);
        raw[k] = 0;
        model_words(raw, out, n);
        if (out[0] && i-- == 0) return 1;     /* an empty item, ";;" by hand, does not count */
        p = *e ? e + 1 : e;
    }
    if (n) out[0] = 0;
    return 0;
}

int optical_list_remove(char *list, size_t n, const char *model)
{
    char rest[2048] = "", m[200];
    int i, gone = 0;
    for (i = 0; optical_list_item(list, i, m, sizeof(m)); i++) {
        if (same_model(model, m, strlen(m))) gone = 1;
        else optical_list_add(rest, sizeof(rest), m);
    }
    if (gone) str_copy(list, n, rest);
    return gone;
}

#ifdef _WIN32
/* IOCTL_STORAGE_QUERY_PROPERTY: the inquiry data Windows keeps for the drive,
 * without opening it for reading (no disc needed) */
int optical_drive_model(char letter, char *out, size_t n)
{
    wchar_t dev[16]; HANDLE h; STORAGE_PROPERTY_QUERY q; DWORD got = 0;
    unsigned char buf[1024]; STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
    char raw[200]; size_t k = 0; DWORD offs[3]; int i;
    const char *env = getenv("RETROARCHIVER_DRIVE_MODEL");      /* tests only */
    if (n) out[0] = 0;
    if (env && *env) { model_words(env, out, n); return out[0] != 0; }
    _snwprintf(dev, 16, L"\\\\.\\%c:", (wchar_t)toupper((unsigned char)letter));
    dev[15] = 0;
    h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    memset(&q, 0, sizeof(q));
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    memset(buf, 0, sizeof(buf));
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf) - 1, &got, NULL) ||
        got < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    offs[0] = d->VendorIdOffset; offs[1] = d->ProductIdOffset; offs[2] = d->ProductRevisionOffset;
    for (i = 0; i < 3; i++) {
        const char *s;
        if (!offs[i] || offs[i] >= got) continue;
        for (s = (const char *)buf + offs[i]; *s && k + 2 < sizeof(raw); s++) raw[k++] = *s;
        raw[k++] = ' ';
    }
    raw[k] = 0;
    model_words(raw, out, n);
    return out[0] != 0;
}
#else
int optical_drive_model(char letter, char *out, size_t n)
{
    const char *env = getenv("RETROARCHIVER_DRIVE_MODEL");      /* tests only */
    (void)letter;
    if (n) out[0] = 0;
    if (env && *env) model_words(env, out, n);
    return n && out[0] != 0;
}
#endif

/* DIC asked for the offset and nobody answered */
static void offset_message(char *p, size_t n, const char *dir, const char *base)
{
    char model[120], what[160];
    drive_model(dir, base, 0, model, sizeof(model));
    if (model[0]) snprintf(what, sizeof(what), "model %s", model);
    else str_copy(what, sizeof(what), "this drive's model (see the _drive.txt log of the read)");
    snprintf(p, n, "The drive's read offset is not set, and DiscImageCreator does not know this drive: "
                   "look it up for %s in the AccurateRip or redump list and set [Optical] DriveOffset "
                   "in RetroArchiver.ini (dskrun: --drive-offset), then read it again.", what);
}

/* with the offset set, DIC's question gets it from a file in the read's
 * folder (removed after the read); without, input stays NUL */
static void cd_input(const char *out_dir, char *input, size_t n, FILE *con)
{
    FILE *f;
    input[0] = 0;
    if (con) fprintf(con, OFFSET_MARK "%s\n" SPEED_MARK "%sx\n", offset[0] ? offset : "not set", cur_speed);
    if (!offset[0]) return;
    u8_join(input, n, out_dir, INPUT_FILE);
    /* binary: DIC's C runtime reads the console's CRLF too */
    if (!(f = u8_fopen(input, "wb"))) { input[0] = 0; return; }
    fprintf(f, "%s\r\n", offset);
    fclose(f);
}

/* why DIC stopped, when it is not the lead-in: missing names what a
 * complete read would have */
static void fail_text(char *p, size_t n, int dvd, int code, const tee *t, const char *out_dir,
                      const char *base, const char *missing)
{
    if (t->stopped)
        snprintf(p, n, "The drive was reset %d times while DiscImageCreator looked for copy "
                       "protections (/sf): stopped it, it would go on like this for an hour. Read the "
                       "CD again without the protection scan (Copy protection: Off).", t->unit_att);
    else if (t->ask_offset && !offset[0])
        offset_message(p, n, out_dir, base);
    else if (t->wrong)
        snprintf(p, n, "The drive does not hold a %s: DiscImageCreator says the disc is of "
                       "another type. Check the disc, or the contents of the box.",
                 dvd ? "DVD" : "CD");
    else if (t->denied)
        snprintf(p, n, "DiscImageCreator cannot open drive %s: (access denied): run the program "
                       "as an administrator.", drive);
    else if (t->c2_left > 0 && !dvd && slowed_to)
        snprintf(p, n, "DiscImageCreator stopped with %d sectors still unreadable after %s rereads (C2 "
                       "errors) at %sx: the disc is scratched or dirty there. Retry reads it again at %dx; "
                       "washing it first helps (water and a drop of soap, dried from the centre outwards).",
                 t->c2_left, C2_REREAD, cur_speed, slowed_to);
    else if (t->c2_left > 0 && !dvd)
        snprintf(p, n, "DiscImageCreator stopped with %d sectors still unreadable after %s rereads (C2 "
                       "errors) at %sx, the lowest speed RetroArchiver uses: the disc is scratched or "
                       "dirty there. Wash it with water and a drop of soap, dry it from the centre "
                       "outwards and try again, or read it with another drive.",
                 t->c2_left, C2_REREAD, cur_speed);
    else if (t->firmware)
        snprintf(p, n, "DiscImageCreator does not read with this drive until its firmware is the "
                       "latest one: update the drive's firmware (DiscImageCreator's log says where "
                       "to find it), restart the PC and read the %s again.", dvd ? "DVD" : "CD");
    else if (code != 0)
        stop_message(p, n, code, t, out_dir);
    else
        snprintf(p, n, "DiscImageCreator finished but %.200s is missing: the read is incomplete.", missing);
    if (t->resets && !t->sf && strlen(p) + 200 < n)
        strcat(p, " The drive was reset during the read (UNIT_ATTENTION, timeouts): the copy protection "
                  "scan would not help, it makes the drive reset even more.");
}

/* ---- the data command: a data CD on a drive without lead-in ---- */

static const char *edge_name(int leadin)
{
    return leadin == 2 ? "lead-out" : "lead-in";
}

static void xml_escaped(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '&') fputs("&amp;", f);
        else if (*s == '<') fputs("&lt;", f);
        else if (*s == '>') fputs("&gt;", f);
        else if (*s == '"') fputs("&quot;", f);
        else fputc(*s, f);
    }
}

/* data writes only the .bin: the .cue and the .dat with its fingerprints
 * are written here, in DIC's formats, so that the verification and the
 * redump lookup treat the read like one of cd. The .dat comes last, as
 * with DIC: with it the read is complete */
static int write_cue_dat(const char *dir, const char *base, const char *bin, runner_line_fn on_line, void *ctx)
{
    char p[1100], name[200], line[700]; romhash h; unsigned char sec[16]; FILE *f; int mode = 1, bad;
    snprintf(line, sizeof(line), "Computing the fingerprints of %s.bin (CRC32, MD5, SHA-1) for the .dat.", base);
    say(on_line, ctx, line);
    if (romhash_file(bin, &h)) {
        snprintf(line, sizeof(line), "Cannot read back %s.bin, just written: check the destination drive.", base);
        say(on_line, ctx, line);
        return -1;
    }
    /* the mode is in the header of the first sector, after sync and address */
    if ((f = u8_fopen(bin, "rb")) != NULL) {
        if (fread(sec, 1, 16, f) == 16 && sec[15] == 2) mode = 2;
        fclose(f);
    }
    snprintf(name, sizeof(name), "%s.cue", base);
    u8_join(p, sizeof(p), dir, name);
    if ((f = u8_fopen(p, "wb")) != NULL) {
        fprintf(f, "FILE \"%s.bin\" BINARY\r\n  TRACK 01 MODE%d/2352\r\n    INDEX 01 00:00:00\r\n", base, mode);
        bad = ferror(f);
        if (fclose(f) || bad) f = NULL; else f = (FILE *)1;
    }
    if (f) {
        snprintf(name, sizeof(name), "%s.dat", base);
        u8_join(p, sizeof(p), dir, name);
        if ((f = u8_fopen(p, "wb")) != NULL) {
            fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM "
                  "Management Datafile//EN\" \"http://www.logiqx.com/Dats/datafile.dtd\">\n<datafile>\n"
                  "\t<header>\n\t\t<name>-insert name-</name>\n\t\t<description>-insert description-"
                  "</description>\n\t</header>\n\t<game name=\"", f);
            xml_escaped(f, base);
            fputs("\">\n\t\t<description>", f);
            xml_escaped(f, base);
            fputs("</description>\n\t\t<rom name=\"", f);
            xml_escaped(f, base);
            fprintf(f, ".bin\" size=\"%lld\" crc=\"%s\" md5=\"%s\" sha1=\"%s\" />\n\t</game>\n</datafile>\n",
                    h.size, h.crc, h.md5, h.sha1);
            bad = ferror(f);
            if (fclose(f) || bad) { u8_remove(p); f = NULL; }
        }
    }
    if (!f) {
        snprintf(line, sizeof(line), "Cannot write the .cue and the .dat of %s: check the destination drive.", base);
        say(on_line, ctx, line);
        return -1;
    }
    return 0;
}

typedef struct { char names[64][200]; int n; } name_list;

static void add_name(void *c, const char *name, int isdir)
{
    name_list *l = (name_list *)c;
    if (!isdir && strcmp(name, CONSOLE) && l->n < 64) str_copy(l->names[l->n++], sizeof(l->names[0]), name);
}

/* before the data read: the logs of the failed cd go, so that they are not
 * taken for the new read's; what DIC printed stays in dic-console.txt */
static void drop_attempt(const char *dir)
{
    name_list *l = (name_list *)calloc(1, sizeof(name_list));
    char p[1100]; int i;
    if (!l) return;
    u8_listdir(dir, add_name, l);
    for (i = 0; i < l->n; i++) {
        u8_join(p, sizeof(p), dir, l->names[i]);
        u8_remove(p);
    }
    free(l);
}

/* the cd failure taught that the drive cannot read the lead-in: remembered
 * by model (from DIC's _drive.txt), and the caller is told */
static char learned_model[160], learned_letter;

static void learn_drive(const char *dir, const char *base, int leadin, runner_line_fn on_line, void *ctx)
{
    char model[160], p[500];
    drive_model(dir, base, 1, model, sizeof(model));
    if (!model[0]) return;
    /* the model of this letter, for when Windows does not say it */
    str_copy(learned_model, sizeof(learned_model), model);
    learned_letter = drive[0];
    if (!optical_list_add(noleadin, sizeof(noleadin), model)) return;
    snprintf(p, sizeof(p), "Drive %s cannot read the %s: remembered, its next data CDs are read with the "
                           "data command straight away.", model, edge_name(leadin));
    say(on_line, ctx, p);
    if (learned_fn) learned_fn(learned_ctx, model);
}

/* a drive already known not to read the lead-in, and a data CD: no cd first */
static int known_noleadin(runner_line_fn on_line, void *ctx)
{
    char model[160], why[300], p[700];
    if (!optical_drive_model(drive[0], model, sizeof(model)) && learned_letter == drive[0])
        str_copy(model, sizeof(model), learned_model);
    if (!noleadin[0] || !model[0] || !optical_drive_listed(noleadin, model)) return 0;
    if (data_only(&g_toc, why, sizeof(why))) return 1;
    snprintf(p, sizeof(p), "Drive %s cannot read the lead-in, but %s: trying the cd command anyway.", model, why);
    say(on_line, ctx, p);
    return 0;
}

/* data from sector 0 to the lead-out, with the same speed, C2 check, scan
 * and offset as cd. con is the read's dic-console.txt, closed here */
static int read_data(const char *out_dir, const char *base, const char *arg, int sf, FILE *con,
                     const char *why, runner_line_fn on_line, void *ctx)
{
    char end[16], input[1100], name[200], bin[1100], p[1600];
    const char *argv[] = { dic, "data", drive, arg, speed_for(base), "0", end, "/c2", C2_REREAD, "/sf", "/q", NULL };
    tee t; int code = 0, rc, have; long long size;

    snprintf(end, sizeof(end), "%ld", g_toc.end);
    if (!sf) { argv[9] = "/q"; argv[10] = NULL; }
    if (con) fprintf(con, METHOD_MARK "data 0-%ld (%s)\n", g_toc.end, why);
    snprintf(p, sizeof(p), "Reading the CD with DiscImageCreator data, sectors 0 to %ld: %s. Every sector is "
                           "checked with its EDC/ECC; it is not a redump-grade dump.", g_toc.end, why);
    say(on_line, ctx, p);
    memset(&t, 0, sizeof(t));
    t.f = con; t.fn = on_line; t.ctx = ctx; t.sf = sf;
    cd_input(out_dir, input, sizeof(input), con);
    rc = runner_run_tty_input(argv, input, tee_line, &t, &code);
    if (input[0]) u8_remove(input);
    if (rc == RUNNER_NOSTART || (rc == RUNNER_DONE && dll_missing(code))) {
        if (con) fclose(con);
        u8_rmtree(out_dir);
        return no_tool(rc == RUNNER_NOSTART ? 0 : code, on_line, ctx);
    }
    if (rc == RUNNER_CANCELED && t.stopped) { runner_clear(); rc = RUNNER_DONE; if (!code) code = 1; }
    if (rc == RUNNER_CANCELED) { if (con) fclose(con); u8_rmtree(out_dir); return 1; }
    snprintf(name, sizeof(name), "%s.bin", base);
    u8_join(bin, sizeof(bin), out_dir, name);
    size = u8_filesize(bin);
    /* every sector up to the lead-out: complete even if DIC exits with an
     * error, and the verification will tell */
    have = size >= (long long)g_toc.end * 2352 || (code == 0 && size > 0 && size % 2352 == 0);
    if (have && code != 0 && con) fprintf(con, EXIT_MARK "%d\n", code);
    if (con) fclose(con);
    prot_used = t.resets ? (t.sf ? 2 : -1) : sf;
    if (t.wrong || t.denied || t.firmware || t.c2_left || (t.ask_offset && !offset[0])) prot_used = -1;
    if (!have) {
        if (t.c2_left) slow_down(base); else slowed_to = 0;
        fail_text(p, sizeof(p), 0, code, &t, out_dir, base, name);
        say(on_line, ctx, p);
        return -1;
    }
    if (code != 0) {
        snprintf(p, sizeof(p), "DiscImageCreator exited with error %d, but the read is complete: "
                               "keeping it, the verification will tell whether it is good.", code);
        say(on_line, ctx, p);
    }
    return write_cue_dat(out_dir, base, bin, on_line, ctx);
}

static int read_media(int dvd, const char *out_dir, runner_line_fn on_line, void *ctx)
{
    char base[96], img[1100], cue[1100], dat[1100], con[1100], arg[1100], parent[1100], *sep;
    const char *cd_argv[] = { dic, "cd", drive, arg, cd_speed, "/c2", C2_REREAD, "/sf", "/q", NULL };
    const char *dvd_argv[] = { dic, "dvd", drive, arg, DVD_SPEED, "/q", NULL };
    tee t; int code = 0, rc, have, sf = 0;
    char p[1600], input[1100] = "";
    strbuf why;
    long long need = need_bytes(dvd);

    prot_used = -1;             /* until DIC starts: no room, no folder */

    str_copy(base, sizeof(base), base_of(out_dir));
    if (!dvd) cd_argv[4] = speed_for(base);
    /* before 30 minutes of reading: is there room, and is the file system fine? */
    str_copy(parent, sizeof(parent), out_dir);
    sep = strrchr(parent, U8_SEP[0]);
    if (sep) *sep = 0;
    sb_init(&why);
    if (optical_room(u8_isdir(parent) ? parent : out_dir, need, dvd ? need : 0, &why)) {
        snprintf(p, sizeof(p), "Cannot read the %s: %s.", dvd ? "DVD" : "CD", why.s);
        say(on_line, ctx, p);
        sb_free(&why);
        return -1;
    }
    if (u8_mkdirs(out_dir)) {
        snprintf(p, sizeof(p), "Cannot create folder %s: check the destination drive.", out_dir);
        say(on_line, ctx, p);
        sb_free(&why);
        return -1;
    }
    snprintf(p, sizeof(p), "%s%s", base, dvd ? ".iso" : ".bin");
    u8_join(img, sizeof(img), out_dir, p);
    if (optical_dic_path(out_dir, p, arg, sizeof(arg), &why)) {
        snprintf(p, sizeof(p), "Cannot read the %s: %s.", dvd ? "DVD" : "CD", why.s);
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
    if (!dvd) {
        sf = choose_sf(t.f, on_line, ctx);
        t.sf = sf;
        prot_used = sf;
        if (!sf) { cd_argv[7] = "/q"; cd_argv[8] = NULL; }
        if (known_noleadin(on_line, ctx))
            return read_data(out_dir, base, arg, sf, t.f, "the drive is known not to read the lead-in", on_line, ctx);
        cd_input(out_dir, input, sizeof(input), t.f);
    }
    /* in a hidden console: on a pipe DIC's C runtime buffers its output in
     * blocks, so the progress lines ("Creating .scm (LBA) 1234/300000")
     * arrived only in bursts or at the end and the bar stood still
     * (seen on the XP VM: 0 bytes in 60 s on a pipe, at once on a console) */
    rc = runner_run_tty_input(dvd ? dvd_argv : cd_argv, input, tee_line, &t, &code);
    if (input[0]) u8_remove(input);     /* not part of the read */
    if (rc == RUNNER_NOSTART || (rc == RUNNER_DONE && dll_missing(code))) {
        if (t.f) fclose(t.f);
        u8_rmtree(out_dir);
        return no_tool(rc == RUNNER_NOSTART ? 0 : code, on_line, ctx);
    }
    /* stopped here, not by the user: a failed read, with DIC's logs */
    if (rc == RUNNER_CANCELED && t.stopped) { runner_clear(); rc = RUNNER_DONE; if (!code) code = 1; }
    if (rc == RUNNER_CANCELED) { if (t.f) fclose(t.f); u8_rmtree(out_dir); return 1; }
    /* the .dat is the last thing DIC writes: if it is there with the .cue
     * (or the .iso) the read is complete, even if DIC exits with an error.
     * On a dying disc it may be the only good one: it is kept, and the
     * verification will tell whether it has errors */
    have = u8_filesize(dat) > 0 && (dvd ? u8_filesize(img) > 0 : u8_filesize(cue) > 0);
    if (have && code != 0 && t.f) fprintf(t.f, EXIT_MARK "%d\n", code);
    if (t.f) fclose(t.f);
    /* the drive was reset during the read: with /sf the scan is the cause
     * (read it again without), without /sf the scan is not the remedy */
    if (t.resets) prot_used = t.sf ? 2 : -1;
    if (have) {
        if (code != 0) {
            snprintf(p, sizeof(p), "DiscImageCreator exited with error %d, but the read is complete: "
                                   "keeping it, the verification will tell whether it is good.", code);
            say(on_line, ctx, p);
        }
        return 0;
    }
    if (t.wrong || t.denied || t.firmware || t.c2_left || t.leadin || (t.ask_offset && !offset[0]))
        prot_used = -1;    /* /sf would not help */
    if (t.leadin && !dvd && !t.stopped && !(t.ask_offset && !offset[0])) {
        /* the TOC DIC read before stopping is the full one (sessions too) */
        optical_toc toc; char why2[300], tocp[1100];
        snprintf(p, sizeof(p), "%s_disc.txt", base);
        u8_join(tocp, sizeof(tocp), out_dir, p);
        if (!optical_toc_file(tocp, &toc)) g_toc = toc;
        learn_drive(out_dir, base, t.leadin, on_line, ctx);
        if (data_only(&g_toc, why2, sizeof(why2))) {
            snprintf(p, sizeof(p), "This drive cannot read the %s, which DiscImageCreator's cd command needs: "
                                   "reading this data CD again with the data command, which does without it.",
                     edge_name(t.leadin));
            say(on_line, ctx, p);
            drop_attempt(out_dir);
            snprintf(p, sizeof(p), "the drive cannot read the %s", edge_name(t.leadin));
            return read_data(out_dir, base, arg, sf, u8_fopen(con, "ab"), p, on_line, ctx);
        }
        snprintf(p, sizeof(p), "This drive cannot read the %s (%s), which DiscImageCreator's cd command needs: "
                               "it stopped there. The copy protection scan would not help, and the data command, "
                               "which does without it, cannot be used: %s. Use a drive that can read the lead-in "
                               "and the lead-out (DiscImageCreator recommends PLEXTOR drives), or skip this CD "
                               "for now.", edge_name(t.leadin),
                 t.leadin == 2 ? "the sectors after the end of the disc" : "the sector before the start of the disc, LBA -1",
                 why2);
    } else {
        if (t.c2_left && !dvd) slow_down(base); else slowed_to = 0;
        fail_text(p, sizeof(p), dvd, code, &t, out_dir, base,
                  u8_filesize(dat) > 0 ? base_of(dvd ? img : cue) : base_of(dat));
    }
    say(on_line, ctx, p);
    /* the .dat or the image is missing: it is not a read. The folder stays,
     * with DIC's logs: the session sets it aside to explain the failure */
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
    int prot_scan;                  /* read with /sf: 1, 0, -1 not recorded (older reads, DVD) */
    char prot_choice[8];            /* "auto" "on" "off" */
    int prot_listed;                /* the disc's files were looked at */
    char prot_files[600];           /* the signature files found, "" none */
    char offset_given[16];          /* the offset RetroArchiver gave DIC, "" none */
    char offset_dic[16];            /* the drive offset DIC used, from its log */
    char speed[8];                  /* "16x", "" in the reads before 261024 */
    int data_read;                  /* read with the data command, not cd */
    char data_why[160];             /* why: "the drive cannot read the lead-in" */
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

/* RetroArchiver's lines on how the read was made, and the offset DIC used:
 * "	-         Drive Offset(Byte)   2832, (Samples)   708" */
static void how_read(const char *line, dump *d)
{
    const char *q;
    if (!strncmp(line, METHOD_MARK, strlen(METHOD_MARK))) {
        q = line + strlen(METHOD_MARK);
        d->data_read = !strncmp(q, "data", 4);
        if ((q = strchr(q, '(')) != NULL) {
            str_copy(d->data_why, sizeof(d->data_why), q + 1);
            if (strrchr(d->data_why, ')')) *strrchr(d->data_why, ')') = 0;
        }
    } else if (!strncmp(line, PROT_MARK, strlen(PROT_MARK))) {
        q = line + strlen(PROT_MARK);
        d->prot_scan = !strncmp(q, "on", 2);
        if ((q = strchr(q, ',')) != NULL) {
            while (*++q == ' ') ;
            str_copy(d->prot_choice, sizeof(d->prot_choice), q);
        }
    } else if (!strncmp(line, FILES_MARK, strlen(FILES_MARK))) {
        q = line + strlen(FILES_MARK);
        d->prot_listed = 1;
        str_copy(d->prot_files, sizeof(d->prot_files), strcmp(q, "none") ? q : "");
    } else if (!strncmp(line, OFFSET_MARK, strlen(OFFSET_MARK))) {
        q = line + strlen(OFFSET_MARK);
        str_copy(d->offset_given, sizeof(d->offset_given), strcmp(q, "not set") ? q : "");
    } else if (!strncmp(line, SPEED_MARK, strlen(SPEED_MARK))) {
        str_copy(d->speed, sizeof(d->speed), line + strlen(SPEED_MARK));
    } else if (!d->offset_dic[0] && (q = strstr(line, "Drive Offset(Byte)")) != NULL &&
               (q = strstr(q, "(Samples)")) != NULL) {
        int v = atoi(q + 9);
        if (v) snprintf(d->offset_dic, sizeof(d->offset_dic), "%+d", v);
        else str_copy(d->offset_dic, sizeof(d->offset_dic), "0");
    }
}

/* the lines that matter in DIC's logs */
static void scan_logs(const char *dir, dump *d)
{
    char p[1100], name[200], line[1000]; FILE *f;
    const char *logs[] = { CONSOLE, "_disc.txt", NULL };
    int i;
    d->c2 = d->edc = d->readerr = -1;
    d->prot_scan = -1;
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
            if ((q = strstr(line, EXIT_MARK)) != NULL) d->dic_exit = atoi(q + strlen(EXIT_MARK));
            else if ((q = strstr(line, EXIT_MARK_261017)) != NULL) d->dic_exit = atoi(q + strlen(EXIT_MARK_261017));
            /* "Detected protection [SafeDisc]", "Detected a protected [x]. LBA..." */
            if ((q = strstr(line, "Detected protection [")) || (q = strstr(line, "Detected a protected ["))) {
                char *a = strchr(q, '['), *b = a ? strchr(a, ']') : NULL;
                if (a && b && !d->protection[0]) snprintf(d->protection, sizeof(d->protection), "%.*s", (int)(b - a - 1), a + 1);
            }
            if (strstr(line, "=> SecuROM") && !d->protection[0])
                str_copy(d->protection, sizeof(d->protection), strstr(line, "=> SecuROM") + 3);
            how_read(line, d);
        }
        fclose(f);
    }
    /* unreadable sectors */
    snprintf(name, sizeof(name), "%s_mainError.txt", d->base);
    u8_join(p, sizeof(p), dir, name);
    if ((f = u8_fopen(p, "rb")) != NULL) {
        long sectors = d->n ? (long)(d->t[0].size / 2352) : 0;
        d->readerr = 0;
        while (fgets(line, sizeof(line), f)) {
            char *q = strstr(line, "LBA[");
            long lba = q ? atol(q + 4) : 0;
            if (!strstr(line, "Read error")) continue;
            /* data reads LBA -1 (or the lead-out) for the offset: a drive
             * without lead-in fails there, outside the disc's sectors */
            if (d->data_read && q && (lba < 0 || lba >= sectors)) continue;
            d->readerr++;
        }
        fclose(f);
    }
    drive_model(dir, d->base, 1, d->drive, sizeof(d->drive));
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
        sb_printf(why, "%s is missing, the fingerprints DiscImageCreator writes at the end", name);
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
    if (!d->n) { sb_printf(why, "%s lists no tracks", name); return -1; }
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
    sb_printf(o, "  %s: %d track%s\n", d->base, d->n, d->n == 1 ? "" : "s");
    for (i = 0; i < d->n; i++)
        sb_printf(o, "    %-11s %10lld bytes  crc %s  sha1 %s  %s\n", d->t[i].mode[0] ? d->t[i].mode : what,
                  d->t[i].size, d->t[i].crc, d->t[i].sha1, d->t[i].name);
    if (d->c2 >= 0 || d->edc >= 0 || d->readerr >= 0)
        sb_printf(o, "    errors: unresolved C2 %d, sectors with bad EDC/ECC %d, unreadable sectors %d\n",
                  d->c2 > 0 ? d->c2 : 0, d->edc > 0 ? d->edc : 0, d->readerr > 0 ? d->readerr : 0);
    if (d->protection[0]) sb_printf(o, "    protection detected by DiscImageCreator: %s\n", d->protection);
    if (d->no_c2) sb_puts(o, "    C2 check not available on this drive: C2 errors were not looked for\n");
    if (d->dic_exit) sb_printf(o, "    DiscImageCreator exited with error %d, but the read is complete\n", d->dic_exit);
    if (d->data_read)
        sb_printf(o, "    read with the data command (%s): not a redump-grade dump\n",
                  d->data_why[0] ? d->data_why : "the drive cannot read the lead-in");
}

/* a data read whose sectors all pass their EDC/ECC, with no C2 errors and
 * none unreadable: every sector proves itself, one read is enough */
static int edc_verified(const dump *d)
{
    return d->data_read && d->edc == 0 && d->c2 == 0 && d->readerr <= 0 && !d->dic_exit;
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

    if (!d) { sb_puts(o, "Out of memory for the comparison.\n"); return finish(r, 2, ST_ERRORS, "error"); }
    sb_init(&why);
    sb_printf(o, "Verification of the %s with DiscImageCreator and redump\n\n", what);
    for (i = 0; i < n; i++) {
        if (load_dump(imgs[i], &d[i], &why)) {
            sb_printf(o, "  %s: %s.\n\n  The read is not complete: read the %s again.\n", base_of(imgs[i]), why.s, what);
            sb_free(&why);
            free(d);
            return finish(r, 2, ST_ERRORS, "error");
        }
        describe(o, what, &d[i]);
    }
    sb_free(&why);
    sb_puts(o, "\n");

    if (n == 1) {
        redump_hit hit; char dir[600]; int dats, m = redump_match(&d[0], &hit, dir, sizeof(dir), &dats);
        r->agree = 1;
        if (m == 1) {
            sb_printf(o, "  Redump: matches \"%s\"\n  (DAT %s, dated %s).\n\n", hit.game, hit.dat,
                      hit.date[0] ? hit.date : "date not given");
            sb_printf(o, "  The %s is identical to the one verified by other archivists: one read is enough.\n", what);
            free(d);
            return finish(r, 0, ST_REDUMP, "redump");
        }
        if (m < 0) sb_printf(o, "  Redump: no redump DAT in %s.\n", dir[0] ? dir : "(the program's folder)");
        else sb_printf(o, "  Redump: not found in %d DATs in %s.\n", dats, dir);
        if (edc_verified(&d[0])) {
            sb_printf(o, "  Every sector matches its EDC/ECC and there are no C2 errors: the data are\n"
                         "  verified with one read. It is not a redump-grade dump (read with the data\n"
                         "  command, without lead-in and lead-out).\n");
            free(d);
            return finish(r, 0, ST_CLEAN, "edc-ecc");
        }
        if (errors_of(&d[0]))
            sb_printf(o, "  The read has %d errors: a second read will tell whether they are on the disc.\n", errors_of(&d[0]));
        sb_printf(o, "  A single read is not verified: a second read is needed to compare.\n");
        free(d);
        return finish(r, 1, ST_READ_ONCE, "single");
    }

    /* two or more reads: one that redump knows is good even if the others
     * are not, and it becomes the master */
    {
        redump_hit hit; char dir[600]; int dats;
        for (i = n - 1; i >= 0; i--) {
            for (k = i + 1; k < n && !same_tracks(&d[i], &d[k]); k++) ;
            if (k < n) continue;                /* already looked up with the same tracks */
            if (redump_match(&d[i], &hit, dir, sizeof(dir), &dats) != 1) continue;
            sb_printf(o, "  Redump: read %s matches \"%s\"\n  (DAT %s, dated %s).\n\n", d[i].base,
                      hit.game, hit.dat, hit.date[0] ? hit.date : "date not given");
            sb_printf(o, "  The %s is identical to the one verified by other archivists: keeping %s.\n", what, d[i].base);
            r->master = i + 1;
            for (k = 0; k < n; k++) r->agree += same_tracks(&d[i], &d[k]);
            free(d);
            return finish(r, 0, ST_REDUMP, "redump");
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
            sb_printf(o, "  The %d reads match, but with %d unresolved errors: the disc is damaged\n"
                         "  (or has a protection redump does not know). Try another drive and\n"
                         "  catalog it with errors.\n", n, errors_of(&d[0]));
            rc = finish(r, 2, ST_ERRORS, "identical");
        } else {
            sb_printf(o, "  The %d reads match track by track: catalog it as clean.\n", n);
            rc = finish(r, 0, ST_CLEAN, "identical");
        }
    } else {
        sb_printf(o, "  The reads do not match (%d tracks differ, %d of %d reads are the same):\n"
                     "  the disc does not read consistently. Clean it and read it again.\n",
                  r->unstable_tracks, agree, n);
        rc = finish(r, 1, ST_UNSTABLE, "data_differ");
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
        jw_str(w, "filesystem_note", "no data track: it is an audio CD");
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
    else if (!fi.joliet) jw_str(w, "filesystem_note", "ISO 9660 names only: the disc has no Joliet");
    iso_free(&fi);
}

static void count_or_null(json_writer *w, const char *key, int v)
{
    if (v >= 0) jw_int(w, key, v); else jw_null(w, key);
}

/* how a CD was read: the copy protection scan and the drive's offset.
 * Reads made before this have neither: null and "unknown" */
static void tech_cd_read(const dump *d, json_writer *w)
{
    if (d->prot_scan >= 0) {
        jw_begin_obj(w, "copy_protection_scan");
        jw_str(w, "choice", d->prot_choice[0] ? d->prot_choice : NULL);
        jw_int(w, "used", d->prot_scan);
        /* null: not looked at (on or off as chosen, or the disc not mounted) */
        jw_str(w, "signature_files", d->prot_listed ? d->prot_files : NULL);
        jw_end_obj(w);
    } else {
        jw_null(w, "copy_protection_scan");
    }
    /* what DIC says it used wins over what it was given */
    jw_str(w, "drive_offset", d->offset_dic[0] ? d->offset_dic : d->offset_given[0] ? d->offset_given : "unknown");
    jw_str(w, "read_method", d->data_read ? "dic-data" : "dic-cd");
    if (d->speed[0]) jw_str(w, "read_speed", d->speed);
    if (d->data_read) {
        char note[300];
        snprintf(note, sizeof(note), "DiscImageCreator data command from sector 0 to the lead-out (%s): "
                                     "checked by EDC/ECC, not redump-grade",
                 d->data_why[0] ? d->data_why : "the drive cannot read the lead-in");
        jw_str(w, "read_note", note);
    }
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
        jw_str(w, "filesystem_note", why.s ? why.s : "the read cannot be loaded");
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
    if (!ends_with_ci(d->t[0].name, ".iso")) tech_cd_read(d, w);
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
    /* "LBA[-00001, 0xffffffff] Reread NG": the data command reading before the disc */
    if (strstr(line, "LBA[-")) return NULL;
    if (strstr(line, "C2 error") || strstr(line, "c2 error") || strstr(line, "(c2)") ||
        strstr(line, "Reread") || strstr(line, "reread sector") || strstr(line, "EccEdc") ||
        strstr(line, "Checking sectors"))
        return "C2 check";
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

static int identity(const char *img, media_identity *out)
{
    dump *d = (dump *)calloc(1, sizeof(dump));
    strbuf why; int rc = -1;
    memset(out, 0, sizeof(*out));
    sb_init(&why);
    if (d && load_dump(img, d, &why) == 0 && d->n > 0) {
        tracks_key(d, out->key);
        str_copy(out->method, sizeof(out->method), d->data_read ? "dic-data" : "dic-cd");
        str_copy(out->drive, sizeof(out->drive), d->drive);
        rc = 0;
    }
    sb_free(&why);
    free(d);
    return rc;
}

const media_ops cd_media = {
    "cd", "", check, scan, read_cd, compare_cd, tech, NULL, NULL,
    "DiscImageCreator", "TOC", 1, phase_of, volume_label,
    set_protection, protection_used, identity
};

const media_ops dvd_media = {
    "dvd", "", check, scan, read_dvd, compare_dvd, tech, NULL, NULL,
    "DiscImageCreator", "TOC", 1, phase_of, volume_label, NULL, NULL, identity
};
