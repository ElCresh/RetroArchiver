/* RetroArchiver - window for archiving boxes: floppies read with
 * SAMdisk, CDs and DVDs read with DiscImageCreator.
 *
 * A single window on top of core/session: the session runs in a worker
 * thread and talks to the window only through PostMessage, so the UI does
 * not freeze during a long read. Texts in UTF-16 ("W" APIs), the core
 * works in UTF-8. The window texts have real accents: the source is UTF-8
 * and the L"" strings become UTF-16.
 *
 * Five views: the archive (the first one, with the boxes already done),
 * setup, waiting for the disk, reading, end. The window is resizable:
 * layout() recomputes the positions on every WM_SIZE.
 */
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dbt.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/session.h"
#include "core/bagit.h"
#include "core/fsu8.h"
#include "core/archive.h"
#include "gui/settings.h"

#define WM_APP_EVENT (WM_APP + 1)   /* there is an event in g_qhead   */
#define WM_APP_WAIT  (WM_APP + 2)   /* the requested medium is in g_turn_in */
#define WM_APP_END   (WM_APP + 3)   /* wParam: result of session_run */

#define CYL_MAX   83                /* disk bar: cylinders 0..83 */
#define WM_APP_SAMDISK (WM_APP + 4) /* wParam: generation, lParam: sd_result* */
#define WM_APP_READY (WM_APP + 5)   /* recompute "Pronto" after a platform change */
#define WM_APP_ARCHIVE (WM_APP + 6) /* wParam: generation, lParam: arc_list* */
#define WM_APP_VERIFY (WM_APP + 7)  /* lParam: vf_result* */
#define WM_APP_FOCUS (WM_APP + 8)   /* lParam: the control that must get the focus */
#define WM_APP_DIC (WM_APP + 9)     /* wParam: generation, lParam: dic_result* */
#define WM_APP_FLOPPY (WM_APP + 10) /* wParam: generation, lParam: fd_result* */
#define WM_APP_LABEL (WM_APP + 11)  /* wParam: generation, lParam: lb_result* */
#define OPT_MAX   1000              /* disk bar for CDs and DVDs: thousandths */

#define TIMER_ID  1
#define TIMER_SAMDISK 2             /* SAMdisk check ~600 ms after the last keystroke */
#define TIMER_ADIR 3                /* archive re-read ~600 ms after the last keystroke */
#define TIMER_LINES 4               /* SAMdisk lines: the window takes them in batches */
#define TIMER_DIC 5                 /* DiscImageCreator check ~600 ms after the last keystroke */
#define TIMER_FLOPPY 6              /* floppy drive check ~600 ms after the last keystroke */
#define LINES_MS  100
#define LINES_CHUNK 65536           /* bytes of lines in a batch; beyond that a new one opens */
#define MIN_CW    640               /* minimum client area: fits an 800x600 screen */
#define MIN_CH    480
#define LOG_LIMIT (4 * 1024 * 1024) /* characters the log EDIT accepts */
#define LOG_KEEP  200000            /* beyond that, the oldest third is removed */
#define LBL       1000              /* id of a field's label: field id + LBL */

enum {
    /* setup: from ID_TITLE to ID_START */
    ID_TITLE = 100, ID_RESUME, ID_PLATFORM, ID_COUNT, ID_PUBLISHER, ID_YEAR,
    ID_LANGUAGE, ID_EDITION, ID_NOTES, ID_MEDIA, ID_CD, ID_DVD, ID_ICFLOP, ID_ICCD, ID_ICDVD,
    ID_HINT, ID_DIR, ID_BROWSE, ID_MAXREAD, ID_SAMDISK, ID_SOURCE, ID_DIC, ID_DRIVE, ID_SOUNDS, ID_BAGPATH,
    ID_BACK, ID_START,
    /* session */
    ID_BANNER = 200, ID_PILLS, ID_LABEL, ID_NOTE, ID_READ, ID_SKIP, ID_FINISH,
    ID_STOP, ID_LIST, ID_LOG, ID_NEW, ID_OPEN, ID_BOXBAR, ID_BOXTXT,
    ID_DISKBAR, ID_CYLTXT, ID_ELAPSED, ID_BAGDIR, ID_BAGCONT, ID_BAGHASH, ID_BAGASIDE,
    ID_SBAR, ID_TOARCH, ID_LABELREAD,
    /* archive: from ID_ADIR to ID_ANEW */
    ID_ADIR = 300, ID_ABROWSE, ID_ASEARCH, ID_APLAT, ID_ASUM, ID_ALIST, ID_ADETAIL,
    ID_ARESUME, ID_AOPEN, ID_AVERIFY, ID_AHINT, ID_ANEW
};

/* what the window knows about a disk: copied by the worker thread, which
 * is the only one touching g_box while the session runs */
typedef struct {
    char master[128], label[128], status[24];
    int  nreads, agree;
} disk_info;

/* EV_LINE carries a batch of lines separated by '\n': len bytes out of cap */
typedef struct gui_event {
    event_kind k; int disk; char *text; disk_info info;
    size_t len, cap;
    struct gui_event *next;
} gui_event;

/* queue of events from the worker thread to the window, in arrival order.
 * One message per SAMdisk line was too much: during a scan hundreds arrive
 * per second and, with messages always queued, WM_TIMER and WM_PAINT came
 * later, even 20 seconds later. Lines are appended to the last batch and
 * the window takes them with TIMER_LINES, which like WM_PAINT arrives only
 * when the queue is empty; the other events post a message that drains
 * the whole queue, lines included, in the right order */
static CRITICAL_SECTION g_qlock;
static gui_event *g_qhead, *g_qtail;

static HWND     g_wnd, g_banner, g_pills, g_list, g_log, g_boxbar, g_boxtxt, g_diskbar;
static HWND     g_cyltxt, g_elapsed, g_sbar, g_label, g_note;
static HFONT    g_font, g_big, g_num, g_bold, g_mono, g_mid;
static HBRUSH   g_black;
static HANDLE   g_wake;                 /* the window answers wait_disk */
static volatile LONG g_cmd;
static char     g_label_u8[128];        /* label and note: from the core to the window and back */
static char     g_note_u8[256];
static int      g_running, g_waiting, g_closing, g_errbeep, g_view;
static meta_box *g_box;
static session_cfg g_cfg;
static char     g_outdir[512], g_title[256], g_bagdir[600];
static int      g_count[MEDIA_KINDS];   /* make-up of the current box */
static char     g_names[MEDIA_KINDS][64];       /* its naming patterns */
static char     g_ini_names[MEDIA_KINDS][64];   /* NomeFloppy, NomeCD, NomeDVD from the ini */
static disk_turn g_turn_in, g_turn;     /* the requested medium: from the worker thread, and for the window */

static disk_info g_disk[META_MAXDISK + 1];
static int      g_total, g_last_done, g_cur;
static int      g_bag_closed, g_bag_complete, g_reread, g_fragile;
static int      g_aside_n;              /* files in messi-da-parte in the closed bag */
static wchar_t  g_phase_name[128], g_last_err[600];
static wchar_t  g_drive_w[8];           /* the CD and DVD drive, "D:", for the texts */
static wchar_t  g_why[600];             /* why DiscImageCreator did not read the disc */
static DWORD    g_t_sess, g_t_phase;

static const wchar_t HINT[] =
    L"Per continuare un cofanetto lasciato a metà: Riprendi..., oppure lo stesso titolo esatto.";

/* setup, waiting for the disk, reading, end; and the archive, where it starts */
enum { V_SETUP, V_WAIT, V_WORK, V_END, V_ARCHIVE };

/* ---- conversions ---- */

/* direct conversions into a short buffer fail and would leave the text
 * empty: convert into a buffer of the right size and truncate */
static void to_w(const char *s, wchar_t *out, int n)
{
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *tmp;
    out[0] = 0;
    if (need <= 0) return;
    if (need <= n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n); return; }
    if (!(tmp = (wchar_t *)malloc(need * sizeof(wchar_t)))) return;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, tmp, need)) {
        int k = n - 1;
        if (k > 0 && tmp[k - 1] >= 0xD800 && tmp[k - 1] <= 0xDBFF) k--;   /* keep surrogate pairs whole */
        memcpy(out, tmp, k * sizeof(wchar_t));
        out[k] = 0;
    }
    free(tmp);
}

static void to_u8(const wchar_t *s, char *out, int n)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *tmp;
    out[0] = 0;
    if (need <= 0) return;
    if (!(tmp = (char *)malloc(need))) return;
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, tmp, need, NULL, NULL))
        str_copy(out, (size_t)n, tmp);      /* cuts on a UTF-8 boundary */
    free(tmp);
}

/* UTF-8 bytes needed for the control's text, terminator excluded */
static int u8_len(int id)
{
    wchar_t w[1024];
    GetDlgItemTextW(g_wnd, id, w, 1024);
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL) - 1;
}

static void get_u8(int id, char *out, int n)
{
    wchar_t w[512];
    GetDlgItemTextW(g_wnd, id, w, 512);
    to_u8(w, out, n);
}

/* like get_u8, without leading and trailing blanks: "   " is not a title */
static void get_u8_trim(int id, char *out, int n)
{
    wchar_t w[512], *p = w; size_t len;
    GetDlgItemTextW(g_wnd, id, w, 512);
    while (*p == L' ' || *p == L'\t') p++;
    len = wcslen(p);
    while (len && (p[len - 1] == L' ' || p[len - 1] == L'\t')) p[--len] = 0;
    to_u8(p, out, n);
}

static void set_u8(int id, const char *s)
{
    wchar_t w[512];
    to_w(s, w, 512);
    SetDlgItemTextW(g_wnd, id, w);
}

/* "1:42", or "1:02:03" beyond one hour */
static void fmt_time(DWORD ms, wchar_t *out, int n)
{
    DWORD s = ms / 1000;
    if (s >= 3600) _snwprintf(out, n, L"%lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
    else _snwprintf(out, n, L"%lu:%02lu", s / 60, s % 60);
    out[n - 1] = 0;
}

/* "1 pulito" / "2 puliti" */
static void plural(wchar_t *out, int n, int k, const wchar_t *one, const wchar_t *many)
{
    _snwprintf(out, n, L"%d %ls", k, k == 1 ? one : many);
    out[n - 1] = 0;
}

/* ---- media kinds ---- */

static const wchar_t *const kind_w[MEDIA_KINDS] = { L"floppy", L"CD", L"DVD" };
static const wchar_t *const kind_cap[MEDIA_KINDS] = { L"Floppy", L"CD", L"DVD" };

/* kind, and number within the kind, of medium n of the current box */
static void slot(int n, int *kind, int *num)
{
    meta_slot_counts(g_count, n, kind, num);
}

/* "floppy 2", "CD 1"; with cap "Floppy 2", at the start of a sentence */
static void slot_name(int n, int cap, wchar_t *out, int len)
{
    int kind, num;
    slot(n, &kind, &num);
    _snwprintf(out, len, L"%ls %d", cap ? kind_cap[kind] : kind_w[kind], num);
    out[len - 1] = 0;
}

/* "3 floppy + 1 CD"; empty if there are no media */
static void comp_text(const int c[MEDIA_KINDS], wchar_t *out, int len)
{
    wchar_t p[32]; int k;
    out[0] = 0;
    for (k = 0; k < MEDIA_KINDS; k++) {
        if (!c[k]) continue;
        _snwprintf(p, 32, L"%ls%d %ls", out[0] ? L" + " : L"", c[k], kind_w[k]); p[31] = 0;
        wcsncat(out, p, len - wcslen(out) - 1);
    }
}

/* all of the same kind: its name ("floppy"); otherwise "supporto" or "supporti" */
static const wchar_t *noun(const int c[MEDIA_KINDS], int many)
{
    int k, kinds = 0, last = MEDIA_FLOPPY;
    for (k = 0; k < MEDIA_KINDS; k++) if (c[k]) { kinds++; last = k; }
    if (kinds > 1) return many ? L"supporti" : L"supporto";
    return kind_w[last];
}

/* ---- log at the bottom of the window ---- */

/* the EDIT accepts up to LOG_LIMIT characters; before reaching it the
 * oldest third is removed, cutting at a newline, so it never gets stuck */
static void log_trim(void)
{
    int len = GetWindowTextLengthW(g_log), cut;
    wchar_t *buf, *p;
    if (len <= LOG_KEEP) return;
    if (!(buf = (wchar_t *)malloc((len + 1) * sizeof(wchar_t)))) { SetWindowTextW(g_log, L""); return; }
    GetWindowTextW(g_log, buf, len + 1);
    p = wcschr(buf + len / 3, L'\n');
    cut = p ? (int)(p - buf) + 1 : len / 3;
    free(buf);
    SendMessageW(g_log, EM_SETSEL, 0, cut);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

/* text already has its newlines: a batch of lines goes in all at once */
static void log_block(const wchar_t *text)
{
    int len;
    log_trim();
    len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void log_append(const wchar_t *text)
{
    int len;
    log_trim();
    len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

/* ---- sounds ---- */

enum { SND_WAIT, SND_ERROR, SND_DONE };

/* with the checkbox on, the system sounds (set in the Control Panel),
 * otherwise speaker tones, which are heard anyway */
static void sound(int s)
{
    if (IsDlgButtonChecked(g_wnd, ID_SOUNDS) == BST_CHECKED) {
        MessageBeep(s == SND_WAIT ? MB_ICONASTERISK : s == SND_ERROR ? MB_ICONEXCLAMATION : MB_OK);
        return;
    }
    if (s == SND_WAIT) { Beep(880, 200); Sleep(90); Beep(1320, 260); }
    else if (s == SND_ERROR) Beep(220, 500);
    else { Beep(660, 150); Sleep(60); Beep(990, 150); Sleep(60); Beep(1320, 300); }
}

/* ---- worker thread ---- */

static void copy_disk(disk_info *o, const meta_disk *d)
{
    str_copy(o->master, sizeof(o->master), d->master);
    str_copy(o->label, sizeof(o->label), d->label);
    str_copy(o->status, sizeof(o->status), d->status);
    o->nreads = d->nreads;
    o->agree = d->agree;
}

/* called with g_qlock held */
static void queue_push(gui_event *e)
{
    if (g_qtail) g_qtail->next = e; else g_qhead = e;
    g_qtail = e;
}

/* the line goes at the end of the open batch if there is room, otherwise
 * into a new one; without memory it is lost, as before */
static void queue_line(const char *text)
{
    gui_event *e = g_qtail;
    size_t n = strlen(text);
    if (!e || e->k != EV_LINE || e->len + n + 1 > e->cap) {
        size_t cap = n + 1 > LINES_CHUNK ? n + 1 : LINES_CHUNK;
        if (!(e = (gui_event *)calloc(1, sizeof(gui_event)))) return;
        if (!(e->text = (char *)malloc(cap + 1))) { free(e); return; }
        e->k = EV_LINE; e->cap = cap;
        queue_push(e);
    }
    memcpy(e->text + e->len, text, n);
    e->len += n;
    e->text[e->len++] = '\n';
    e->text[e->len] = 0;
}

static void host_event(void *ctx, event_kind k, int disk, const char *text)
{
    gui_event *e;
    size_t n = strlen(text) + 1;
    (void)ctx;
    if (k == EV_LINE) {             /* no message: TIMER_LINES handles it */
        EnterCriticalSection(&g_qlock);
        queue_line(text);
        LeaveCriticalSection(&g_qlock);
        return;
    }
    if (!(e = (gui_event *)calloc(1, sizeof(gui_event)))) return;
    e->k = k; e->disk = disk;
    e->text = (char *)malloc(n);
    if (e->text) memcpy(e->text, text, n);
    /* here, on the worker thread, g_box can be read safely */
    if (k == EV_DISK_DONE && g_box) {
        meta_disk *d = meta_disk_get(g_box, disk);
        if (d) copy_disk(&e->info, d);
    }
    EnterCriticalSection(&g_qlock);
    queue_push(e);
    LeaveCriticalSection(&g_qlock);
    /* if the message is not posted the event stays queued and the next
     * drain takes it, at the latest the one on WM_APP_END */
    PostMessageW(g_wnd, WM_APP_EVENT, 0, 0);
}

/* the label and note the core already knows (resumed disk or one to
 * retry) go into the fields; those in the fields go back to the core */
static int host_wait(void *ctx, const disk_turn *t, char *label, size_t llen, char *note, size_t nlen)
{
    (void)ctx;
    if (g_closing) return CMD_QUIT;
    str_copy(g_label_u8, sizeof(g_label_u8), label);
    str_copy(g_note_u8, sizeof(g_note_u8), note);
    g_turn_in = *t;             /* the window copies it into g_turn when it gets it */
    PostMessageW(g_wnd, WM_APP_WAIT, 0, 0);
    WaitForSingleObject(g_wake, INFINITE);
    str_copy(label, llen, g_label_u8);
    str_copy(note, nlen, g_note_u8);
    return (int)g_cmd;
}

static DWORD WINAPI worker(LPVOID p)
{
    session_host host;
    int rc;
    /* the setup's floppy drive check, if still running: two SAMdisk on
     * the same drive would interfere with each other */
    if (p) { WaitForSingleObject((HANDLE)p, 10000); CloseHandle((HANDLE)p); }
    memset(&host, 0, sizeof(host));
    host.event = host_event;
    host.wait_disk = host_wait;
    host.ctx = NULL;
    host.ask_again = 1;         /* read failed or interrupted: Riprova on the same disk */
    rc = session_run(&g_cfg, &host);
    PostMessageW(g_wnd, WM_APP_END, (WPARAM)rc, 0);
    return 0;
}

/* ---- building the window ---- */

static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    int edge = !wcscmp(cls, L"EDIT") || !wcscmp(cls, WC_LISTVIEWW);
    HWND c = CreateWindowExW(edge ? WS_EX_CLIENTEDGE : 0,
                             cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void field(const wchar_t *label, int y, int id, const wchar_t *value, int w)
{
    make(L"STATIC", label, 0, 12, y + 3, 110, 18, id + LBL);
    make(L"EDIT", value, WS_TABSTOP | ES_AUTOHSCROLL, 125, y, w, 21, id);
}

/* the user cannot type more than what gets saved: up to 3 UTF-8 bytes
 * per character, so about a third of the destination field's bytes */
static void limit(int id, int bytes)
{
    HWND c = GetDlgItem(g_wnd, id);
    wchar_t cls[16];
    GetClassNameW(c, cls, 16);
    SendMessageW(c, _wcsicmp(cls, L"ComboBox") ? EM_LIMITTEXT : CB_LIMITTEXT, (bytes - 1) / 3, 0);
}

static const wchar_t *platforms[] = {
    L"MS-DOS", L"Windows 3.x", L"Windows 9x", L"Windows NT", L"Amiga", L"Atari ST", L"Commodore 64",
    L"Amstrad CPC", L"ZX Spectrum", L"MSX", L"Apple II", L"Macintosh", NULL
};

/* setup controls: ids from ID_TITLE to ID_START, labels + LBL.
 * Positions are those at 640 points; layout_setup widens the long fields */
static void build_setup(void)
{
    HWND cb; int i;
    wchar_t arc[MAX_PATH];

    field(L"Titolo *", 24, ID_TITLE, L"", 380);
    make(L"BUTTON", L"Riprendi...", WS_TABSTOP, 520, 23, 95, 23, ID_RESUME);
    make(L"STATIC", L"Piattaforma *", 0, 12, 55, 110, 18, ID_PLATFORM + LBL);
    cb = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL, 125, 52, 200, 200, ID_PLATFORM);
    for (i = 0; platforms[i]; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)platforms[i]);
    make(L"STATIC", L"Anno", 0, 345, 55, 60, 18, ID_YEAR + LBL);
    make(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, 410, 52, 50, 21, ID_YEAR);
    field(L"Editore", 82, ID_PUBLISHER, L"", 200);
    make(L"STATIC", L"Lingua", 0, 345, 85, 60, 18, ID_LANGUAGE + LBL);
    make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 410, 82, 210, 21, ID_LANGUAGE);
    field(L"Versione", 110, ID_EDITION, L"", 200);
    field(L"Note", 138, ID_NOTES, L"", 495);
    /* the make-up: how many floppies, CDs and DVDs, each with its kind's symbol */
    make(L"STATIC", L"Supporti *", 0, 12, 169, 110, 18, ID_MEDIA);
    make(L"STATIC", L"", SS_OWNERDRAW, 125, 169, 14, 14, ID_ICFLOP);
    make(L"STATIC", L"Floppy", 0, 143, 169, 40, 18, ID_COUNT + LBL);
    make(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER, 185, 166, 36, 21, ID_COUNT);
    make(L"STATIC", L"", SS_OWNERDRAW, 239, 169, 14, 14, ID_ICCD);
    make(L"STATIC", L"CD", 0, 257, 169, 22, 18, ID_CD + LBL);
    make(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER, 281, 166, 36, 21, ID_CD);
    make(L"STATIC", L"", SS_OWNERDRAW, 335, 169, 14, 14, ID_ICDVD);
    make(L"STATIC", L"DVD", 0, 353, 169, 28, 18, ID_DVD + LBL);
    make(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER, 383, 166, 36, 21, ID_DVD);
    make(L"STATIC", HINT, SS_NOPREFIX, 12, 198, 610, 18, ID_HINT);

    set_default_archive(arc, MAX_PATH);         /* with no ini at all: C:\RetroArchive */
    field(L"Cartella", 254, ID_DIR, arc, 460);
    make(L"BUTTON", L"...", WS_TABSTOP, 590, 254, 30, 21, ID_BROWSE);
    field(L"Letture max", 282, ID_MAXREAD, L"3", 40);
    /* the two programs, each with where it reads from: same row, same label */
    field(L"SAMdisk", 310, ID_SAMDISK, L"samdisk", 300);
    make(L"STATIC", L"Leggi da", 0, 435, 313, 60, 18, ID_SOURCE + LBL);
    make(L"EDIT", L"a:", WS_TABSTOP | ES_AUTOHSCROLL, 500, 310, 120, 21, ID_SOURCE);
    field(L"DiscImageCreator", 338, ID_DIC, L"DiscImageCreator", 300);
    make(L"STATIC", L"Leggi da", 0, 435, 341, 60, 18, ID_DRIVE + LBL);
    make(L"EDIT", L"D:", WS_TABSTOP | ES_AUTOHSCROLL, 500, 338, 120, 21, ID_DRIVE);
    make(L"STATIC", L"", SS_NOPREFIX | SS_PATHELLIPSIS, 12, 369, 610, 18, ID_BAGPATH);

    make(L"BUTTON", L"Suoni di Windows", WS_TABSTOP | BS_AUTOCHECKBOX, 12, 428, 200, 20, ID_SOUNDS);
    make(L"BUTTON", L"Archivio", WS_TABSTOP, 426, 423, 100, 28, ID_BACK);
    make(L"BUTTON", L"Inizia", WS_TABSTOP | BS_DEFPUSHBUTTON, 534, 423, 100, 28, ID_START);
}

/* list dots: grey, green, ochre, red; blue for open boxes */
enum { DOT_GREY, DOT_GREEN, DOT_OCHRE, DOT_RED, DOT_BLUE, DOT_COUNT };

static HIMAGELIST make_dots(void)
{
    static const COLORREF col[DOT_COUNT] = {
        RGB(0xb4, 0xb4, 0xb4), RGB(0x2e, 0x8b, 0x3a), RGB(0xd9, 0xa4, 0x00), RGB(0xc8, 0x32, 0x28),
        RGB(0x31, 0x6a, 0xc5)
    };
    HIMAGELIST il = ImageList_Create(16, 16, ILC_COLOR24 | ILC_MASK, DOT_COUNT, 0);
    HDC sc = GetDC(NULL), dc = CreateCompatibleDC(sc);
    HBITMAP bmp = CreateCompatibleBitmap(sc, 16 * DOT_COUNT, 16);
    HGDIOBJ ob = SelectObject(dc, bmp);
    HBRUSH mag = CreateSolidBrush(RGB(255, 0, 255));
    RECT r = { 0, 0, 16 * DOT_COUNT, 16 };
    int i;
    FillRect(dc, &r, mag);
    DeleteObject(mag);
    for (i = 0; i < DOT_COUNT; i++) {
        COLORREF c = col[i];
        HBRUSH br = CreateSolidBrush(c);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(GetRValue(c) * 3 / 4, GetGValue(c) * 3 / 4, GetBValue(c) * 3 / 4));
        HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, br);
        Ellipse(dc, i * 16 + 3, 3, i * 16 + 13, 13);
        SelectObject(dc, op); SelectObject(dc, obr);
        DeleteObject(pen); DeleteObject(br);
    }
    SelectObject(dc, ob);
    DeleteDC(dc);
    ReleaseDC(NULL, sc);
    if (il) ImageList_AddMasked(il, bmp, RGB(255, 0, 255));   /* the image list makes a copy */
    DeleteObject(bmp);
    return il;
}

static void add_column(HWND lv, int i, const wchar_t *name, int w, int fmt)
{
    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = fmt;
    c.cx = w;
    c.pszText = (wchar_t *)name;
    SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
}

static void build_list(void)
{
    g_list = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL |
                  LVS_NOSORTHEADER, 8, 100, 600, 200, ID_LIST);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SendMessageW(g_list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)make_dots());
    add_column(g_list, 0, L"Supporto", 130, LVCFMT_LEFT);
    add_column(g_list, 1, L"Etichetta", 200, LVCFMT_LEFT);
    add_column(g_list, 2, L"Letture", 60, LVCFMT_RIGHT);
    add_column(g_list, 3, L"Esito", 190, LVCFMT_LEFT);
}

/* the archive: folder, search, summary, list of boxes, the chosen box */
static HWND g_alist, g_asum, g_adetail;
static arc_list g_arc;                  /* the last read of the folder */
static int     *g_rows, g_nrows;        /* list rows: indexes into g_arc after the filter */
static int      g_pick = -1;            /* the chosen box, index into g_arc */
static int      g_arc_loaded, g_arc_empty, g_arc_missing, g_verifying;
static volatile LONG g_arc_gen;
static char     g_arc_root[512];        /* the folder that was read */
static char     g_want_dir[600];        /* after the re-read this box gets selected */
static RECT     g_empty;                /* the white panel of the empty archive */

static void build_archive(void)
{
    make(L"STATIC", L"Archivio", 0, 8, 0, 52, 18, ID_ADIR + LBL);
    make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 62, 0, 300, 21, ID_ADIR);
    make(L"BUTTON", L"...", WS_TABSTOP, 0, 0, 30, 21, ID_ABROWSE);
    make(L"STATIC", L"Cerca", 0, 0, 0, 34, 18, ID_ASEARCH + LBL);
    make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 150, 21, ID_ASEARCH);
    /* with the comctl32 6 manifest the grey placeholder text works on XP too */
    SendDlgItemMessageW(g_wnd, ID_ASEARCH, EM_SETCUEBANNER, 0, (LPARAM)L"titolo, editore...");
    make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL, 0, 0, 150, 240, ID_APLAT);
    g_asum = make(L"STATIC", L"", SS_OWNERDRAW, 8, 0, 600, 22, ID_ASUM);
    g_alist = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL |
                   LVS_NOSORTHEADER, 8, 0, 600, 200, ID_ALIST);
    SendMessageW(g_alist, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SendMessageW(g_alist, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)make_dots());
    add_column(g_alist, 0, L"Titolo", 220, LVCFMT_LEFT);
    add_column(g_alist, 1, L"Piattaforma", 92, LVCFMT_LEFT);
    add_column(g_alist, 2, L"Anno", 44, LVCFMT_LEFT);
    add_column(g_alist, 3, L"Supporti", 132, LVCFMT_LEFT);
    add_column(g_alist, 4, L"Esito", 168, LVCFMT_LEFT);
    add_column(g_alist, 5, L"Archiviato", 76, LVCFMT_LEFT);
    g_adetail = make(L"STATIC", L"", SS_OWNERDRAW, 0, 0, 400, 140, ID_ADETAIL);
    make(L"BUTTON", L"Riprendi", WS_TABSTOP, 0, 0, 130, 26, ID_ARESUME);
    make(L"BUTTON", L"Apri cartella", WS_TABSTOP, 0, 0, 130, 26, ID_AOPEN);
    make(L"BUTTON", L"Ricontrolla impronte", WS_TABSTOP, 0, 0, 130, 26, ID_AVERIFY);
    make(L"STATIC", L"Doppio clic su un cofanetto aperto per riprenderlo dal supporto che manca.",
         SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 400, 18, ID_AHINT);
    make(L"BUTTON", L"Nuovo cofanetto", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 150, 28, ID_ANEW);
    SendDlgItemMessageW(g_wnd, ID_ARESUME, WM_SETFONT, (WPARAM)g_bold, TRUE);
    SendDlgItemMessageW(g_wnd, ID_ANEW, WM_SETFONT, (WPARAM)g_bold, TRUE);
}

/* the square magnifier button next to the Etichetta field: the magnifier
 * is the Webdings "L", which XP has too; the tooltip says what it does */
static void label_button(void)
{
    HWND b = make(L"BUTTON", L"L", WS_TABSTOP, 0, 0, 24, 21, ID_LABELREAD);
    HWND tip; TOOLINFOW ti;
    HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, SYMBOL_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Webdings");
    if (f) SendMessageW(b, WM_SETFONT, (WPARAM)f, TRUE);
    tip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0,
                          g_wnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!tip) return;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = TTTOOLINFOW_V2_SIZE;        /* works with common controls 5 too */
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_wnd;
    ti.uId = (UINT_PTR)b;
    ti.lpszText = (wchar_t *)L"Leggi l'etichetta dal disco inserito (F2)";
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

/* running session: banner, phases, bars, list, log, buttons */
static void build_work(void)
{
    g_banner = make(L"STATIC", L"", SS_OWNERDRAW, 8, 8, 600, 64, ID_BANNER);
    g_pills = make(L"STATIC", L"", SS_OWNERDRAW, 8, 80, 600, 24, ID_PILLS);
    make(L"STATIC", L"Etichetta", 0, 8, 0, 60, 18, ID_LABEL + LBL);
    g_label = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 70, 0, 200, 21, ID_LABEL);
    label_button();
    make(L"STATIC", L"Nota", 0, 280, 0, 40, 18, ID_NOTE + LBL);
    g_note = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 320, 0, 280, 21, ID_NOTE);
    build_list();
    make(L"BUTTON", L"Fine cofanetto", WS_TABSTOP, 0, 0, 110, 26, ID_FINISH);
    make(L"BUTTON", L"Salta", WS_TABSTOP, 0, 0, 90, 26, ID_SKIP);
    make(L"BUTTON", L"Leggi", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 90, 26, ID_READ);
    make(L"BUTTON", L"Interrompi", WS_TABSTOP, 0, 0, 100, 26, ID_STOP);
    make(L"BUTTON", L"Apri cartella", WS_TABSTOP, 0, 0, 110, 26, ID_OPEN);
    make(L"BUTTON", L"Nuovo cofanetto", WS_TABSTOP, 0, 0, 120, 26, ID_NEW);
    make(L"BUTTON", L"Archivio", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 100, 26, ID_TOARCH);

    make(L"STATIC", L"Disco", 0, 0, 0, 60, 18, ID_DISKBAR + LBL);
    g_diskbar = make(PROGRESS_CLASSW, L"", 0, 0, 0, 300, 16, ID_DISKBAR);
    SendMessageW(g_diskbar, PBM_SETRANGE32, 0, CYL_MAX);
    g_cyltxt = make(L"STATIC", L"", SS_LEFT, 0, 0, 120, 18, ID_CYLTXT);
    g_elapsed = make(L"STATIC", L"", SS_RIGHT, 0, 0, 52, 18, ID_ELAPSED);
    make(L"STATIC", L"Cofanetto", 0, 0, 0, 60, 18, ID_BOXBAR + LBL);
    g_boxbar = make(PROGRESS_CLASSW, L"", 0, 0, 0, 300, 16, ID_BOXBAR);
    g_boxtxt = make(L"STATIC", L"", SS_LEFT, 0, 0, 180, 18, ID_BOXTXT);

    g_log = make(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                 8, 300, 600, 150, ID_LOG);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_mono, TRUE);
    /* without this the EDIT stops at about 32K characters and rejects the rest */
    SendMessageW(g_log, EM_SETLIMITTEXT, LOG_LIMIT, 0);

    make(L"STATIC", L"Cartella", 0, 0, 0, 80, 18, ID_BAGDIR + LBL);
    make(L"STATIC", L"", SS_NOPREFIX | SS_PATHELLIPSIS, 0, 0, 400, 18, ID_BAGDIR);
    make(L"STATIC", L"Contenuto", 0, 0, 0, 80, 18, ID_BAGCONT + LBL);
    make(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 400, 18, ID_BAGCONT);
    make(L"STATIC", L"Impronte", 0, 0, 0, 80, 18, ID_BAGHASH + LBL);
    make(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 400, 18, ID_BAGHASH);
    make(L"STATIC", L"Messi da parte", 0, 0, 0, 80, 18, ID_BAGASIDE + LBL);
    make(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 400, 18, ID_BAGASIDE);

    g_sbar = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                             0, 0, 0, 0, g_wnd, (HMENU)(INT_PTR)ID_SBAR, GetModuleHandleW(NULL), NULL);
    SendMessageW(g_sbar, WM_SETFONT, (WPARAM)g_font, TRUE);
}

#define FSIZE(f) ((int)sizeof(((meta_box *)0)->f))

static void apply_limits(void)
{
    limit(ID_TITLE, FSIZE(title));         limit(ID_PLATFORM, FSIZE(platform));
    limit(ID_PUBLISHER, FSIZE(publisher)); limit(ID_YEAR, FSIZE(year));
    limit(ID_LANGUAGE, FSIZE(language));   limit(ID_EDITION, FSIZE(edition));
    limit(ID_NOTES, FSIZE(notes));         limit(ID_LABEL, (int)sizeof(g_label_u8));
    limit(ID_NOTE, (int)sizeof(g_note_u8));
    limit(ID_DIR, (int)sizeof(g_outdir));
    limit(ID_SAMDISK, 512);                limit(ID_SOURCE, 512);   /* start()'s buf */
    limit(ID_DIC, 512);                    limit(ID_DRIVE, 16);
    SendDlgItemMessageW(g_wnd, ID_COUNT, EM_LIMITTEXT, 2, 0);      /* at most 64 */
    SendDlgItemMessageW(g_wnd, ID_CD, EM_LIMITTEXT, 2, 0);
    SendDlgItemMessageW(g_wnd, ID_DVD, EM_LIMITTEXT, 2, 0);
}

/* ---- coloured banner at the top ---- */

enum { B_YELLOW, B_ORANGE, B_GREEN, B_RED, B_BLUE };

static const COLORREF ban_bg[] = {
    RGB(0xff, 0xf8, 0xc4), RGB(0xfd, 0xe7, 0xc4), RGB(0xdf, 0xf0, 0xdd), RGB(0xfb, 0xdf, 0xdc), RGB(0xde, 0xea, 0xfa)
};
static const COLORREF ban_edge[] = {
    RGB(0xe0, 0xc6, 0x4b), RGB(0xe3, 0xa9, 0x4b), RGB(0x8f, 0xc4, 0x8a), RGB(0xd8, 0x8c, 0x84), RGB(0x8f, 0xb2, 0xe0)
};
static const COLORREF ban_dot[] = {
    RGB(0xd9, 0xa4, 0x00), RGB(0xc9, 0x8a, 0x00), RGB(0x2e, 0x8b, 0x3a), RGB(0xc0, 0x39, 0x2b), RGB(0x31, 0x6a, 0xc5)
};

#define BAN_CIRCLE 40
#define BAN_TEXT_X (12 + BAN_CIRCLE + 14)

/* in the banner's circle: the symbol (a number, "!"), the check mark or a CD */
enum { ICON_SYM, ICON_CHECK, ICON_DISC };

static struct {
    int kind, icon;
    wchar_t sym[8], title[200], text[700];
} g_ban;

/* height needed by a banner w wide: the text wraps */
static int banner_height(int w)
{
    HDC dc = GetDC(g_wnd);
    HGDIOBJ o = SelectObject(dc, g_big);
    RECT r = { 0, 0, w - BAN_TEXT_X - 12, 0 };
    int th, sh = 0, h;
    DrawTextW(dc, g_ban.title, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    th = r.bottom;
    if (g_ban.text[0]) {
        SetRect(&r, 0, 0, w - BAN_TEXT_X - 12, 0);
        SelectObject(dc, g_font);
        DrawTextW(dc, g_ban.text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        sh = r.bottom + 4;
    }
    SelectObject(dc, o);
    ReleaseDC(g_wnd, dc);
    h = th + sh + 24;
    return h < BAN_CIRCLE + 24 ? BAN_CIRCLE + 24 : h;
}

static void layout(void);

/* immediate repaint: with SAMdisk output arriving in bursts the queued
 * messages jump ahead of WM_PAINT, and banner and phases lagged behind
 * the log (seen with the real SAMdisk) */
static void repaint(HWND w)
{
    RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void banner(int kind, const wchar_t *sym, int icon, const wchar_t *title, const wchar_t *text)
{
    RECT r; int before;
    GetClientRect(g_banner, &r);
    before = banner_height(r.right);
    g_ban.kind = kind; g_ban.icon = icon;
    wcsncpy(g_ban.sym, sym ? sym : L"", 7); g_ban.sym[7] = 0;
    wcsncpy(g_ban.title, title, 199); g_ban.title[199] = 0;
    wcsncpy(g_ban.text, text ? text : L"", 699); g_ban.text[699] = 0;
    if (banner_height(r.right) != before) layout();
    repaint(g_banner);
}

static void banner_num(int kind, int n, const wchar_t *title, const wchar_t *text)
{
    wchar_t num[8];
    _snwprintf(num, 8, L"%d", n); num[7] = 0;
    banner(kind, num, 0, title, text);
}

/* a filled circle or rectangle, with a border of its own colour */
static void shape(HDC dc, int ellipse, int x0, int y0, int x1, int y1, COLORREF fill, COLORREF edge)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    if (ellipse) Ellipse(dc, x0, y0, x1, y1); else Rectangle(dc, x0, y0, x1, y1);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
}

/* the media kind symbol in a square of side s, as in the prototype
 * (viewBox 34): the floppy blue, the disc silver */
static void paint_kind(HDC dc, int kind, int x, int y, int s)
{
#define P(v) ((v) * s / 34)
    if (kind == MEDIA_FLOPPY) {
        shape(dc, 0, x + P(5), y + P(4), x + P(29) + 1, y + P(30) + 1, RGB(0x2a, 0x63, 0xd8), RGB(0x0a, 0x27, 0x66));
        shape(dc, 0, x + P(10), y + P(4), x + P(23), y + P(12), RGB(0xd5, 0xdb, 0xe4), RGB(0xd5, 0xdb, 0xe4));
        shape(dc, 0, x + P(9), y + P(17), x + P(25), y + P(27), RGB(255, 255, 255), RGB(255, 255, 255));
        return;
    }
    shape(dc, 1, x + P(2), y + P(2), x + P(32), y + P(32), RGB(0xd9, 0xdd, 0xe6), RGB(0x6b, 0x76, 0x85));
    if (s >= 24) {
        HPEN pen = CreatePen(PS_SOLID, P(3), RGB(0xa8, 0xd0, 0xff));
        HGDIOBJ op = SelectObject(dc, pen);
        shape(dc, 1, x + P(6), y + P(6), x + P(28), y + P(28), RGB(0xee, 0xf2, 0xfb), RGB(0xee, 0xf2, 0xfb));
        Arc(dc, x + P(5), y + P(5), x + P(29), y + P(29), x + P(16), y + P(5), x + P(5), y + P(14));
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    shape(dc, 1, x + P(13), y + P(13), x + P(21), y + P(21), RGB(255, 255, 255), RGB(0x6b, 0x76, 0x85));
#undef P
}

static void draw_circle(HDC dc, int x, int y)
{
    RECT r = { x, y, x + BAN_CIRCLE, y + BAN_CIRCLE };
    HPEN pen; HGDIOBJ op;
    if (g_ban.icon == ICON_DISC) { paint_kind(dc, MEDIA_CD, x, y, BAN_CIRCLE); return; }
    shape(dc, 1, r.left, r.top, r.right, r.bottom, ban_dot[g_ban.kind], ban_dot[g_ban.kind]);
    if (g_ban.icon == ICON_CHECK) {     /* the check mark is drawn: Tahoma on XP lacks the glyph */
        POINT p[3] = { { x + 11, y + 21 }, { x + 17, y + 27 }, { x + 29, y + 14 } };
        pen = CreatePen(PS_SOLID, 4, RGB(255, 255, 255));
        op = SelectObject(dc, pen);
        Polyline(dc, p, 3);
        SelectObject(dc, op);
        DeleteObject(pen);
        return;
    }
    SelectObject(dc, g_num);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, g_ban.sym, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void paint_banner(HDC dc, int w, int h)
{
    HBRUSH br = CreateSolidBrush(ban_bg[g_ban.kind]);
    HPEN pen = CreatePen(PS_SOLID, 1, ban_edge[g_ban.kind]);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    RECT t = { BAN_TEXT_X, 0, w - 12, 0 }, s;
    int th, total;
    RoundRect(dc, 0, 0, w, h, 8, 8);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
    draw_circle(dc, 12, (h - BAN_CIRCLE) / 2);

    /* title and text, vertically centred */
    SelectObject(dc, g_big);
    DrawTextW(dc, g_ban.title, -1, &t, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    th = t.bottom;
    s = t; s.right = w - 12; s.bottom = 0;
    SelectObject(dc, g_font);
    if (g_ban.text[0]) DrawTextW(dc, g_ban.text, -1, &s, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    total = th + (g_ban.text[0] ? 4 + s.bottom : 0);
    SetRect(&t, BAN_TEXT_X, (h - total) / 2, w - 12, (h - total) / 2 + th);
    SetTextColor(dc, RGB(0x20, 0x20, 0x20));
    SelectObject(dc, g_big);
    DrawTextW(dc, g_ban.title, -1, &t, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    if (g_ban.text[0]) {
        SetRect(&s, BAN_TEXT_X, t.bottom + 4, w - 12, h);
        SetTextColor(dc, RGB(0x33, 0x33, 0x33));
        SelectObject(dc, g_font);
        DrawTextW(dc, g_ban.text, -1, &s, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    }
}

/* ---- phases of the current disk: "pills" ---- */

#define MAXPILL 16
static wchar_t g_pill[MAXPILL][24];
static int     g_npill, g_pcur = -1;     /* g_pcur: the running phase */

static void pill_add(const wchar_t *t)
{
    if (g_npill >= MAXPILL) return;
    wcsncpy(g_pill[g_npill], t, 23); g_pill[g_npill][23] = 0;
    g_npill++;
}

static int last_read(void)
{
    int i, k = 0;
    for (i = 0; i <= g_pcur && i < g_npill; i++)
        if (!wcsncmp(g_pill[i], L"Lettura ", 8) && _wtoi(g_pill[i] + 8) > k) k = _wtoi(g_pill[i] + 8);
    return k;
}

/* the phases expected after the running one: corrected as they arrive */
static int cur_optical(void);
static int cur_cd(void);

/* CDs and DVDs: TOC, Lettura 1, Controllo C2 (CD only), Redump; if redump
 * does not know it, Lettura 2, Controllo C2, Confronto */
static void pills_future_optical(void)
{
    const wchar_t *p = g_pill[g_pcur];
    int reads = last_read();
    g_npill = g_pcur + 1;
    if (!wcscmp(p, L"TOC")) {
        pill_add(L"Lettura 1");
        if (cur_cd()) pill_add(L"Controllo C2");
        pill_add(L"Redump");
    } else if (!wcsncmp(p, L"Lettura ", 8)) {
        if (cur_cd()) pill_add(L"Controllo C2");
        pill_add(_wtoi(p + 8) == 1 ? L"Redump" : L"Confronto");
    } else if (!wcscmp(p, L"Controllo C2")) {
        pill_add(reads <= 1 ? L"Redump" : L"Confronto");
    }
    pill_add(L"Esito");
}

static void pills_future(void)
{
    const wchar_t *p = g_pill[g_pcur];
    if (cur_optical()) { pills_future_optical(); return; }
    g_npill = g_pcur + 1;
    if (!wcscmp(p, L"Scansione")) {
        pill_add(L"Lettura 1");
        if (!g_fragile) { pill_add(L"Lettura 2"); pill_add(L"Confronto"); }
    } else if (!wcsncmp(p, L"Lettura ", 8) && !g_fragile) {
        if (_wtoi(p + 8) == 1) pill_add(L"Lettura 2");
        pill_add(L"Confronto");
    }
    pill_add(L"Esito");
}

static void pills_phase(const wchar_t *name)
{
    if (!wcscmp(name, L"Scansione") || !wcscmp(name, L"TOC") || g_pcur < 0) g_npill = g_pcur = 0;
    else if (g_pcur < MAXPILL - 1) g_pcur++;
    wcsncpy(g_pill[g_pcur], name, 23); g_pill[g_pcur][23] = 0;
    pills_future();
    repaint(g_pills);
}

/* "Le letture non coincidono": after the comparison, one more read */
static void pills_reread(void)
{
    wchar_t w[24];
    if (g_pcur < 0) return;
    g_npill = g_pcur + 1;
    _snwprintf(w, 24, L"Lettura %d", last_read() + 1); w[23] = 0;
    pill_add(w);
    if (cur_cd()) pill_add(L"Controllo C2");
    pill_add(L"Confronto");
    pill_add(L"Esito");
    repaint(g_pills);
}

static void pills_done(void)
{
    if (g_pcur < 0) return;
    if (g_pcur < MAXPILL - 1) g_pcur++;
    wcscpy(g_pill[g_pcur], L"Esito");
    g_npill = g_pcur + 1;
    repaint(g_pills);
}

static void pills_clear(void)
{
    g_npill = 0; g_pcur = -1;
    repaint(g_pills);
}

static int pill_width(HDC dc, int i)
{
    SIZE sz;
    SelectObject(dc, i == g_pcur ? g_bold : g_font);
    GetTextExtentPoint32W(dc, g_pill[i], (int)wcslen(g_pill[i]), &sz);
    return sz.cx + 22;
}

static void paint_pill(HDC dc, int i, int x, int w, int h)
{
    COLORREF bg, edge, fg;
    HBRUSH br; HPEN pen; HGDIOBJ op, ob;
    RECT r = { x, 0, x + w, h };
    if (i < g_pcur)       { bg = RGB(0xdf, 0xf0, 0xdd); edge = RGB(0x8f, 0xc4, 0x8a); fg = RGB(0x1e, 0x5e, 0x26); }
    else if (i == g_pcur) { bg = edge = RGB(0x31, 0x6a, 0xc5); fg = RGB(255, 255, 255); }
    else                  { bg = RGB(0xf0, 0xf0, 0xf0); edge = RGB(0xc8, 0xc8, 0xc8); fg = RGB(0x80, 0x80, 0x80); }
    br = CreateSolidBrush(bg); pen = CreatePen(PS_SOLID, 1, edge);
    op = SelectObject(dc, pen); ob = SelectObject(dc, br);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, h, h);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
    SelectObject(dc, i == g_pcur ? g_bold : g_font);
    SetTextColor(dc, fg);
    DrawTextW(dc, g_pill[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void paint_pills(HDC dc, int w, int h)
{
    int i, x = 0, first = 0, total = 0;
    for (i = 0; i < g_npill; i++) total += pill_width(dc, i) + 6;
    /* too many phases for the width: leave out the first ones already done */
    while (total > w && first < g_pcur) total -= pill_width(dc, first++) + 6;
    for (i = first; i < g_npill; i++) {
        int pw = pill_width(dc, i);
        paint_pill(dc, i, x, pw, h);
        x += pw + 6;
    }
}

static void paint_summary(HDC dc, int w, int h);
static void paint_detail(HDC dc, int w, int h);

/* draw in memory and then all at once: no flicker */
static void draw_item(const DRAWITEMSTRUCT *di)
{
    int w = di->rcItem.right - di->rcItem.left, h = di->rcItem.bottom - di->rcItem.top;
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bmp), of = SelectObject(dc, g_font);
    RECT r = { 0, 0, w, h };
    FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    if (di->CtlID == ID_BANNER) paint_banner(dc, w, h);
    else if (di->CtlID == ID_ASUM) paint_summary(dc, w, h);
    else if (di->CtlID == ID_ADETAIL) paint_detail(dc, w, h);
    else if (di->CtlID == ID_ICFLOP) paint_kind(dc, MEDIA_FLOPPY, 0, 0, w);
    else if (di->CtlID == ID_ICCD) paint_kind(dc, MEDIA_CD, 0, 0, w);
    else if (di->CtlID == ID_ICDVD) paint_kind(dc, MEDIA_DVD, 0, 0, w);
    else paint_pills(dc, w, h);
    BitBlt(di->hDC, di->rcItem.left, di->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, of);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---- group boxes ----
 * They are not BS_GROUPBOX controls: with WS_CLIPCHILDREN the parent does
 * not paint beneath them and, made transparent, on XP the parent painted
 * over them and they vanished. Frame and title are drawn in the window's
 * WM_PAINT, which paints only outside the controls: there they stay put. */

enum { G_BOX, G_ARCH, G_BAG, G_SEL, G_COUNT };

static const wchar_t *group_title[G_COUNT] = {
    L"Cofanetto", L"Archiviazione", L"Pacchetto BagIt", L"Il cofanetto scelto"
};
static RECT g_group[G_COUNT];

/* uxtheme is loaded by hand: with the classic theme, or if it is missing,
 * drawing uses DrawEdge. Constants from vssym32.h */
#define RA_BP_GROUPBOX   4
#define RA_GBS_NORMAL    1
#define RA_TMT_TEXTCOLOR 3803

typedef HANDLE  (WINAPI *open_theme_fn)(HWND, LPCWSTR);
typedef HRESULT (WINAPI *close_theme_fn)(HANDLE);
typedef HRESULT (WINAPI *draw_theme_bg_fn)(HANDLE, HDC, int, int, const RECT *, const RECT *);
typedef HRESULT (WINAPI *theme_color_fn)(HANDLE, int, int, int, COLORREF *);

static struct {
    open_theme_fn open; close_theme_fn close; draw_theme_bg_fn draw; theme_color_fn color;
} g_ux;

static void load_uxtheme(void)
{
    HMODULE m = LoadLibraryW(L"uxtheme.dll");
    if (!m) return;
    g_ux.open = (open_theme_fn)(void (*)(void))GetProcAddress(m, "OpenThemeData");
    g_ux.close = (close_theme_fn)(void (*)(void))GetProcAddress(m, "CloseThemeData");
    g_ux.draw = (draw_theme_bg_fn)(void (*)(void))GetProcAddress(m, "DrawThemeBackground");
    g_ux.color = (theme_color_fn)(void (*)(void))GetProcAddress(m, "GetThemeColor");
    if (!g_ux.open || !g_ux.close || !g_ux.draw) memset(&g_ux, 0, sizeof(g_ux));
}

static int group_visible(int i)
{
    if (i == G_BAG) return g_view == V_END && g_bag_closed;
    if (i == G_SEL) return g_view == V_ARCHIVE && !g_arc_empty;
    return g_view == V_SETUP;
}

/* the layout moves the boxes; if they change, repaint where they were and where they go */
static void set_group(int i, int x, int y, int w, int h)
{
    RECT r;
    SetRect(&r, x, y, x + w, y + h);
    if (EqualRect(&r, &g_group[i])) return;
    InvalidateRect(g_wnd, &g_group[i], TRUE);
    g_group[i] = r;
    InvalidateRect(g_wnd, &r, TRUE);
}

/* the frame starts at half the title's height, and the title breaks it */
static void paint_group(HDC dc, HANDLE theme, int i)
{
    const wchar_t *t = group_title[i];
    RECT fr = g_group[i], tr;
    SIZE sz;
    COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
    SelectObject(dc, g_font);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    fr.top += sz.cy / 2;
    SetRect(&tr, fr.left + 7, g_group[i].top, fr.left + 7 + sz.cx + 4, g_group[i].top + sz.cy);
    SaveDC(dc);
    ExcludeClipRect(dc, tr.left, tr.top, tr.right, tr.bottom);
    if (theme) g_ux.draw(theme, dc, RA_BP_GROUPBOX, RA_GBS_NORMAL, &fr, NULL);
    else DrawEdge(dc, &fr, EDGE_ETCHED, BF_RECT);
    RestoreDC(dc, -1);
    if (theme && (!g_ux.color || g_ux.color(theme, RA_BP_GROUPBOX, RA_GBS_NORMAL, RA_TMT_TEXTCOLOR, &fg) != S_OK))
        fg = RGB(0x00, 0x46, 0xd5);
    SetTextColor(dc, fg);
    SetBkMode(dc, TRANSPARENT);
    tr.left += 2;
    DrawTextW(dc, t, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
}

/* ---- empty archive: white panel with a drawing, two lines and the button ----
 * The window paints this too: the Nuovo cofanetto button sits on top of
 * it as a normal control, with no overlapping children */

#define EMPTY_ICON 96
#define EMPTY_TEXT_W 420

static struct { int icon_y, title_y, text_y, text_h, btn_y, hint_y; } g_emp;

static void empty_text(wchar_t *out, int n)
{
    wchar_t dir[512];
    to_w(g_arc_root, dir, 512);
    if (g_arc_missing)
        _snwprintf(out, n, L"La cartella %ls non esiste ancora: la creo con il primo cofanetto. "
                           L"Compili titolo, piattaforma e supporti, poi inserisci i dischetti "
                           L"uno alla volta.", dir);
    else
        _snwprintf(out, n, L"In %ls non c'è ancora nessun cofanetto. Comincia dal primo: compili titolo, "
                           L"piattaforma e supporti, poi inserisci i dischetti uno alla volta.", dir);
    out[n - 1] = 0;
}

static const wchar_t EMPTY_TITLE[] = L"L'archivio è ancora vuoto";
static const wchar_t EMPTY_HINT[] = L"Hai già un archivio altrove? Scegli la sua cartella con il pulsante «...» qui sopra.";

/* the positions inside g_empty, vertically centred: used by layout and painting */
static void empty_metrics(void)
{
    wchar_t t[900];
    HDC dc = GetDC(g_wnd);
    HGDIOBJ o = SelectObject(dc, g_font);
    RECT r = { 0, 0, EMPTY_TEXT_W, 0 };
    int total, y;
    empty_text(t, 900);
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, o);
    ReleaseDC(g_wnd, dc);
    g_emp.text_h = r.bottom;
    total = EMPTY_ICON + 14 + 22 + 14 + r.bottom + 16 + 30 + 14 + 16;
    y = g_empty.top + (g_empty.bottom - g_empty.top - total) / 2;
    if (y < g_empty.top + 8) y = g_empty.top + 8;
    g_emp.icon_y = y;
    g_emp.title_y = y += EMPTY_ICON + 14;
    g_emp.text_y = y += 22 + 14;
    g_emp.btn_y = y += r.bottom + 16;
    g_emp.hint_y = y + 30 + 14;
}

/* the mock-up's empty tray, at 2 points per unit of its 48x48 viewBox */
static void paint_tray(HDC dc, int x, int y)
{
    POINT top[4];
    HBRUSH br; HPEN pen; HGDIOBJ op, ob;
#define S(v) ((int)((v) * 2 + 0.5))
    br = CreateSolidBrush(RGB(0xd9, 0xd9, 0xd9)); pen = CreatePen(PS_NULL, 0, 0);
    op = SelectObject(dc, pen); ob = SelectObject(dc, br);
    Ellipse(dc, x + S(4), y + S(42.4), x + S(46), y + S(47.6));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br); DeleteObject(pen);

    top[0].x = x + S(3.5); top[0].y = y + S(30.5); top[1].x = x + S(8);    top[1].y = y + S(26);
    top[2].x = x + S(40);  top[2].y = y + S(26);   top[3].x = x + S(44.5); top[3].y = y + S(30.5);
    br = CreateSolidBrush(RGB(0xee, 0xf1, 0xf5)); pen = CreatePen(PS_SOLID, 2, RGB(0x4d, 0x58, 0x66));
    op = SelectObject(dc, pen); ob = SelectObject(dc, br);
    Polygon(dc, top, 4);
    SelectObject(dc, ob); DeleteObject(br);
    br = CreateSolidBrush(RGB(0xb9, 0xc2, 0xcf)); ob = SelectObject(dc, br);
    RoundRect(dc, x + S(3.5), y + S(30.5), x + S(44.5), y + S(43.5), 6, 6);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br); DeleteObject(pen);

    pen = CreatePen(PS_SOLID, 2, RGB(0x5a, 0x65, 0x74)); op = SelectObject(dc, pen);
    MoveToEx(dc, x + S(8.5), y + S(26.8), NULL); LineTo(dc, x + S(39.5), y + S(26.8));
    SelectObject(dc, op); DeleteObject(pen);

    br = CreateSolidBrush(RGB(0xfd, 0xfc, 0xf6)); pen = CreatePen(PS_SOLID, 1, RGB(0x6b, 0x76, 0x85));
    op = SelectObject(dc, pen); ob = SelectObject(dc, br);
    Rectangle(dc, x + S(17), y + S(33), x + S(31), y + S(37.5));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br); DeleteObject(pen);

    br = CreateSolidBrush(RGB(0x5a, 0x65, 0x74)); pen = CreatePen(PS_NULL, 0, 0);
    op = SelectObject(dc, pen); ob = SelectObject(dc, br);
    RoundRect(dc, x + S(17.5), y + S(39.5), x + S(30.5), y + S(42.5), 6, 6);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br); DeleteObject(pen);
#undef S
}

static void paint_empty(HDC dc)
{
    wchar_t t[900];
    int cx = (g_empty.left + g_empty.right) / 2;
    RECT r = g_empty;
    HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x7f, 0x9d, 0xb9));
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br); DeleteObject(pen);
    paint_tray(dc, cx - EMPTY_ICON / 2, g_emp.icon_y);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    SelectObject(dc, g_big);
    SetRect(&r, g_empty.left + 8, g_emp.title_y, g_empty.right - 8, g_emp.title_y + 22);
    DrawTextW(dc, EMPTY_TITLE, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(0x33, 0x33, 0x33));
    empty_text(t, 900);
    SetRect(&r, cx - EMPTY_TEXT_W / 2, g_emp.text_y, cx + EMPTY_TEXT_W / 2, g_emp.text_y + g_emp.text_h);
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SetTextColor(dc, RGB(0x44, 0x44, 0x44));
    SetRect(&r, g_empty.left + 8, g_emp.hint_y, g_empty.right - 8, g_emp.hint_y + 16);
    DrawTextW(dc, EMPTY_HINT, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void paint_groups(void)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g_wnd, &ps);
    HANDLE theme = g_ux.open ? g_ux.open(g_wnd, L"BUTTON") : NULL;
    HGDIOBJ of = SelectObject(dc, g_font);
    int i;
    if (g_view == V_ARCHIVE && g_arc_empty && !IsRectEmpty(&g_empty)) paint_empty(dc);
    for (i = 0; i < G_COUNT; i++)
        if (group_visible(i) && !IsRectEmpty(&g_group[i])) paint_group(dc, theme, i);
    SelectObject(dc, of);
    if (theme) g_ux.close(theme);
    EndPaint(g_wnd, &ps);
}

/* ---- control layout ---- */

static void put(int id, int x, int y, int w, int h)
{
    HWND c = GetDlgItem(g_wnd, id);
    if (c) MoveWindow(c, x, y, w > 0 ? w : 0, h > 0 ? h : 0, TRUE);
}

/* setup: the long fields widen, the buttons go to the bottom */
static void layout_setup(int W, int H)
{
    int R = W - 16;                     /* right edge inside the group boxes */
    set_group(G_BOX, 6, 4, W - 12, 222);
    put(ID_TITLE, 125, 24, R - 125 - 95 - 8, 21);
    put(ID_RESUME, R - 95, 23, 95, 23);
    put(ID_LANGUAGE, 410, 82, R - 410, 21);
    put(ID_NOTES, 125, 138, R - 125, 21);
    put(ID_HINT, 12, 198, R - 12, 18);
    set_group(G_ARCH, 6, 234, W - 12, 163);
    put(ID_DIR, 125, 254, R - 125 - 36, 21);
    put(ID_BROWSE, R - 30, 254, 30, 21);
    /* SAMdisk and DIC: the path widens, "Leggi da" stays on the right */
    put(ID_SAMDISK, 125, 310, R - 125 - 120 - 73, 21);
    put(ID_SOURCE + LBL, R - 120 - 65, 313, 60, 18);
    put(ID_SOURCE, R - 120, 310, 120, 21);
    put(ID_DIC, 125, 338, R - 125 - 120 - 73, 21);
    put(ID_DRIVE + LBL, R - 120 - 65, 341, 60, 18);
    put(ID_DRIVE, R - 120, 338, 120, 21);
    put(ID_BAGPATH, 12, 369, R - 12, 18);
    /* right below the group boxes: with a tall window the space stays at the bottom */
    (void)H;
    put(ID_SOUNDS, 12, 411, 200, 20);
    put(ID_BACK, W - 6 - 100 - 8 - 100, 407, 100, 28);
    put(ID_START, W - 6 - 100, 407, 100, 28);
}

/* ---- the archive ---- */

#define SEL_H 172                       /* height of the "Il cofanetto scelto" box */
#define ARC_FIXED_COLS (92 + 44 + 132 + 168 + 76)

static int pick_open(void)
{
    return g_pick >= 0 && g_pick < g_arc.n && g_arc.box[g_pick].state == ARC_OPEN;
}

static void set_empty_rect(int x, int y, int w, int h)
{
    RECT r;
    SetRect(&r, x, y, x + (w > 0 ? w : 0), y + (h > 0 ? h : 0));
    if (EqualRect(&r, &g_empty)) return;
    InvalidateRect(g_wnd, &g_empty, TRUE);
    g_empty = r;
    InvalidateRect(g_wnd, &r, TRUE);
}

static void layout_archive(int W, int H)
{
    int by = H - 8 - 28, y = 10, x, gy, bx, k, right = W - 8;
    if (g_arc_empty) {                  /* only the folder, then the white panel */
        put(ID_ADIR + LBL, 8, y + 3, 52, 18);
        put(ID_ADIR, 62, y, right - 36 - 62, 21);
        put(ID_ABROWSE, right - 30, y, 30, 21);
        set_empty_rect(8, y + 31, W - 16, H - 8 - (y + 31));
        empty_metrics();
        put(ID_ANEW, W / 2 - 85, g_emp.btn_y, 170, 30);
        set_group(G_SEL, 0, 0, 0, 0);
        return;
    }
    set_empty_rect(0, 0, 0, 0);
    /* folder | ... | Cerca | platform: only the folder widens */
    put(ID_APLAT, right - 150, y, 150, 240);
    put(ID_ASEARCH, right - 306, y, 150, 21);
    x = right - 306 - 4 - 34;
    put(ID_ASEARCH + LBL, x, y + 3, 34, 18);
    put(ID_ABROWSE, x - 12 - 30, y, 30, 21);
    put(ID_ADIR + LBL, 8, y + 3, 52, 18);
    put(ID_ADIR, 62, y, x - 12 - 30 - 6 - 62, 21);
    y += 29;
    put(ID_ASUM, 8, y, W - 16, 22);
    y += 30;
    gy = by - 8 - SEL_H;
    put(ID_ALIST, 8, y, W - 16, gy - 8 - y);
    set_group(G_SEL, 8, gy, W - 16, SEL_H);
    bx = W - 8 - 10 - 130;
    put(ID_ADETAIL, 18, gy + 20, bx - 14 - 18, SEL_H - 28);
    k = gy + 20;
    if (pick_open()) { put(ID_ARESUME, bx, k, 130, 26); k += 32; }
    put(ID_AOPEN, bx, k, 130, 26);
    put(ID_AVERIFY, bx, k + 32, 130, 26);
    put(ID_AHINT, 8, by + 6, W - 16 - 150 - 12, 18);
    put(ID_ANEW, W - 8 - 150, by, 150, 28);
}

static void archive_columns(void)
{
    RECT r; int w;
    GetClientRect(g_alist, &r);
    w = r.right - ARC_FIXED_COLS;
    SendMessageW(g_alist, LVM_SETCOLUMNWIDTH, 0, w < 120 ? 120 : w);
}

static void list_columns(void)
{
    RECT r; int w;
    GetClientRect(g_list, &r);
    w = r.right - 130 - 60 - 190;
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 1, w < 80 ? 80 : w);
}

static void layout_wait(int W, int y, int by)
{
    int lw = (W - 16 - 62 - 42) * 2 / 5, sx = g_turn.readable ? 186 : 90;
    put(ID_LABEL + LBL, 8, y + 3, 60, 18);
    put(ID_LABEL, 70, y, lw - 28, 21);
    put(ID_LABELREAD, 70 + lw - 24, y, 24, 21);
    put(ID_NOTE + LBL, 70 + lw + 10, y + 3, 32, 18);
    put(ID_NOTE, 70 + lw + 42, y, W - 8 - (70 + lw + 42), 21);
    y += 31;
    put(ID_LIST, 8, y, W - 16, by - 8 - y);
    /* a CD that cannot be read yet: no Leggi, Salta in its place */
    put(ID_READ, W - 8 - 90, by, 90, 26);
    put(ID_SKIP, W - 8 - sx, by, 90, 26);
    put(ID_FINISH, W - 8 - sx - 116, by, 110, 26);
}

static void layout_work(int W, int y, int by)
{
    int bw = W - 8 - 72 - 188;
    put(ID_PILLS, 8, y, W - 16, 24);
    y += 34;
    put(ID_DISKBAR + LBL, 8, y + 1, 62, 18);
    put(ID_DISKBAR, 72, y, bw, 16);
    put(ID_CYLTXT, W - 8 - 180, y + 1, 124, 18);
    put(ID_ELAPSED, W - 8 - 52, y + 1, 52, 18);
    y += 24;
    put(ID_BOXBAR + LBL, 8, y + 1, 62, 18);
    put(ID_BOXBAR, 72, y, bw, 16);
    put(ID_BOXTXT, W - 8 - 180, y + 1, 180, 18);
    y += 28;
    put(ID_LOG, 8, y, W - 16, by - 8 - y);
    put(ID_STOP, W - 8 - 100, by, 100, 26);
}

static void layout_end(int W, int y, int by)
{
    int rest, lh, i, nrow = g_aside_n ? 4 : 3;
    static const int rows[4] = { ID_BAGDIR, ID_BAGCONT, ID_BAGHASH, ID_BAGASIDE };
    if (g_bag_closed) {
        set_group(G_BAG, 8, y, W - 16, 22 + nrow * 22);
        for (i = 0; i < nrow; i++) {
            put(rows[i] + LBL, 20, y + 20 + i * 22, 84, 18);
            put(rows[i], 108, y + 20 + i * 22, W - 16 - 108, 18);
        }
        y += 30 + nrow * 22;
    }
    rest = by - 8 - y;
    lh = rest * 45 / 100;
    if (lh < 70) lh = 70;
    put(ID_LIST, 8, y, W - 16, lh);
    put(ID_LOG, 8, y + lh + 6, W - 16, rest - lh - 6);
    put(ID_TOARCH, W - 8 - 100, by, 100, 26);
    put(ID_NEW, W - 8 - 226, by, 120, 26);
    put(ID_OPEN, W - 8 - 342, by, 110, 26);
}

static void sbar_parts(void);

static void layout(void)
{
    RECT rc, sr; int W, H, bh, by;
    if (!g_sbar || IsIconic(g_wnd)) return;
    SendMessageW(g_sbar, WM_SIZE, 0, 0);
    GetClientRect(g_wnd, &rc);
    GetWindowRect(g_sbar, &sr);
    W = rc.right;
    H = rc.bottom - (sr.bottom - sr.top);
    layout_setup(W, H);
    layout_archive(W, H);
    archive_columns();
    bh = banner_height(W - 16);
    put(ID_BANNER, 8, 8, W - 16, bh);
    by = H - 8 - 26;
    if (g_view == V_WAIT) layout_wait(W, 8 + bh + 10, by);
    else if (g_view == V_WORK) layout_work(W, 8 + bh + 8, by);
    else if (g_view == V_END) layout_end(W, 8 + bh + 10, by);
    list_columns();
    sbar_parts();
    /* a resized EDIT keeps the first line visible: scroll back to the bottom */
    SendMessageW(g_log, WM_VSCROLL, SB_BOTTOM, 0);
}

/* ---- RetroArchiver.ini: the defaults next to the executable, the user's
 * choices in Application Data (settings.c) ---- */

static settings g_set;

static const struct { const wchar_t *key; int id; } ini_keys[] = {
    { L"Cartella", ID_DIR }, { L"LettureMax", ID_MAXREAD },
    { L"SAMdisk", ID_SAMDISK }, { L"LeggiDa", ID_SOURCE }, { L"Piattaforma", ID_PLATFORM },
    { L"DiscImageCreator", ID_DIC }, { L"LettoreCD", ID_DRIVE },
    { L"Lingua", ID_LANGUAGE },
    { NULL, 0 }
};

/* the file naming patterns, per kind: FLOPPY01.dsk, CD01\, DVD01\ */
static const wchar_t *const name_keys[MEDIA_KINDS] = { L"NomeFloppy", L"NomeCD", L"NomeDVD" };
static const wchar_t *const name_defaults[MEDIA_KINDS] = { L"FLOPPY{nn}", L"CD{nn}", L"DVD{nn}" };

/* the hard-coded values (the fields' initial texts): the user gets them
 * if there is no ini, and a choice equal to the default is not saved */
#define INI_KEYS 8
static wchar_t g_builtin[INI_KEYS][512];
typedef char ini_keys_count[sizeof(ini_keys) / sizeof(ini_keys[0]) == INI_KEYS + 1 ? 1 : -1];

/* the older ini said C:\samdisk and C:\DIC: if nothing is there any more,
 * the field takes the one in tools\ next to the exe, and the status bar
 * shows that one's version */
static void tool_fallback(int id, const wchar_t *rel)
{
    wchar_t v[512], alt[MAX_PATH];
    GetDlgItemTextW(g_wnd, id, v, 512);
    if (set_tool_fallback(v, rel, alt, MAX_PATH)) SetDlgItemTextW(g_wnd, id, alt);
}

static void ini_load(void)
{
    wchar_t cur[512], v[512];
    int i;
    set_init(&g_set);
    for (i = 0; ini_keys[i].key; i++) {
        GetDlgItemTextW(g_wnd, ini_keys[i].id, cur, 512);   /* the default stays the current one */
        wcscpy(g_builtin[i], cur);
        set_get(&g_set, L"Archiviazione", ini_keys[i].key, cur, v, 512);
        SetDlgItemTextW(g_wnd, ini_keys[i].id, v);
    }
    tool_fallback(ID_SAMDISK, SET_TOOL_SAMDISK);
    tool_fallback(ID_DIC, SET_TOOL_DIC);
    /* the checkbox is not a text: Suoni=1 or 0 */
    CheckDlgButton(g_wnd, ID_SOUNDS,
                   set_int(&g_set, L"Archiviazione", L"Suoni", 0) ? BST_CHECKED : BST_UNCHECKED);
    /* the file names are not in the window: they change only here */
    for (i = 0; i < MEDIA_KINDS; i++) {
        set_get(&g_set, L"Archiviazione", name_keys[i], name_defaults[i], v, 512);
        to_u8(v[0] ? v : name_defaults[i], g_ini_names[i], sizeof(g_ini_names[i]));
    }
}

/* a window field into the user's ini, if different from the default */
static void ini_store(int i)
{
    wchar_t v[512];
    GetDlgItemTextW(g_wnd, ini_keys[i].id, v, 512);
    set_save(&g_set, L"Archiviazione", ini_keys[i].key, g_builtin[i], v);
}

static void ini_save(void)
{
    wchar_t v[512];
    int i;
    for (i = 0; ini_keys[i].key; i++) ini_store(i);
    set_save(&g_set, L"Archiviazione", L"Suoni", L"0",
             IsDlgButtonChecked(g_wnd, ID_SOUNDS) == BST_CHECKED ? L"1" : L"0");
    /* the names change only by hand in the ini: one that differs from the default stays */
    for (i = 0; i < MEDIA_KINDS; i++) {
        to_w(g_ini_names[i], v, 512);
        set_save(&g_set, L"Archiviazione", name_keys[i], name_defaults[i], v);
    }
    /* the old key: names are now per kind */
    set_put(&g_set, L"Archiviazione", L"NomeFile", NULL);
}

/* window position and size, in work-area coordinates as
 * GetWindowPlacement gives them */
static void place_save(void)
{
    WINDOWPLACEMENT wp;
    RECT *r = &wp.rcNormalPosition;
    wp.length = sizeof(wp);
    if (!g_set.user[0] || !GetWindowPlacement(g_wnd, &wp)) return;
    set_put_int(&g_set, L"Finestra", L"X", r->left);
    set_put_int(&g_set, L"Finestra", L"Y", r->top);
    set_put_int(&g_set, L"Finestra", L"Larghezza", r->right - r->left);
    set_put_int(&g_set, L"Finestra", L"Altezza", r->bottom - r->top);
    set_put_int(&g_set, L"Finestra", L"Massimizzata", wp.showCmd == SW_SHOWMAXIMIZED ||
                (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED)));
}

/* the saved position applies only if the window fits entirely on one screen
 * (a monitor removed, a lower resolution): otherwise the default one */
static int place_load(int show_cmd)
{
    WINDOWPLACEMENT wp; MONITORINFO mi; HMONITOR mon; RECT r, wa;
    int x = set_int(&g_set, L"Finestra", L"X", -32000);
    int y = set_int(&g_set, L"Finestra", L"Y", -32000);
    int w = set_int(&g_set, L"Finestra", L"Larghezza", 0);
    int h = set_int(&g_set, L"Finestra", L"Altezza", 0);
    RECT minr = { 0, 0, MIN_CW, MIN_CH };
    if (x == -32000 || y == -32000 || w <= 0 || h <= 0) return 0;
    AdjustWindowRectEx(&minr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT);
    if (w < minr.right - minr.left || h < minr.bottom - minr.top) return 0;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetRect(&r, x + wa.left, y + wa.top, x + wa.left + w, y + wa.top + h);   /* in screen coordinates */
    if (!(mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL))) return 0;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return 0;
    if (r.left < mi.rcWork.left || r.top < mi.rcWork.top ||
        r.right > mi.rcWork.right || r.bottom > mi.rcWork.bottom) return 0;
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    wp.showCmd = set_int(&g_set, L"Finestra", L"Massimizzata", 0) ? SW_SHOWMAXIMIZED : show_cmd;
    SetRect(&wp.rcNormalPosition, x, y, x + w, y + h);
    return SetWindowPlacement(g_wnd, &wp) != 0;
}

/* ---- SAMdisk check during setup ----
 * Without arguments SAMdisk 3.x prints "SAMdisk 3.8.12 (date), (c) ..."
 * and exits. Runs in a thread with its own process and pipe: runner is
 * global and its cancellation would also hit the session. */

enum { SD_CHECKING, SD_OK, SD_NOTFOUND, SD_V4, SD_NOTSAM };

typedef struct { LONG gen; wchar_t cmd[600]; } sd_job;
typedef struct { int state; wchar_t ver[32]; } sd_result;

static volatile LONG g_sd_gen;
static int     g_sd_state = SD_CHECKING;
static wchar_t g_sd_ver[32];

/* the program's output, at most n-1 bytes; 0 if it does not start. code
 * may be NULL: DiscImageCreator without the Visual C++ runtime starts but
 * exits at once with STATUS_DLL_NOT_FOUND */
static int run_capture(wchar_t *cmd, char *out, int n, DWORD *code)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    HANDLE rd, wr, nul;
    DWORD got; int len = 0, ok;
    out[0] = 0;
    if (!CreatePipe(&rd, &wr, &sa, 65536)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    /* how CreateProcess finds "samdisk": exe folder, current, system, PATH */
    ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return 0; }
    if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    while (len < n - 1 && ReadFile(rd, out + len, n - 1 - len, &got, NULL) && got) len += (int)got;
    out[len] = 0;
    if (code && !GetExitCodeProcess(pi.hProcess, code)) *code = 0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
    return 1;
}

static DWORD WINAPI samdisk_worker(LPVOID p)
{
    sd_job *job = (sd_job *)p;
    sd_result *r = (sd_result *)calloc(1, sizeof(sd_result));
    char out[4096], ver[32]; int series;
    if (r) {
        if (!run_capture(job->cmd, out, sizeof(out), NULL)) r->state = SD_NOTFOUND;
        else if (!(series = floppy_samdisk_series(out, ver, sizeof(ver)))) r->state = SD_NOTSAM;
        else {
            r->state = series == 4 ? SD_V4 : SD_OK;
            to_w(ver, r->ver, 32);
        }
        if (!PostMessageW(g_wnd, WM_APP_SAMDISK, (WPARAM)job->gen, (LPARAM)r)) free(r);
    }
    free(job);
    return 0;
}

static void sbar_setup(void);
static void sbar_update(void);

/* a new check makes those still running obsolete */
static void samdisk_pending(void)
{
    InterlockedIncrement(&g_sd_gen);
    g_sd_state = SD_CHECKING;
    if (g_view == V_SETUP || g_view == V_ARCHIVE) sbar_update();
}

static void samdisk_check(void)
{
    sd_job *job = (sd_job *)calloc(1, sizeof(sd_job));
    wchar_t v[512];
    HANDLE th;
    KillTimer(g_wnd, TIMER_SAMDISK);
    samdisk_pending();
    if (!job) return;
    GetDlgItemTextW(g_wnd, ID_SAMDISK, v, 512);
    if (!v[0]) wcscpy(v, L"samdisk");      /* like start(): empty field = "samdisk" */
    _snwprintf(job->cmd, 600, L"\"%ls\"", v); job->cmd[599] = 0;
    job->gen = g_sd_gen;
    th = CreateThread(NULL, 0, samdisk_worker, job, 0, NULL);
    if (th) CloseHandle(th);
    else free(job);
}

static void on_samdisk(WPARAM gen, sd_result *r)
{
    if ((LONG)gen == g_sd_gen) {        /* the field has not changed in the meantime */
        g_sd_state = r->state;
        wcscpy(g_sd_ver, r->ver);
        if (g_view == V_SETUP || g_view == V_ARCHIVE) sbar_update();
    }
    free(r);
}

static void samdisk_text(wchar_t *out, int n)
{
    switch (g_sd_state) {
    case SD_CHECKING: _snwprintf(out, n, L"Controllo SAMdisk..."); break;
    case SD_OK:       _snwprintf(out, n, L"SAMdisk %ls", g_sd_ver); break;
    case SD_NOTFOUND: _snwprintf(out, n, L"SAMdisk non trovato: controlla il campo SAMdisk"); break;
    case SD_V4:       _snwprintf(out, n, L"SAMdisk 4.x: serve la serie 3.x (la 4.0 non legge i floppy veri)"); break;
    default:          _snwprintf(out, n, L"Il programma indicato non sembra SAMdisk"); break;
    }
    out[n - 1] = 0;
}

/* ---- floppy drive check during setup ----
 * "samdisk rpm a:" measures the rotation and answers in a couple of
 * seconds; with no disk, or no controller, it errors out or waits. Runs
 * in a thread with runner_capture, which does not touch the session's
 * cancellation (runner_run is global), and stops after FD_TIMEOUT ms.
 * Only if the box has floppies and "Leggi da" is a drive, not a .dsk file.
 * A check waits for the previous one to finish, and the session waits for
 * the last one: two SAMdisk at once on the same drive would interfere. */

#define FD_TIMEOUT 6000

enum { FD_NONE, FD_CHECKING, FD_OK, FD_DEAD };

typedef struct { LONG gen; HANDLE prev; char samdisk[512], source[512]; } fd_job;
typedef struct { int state, rpm; } fd_result;

static volatile LONG g_fd_gen;
static int     g_fd_state = FD_NONE, g_fd_rpm;
static wchar_t g_fd_drive[4] = L"A:";
static HANDLE  g_fd_thread;             /* the last check started */

static int setup_counts(int c[MEDIA_KINDS]);

static DWORD WINAPI floppy_worker(LPVOID p)
{
    fd_job *job = (fd_job *)p;
    fd_result *r = (fd_result *)calloc(1, sizeof(fd_result));
    const char *argv[4];
    char out[2048]; int code = 0;
    if (job->prev) { WaitForSingleObject(job->prev, INFINITE); CloseHandle(job->prev); }
    argv[0] = job->samdisk; argv[1] = "rpm"; argv[2] = job->source; argv[3] = NULL;
    if (r) {
        /* even if stopped by the timeout: if it already printed the rpm, the drive answers */
        r->rpm = runner_capture(argv, out, sizeof(out), FD_TIMEOUT, &code) == RUNNER_NOSTART
               ? -1 : floppy_parse_rpm(out);
        r->state = r->rpm > 0 ? FD_OK : FD_DEAD;
        if (!PostMessageW(g_wnd, WM_APP_FLOPPY, (WPARAM)job->gen, (LPARAM)r)) free(r);
    }
    free(job);
    return 0;
}

static void floppy_pending(void)
{
    InterlockedIncrement(&g_fd_gen);
    if (g_fd_state != FD_NONE) g_fd_state = FD_CHECKING;
    if (g_view == V_SETUP) sbar_update();
}

static void floppy_check(void)
{
    fd_job *job; char src[512]; int c[MEDIA_KINDS]; HANDLE th;
    KillTimer(g_wnd, TIMER_FLOPPY);
    InterlockedIncrement(&g_fd_gen);
    get_u8_trim(ID_SOURCE, src, sizeof(src));
    if (!src[0]) str_copy(src, sizeof(src), "a:");
    setup_counts(c);
    g_fd_state = FD_NONE;
    if (g_running || g_view != V_SETUP || c[MEDIA_FLOPPY] < 1 || !floppy_source_is_drive(src)
        || !(job = (fd_job *)calloc(1, sizeof(fd_job)))) {
        if (g_view == V_SETUP) sbar_update();
        return;
    }
    {   /* " a:\" -> "A:" for the texts */
        const char *q = src;
        while (*q == ' ') q++;
        g_fd_drive[0] = (wchar_t)toupper((unsigned char)*q); g_fd_drive[1] = L':'; g_fd_drive[2] = 0;
    }
    get_u8_trim(ID_SAMDISK, job->samdisk, sizeof(job->samdisk));
    if (!job->samdisk[0]) str_copy(job->samdisk, sizeof(job->samdisk), "samdisk");
    str_copy(job->source, sizeof(job->source), src);
    job->gen = g_fd_gen;
    if (g_fd_thread && !DuplicateHandle(GetCurrentProcess(), g_fd_thread, GetCurrentProcess(),
                                        &job->prev, SYNCHRONIZE, FALSE, 0))
        job->prev = NULL;
    th = CreateThread(NULL, 0, floppy_worker, job, 0, NULL);
    if (!th) { if (job->prev) CloseHandle(job->prev); free(job); sbar_update(); return; }
    if (g_fd_thread) CloseHandle(g_fd_thread);
    g_fd_thread = th;
    g_fd_state = FD_CHECKING;
    sbar_update();
}

static void on_floppy(WPARAM gen, fd_result *r)
{
    if ((LONG)gen == g_fd_gen) {
        g_fd_state = r->state;
        g_fd_rpm = r->rpm;
        if (g_view == V_SETUP) sbar_update();
    }
    free(r);
}

/* "Lettore A: 300 rpm"; empty if there is nothing to say */
static void floppy_text(wchar_t *out, int n)
{
    out[0] = 0;
    switch (g_fd_state) {
    case FD_CHECKING: _snwprintf(out, n, L"Controllo il lettore %ls...", g_fd_drive); break;
    case FD_OK:       _snwprintf(out, n, L"Lettore %ls %d rpm", g_fd_drive, g_fd_rpm); break;
    case FD_DEAD:     _snwprintf(out, n, L"Lettore %ls non risponde (dischetto inserito?)", g_fd_drive); break;
    }
    out[n - 1] = 0;
}

/* ---- label of the inserted disk, before the read ----
 * The magnifier button next to the Etichetta field asks Windows for it
 * (GetVolumeInformation on the drive); with "Leggi da" on a .dsk file it
 * reads it from the image's FAT12. For CDs and DVDs it starts by itself
 * when Windows says a disc has arrived. Not the floppy: the drive gives
 * no notice, and polling it blindly spins the motor. */

enum { LB_FOUND, LB_NONE, LB_NODISK, LB_UNKNOWN };

typedef struct { LONG gen; wchar_t root[8]; char file[512]; } lb_job;
typedef struct { int state; wchar_t label[64]; wchar_t root[8]; } lb_result;

static volatile LONG g_lb_gen;
static wchar_t g_lb_msg[200];          /* the outcome, in the status bar while waiting for the disk */

static void sbar_text(int part, const wchar_t *t);
static void focus_to(HWND h);

static DWORD WINAPI label_worker(LPVOID p)
{
    lb_job *job = (lb_job *)p;
    lb_result *r = (lb_result *)calloc(1, sizeof(lb_result));
    char u8[128];
    if (r) {
        wcscpy(r->root, job->root);
        if (job->file[0]) {
            r->state = floppy_media.volume_label(job->file, u8, sizeof(u8)) ? LB_FOUND : LB_NONE;
            if (r->state == LB_FOUND) to_w(u8, r->label, 64);
        } else {
            /* with no disk Windows must not open "Disco non presente" */
            UINT old = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
            DWORD e;
            if (GetVolumeInformationW(job->root, r->label, 64, NULL, NULL, NULL, NULL, 0))
                r->state = r->label[0] ? LB_FOUND : LB_NONE;
            else {
                e = GetLastError();
                r->state = e == ERROR_NOT_READY || e == ERROR_NO_MEDIA_IN_DRIVE ? LB_NODISK : LB_UNKNOWN;
            }
            SetErrorMode(old);
        }
        if (!PostMessageW(g_wnd, WM_APP_LABEL, (WPARAM)job->gen, (LPARAM)r)) free(r);
    }
    free(job);
    return 0;
}

static void label_read(void)
{
    lb_job *job; char src[512]; HANDLE th; const wchar_t *d;
    if (!g_waiting || !(job = (lb_job *)calloc(1, sizeof(lb_job)))) return;
    job->gen = InterlockedIncrement(&g_lb_gen);
    if (g_turn.kind == MEDIA_FLOPPY) {
        get_u8_trim(ID_SOURCE, src, sizeof(src));
        if (!src[0]) str_copy(src, sizeof(src), "a:");
        if (floppy_source_is_drive(src)) {
            while (*src == ' ') memmove(src, src + 1, strlen(src));
            _snwprintf(job->root, 8, L"%c:\\", (wchar_t)toupper((unsigned char)src[0]));
        } else {
            str_copy(job->file, sizeof(job->file), src);
            wcscpy(job->root, L"A:\\");
        }
    } else {
        d = g_drive_w[0] ? g_drive_w : L"D:";
        _snwprintf(job->root, 8, L"%c:\\", d[0]);
    }
    job->root[7] = 0;
    wcscpy(g_lb_msg, L"Leggo l'etichetta del disco...");
    sbar_text(0, g_lb_msg);
    th = CreateThread(NULL, 0, label_worker, job, 0, NULL);
    if (th) CloseHandle(th); else free(job);
}

static void on_label(WPARAM gen, lb_result *r)
{
    wchar_t *w = g_lb_msg, u[3];
    u[0] = r->root[0]; u[1] = L':'; u[2] = 0;
    if ((LONG)gen == g_lb_gen && g_waiting) {
        switch (r->state) {
        case LB_FOUND:
            SetWindowTextW(g_label, r->label);
            focus_to(g_label);
            SendMessageW(g_label, EM_SETSEL, 0, -1);
            _snwprintf(w, 200, L"Etichetta letta dal disco: %ls", r->label);
            break;
        case LB_NONE:    _snwprintf(w, 200, L"Il disco non ha un'etichetta di volume"); break;
        case LB_NODISK:  _snwprintf(w, 200, L"Nessun disco nel lettore %ls", u); break;
        default:         _snwprintf(w, 200, L"Windows non riconosce il disco: l'etichetta si prende "
                                            L"dall'immagine dopo la lettura"); break;
        }
        w[199] = 0;
        sbar_text(0, w);
    }
    free(r);
}

/* a CD or DVD just inserted in the awaited drive: label read by itself,
 * if the field is still empty */
static void on_media_arrival(const DEV_BROADCAST_HDR *h)
{
    const DEV_BROADCAST_VOLUME *v = (const DEV_BROADCAST_VOLUME *)h;
    int bit;
    if (!h || h->dbch_devicetype != DBT_DEVTYP_VOLUME || !(v->dbcv_flags & DBTF_MEDIA)) return;
    if (!g_waiting || g_turn.kind == MEDIA_FLOPPY || GetWindowTextLengthW(g_label) > 0) return;
    bit = toupper(g_drive_w[0] ? g_drive_w[0] : L'D') - 'A';
    if (bit >= 0 && bit < 26 && (v->dbcv_unitmask & (1UL << bit))) label_read();
}

/* ---- DiscImageCreator check during setup ----
 * With /v it prints "AppVersion" and below it "32 bit, AnsiBuild,
 * 20260101T233129". The same thread looks for the redump DAT next to the
 * program. */

enum { DIC_CHECKING, DIC_OK, DIC_NOTFOUND, DIC_NODLL, DIC_NOTDIC };

typedef struct { LONG gen; wchar_t cmd[600]; } dic_job;
typedef struct { int state; wchar_t ver[32], dat[40]; int has_dat; } dic_result;

static volatile LONG g_dic_gen;
static int     g_dic_state = DIC_CHECKING, g_has_dat;
static wchar_t g_dic_ver[32], g_dat_date[40];

/* the line after "AppVersion": the last word, "20260101T233129" */
static int parse_dic_version(const char *s, char *ver, int n)
{
    const char *p = strstr(s, "AppVersion"), *e, *w;
    int k = 0;
    if (!p || !(p = strchr(p, '\n'))) return 0;
    p++;
    for (e = p; *e && *e != '\r' && *e != '\n'; e++) ;
    for (w = e; w > p && w[-1] != ' ' && w[-1] != '\t' && w[-1] != ','; w--) ;
    while (w < e && k < n - 1) ver[k++] = *w++;
    ver[k] = 0;
    return k > 0;
}

static DWORD WINAPI dic_worker(LPVOID p)
{
    dic_job *job = (dic_job *)p;
    dic_result *r = (dic_result *)calloc(1, sizeof(dic_result));
    char out[8192], ver[32], date[40]; DWORD code = 0;
    if (r) {
        if (!run_capture(job->cmd, out, sizeof(out), &code)) r->state = DIC_NOTFOUND;
        else if (code == 0xC0000135 || code == 0xC0000139 || code == 0xC0000142) r->state = DIC_NODLL;
        else if (!parse_dic_version(out, ver, sizeof(ver))) r->state = DIC_NOTDIC;
        else {
            r->state = DIC_OK;
            ver[8] = 0;                             /* the build date is enough */
            to_w(ver, r->ver, 32);
        }
        r->has_dat = optical_find_dat(NULL, 0, date, sizeof(date));
        to_w(date, r->dat, 40);
        if (!PostMessageW(g_wnd, WM_APP_DIC, (WPARAM)job->gen, (LPARAM)r)) free(r);
    }
    free(job);
    return 0;
}

static void dic_pending(void)
{
    InterlockedIncrement(&g_dic_gen);
    g_dic_state = DIC_CHECKING;
    if (g_view == V_SETUP || g_view == V_ARCHIVE) sbar_update();
}

static void dic_check(void)
{
    dic_job *job = (dic_job *)calloc(1, sizeof(dic_job));
    wchar_t v[512];
    HANDLE th;
    KillTimer(g_wnd, TIMER_DIC);
    dic_pending();
    if (!job) return;
    GetDlgItemTextW(g_wnd, ID_DIC, v, 512);
    if (!v[0]) wcscpy(v, L"DiscImageCreator");
    _snwprintf(job->cmd, 600, L"\"%ls\" /v", v); job->cmd[599] = 0;
    job->gen = g_dic_gen;
    th = CreateThread(NULL, 0, dic_worker, job, 0, NULL);
    if (th) CloseHandle(th);
    else free(job);
}

static void on_dic(WPARAM gen, dic_result *r)
{
    if ((LONG)gen == g_dic_gen) {
        g_dic_state = r->state;
        wcscpy(g_dic_ver, r->ver);
        g_has_dat = r->has_dat;
        wcscpy(g_dat_date, r->dat);
        if (g_view == V_SETUP || g_view == V_ARCHIVE) sbar_update();
    }
    free(r);
}

/* a redump DAT older than six months: discs added later are missing */
static int dat_old(void)
{
    SYSTEMTIME now; int y, m, d, age;
    if (swscanf(g_dat_date, L"%d-%d-%d", &y, &m, &d) != 3) return 0;
    GetLocalTime(&now);
    age = (now.wYear - y) * 12 + (now.wMonth - m);
    return age > 6;
}

/* brief: for the status bar, which has little room */
static void dic_text(wchar_t *out, int n, int brief)
{
    wchar_t dat[120];
    switch (g_dic_state) {
    case DIC_CHECKING: _snwprintf(out, n, L"Controllo DiscImageCreator..."); out[n - 1] = 0; return;
    case DIC_NOTFOUND:
        _snwprintf(out, n, brief ? L"DiscImageCreator non trovato" : L"DiscImageCreator non trovato: controlla il campo");
        out[n - 1] = 0;
        return;
    case DIC_NODLL:
        _snwprintf(out, n, brief ? L"DiscImageCreator: mancano i runtime Visual C++"
                                 : L"DiscImageCreator non parte: mancano i runtime di Visual C++ 2015-2019 x86");
        out[n - 1] = 0;
        return;
    case DIC_NOTDIC:
        _snwprintf(out, n, brief ? L"non è DiscImageCreator" : L"Il programma indicato non sembra DiscImageCreator");
        out[n - 1] = 0;
        return;
    }
    if (!g_has_dat) _snwprintf(dat, 120, brief ? L"senza DAT redump" : L"nessun DAT di redump: due letture per disco");
    else if (dat_old()) _snwprintf(dat, 120, brief ? L"DAT redump %.10ls, vecchio"
                                                   : L"DAT di redump del %.10ls, vecchio: scaricane uno nuovo", g_dat_date);
    else _snwprintf(dat, 120, brief ? L"DAT redump %.10ls" : L"DAT di redump del %.10ls", g_dat_date[0] ? g_dat_date : L"?");
    dat[119] = 0;
    _snwprintf(out, n, L"DiscImageCreator %ls, %ls", g_dic_ver, dat);
    out[n - 1] = 0;
}

/* the programs needed: SAMdisk for floppies, DIC for CDs and DVDs */
static void tools_text(wchar_t *out, int n)
{
    wchar_t a[200], b[200], f[100];
    samdisk_text(a, 200);
    dic_text(b, 200, 1);
    /* the drive only when SAMdisk is fine: otherwise that is the reason */
    if (g_view == V_SETUP && g_sd_state == SD_OK) floppy_text(f, 100); else f[0] = 0;
    _snwprintf(out, n, L"%ls%ls%ls | %ls", a, f[0] ? L" | " : L"", f, b);
    out[n - 1] = 0;
}

/* the first thing missing before starting, or "Pronto" */
static const wchar_t *READY_TEXTS[] = {
    L"Manca il titolo", L"Manca la piattaforma", L"Supporti: da 1 a 64", L"Manca la cartella",
    L"Controllo SAMdisk...", L"SAMdisk da sistemare", L"Pronto",
    L"Leggo l'archivio...", L"Ricontrollo le impronte...",      /* archive view only */
    L"Controllo DiscImageCreator...", L"DiscImageCreator da sistemare", NULL
};
#define READY_ARC_LOADING 7
#define READY_ARC_VERIFY  8
#define READY_DIC_CHECK   9
#define READY_DIC_BAD     10

/* the make-up written in the three fields; an empty field counts as 0.
 * -1 if invalid: no media, or more than META_MAXDISK */
static int setup_counts(int c[MEDIA_KINDS])
{
    static const int ids[MEDIA_KINDS] = { ID_COUNT, ID_CD, ID_DVD };
    int k, sum = 0;
    for (k = 0; k < MEDIA_KINDS; k++) sum += c[k] = (int)GetDlgItemInt(g_wnd, ids[k], NULL, FALSE);
    return sum < 1 || sum > META_MAXDISK ? -1 : 0;
}

static const wchar_t *ready_text(void)
{
    char s[256]; int c[MEDIA_KINDS];
    get_u8_trim(ID_TITLE, s, sizeof(s));
    if (!s[0]) return READY_TEXTS[0];
    get_u8_trim(ID_PLATFORM, s, sizeof(s));
    if (!s[0]) return READY_TEXTS[1];
    if (setup_counts(c)) return READY_TEXTS[2];
    get_u8_trim(ID_DIR, s, sizeof(s));
    if (!s[0]) return READY_TEXTS[3];
    /* SAMdisk is needed only with floppies, DiscImageCreator with CDs or DVDs */
    if (c[MEDIA_FLOPPY] && g_sd_state == SD_CHECKING) return READY_TEXTS[4];
    if (c[MEDIA_FLOPPY] && g_sd_state != SD_OK) return READY_TEXTS[5];
    if ((c[MEDIA_CD] || c[MEDIA_DVD]) && g_dic_state == DIC_CHECKING) return READY_TEXTS[READY_DIC_CHECK];
    if ((c[MEDIA_CD] || c[MEDIA_DVD]) && g_dic_state != DIC_OK) return READY_TEXTS[READY_DIC_BAD];
    return READY_TEXTS[6];
}

/* the right part is as wide as the longest text it can hold */
static int ready_width(void)
{
    HDC dc = GetDC(g_sbar);
    HGDIOBJ o = SelectObject(dc, (HFONT)SendMessageW(g_sbar, WM_GETFONT, 0, 0));
    SIZE sz; int i, w = 0;
    for (i = 0; READY_TEXTS[i]; i++) {
        GetTextExtentPoint32W(dc, READY_TEXTS[i], (int)wcslen(READY_TEXTS[i]), &sz);
        if (sz.cx > w) w = sz.cx;
    }
    SelectObject(dc, o);
    ReleaseDC(g_sbar, dc);
    return w + 16;                                      /* the part's margins */
}

/* ---- status bar ---- */

static int is_done(const char *st)
{
    return meta_status_done(st);
}

/* the last outcome of each disk, like dskrun's summary; opt (may be NULL)
 * counts per kind the CDs and DVDs still to be read */
static void counts(int *pul, int *mar, int *err, int *alt, int *opt)
{
    int n, kind, num;
    *pul = *mar = *err = *alt = 0;
    if (opt) memset(opt, 0, MEDIA_KINDS * sizeof(int));
    for (n = 1; n <= g_total && n <= META_MAXDISK; n++) {
        const char *s = g_disk[n].status;
        slot(n, &kind, &num);
        char c = meta_status_class(s);
        if (c == 'p') (*pul)++;
        else if (c == 'm' || c == 'f') (*mar)++;    /* read once: worth a look */
        else if (c == 'e') (*err)++;
        else if (opt && kind != MEDIA_FLOPPY && !s[0] && !g_cfg.media[kind]) opt[kind]++;
        else (*alt)++;          /* skipped, failed, interrupted, never asked */
    }
}

static void sbar_parts(void)
{
    RECT r; int p[4];
    GetClientRect(g_wnd, &r);
    if (g_view == V_SETUP) {
        /* left | make-up | "Pronto" right-aligned | empty space for the
         * size grip, which would otherwise cover the end of the text */
        int grip = GetSystemMetrics(SM_CXVSCROLL) + 4;
        p[2] = r.right - grip; p[1] = p[2] - ready_width(); p[0] = p[1] - 170; p[3] = -1;
        SendMessageW(g_sbar, SB_SETPARTS, 4, (LPARAM)p);
    } else if (g_view == V_ARCHIVE) {
        /* SAMdisk | "N cofanetti letti da ..." | Pronto | grip */
        int grip = GetSystemMetrics(SM_CXVSCROLL) + 4;
        p[2] = r.right - grip; p[1] = p[2] - ready_width(); p[0] = p[1] / 2; p[3] = -1;
        SendMessageW(g_sbar, SB_SETPARTS, 4, (LPARAM)p);
    } else if (g_view == V_WAIT) {
        /* while waiting the first part may show the magnifier's outcome: it stretches */
        int first = r.right - 170 - 110 - 130;
        p[0] = first > 110 ? first : 110; p[1] = p[0] + 170; p[2] = p[1] + 110; p[3] = -1;
        SendMessageW(g_sbar, SB_SETPARTS, 4, (LPARAM)p);
    } else {
        /* the disk count is the part that stretches: it takes what is left */
        int rest = r.right - 110 - 110 - 130;
        p[0] = 110; p[1] = 110 + (rest > 220 ? rest : 220); p[2] = p[1] + 110; p[3] = -1;
        SendMessageW(g_sbar, SB_SETPARTS, 4, (LPARAM)p);
    }
}

static void sbar_text(int part, const wchar_t *t)
{
    SendMessageW(g_sbar, SB_SETTEXTW, part, (LPARAM)t);
}

static void sbar_setup(void)
{
    wchar_t w[420]; int c[MEDIA_KINDS];
    tools_text(w, 420);
    sbar_text(0, w);
    if (setup_counts(c)) w[0] = 0; else comp_text(c, w, 200);
    sbar_text(1, w);
    /* two tabs: the text is right-aligned within its part */
    _snwprintf(w, 200, L"\t\t%ls", ready_text()); w[199] = 0;
    sbar_text(2, w);
    SendMessageW(g_sbar, SB_SETTEXTW, 3 | SBT_NOBORDERS, (LPARAM)L"");
}

static void sbar_archive(void)
{
    wchar_t w[700], dir[512], n[64];
    const wchar_t *ready = READY_TEXTS[6];
    tools_text(w, 420);
    sbar_text(0, w);
    to_w(g_arc_root, dir, 512);
    if (!g_arc_loaded) { _snwprintf(w, 700, L"Leggo %ls", dir); ready = READY_TEXTS[READY_ARC_LOADING]; }
    else if (g_arc_missing) _snwprintf(w, 700, L"La cartella %ls non esiste ancora", dir);
    else if (!g_arc.n) _snwprintf(w, 700, L"Nessun cofanetto in %ls", dir);
    else {
        plural(n, 64, g_arc.n, L"cofanetto letto", L"cofanetti letti");
        _snwprintf(w, 700, L"%ls da %ls", n, dir);
    }
    w[699] = 0;
    sbar_text(1, w);
    if (g_verifying) ready = READY_TEXTS[READY_ARC_VERIFY];
    _snwprintf(w, 200, L"\t\t%ls", ready); w[199] = 0;
    sbar_text(2, w);
    SendMessageW(g_sbar, SB_SETTEXTW, 3 | SBT_NOBORDERS, (LPARAM)L"");
}

static void sbar_update(void)
{
    wchar_t w[200], a[64], b[64];
    int pul, mar, err, alt, kind, num;
    if (g_view == V_SETUP) { sbar_setup(); return; }
    if (g_view == V_ARCHIVE) { sbar_archive(); return; }
    counts(&pul, &mar, &err, &alt, NULL);
    if (g_view == V_WAIT && g_lb_msg[0]) wcscpy(w, g_lb_msg);
    else if (g_view == V_END || g_cur < 1) comp_text(g_count, w, 200);
    else {
        slot(g_cur, &kind, &num);
        _snwprintf(w, 200, L"%ls %d di %d", kind_cap[kind], num, g_count[kind]); w[199] = 0;
    }
    sbar_text(0, w);
    plural(w, 200, pul + mar + err, L"archiviato", L"archiviati");
    if (mar) { plural(a, 64, mar, L"marginale", L"marginali"); wcscat(w, L", "); wcscat(w, a); }
    if (err) { _snwprintf(b, 64, L", %d con errori", err); b[63] = 0; wcscat(w, b); }
    sbar_text(1, w);
    sbar_text(2, g_bag_closed ? L"Bag: chiuso" : L"Bag: aperto");
    fmt_time(GetTickCount() - g_t_sess, a, 64);
    _snwprintf(w, 200, L"Sessione %ls", a); w[199] = 0;
    sbar_text(3, w);
}

/* ---- views ---- */

static const int in_wait[] = { ID_BANNER, ID_LABEL, ID_LABEL + LBL, ID_LABELREAD, ID_NOTE, ID_NOTE + LBL, ID_LIST,
                               ID_READ, ID_SKIP, ID_FINISH, 0 };
static const int in_work[] = { ID_BANNER, ID_PILLS, ID_DISKBAR, ID_DISKBAR + LBL, ID_CYLTXT, ID_ELAPSED,
                               ID_BOXBAR, ID_BOXBAR + LBL, ID_BOXTXT, ID_LOG, ID_STOP, 0 };
static const int in_end[]  = { ID_BANNER, ID_LIST, ID_LOG, ID_NEW, ID_OPEN, ID_TOARCH, 0 };
static const int in_empty[] = { ID_ADIR, ID_ADIR + LBL, ID_ABROWSE, ID_ANEW, 0 };
static const int in_bag[]  = { ID_BAGDIR, ID_BAGDIR + LBL, ID_BAGCONT, ID_BAGCONT + LBL,
                               ID_BAGHASH, ID_BAGHASH + LBL, 0 };

static int listed(const int *ids, int id)
{
    for (; *ids; ids++) if (*ids == id) return 1;
    return 0;
}

static int visible_in(int v, int id)
{
    if ((id >= ID_TITLE && id <= ID_START) || (id >= ID_TITLE + LBL && id <= ID_START + LBL))
        return v == V_SETUP;
    if (id == ID_SBAR) return 1;
    if ((id >= ID_ADIR && id <= ID_ANEW) || (id >= ID_ADIR + LBL && id <= ID_ANEW + LBL)) {
        if (v != V_ARCHIVE) return 0;
        if (g_arc_empty) return listed(in_empty, id);
        if (id == ID_ARESUME) return pick_open();
        if (id == ID_AOPEN || id == ID_AVERIFY) return g_pick >= 0;
        return 1;
    }
    if (v == V_WAIT) return listed(in_wait, id) && (id != ID_READ || g_turn.readable);
    if (v == V_WORK) return listed(in_work, id);
    if (v == V_END && (id == ID_BAGASIDE || id == ID_BAGASIDE + LBL)) return g_bag_closed && g_aside_n;
    if (v == V_END) return listed(in_end, id) || (g_bag_closed && listed(in_bag, id));
    return 0;
}

/* ---- focus ----
 * The window is not a dialog: DefWindowProc, when reactivating it (e.g.
 * after a MessageBox), puts the focus on the window itself and not on the
 * previous control, and keystrokes reach no field. The focus is saved when
 * the window deactivates and restored when it becomes active again. */

static HWND g_focus;                    /* the control to give the focus back to */

static HWND first_tab(void)
{
    return GetNextDlgTabItem(g_wnd, NULL, FALSE);
}

static int can_focus(HWND c)
{
    return c && IsWindow(c) && IsChild(g_wnd, c) && IsWindowVisible(c) && IsWindowEnabled(c);
}

static void focus_save(void)
{
    HWND f = GetFocus();
    if (f && IsChild(g_wnd, f)) g_focus = f;
}

static void focus_restore(void)
{
    HWND c = can_focus(g_focus) ? g_focus : first_tab();
    if (c) SetFocus(c);
}

/* focus on c at once, and again when the current message is done: Enter on
 * a button goes through IsDialogMessage, which after the command may put
 * the focus back on the button, hidden meanwhile by the view change */
static void focus_to(HWND c)
{
    if (!c) return;
    SetFocus(c);
    g_focus = c;
    PostMessageW(g_wnd, WM_APP_FOCUS, 0, (LPARAM)c);
}

static void view(int v)
{
    HWND c, f;
    g_view = v;
    SetWindowTextW(g_wnd, v == V_ARCHIVE ? L"RetroArchiver " RA_VERSION L" - archivio" : L"RetroArchiver " RA_VERSION L" - archiviazione");
    layout();                   /* move first, then show */
    for (c = GetWindow(g_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        ShowWindow(c, visible_in(v, GetDlgCtrlID(c)) ? SW_SHOW : SW_HIDE);
    /* a hidden control keeps the focus: keystrokes would be lost */
    f = GetFocus();
    if (f && IsChild(g_wnd, f) && !IsWindowVisible(f) && (f = first_tab()) != NULL) SetFocus(f);
    InvalidateRect(g_wnd, NULL, TRUE);     /* the group boxes change with the view */
    sbar_update();
}

/* ---- disk list ---- */

/* the master's file name, or the one the medium will get if not read yet */
static void disk_name(int n, wchar_t *out, int len)
{
    meta_box *b; const char *names[MEDIA_KINDS]; char base[96]; int kind, num, k;
    if (g_disk[n].master[0]) { to_w(g_disk[n].master, out, len); return; }
    out[0] = 0;
    if (!(b = (meta_box *)calloc(1, sizeof(meta_box)))) return;    /* title and names only */
    str_copy(b->title, sizeof(b->title), g_title);
    for (k = 0; k < MEDIA_KINDS; k++) names[k] = g_names[k];
    slot(n, &kind, &num);
    session_disk_base(b, names, kind, num, base, sizeof(base));
    free(b);
    to_w(base, out, len);
}

static int outcome(const disk_info *d, wchar_t *out, int len)
{
    const char *s = d->status;
    int dot = DOT_GREY;
    if (!s[0]) wcsncpy(out, L"da leggere", len);
    else if (!strcmp(s, "pulito")) { wcsncpy(out, L"pulito", len); dot = DOT_GREEN; }
    else if (!strcmp(s, "marginale")) {
        if (d->agree >= 2) _snwprintf(out, len, L"marginale (%d concordano)", d->agree);
        else wcsncpy(out, L"marginale", len);
        dot = DOT_OCHRE;
    }
    else if (!strcmp(s, "fragile")) { wcsncpy(out, L"fragile: una sola lettura", len); dot = DOT_OCHRE; }
    else if (!strcmp(s, "errore")) { wcsncpy(out, L"errore", len); dot = DOT_RED; }
    else if (!strcmp(s, "verificato redump")) { wcsncpy(out, L"verificato con redump", len); dot = DOT_GREEN; }
    else if (!strcmp(s, "letto una volta")) { wcsncpy(out, L"letto una volta: non verificato", len); dot = DOT_OCHRE; }
    else if (!strcmp(s, "instabile")) {
        if (d->agree >= 2) _snwprintf(out, len, L"instabile (%d letture uguali)", d->agree);
        else wcsncpy(out, L"instabile", len);
        dot = DOT_OCHRE;
    }
    else if (!strcmp(s, "errori")) { wcsncpy(out, L"errori non risolti", len); dot = DOT_RED; }
    else if (!strcmp(s, "lettura fallita")) { wcsncpy(out, L"lettura fallita", len); dot = DOT_RED; }
    else if (!strcmp(s, "mancante")) wcsncpy(out, L"saltato", len);
    else to_w(s, out, len);
    out[len - 1] = 0;
    return dot;
}

static void list_text(int row, int col, const wchar_t *t)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.iSubItem = col;
    it.pszText = (wchar_t *)t;
    SendMessageW(g_list, LVM_SETITEMTEXTW, row, (LPARAM)&it);
}

static void list_row(int n)
{
    LVITEMW it; wchar_t w[200];
    if (n < 1 || n > g_total || n > META_MAXDISK) return;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_IMAGE;
    it.iItem = n - 1;
    disk_name(n, w, 200);
    it.pszText = w;
    it.iImage = outcome(&g_disk[n], w + 100, 100);
    SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
    list_text(n - 1, 3, w + 100);
    to_w(g_disk[n].label, w, 200);
    list_text(n - 1, 1, w);
    if (g_disk[n].nreads > 0) _snwprintf(w, 200, L"%d", g_disk[n].nreads);
    else w[0] = 0;
    list_text(n - 1, 2, w);
}

/* one row per disk of the box right from the start */
static void list_rebuild(void)
{
    LVITEMW it; int n;
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    for (n = 1; n <= g_total && n <= META_MAXDISK; n++) {
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = n - 1;
        it.lParam = n;
        it.pszText = L"";
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        list_row(n);
    }
}

static void list_select(int n)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(g_list, LVM_SETITEMSTATE, n - 1, (LPARAM)&it);
    SendMessageW(g_list, LVM_ENSUREVISIBLE, n - 1, FALSE);
}

/* the disks still to be read in grey */
static LRESULT list_draw(NMLVCUSTOMDRAW *cd)
{
    int n;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    n = (int)cd->nmcd.lItemlParam;
    if (n >= 1 && n <= META_MAXDISK && !g_disk[n].status[0]) cd->clrText = RGB(0x80, 0x80, 0x80);
    return CDRF_DODEFAULT;
}

/* ---- disk outcomes: box bar, summary ---- */

static void box_progress(void)
{
    wchar_t w[64]; int n, done = 0;
    for (n = 1; n <= g_total && n <= META_MAXDISK; n++) done += is_done(g_disk[n].status);
    SendMessageW(g_boxbar, PBM_SETRANGE32, 0, g_total > 0 ? g_total : 1);
    SendMessageW(g_boxbar, PBM_SETPOS, done, 0);
    _snwprintf(w, 64, L"%d di %d %ls", done, g_total, noun(g_count, g_total != 1)); w[63] = 0;
    SetWindowTextW(g_boxtxt, w);
}

/* "2 puliti, 1 marginale, 1 saltato o da rifare" */
static void summary(wchar_t *out, int len)
{
    wchar_t part[64];
    int pul, mar, err, alt, opt[MEDIA_KINDS], k, red = 0, n;
    counts(&pul, &mar, &err, &alt, opt);
    for (n = 1; n <= g_total && n <= META_MAXDISK; n++) red += !strcmp(g_disk[n].status, "verificato redump");
    out[0] = 0;
    if (pul) { plural(part, 64, pul, L"pulito", L"puliti"); wcscat(out, part); }
    if (red) {              /* "2 puliti (1 verificato con redump)" */
        _snwprintf(part, 64, L" (%d verificat%ls con redump)", red, red == 1 ? L"o" : L"i"); part[63] = 0;
        wcscat(out, part);
    }
    if (mar) { plural(part, 64, mar, L"marginale", L"marginali"); if (out[0]) wcscat(out, L", "); wcscat(out, part); }
    if (err) { _snwprintf(part, 64, L"%d con errori", err); part[63] = 0; if (out[0]) wcscat(out, L", "); wcscat(out, part); }
    if (alt) {
        plural(part, 64, alt, L"saltato o da rifare", L"saltati o da rifare");
        if (out[0]) wcscat(out, L", ");
        wcscat(out, part);
    }
    for (k = 0; k < MEDIA_KINDS; k++) {     /* "1 CD da leggere" */
        if (!opt[k]) continue;
        _snwprintf(part, 64, L"%d %ls da leggere", opt[k], kind_w[k]); part[63] = 0;
        if (out[0]) wcscat(out, L", ");
        wcscat(out, part);
    }
    if (!out[0]) wcsncpy(out, L"nessun supporto", len);
    (void)len;
}

/* an open box resumed with the same title: the core does not ask again
 * for the disks already done, but they go in the list and the count */
static void seed_previous(void)
{
    char data[640]; meta_box *b; strbuf err; int i, k;
    const char *names[MEDIA_KINDS];
    if (!bag_is_open(g_bagdir)) return;
    u8_join(data, sizeof(data), g_bagdir, "data");
    if (!(b = meta_new())) return;
    sb_init(&err);
    if (meta_load(b, data, &err) == 0 && !strcmp(b->title, g_box->title)) {
        if (b->media_expected > 0) g_total = b->media_expected;
        /* the make-up and names are those it was opened with */
        for (k = 0; k < MEDIA_KINDS; k++) { g_count[k] = meta_count(b, k); names[k] = g_ini_names[k]; }
        session_settle_names(b, names);
        for (k = 0; k < MEDIA_KINDS; k++) str_copy(g_names[k], sizeof(g_names[k]), b->names[k]);
        for (i = 0; i < b->ndisk; i++) {
            const meta_disk *d = &b->disk[i];
            if (d->n < 1 || d->n > META_MAXDISK) continue;
            copy_disk(&g_disk[d->n], d);
            /* the disks to redo are asked for again: for now they are "da leggere" */
            if (!meta_disk_done(d)) g_disk[d->n].status[0] = 0;
        }
    }
    sb_free(&err);
    meta_delete(b);
}

/* ---- closed bag: what is in data/ ---- */

typedef struct { const char *dir; int img, scan, meta; long long bytes; } bag_count;

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !_stricmp(s + a - b, tail);
}

static void count_file(void *ctx, const char *name, int isdir)
{
    bag_count *c = (bag_count *)ctx;
    char path[1024]; long long sz;
    u8_join(path, sizeof(path), c->dir, name);
    if (isdir) {                        /* data\CD01\: one read of a CD or DVD */
        if ((sz = u8_treesize(path)) > 0) c->bytes += sz;
        c->img++;
        return;
    }
    if ((sz = u8_filesize(path)) > 0) c->bytes += sz;
    if (ends_with(name, floppy_media.ext)) c->img++;
    else if (ends_with(name, ".scan.txt")) c->scan++;
    else if (!strcmp(name, "metadata.json")) c->meta = 1;
}

/* the reads set aside on closing: outside data/, not in the manifest.
 * "messi-da-parte\FLOPPY01.dsk.danneggiato, ...", relative to the Cartella;
 * the log has them with the full path */
static void aside_fill(void)
{
    strbuf s; strbuf t; char *p, *nl; wchar_t w[1100];
    sb_init(&s); sb_init(&t);
    g_aside_n = session_aside_list(g_bagdir, &s);
    for (p = s.s; g_aside_n && *p; p = nl + 1) {
        nl = strchr(p, '\n');
        *nl = 0;
        sb_printf(&t, "%s%s\\%s", p == s.s ? "" : ", ", SESSION_ASIDE_DIR, p);
    }
    to_w(t.len ? t.s : "", w, 1100);
    SetDlgItemTextW(g_wnd, ID_BAGASIDE, w);
    sb_free(&s); sb_free(&t);
}

static void bag_fill(void)
{
    char data[640]; bag_count c; wchar_t w[700], a[64], b[64], mb[32], *p;
    to_w(g_bagdir, w, 700);
    SetDlgItemTextW(g_wnd, ID_BAGDIR, w);
    memset(&c, 0, sizeof(c));
    u8_join(data, sizeof(data), g_bagdir, "data");
    c.dir = data;
    u8_listdir(data, count_file, &c);
    plural(a, 64, c.img, L"immagine", L"immagini");
    plural(b, 64, c.scan, L"scansione", L"scansioni");
    _snwprintf(mb, 32, L"%.1f", (double)c.bytes / (1024.0 * 1024.0)); mb[31] = 0;
    if ((p = wcschr(mb, L'.')) != NULL) *p = L',';          /* Italian decimal comma */
    _snwprintf(w, 700, L"%ls, %ls%ls - %ls MB", a, b, c.meta ? L", metadata.json" : L"", mb); w[699] = 0;
    SetDlgItemTextW(g_wnd, ID_BAGCONT, w);
    /* EV_BOX_DONE arrives only after bag_close has recomputed and compared the hashes */
    SetDlgItemTextW(g_wnd, ID_BAGHASH, L"SHA-256 calcolate e ricontrollate: tutte coincidono");
    aside_fill();
}

/* ---- Riprendi: the open boxes in the folder ---- */

#define MAX_OPEN 40

typedef struct {
    const char *root;
    int n;
    char dir[MAX_OPEN][600];
    wchar_t text[MAX_OPEN][400];
    meta_box *tmp;
} open_list;

static void menu_text(const meta_box *b, wchar_t *out, int len)
{
    wchar_t t[300], esc[600]; int i, k = 0, done = 0, c[MEDIA_KINDS];
    to_w(b->title, t, 300);
    for (i = 0; t[i] && k < 598; i++) {     /* in menus "&" underlines: it must be doubled */
        if (t[i] == L'&') esc[k++] = L'&';
        esc[k++] = t[i];
    }
    esc[k] = 0;
    for (i = 0; i < b->ndisk; i++) done += meta_disk_done(&b->disk[i]);
    for (k = 0; k < MEDIA_KINDS; k++) c[k] = meta_count(b, k);
    _snwprintf(out, len, L"%ls  (%d di %d %ls %ls)", esc, done, b->media_expected,
               noun(c, b->media_expected != 1), b->media_expected == 1 ? L"fatto" : L"fatti");
    out[len - 1] = 0;
}

static void add_open(void *ctx, const char *name, int isdir)
{
    open_list *l = (open_list *)ctx;
    char bag[600], data[640]; strbuf err;
    if (!isdir || l->n >= MAX_OPEN) return;
    u8_join(bag, sizeof(bag), l->root, name);
    if (!bag_is_open(bag)) return;
    u8_join(data, sizeof(data), bag, "data");
    sb_init(&err);
    if (meta_load(l->tmp, data, &err) == 0) {
        str_copy(l->dir[l->n], sizeof(l->dir[0]), bag);
        menu_text(l->tmp, l->text[l->n], 400);
        l->n++;
    }
    sb_free(&err);
}

static void fill_from(const meta_box *b)
{
    wchar_t t[300], w[400];
    set_u8(ID_TITLE, b->title);
    set_u8(ID_PLATFORM, b->platform);
    SetDlgItemInt(g_wnd, ID_COUNT, meta_count(b, MEDIA_FLOPPY), FALSE);
    SetDlgItemInt(g_wnd, ID_CD, meta_count(b, MEDIA_CD), FALSE);
    SetDlgItemInt(g_wnd, ID_DVD, meta_count(b, MEDIA_DVD), FALSE);
    set_u8(ID_PUBLISHER, b->publisher);
    set_u8(ID_YEAR, b->year);
    set_u8(ID_LANGUAGE, b->language);
    set_u8(ID_EDITION, b->edition);
    set_u8(ID_NOTES, b->notes);
    to_w(b->title, t, 300);
    _snwprintf(w, 400, L"Riprendo «%ls»: premi Inizia.", t); w[399] = 0;
    SetDlgItemTextW(g_wnd, ID_HINT, w);
}

static void resume(void)
{
    static open_list l;
    char root[512], data[640]; strbuf err;
    wchar_t w[700], wr[512]; HMENU m; RECT rc; int i, pick;

    get_u8(ID_DIR, root, sizeof(root));
    if (!root[0]) str_copy(root, sizeof(root), ".");
    memset(&l, 0, sizeof(l));
    l.root = root;
    if (!(l.tmp = meta_new())) return;
    to_w(root, wr, 512);
    if (u8_listdir(root, add_open, &l) || !l.n) {
        _snwprintf(w, 700, L"Nella cartella %ls non c'è nessun cofanetto aperto da riprendere.\n\n"
                            L"Se è in un'altra cartella, scegli quella in Cartella e premi di nuovo Riprendi.", wr);
        w[699] = 0;
        MessageBoxW(g_wnd, w, L"RetroArchiver", MB_ICONINFORMATION);
        meta_delete(l.tmp);
        return;
    }
    m = CreatePopupMenu();
    for (i = 0; i < l.n; i++) AppendMenuW(m, MF_STRING, i + 1, l.text[i]);
    GetWindowRect(GetDlgItem(g_wnd, ID_RESUME), &rc);
    pick = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN,
                               rc.right, rc.bottom, 0, g_wnd, NULL);
    DestroyMenu(m);
    if (pick >= 1 && pick <= l.n) {
        u8_join(data, sizeof(data), l.dir[pick - 1], "data");
        sb_init(&err);
        if (meta_load(l.tmp, data, &err) == 0) fill_from(l.tmp);
        else {
            to_w(err.s ? err.s : "", w, 700);
            MessageBoxW(g_wnd, w, L"RetroArchiver", MB_ICONEXCLAMATION);
        }
        sb_free(&err);
    }
    meta_delete(l.tmp);
}

/* ---- setup ---- */

static void browse(int id, const wchar_t *title)
{
    BROWSEINFOW bi; LPITEMIDLIST pidl; wchar_t path[MAX_PATH];
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (SHGetPathFromIDListW(pidl, path)) SetDlgItemTextW(g_wnd, id, path);
    CoTaskMemFree(pidl);
}

/* the "file FLOPPY01..03 e CD01" suffix, after a middle dot: the names the
 * media will get */
static void files_text(const char *title, wchar_t *out, int len)
{
    meta_box *b; const char *names[MEDIA_KINDS];
    char first[96], last[96], part[200]; wchar_t w[200];
    int c[MEDIA_KINDS], k, i, parts = 0, done = 0;
    out[0] = 0;
    if (setup_counts(c) || !(b = (meta_box *)calloc(1, sizeof(meta_box)))) return;
    str_copy(b->title, sizeof(b->title), title);
    for (k = 0; k < MEDIA_KINDS; k++) { names[k] = g_ini_names[k]; parts += c[k] > 0; }
    wcsncat(out, L" · file ", len - wcslen(out) - 1);
    for (k = 0; k < MEDIA_KINDS; k++) {
        if (!c[k]) continue;
        session_disk_base(b, names, k, 1, first, sizeof(first));
        session_disk_base(b, names, k, c[k], last, sizeof(last));
        /* FLOPPY01..03: of the last one keep from where the digits start */
        for (i = 0; first[i] && first[i] == last[i]; i++) ;
        while (i > 0 && last[i - 1] >= '0' && last[i - 1] <= '9') i--;
        if (c[k] > 1) snprintf(part, sizeof(part), "%s..%s", first, last + i);
        else str_copy(part, sizeof(part), first);
        to_w(part, w, 200);
        done++;
        if (done > 1) wcsncat(out, done == parts ? L" e " : L", ", len - wcslen(out) - 1);
        wcsncat(out, w, len - wcslen(out) - 1);
    }
    free(b);
}

/* the grey line below the fields: where the bag will go, and the file names */
static void bagpath_update(void)
{
    char dir[512], title[256], bag[800]; wchar_t wb[800], w[900], f[300];
    get_u8_trim(ID_TITLE, title, sizeof(title));
    get_u8(ID_DIR, dir, sizeof(dir));
    files_text(title, f, 300);
    if (title[0]) {
        session_bagdir(bag, sizeof(bag), dir, title);
        to_w(bag, wb, 800);
        _snwprintf(w, 900, L"Il pacchetto sarà creato in %ls%ls", wb, f);
    } else {
        to_w(dir, wb, 800);
        _snwprintf(w, 900, L"Il pacchetto sarà creato in %ls%ls<titolo>%ls", wb,
                   wb[0] && wb[wcslen(wb) - 1] != L'\\' ? L"\\" : L"", f);
    }
    w[899] = 0;
    SetDlgItemTextW(g_wnd, ID_BAGPATH, w);
}

/* NULL if the required fields are there; otherwise what is missing, and the focus goes there */
static const wchar_t *missing(int bad_counts)
{
    int id = 0; const wchar_t *msg = NULL;
    if (!g_box->title[0])         { id = ID_TITLE; msg = L"Manca il titolo del cofanetto."; }
    else if (!g_box->platform[0]) { id = ID_PLATFORM; msg = L"Manca la piattaforma: scegline una dall'elenco o scrivila."; }
    else if (bad_counts) {
        id = ID_COUNT; msg = L"Mancano i supporti del cofanetto: quanti floppy, CD e DVD (da 1 a 64 in tutto).";
    }
    else if (!GetWindowTextLengthW(GetDlgItem(g_wnd, ID_DIR))) {
        id = ID_DIR; msg = L"Manca la cartella dove salvare i cofanetti.";
    }
    /* a path chosen with "..." or read from the ini bypasses the typing limit */
    else if (u8_len(ID_DIR) >= (int)sizeof(g_outdir)) {
        id = ID_DIR; msg = L"Il percorso della cartella è troppo lungo: scegline uno più corto.";
    }
    if (id) SetFocus(GetDlgItem(g_wnd, id));
    return msg;
}

/* window state for a new or resumed box */
static void reset_state(void)
{
    int n;
    memset(g_disk, 0, sizeof(g_disk));
    g_total = g_box->media_expected;
    for (n = 0; n < MEDIA_KINDS; n++) {
        g_count[n] = meta_count(g_box, n);
        str_copy(g_names[n], sizeof(g_names[n]), g_ini_names[n]);
    }
    g_last_done = 0; g_errbeep = 0; g_closing = 0; g_cur = 0;
    memset(&g_turn, 0, sizeof(g_turn));
    g_bag_closed = g_bag_complete = g_reread = g_fragile = g_aside_n = 0;
    g_phase_name[0] = 0; g_last_err[0] = 0;
    pills_clear();
    SendMessageW(g_diskbar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_cyltxt, L"");
    SetWindowTextW(g_elapsed, L"");
    SetWindowTextW(g_log, L"");
    seed_previous();
    list_rebuild();
    box_progress();
}

/* DiscImageCreator not working, with CDs or DVDs in the box */
static int dic_ready(void)
{
    wchar_t w[700], t[200];
    if (g_dic_state == DIC_OK) return 1;
    if (g_dic_state == DIC_CHECKING) {
        MessageBoxW(g_wnd, L"Sto ancora controllando DiscImageCreator: riprova fra un attimo.",
                    L"RetroArchiver", MB_ICONINFORMATION);
        return 0;
    }
    dic_text(t, 200, 0);
    _snwprintf(w, 700, L"%ls.\n\nNel campo DiscImageCreator scrivi il percorso completo di DiscImageCreator.exe "
                       L"(per esempio C:\\DIC\\DiscImageCreator.exe), oppure copialo accanto a RetroArchiver.exe "
                       L"con le sue DLL. Su Windows XP servono i runtime di Visual C++ 2015-2019 x86, versione 14.27.", t);
    w[699] = 0;
    MessageBoxW(g_wnd, w, L"RetroArchiver", MB_ICONEXCLAMATION);
    SetFocus(GetDlgItem(g_wnd, ID_DIC));
    return 0;
}

/* SAMdisk not working: say what to do instead of starting */
static int samdisk_ready(void)
{
    wchar_t w[600], t[200];
    if (g_sd_state == SD_OK) return 1;
    if (g_sd_state == SD_CHECKING) {
        MessageBoxW(g_wnd, L"Sto ancora controllando SAMdisk: riprova fra un attimo.",
                    L"RetroArchiver", MB_ICONINFORMATION);
        return 0;
    }
    samdisk_text(t, 200);
    _snwprintf(w, 600, L"%ls.\n\nNel campo SAMdisk scrivi il percorso completo di samdisk.exe della serie 3.x "
                       L"(per esempio C:\\SAMdisk\\samdisk.exe), oppure copia samdisk.exe accanto a "
                       L"RetroArchiver.exe e lascia scritto solo samdisk.", t);
    w[599] = 0;
    MessageBoxW(g_wnd, w, L"RetroArchiver", MB_ICONEXCLAMATION);
    SetFocus(GetDlgItem(g_wnd, ID_SAMDISK));
    return 0;
}

static void start(void)
{
    char buf[512];
    int c[MEDIA_KINDS], bad = setup_counts(c), k;
    int mr = GetDlgItemInt(g_wnd, ID_MAXREAD, NULL, FALSE);
    const wchar_t *why;
    DWORD tid;
    HANDLE th;

    if (!g_box) g_box = meta_new();
    if (!g_box) { MessageBoxW(g_wnd, L"Memoria insufficiente.", L"RetroArchiver", MB_ICONERROR); return; }
    memset(g_box, 0, sizeof(*g_box));
    get_u8_trim(ID_TITLE, g_box->title, sizeof(g_box->title));
    get_u8_trim(ID_PLATFORM, g_box->platform, sizeof(g_box->platform));
    get_u8(ID_PUBLISHER, g_box->publisher, sizeof(g_box->publisher));
    get_u8(ID_YEAR, g_box->year, sizeof(g_box->year));
    get_u8(ID_LANGUAGE, g_box->language, sizeof(g_box->language));
    get_u8(ID_EDITION, g_box->edition, sizeof(g_box->edition));
    get_u8(ID_NOTES, g_box->notes, sizeof(g_box->notes));
    if ((why = missing(bad)) != NULL) {
        MessageBoxW(g_wnd, why, L"RetroArchiver", MB_ICONINFORMATION);
        return;
    }
    if (c[MEDIA_FLOPPY] && !samdisk_ready()) return;
    if ((c[MEDIA_CD] || c[MEDIA_DVD]) && !dic_ready()) return;
    meta_set_counts(g_box, c[MEDIA_FLOPPY], c[MEDIA_CD], c[MEDIA_DVD]);
    ini_save();
    get_u8(ID_DIR, g_outdir, sizeof(g_outdir));
    get_u8(ID_SAMDISK, buf, sizeof(buf));
    floppy_set_samdisk(buf[0] ? buf : "samdisk");
    get_u8(ID_SOURCE, buf, sizeof(buf));
    floppy_set_source(buf[0] ? buf : "a:");
    get_u8(ID_DIC, buf, sizeof(buf));
    optical_set_dic(buf[0] ? buf : "DiscImageCreator");
    get_u8_trim(ID_DRIVE, buf, sizeof(buf));
    optical_set_drive(buf[0] ? buf : "D:");
    {   /* "d", "d:", "D:\" -> "D:" for the texts */
        const char *q = buf;
        while (*q && !isalpha((unsigned char)*q)) q++;
        _snwprintf(g_drive_w, 8, L"%c:", *q ? (wchar_t)toupper((unsigned char)*q) : L'D');
        g_drive_w[7] = 0;
    }

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.box = g_box;
    g_cfg.outroot = g_outdir;
    for (k = 0; k < MEDIA_KINDS; k++) g_cfg.names[k] = g_ini_names[k];
    g_cfg.maxread = mr < 2 ? 2 : (mr > META_MAXREAD ? META_MAXREAD : mr);
    g_cfg.start = 1;
    g_cfg.media[MEDIA_FLOPPY] = &floppy_media;
    g_cfg.media[MEDIA_CD] = &cd_media;
    g_cfg.media[MEDIA_DVD] = &dvd_media;
    /* copies for the window: while the session runs g_box belongs to the worker thread */
    str_copy(g_title, sizeof(g_title), g_box->title);
    session_bagdir(g_bagdir, sizeof(g_bagdir), g_outdir, g_box->title);

    reset_state();
    g_t_sess = GetTickCount();
    SetTimer(g_wnd, TIMER_ID, 1000, NULL);
    SetTimer(g_wnd, TIMER_LINES, LINES_MS, NULL);
    banner(B_BLUE, L"...", 0, L"Preparo il cofanetto...", L"Creo il pacchetto o riprendo quello aperto.");
    view(V_WORK);
    focus_to(g_log);
    g_running = 1;
    {   /* a drive check still running: the session waits for it */
        HANDLE fd = NULL;
        KillTimer(g_wnd, TIMER_FLOPPY);
        InterlockedIncrement(&g_fd_gen);
        if (g_fd_thread && !DuplicateHandle(GetCurrentProcess(), g_fd_thread, GetCurrentProcess(),
                                            &fd, SYNCHRONIZE, FALSE, 0))
            fd = NULL;
        th = CreateThread(NULL, 0, worker, fd, 0, &tid);
        if (!th && fd) CloseHandle(fd);
    }
    if (th) CloseHandle(th);
    else {
        g_running = 0;
        KillTimer(g_wnd, TIMER_ID);
        KillTimer(g_wnd, TIMER_LINES);
        view(V_SETUP);
        MessageBoxW(g_wnd, L"Non riesco ad avviare la sessione.", L"RetroArchiver", MB_ICONERROR);
    }
}

/* ---- archive: reading the folder in a thread ----
 * Like the SAMdisk check: a new re-read makes those still running
 * obsolete, and the result arrives with a PostMessage. */

typedef struct { LONG gen; char root[512]; } arc_job;
typedef struct { arc_list l; int missing; } arc_result;

static DWORD WINAPI archive_worker(LPVOID p)
{
    arc_job *job = (arc_job *)p;
    arc_result *r = (arc_result *)calloc(1, sizeof(arc_result));
    if (r) {
        r->missing = !u8_isdir(job->root);
        if (r->missing || arc_scan(job->root, &r->l)) memset(&r->l, 0, sizeof(r->l));
        if (!PostMessageW(g_wnd, WM_APP_ARCHIVE, (WPARAM)job->gen, (LPARAM)r)) { arc_free(&r->l); free(r); }
    }
    free(job);
    return 0;
}

static void archive_scan(void)
{
    arc_job *job = (arc_job *)calloc(1, sizeof(arc_job));
    HANDLE th;
    KillTimer(g_wnd, TIMER_ADIR);
    get_u8_trim(ID_ADIR, g_arc_root, sizeof(g_arc_root));
    InterlockedIncrement(&g_arc_gen);
    g_arc_loaded = 0;
    if (g_view == V_ARCHIVE) { sbar_update(); repaint(g_asum); repaint(g_adetail); }
    if (!job) return;
    str_copy(job->root, sizeof(job->root), g_arc_root);
    job->gen = g_arc_gen;
    th = CreateThread(NULL, 0, archive_worker, job, 0, NULL);
    if (th) CloseHandle(th);
    else free(job);
}

/* the archive folder is the same as the setup one: it is remembered */
static void save_dir(void)
{
    ini_store(0);                       /* Cartella */
}

/* ---- archive: texts ---- */

static void fmt_mb(long long bytes, wchar_t *out, int n)
{
    wchar_t *p;
    _snwprintf(out, n, L"%.1f", (double)bytes / (1024.0 * 1024.0)); out[n - 1] = 0;
    if ((p = wcschr(out, L'.')) != NULL) *p = L',';          /* Italian decimal comma */
}

/* "2026-10-14T21:20:11" -> "14/10/2026" */
static void fmt_date(const char *d, wchar_t *out, int n)
{
    out[0] = 0;
    if (strlen(d) < 10) return;
    _snwprintf(out, n, L"%c%c/%c%c/%c%c%c%c", d[8], d[9], d[5], d[6], d[0], d[1], d[2], d[3]);
    out[n - 1] = 0;
}

static int tone_ban(int tone)
{
    static const int k[] = { B_BLUE, B_GREEN, B_ORANGE, B_RED };
    return k[tone];
}

static int tone_dot(int tone)
{
    static const int k[] = { DOT_BLUE, DOT_GREEN, DOT_OCHRE, DOT_RED };
    return k[tone];
}

/* ---- archive: filter and list ---- */

static void lower(wchar_t *s)
{
    CharLowerBuffW(s, (DWORD)wcslen(s));
}

/* searches title and publisher, case-insensitively */
static int matches(const arc_box *b, const wchar_t *q, const char *plat)
{
    wchar_t t[300];
    if (plat[0] && strcmp(b->platform, plat)) return 0;
    if (!q[0]) return 1;
    to_w(b->title, t, 300); lower(t);
    if (wcsstr(t, q)) return 1;
    to_w(b->publisher, t, 300); lower(t);
    return wcsstr(t, q) != NULL;
}

static void platforms_fill(void)
{
    HWND cb = GetDlgItem(g_wnd, ID_APLAT);
    wchar_t cur[100], w[100];
    int i, k;
    k = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    cur[0] = 0;
    if (k > 0) SendMessageW(cb, CB_GETLBTEXT, k, (LPARAM)cur);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_arc.n; i++) {
        if (!g_arc.box[i].platform[0]) continue;
        to_w(g_arc.box[i].platform, w, 100);
        if (SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)w) == CB_ERR)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
    }
    /* the list is sorted: "Tutte" is put on top by hand */
    SendMessageW(cb, CB_INSERTSTRING, 0, (LPARAM)L"Tutte le piattaforme");
    k = cur[0] ? (int)SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)cur) : 0;
    SendMessageW(cb, CB_SETCURSEL, k == CB_ERR ? 0 : k, 0);
}

static void arc_text(int row, int col, const wchar_t *t)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.iSubItem = col;
    it.pszText = (wchar_t *)t;
    SendMessageW(g_alist, LVM_SETITEMTEXTW, row, (LPARAM)&it);
}

static void arc_row(int row, int idx)
{
    const arc_box *b = &g_arc.box[idx];
    LVITEMW it; wchar_t w[300]; char o[200];
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
    it.iItem = row;
    it.lParam = idx;
    it.iImage = tone_dot(arc_tone(b));
    to_w(b->title, w, 300);
    it.pszText = w;
    SendMessageW(g_alist, LVM_INSERTITEMW, 0, (LPARAM)&it);
    to_w(b->platform, w, 300); arc_text(row, 1, w);
    to_w(b->year, w, 300);     arc_text(row, 2, w);
    arc_media(b, o, sizeof(o));                     /* "3 / 3 floppy, 0 / 1 CD" */
    to_w(o, w, 300);           arc_text(row, 3, w);
    arc_outcome(b, o, sizeof(o));
    to_w(o, w, 300);           arc_text(row, 4, w);
    fmt_date(b->date, w, 300); arc_text(row, 5, w);
}

static void archive_buttons(void)
{
    int i;
    static const int ids[] = { ID_ARESUME, ID_AOPEN, ID_AVERIFY, 0 };
    layout();
    if (g_view != V_ARCHIVE) return;
    EnableWindow(GetDlgItem(g_wnd, ID_AVERIFY), !g_verifying);
    /* moved by layout, on XP they kept the neighbour's image: redraw them */
    for (i = 0; ids[i]; i++) {
        HWND b = GetDlgItem(g_wnd, ids[i]);
        ShowWindow(b, visible_in(V_ARCHIVE, ids[i]) ? SW_SHOW : SW_HIDE);
        RedrawWindow(b, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    }
}

static void on_pick(int idx)
{
    if (idx == g_pick) return;
    g_pick = idx;
    repaint(g_adetail);
    archive_buttons();
}

/* the list after the filter; the box in want stays selected, if present */
static void archive_fill(const char *want)
{
    wchar_t q[200]; char plat[64];
    LVITEMW it;
    int i, sel = 0, k;
    GetDlgItemTextW(g_wnd, ID_ASEARCH, q, 200);
    lower(q);
    k = (int)SendDlgItemMessageW(g_wnd, ID_APLAT, CB_GETCURSEL, 0, 0);
    plat[0] = 0;
    if (k > 0) {
        wchar_t w[100];
        SendDlgItemMessageW(g_wnd, ID_APLAT, CB_GETLBTEXT, k, (LPARAM)w);
        to_u8(w, plat, sizeof(plat));
    }
    free(g_rows);
    g_rows = (int *)malloc((size_t)(g_arc.n ? g_arc.n : 1) * sizeof(int));
    g_nrows = 0;
    SendMessageW(g_alist, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_alist, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; g_rows && i < g_arc.n; i++) {
        if (!matches(&g_arc.box[i], q, plat)) continue;
        if (want && want[0] && !strcmp(g_arc.box[i].dir, want)) sel = g_nrows;
        arc_row(g_nrows, i);
        g_rows[g_nrows++] = i;
    }
    SendMessageW(g_alist, WM_SETREDRAW, TRUE, 0);
    g_pick = -2;                        /* on_pick repaints even if the index stays the same */
    if (g_nrows) {
        memset(&it, 0, sizeof(it));
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        it.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_alist, LVM_SETITEMSTATE, sel, (LPARAM)&it);
        SendMessageW(g_alist, LVM_ENSUREVISIBLE, sel, FALSE);
        on_pick(g_rows[sel]);
    } else {
        on_pick(-1);
    }
    InvalidateRect(g_alist, NULL, TRUE);
    repaint(g_asum);
}

/* chosen box: its folder, to find it again after a re-read */
static const char *pick_dir(void)
{
    return g_pick >= 0 && g_pick < g_arc.n ? g_arc.box[g_pick].dir : "";
}

static void on_archive(WPARAM gen, arc_result *r)
{
    char want[600];
    int was_empty = g_arc_empty;
    if ((LONG)gen != g_arc_gen) { arc_free(&r->l); free(r); return; }
    str_copy(want, sizeof(want), g_want_dir[0] ? g_want_dir : pick_dir());
    g_want_dir[0] = 0;
    arc_free(&g_arc);
    g_arc = r->l;
    g_arc_missing = r->missing;
    free(r);
    g_arc_loaded = 1;
    g_arc_empty = g_arc.n == 0;
    g_pick = -1;
    platforms_fill();
    archive_fill(want);
    if (g_view != V_ARCHIVE) return;
    if (was_empty != g_arc_empty) view(V_ARCHIVE);   /* the visible controls change */
    else { layout(); InvalidateRect(g_wnd, &g_empty, TRUE); }
    sbar_update();
}

/* ---- archive: summary and chosen box ---- */

#define PILL_GREY (-1)                  /* neutral tag: nothing to worry about */

static int pill(HDC dc, int x, int h, const wchar_t *t, int kind)
{
    SIZE sz; RECT r;
    HBRUSH br; HPEN pen; HGDIOBJ op, ob;
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    SetRect(&r, x, (h - 18) / 2, x + sz.cx + 16, (h - 18) / 2 + 18);
    br = CreateSolidBrush(kind == PILL_GREY ? RGB(0xec, 0xec, 0xec) : ban_bg[kind]);
    pen = CreatePen(PS_SOLID, 1, kind == PILL_GREY ? RGB(0xb8, 0xb8, 0xb8) : ban_edge[kind]);
    op = SelectObject(dc, pen); ob = SelectObject(dc, br);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 18, 18);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    return r.right + 6;
}

/* "7 cofanetti, 37 floppy + 1 CD, 58,9 MB" and the coloured tags, over the rows shown */
static void paint_summary(HDC dc, int w, int h)
{
    wchar_t t[200], a[64], b[64], mb[32];
    RECT r = { 0, 0, w, h };
    SIZE sz;
    int i, k, x, disks[MEDIA_KINDS] = { 0 }, comp = 0, inc = 0, open = 0, err = 0, broken = 0;
    long long bytes = 0;
    if (!g_arc_loaded) {
        SetTextColor(dc, RGB(0x44, 0x44, 0x44));
        DrawTextW(dc, L"Leggo l'archivio...", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    for (i = 0; i < g_nrows; i++) {
        const arc_box *bx = &g_arc.box[g_rows[i]];
        for (k = 0; k < MEDIA_KINDS; k++) disks[k] += bx->kind_archived[k];
        bytes += bx->bytes; err += bx->errors;
        if (bx->state == ARC_COMPLETE) comp++;
        else if (bx->state == ARC_INCOMPLETE) inc++;
        else if (bx->state == ARC_OPEN) open++;
        else broken++;
    }
    plural(a, 64, g_nrows, L"cofanetto", L"cofanetti");
    comp_text(disks, b, 64);                /* the archived media, per kind */
    if (!b[0]) wcscpy(b, L"nessun supporto");
    fmt_mb(bytes, mb, 32);
    _snwprintf(t, 200, L"%ls, %ls, %ls MB", a, b, mb); t[199] = 0;
    SelectObject(dc, g_mid);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    x = sz.cx + 10;
    SelectObject(dc, g_font);
    if (comp) { plural(t, 200, comp, L"completo", L"completi"); x = pill(dc, x, h, t, B_GREEN); }
    if (inc)  { plural(t, 200, inc, L"incompleto", L"incompleti"); x = pill(dc, x, h, t, B_ORANGE); }
    if (open) { plural(t, 200, open, L"aperto, da riprendere", L"aperti, da riprendere"); x = pill(dc, x, h, t, B_BLUE); }
    /* disks with errors are always reported, even when there are none */
    if (err) { plural(t, 200, err, L"disco con errori", L"dischi con errori"); x = pill(dc, x, h, t, B_RED); }
    else x = pill(dc, x, h, L"nessun disco con errori", PILL_GREY);
    if (broken) { plural(t, 200, broken, L"illeggibile", L"illeggibili"); pill(dc, x, h, t, B_RED); }
}

static void dot(HDC dc, int x, int y, int d, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    Ellipse(dc, x, y, x + d, y + d);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
}

/* publisher, year, platform, language, edition, size: joined by middle dots */
static void detail_meta(const arc_box *b, wchar_t *out, int n)
{
    const char *f[4];
    wchar_t w[200], mb[32];
    int i;
    f[0] = b->publisher; f[1] = b->year; f[2] = b->platform; f[3] = b->language;
    out[0] = 0;
    for (i = 0; i < 4; i++) {
        if (!f[i][0]) continue;
        to_w(f[i], w, 200);
        if (out[0]) wcsncat(out, L" · ", n - wcslen(out) - 1);
        wcsncat(out, w, n - wcslen(out) - 1);
    }
    if (b->edition[0]) {
        to_w(b->edition, w, 200);
        if (out[0]) wcsncat(out, L" · ", n - wcslen(out) - 1);
        wcsncat(out, L"versione ", n - wcslen(out) - 1);
        wcsncat(out, w, n - wcslen(out) - 1);
    }
    fmt_mb(b->bytes, mb, 32);
    _snwprintf(w, 200, L"%ls%ls MB", out[0] ? L" · " : L"", mb); w[199] = 0;
    wcsncat(out, w, n - wcslen(out) - 1);
}

/* one small square per medium, with the kind's symbol: green clean, ochre
 * marginal or fragile, red error, grey still to do */
static int paint_chips(HDC dc, const arc_box *b, int y, int w)
{
    SIZE sz, ts; RECT r; wchar_t num[12];
    int i, x, cw, kind, n;
    SetTextColor(dc, RGB(0x44, 0x44, 0x44));
    GetTextExtentPoint32W(dc, L"Supporti", 8, &sz);
    SetRect(&r, 0, y, sz.cx, y + 18);
    DrawTextW(dc, L"Supporti", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x = sz.cx + 8;
    SelectObject(dc, g_bold);
    for (i = 0; b->disks[i]; i++) {
        char c = b->disks[i];
        COLORREF col = c == 'p' ? ban_dot[B_GREEN] : c == 'e' ? ban_dot[B_RED]
                     : (c == 'm' || c == 'f') ? ban_dot[B_ORANGE] : RGB(0xa0, 0xa0, 0xa0);
        HBRUSH br = CreateSolidBrush(col);
        HPEN pen = CreatePen(PS_SOLID, 1, col);
        HGDIOBJ op, ob;
        /* floppies with their number; a single CD is called "CD", several CDs "CD1", "CD2" */
        arc_slot(b, i + 1, &kind, &n);
        if (kind == MEDIA_FLOPPY) _snwprintf(num, 12, L"%d", n);
        else if (b->count[kind] > 1) _snwprintf(num, 12, L"%ls%d", kind_w[kind], n);
        else _snwprintf(num, 12, L"%ls", kind_w[kind]);
        num[11] = 0;
        GetTextExtentPoint32W(dc, num, (int)wcslen(num), &ts);
        cw = 4 + 10 + 3 + ts.cx + 5;
        if (cw < 30) cw = 30;
        if (x + cw > w) { x = sz.cx + 8; y += 22; }
        op = SelectObject(dc, pen); ob = SelectObject(dc, br);
        RoundRect(dc, x, y, x + cw, y + 18, 5, 5);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(pen); DeleteObject(br);
        paint_kind(dc, kind, x + 4, y + 4, 10);
        SetRect(&r, x + 4 + 10 + 2, y, x + cw - 2, y + 18);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, num, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        x += cw + 4;
    }
    SelectObject(dc, g_font);
    return y + 18;
}

static void paint_detail(HDC dc, int w, int h)
{
    const arc_box *b;
    wchar_t t[1200], n[600];
    char adv[1000];
    RECT r = { 0, 0, w, h };
    int y = 0, k, th;
    if (g_pick < 0 || g_pick >= g_arc.n) {
        SetTextColor(dc, RGB(0x60, 0x60, 0x60));
        DrawTextW(dc, !g_arc_loaded ? L"Leggo l'archivio..."
                      : L"Nessun cofanetto corrisponde alla ricerca: cambia le parole cercate o la piattaforma.",
                  -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        return;
    }
    b = &g_arc.box[g_pick];
    k = tone_ban(arc_tone(b));
    dot(dc, 0, 6, 10, ban_dot[k]);
    SelectObject(dc, g_big);
    SetTextColor(dc, RGB(0, 0, 0));
    to_w(b->title, t, 300);
    SetRect(&r, 16, 0, w, 22);
    DrawTextW(dc, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    y = 24;
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(0x44, 0x44, 0x44));
    detail_meta(b, t, 600);
    SetRect(&r, 0, y, w, y + 16);
    DrawTextW(dc, t, -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    y += 20;

    /* the state and what to do, in a panel of its colour */
    arc_advice(b, adv, sizeof(adv));
    to_w(adv, t, 1200);
    SetRect(&r, 0, 0, w - 16, 0);
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    th = r.bottom;
    {
        HBRUSH br = CreateSolidBrush(ban_bg[k]);
        HPEN pen = CreatePen(PS_SOLID, 1, ban_edge[k]);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
        RoundRect(dc, 0, y, w, y + th + 10, 6, 6);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(pen); DeleteObject(br);
    }
    SetTextColor(dc, RGB(0x20, 0x20, 0x20));
    SetRect(&r, 8, y + 5, w - 8, y + 5 + th);
    DrawTextW(dc, t, -1, &r, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    y += th + 16;

    if (b->disks[0]) y = paint_chips(dc, b, y, w) + 6;
    SetTextColor(dc, RGB(0x44, 0x44, 0x44));
    if (b->notes[0]) { to_w(b->notes, n, 600); _snwprintf(t, 1200, L"Note: %ls", n); t[1199] = 0; }
    else wcscpy(t, L"Nessuna nota.");
    SetRect(&r, 0, y, w, y + 16);
    if (y + 12 < h) DrawTextW(dc, t, -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

/* ---- archive: switching between views ---- */

static void archive_show(const char *want)
{
    wchar_t w[512];
    GetDlgItemTextW(g_wnd, ID_DIR, w, 512);
    SetDlgItemTextW(g_wnd, ID_ADIR, w);
    save_dir();
    str_copy(g_want_dir, sizeof(g_want_dir), want ? want : "");
    archive_scan();                     /* also kills the timer set by SetDlgItemText */
    view(V_ARCHIVE);
    focus_to(g_arc_empty ? GetDlgItem(g_wnd, ID_ANEW) : g_alist);
}

/* the folder typed by hand or chosen with "...": it applies to setup too */
static void archive_dir_changed(void)
{
    wchar_t w[512];
    GetDlgItemTextW(g_wnd, ID_ADIR, w, 512);
    SetDlgItemTextW(g_wnd, ID_DIR, w);
    save_dir();
    archive_scan();
}

static void go_setup(int from_archive)
{
    static const int clear[] = { ID_TITLE, ID_PUBLISHER, ID_YEAR, ID_EDITION, ID_NOTES, 0 };
    wchar_t w[512];
    int i;
    /* a new box starts from empty fields; platform and language stay */
    for (i = 0; clear[i]; i++) SetDlgItemTextW(g_wnd, clear[i], L"");
    SetDlgItemTextW(g_wnd, ID_COUNT, L"0");
    SetDlgItemTextW(g_wnd, ID_CD, L"0");
    SetDlgItemTextW(g_wnd, ID_DVD, L"0");
    if (from_archive) {
        GetDlgItemTextW(g_wnd, ID_ADIR, w, 512);
        SetDlgItemTextW(g_wnd, ID_DIR, w);
    }
    SetDlgItemTextW(g_wnd, ID_HINT, HINT);
    view(V_SETUP);
    bagpath_update();
    /* coming from the archive SAMdisk is already checked: checking again
     * would make Riprendi wait, and it starts at once */
    if (!from_archive) { samdisk_check(); dic_check(); }
    floppy_check();             /* floppies restart from 0: reset the drive state */
    focus_to(GetDlgItem(g_wnd, ID_TITLE));
}

/* Riprendi: the open box's fields, then start as with Inizia */
static void resume_pick(void)
{
    char data[640]; meta_box *m; strbuf err; wchar_t w[700];
    if (!pick_open() || g_running) return;
    u8_join(data, sizeof(data), g_arc.box[g_pick].dir, "data");
    if (!(m = meta_new())) return;
    sb_init(&err);
    if (meta_load(m, data, &err) == 0) {
        go_setup(1);
        set_u8(ID_DIR, g_arc_root);     /* the folder it was read from */
        fill_from(m);
        start();
    } else {
        to_w(err.s ? err.s : "", w, 700);
        MessageBoxW(g_wnd, w, L"RetroArchiver", MB_ICONEXCLAMATION);
    }
    sb_free(&err);
    meta_delete(m);
}

static void open_pick(void)
{
    wchar_t w[700];
    if (g_pick < 0) return;
    to_w(g_arc.box[g_pick].dir, w, 700);
    ShellExecuteW(g_wnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- archive: Ricontrolla impronte, in a thread ---- */

typedef struct { char dir[600]; } vf_job;
typedef struct { int rc, open; arc_check c; char dir[600], err[400]; } vf_result;

static DWORD WINAPI verify_worker(LPVOID p)
{
    vf_job *job = (vf_job *)p;
    vf_result *r = (vf_result *)calloc(1, sizeof(vf_result));
    strbuf err;
    if (r) {
        sb_init(&err);
        str_copy(r->dir, sizeof(r->dir), job->dir);
        r->open = bag_is_open(job->dir);
        r->rc = arc_verify(job->dir, &r->c, &err);
        str_copy(r->err, sizeof(r->err), err.s ? err.s : "");
        sb_free(&err);
        if (!PostMessageW(g_wnd, WM_APP_VERIFY, 0, (LPARAM)r)) free(r);
    }
    free(job);
    return 0;
}

static void verify_pick(void)
{
    vf_job *job;
    HANDLE th;
    if (g_pick < 0 || g_verifying || !(job = (vf_job *)calloc(1, sizeof(vf_job)))) return;
    str_copy(job->dir, sizeof(job->dir), g_arc.box[g_pick].dir);
    g_verifying = 1;
    th = CreateThread(NULL, 0, verify_worker, job, 0, NULL);
    if (th) CloseHandle(th);
    else { free(job); g_verifying = 0; }
    archive_buttons();
    sbar_update();
}

static void add_count(wchar_t *out, int n, int k, const wchar_t *one, const wchar_t *many)
{
    wchar_t p[100];
    if (!k) return;
    plural(p, 100, k, one, many);
    if (out[0]) wcsncat(out, L", ", n - wcslen(out) - 1);
    wcsncat(out, p, n - wcslen(out) - 1);
}

static void on_verify(vf_result *r)
{
    wchar_t w[1200], what[300], title[300], first[200];
    int i, bad = r->c.changed + r->c.lost, icon = MB_ICONINFORMATION;
    g_verifying = 0;
    title[0] = 0;
    for (i = 0; i < g_arc.n; i++) if (!strcmp(g_arc.box[i].dir, r->dir)) to_w(g_arc.box[i].title, title, 300);
    if (!title[0]) to_w(r->dir, title, 300);
    to_w(r->c.first, first, 200);
    what[0] = 0;
    add_count(what, 300, r->c.changed, L"file cambiato", L"file cambiati");
    add_count(what, 300, r->c.lost, L"file mancante", L"file mancanti");
    add_count(what, 300, r->c.extra, L"file in più che il manifest non elenca", L"file in più che il manifest non elenca");
    if (r->rc) {
        to_w(r->err, what, 300);
        _snwprintf(w, 1200, L"Non riesco a ricontrollare le impronte di «%ls»: %ls.", title, what);
        icon = MB_ICONEXCLAMATION;
    } else if (r->open && !r->c.files) {
        _snwprintf(w, 1200, L"«%ls» non ha ancora dischi letti: non c'è niente da ricontrollare.", title);
    } else if (!bad && !r->c.extra) {
        _snwprintf(w, 1200, r->open ? L"«%ls»: %d file ricontrollati, tutti identici a subito dopo la lettura."
                                    : L"«%ls»: %d file ricontrollati, tutti identici a quando il pacchetto è stato chiuso.",
                   title, r->c.files);
    } else {
        _snwprintf(w, 1200, L"In «%ls» ci sono %ls (il primo: %ls).\n\n%ls", title, what, first,
                   r->open ? L"Riprendi il cofanetto: alla chiusura il disco con l'impronta diversa viene messo "
                             L"da parte e richiesto di nuovo."
                   : bad ? L"Il pacchetto non è più integro: rimetti quei file da una copia dell'archivio, "
                           L"oppure rileggi i dischi con un nuovo cofanetto."
                         : L"Togli da data i file in più: il resto del pacchetto è integro.");
        icon = MB_ICONEXCLAMATION;
    }
    w[1199] = 0;
    free(r);
    archive_buttons();
    sbar_update();
    MessageBoxW(g_wnd, w, L"RetroArchiver", icon);
}

/* ---- running session ---- */

/* the banner during the read: blue, orange if re-reading or the disk is
 * fragile */
static void work_banner(void)
{
    wchar_t sub[300], title[120];
    int kind, num;
    slot(g_cur, &kind, &num);
    if (kind == MEDIA_FLOPPY) {
        _snwprintf(sub, 300, L"%ls %d di %d%ls%ls. Non togliere il dischetto finché la lettura non è finita.",
                   kind_cap[kind], num, g_count[kind], g_phase_name[0] ? L": " : L"", g_phase_name);
        sub[299] = 0;
        if (g_reread) banner_num(B_ORANGE, g_cur, L"Le letture non coincidono: faccio un'altra lettura", sub);
        else if (g_fragile) banner_num(B_ORANGE, g_cur, L"Disco fragile: una sola lettura prudente", sub);
        else banner_num(B_BLUE, g_cur, L"Lettura in corso: non togliere il dischetto", sub);
        return;
    }
    /* CDs and DVDs: DiscImageCreator, 10 to 30 minutes per read */
    _snwprintf(sub, 300, L"%ls %d di %d nel lettore %ls%ls%ls%ls. Non aprire il lettore finché la lettura non è "
                         L"finita: un CD intero richiede da 10 a 30 minuti.",
               kind_cap[kind], num, g_count[kind], g_drive_w, g_phase_name[0] ? L" (" : L"", g_phase_name,
               g_phase_name[0] ? L")" : L"");
    sub[299] = 0;
    if (g_reread) {
        _snwprintf(title, 120, L"Una lettura non basta: rileggo il %ls per il confronto", kind_w[kind]);
        title[119] = 0;
        banner(B_ORANGE, NULL, ICON_DISC, title, sub);
    } else {
        _snwprintf(title, 120, L"Lettura in corso: non togliere il %ls", kind_w[kind]);
        title[119] = 0;
        banner(B_BLUE, NULL, ICON_DISC, title, sub);
    }
}

/* the disk being read is a CD or a DVD */
static int cur_optical(void)
{
    int kind, num;
    if (g_cur < 1) return 0;
    slot(g_cur, &kind, &num);
    return kind != MEDIA_FLOPPY;
}

static int cur_cd(void)
{
    int kind, num;
    if (g_cur < 1) return 0;
    slot(g_cur, &kind, &num);
    return kind == MEDIA_CD;
}

static void answer(int cmd)
{
    wchar_t w[300], who[32];
    if (!g_waiting) return;
    slot_name(g_cur, 0, who, 32);
    GetWindowTextW(g_label, w, 128);
    to_u8(w, g_label_u8, sizeof(g_label_u8));
    GetWindowTextW(g_note, w, 300);
    to_u8(w, g_note_u8, sizeof(g_note_u8));
    g_cmd = cmd;
    g_waiting = 0;
    g_reread = g_fragile = 0;
    g_last_err[0] = 0;          /* the final banner shows only the error that stops the session */
    g_why[0] = 0;
    g_phase_name[0] = 0;
    pills_clear();
    SendMessageW(g_diskbar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_cyltxt, L"");
    SetWindowTextW(g_elapsed, L"");
    if (cmd == CMD_SKIP || (cmd == CMD_READ && !g_turn.readable)) {
        _snwprintf(w, 300, g_turn.readable ? L"Salto il %ls." : L"Il %ls resta da leggere.", who); w[299] = 0;
        banner(B_BLUE, L"...", 0, w, NULL);
    }
    else if (cmd == CMD_FINISH)
        banner(B_BLUE, L"...", 0, L"Chiudo il cofanetto...", L"Calcolo e ricontrollo le impronte SHA-256 dei file.");
    else work_banner();
    view(V_WORK);
    SetFocus(g_log);            /* not on Interrompi: one Enter too many would stop the read */
    SetEvent(g_wake);
}

/* closing while waiting: the disk stays to be read, the label and note in
 * the fields are saved as with answer */
static void quit_waiting(void)
{
    wchar_t w[300];
    GetWindowTextW(g_label, w, 128);
    to_u8(w, g_label_u8, sizeof(g_label_u8));
    GetWindowTextW(g_note, w, 300);
    to_u8(w, g_note_u8, sizeof(g_note_u8));
    g_cmd = CMD_QUIT;
    g_waiting = 0;
    SetEvent(g_wake);
}

static void attention(int retry)
{
    FLASHWINFO fi;
    sound(retry ? SND_ERROR : SND_WAIT);
    memset(&fi, 0, sizeof(fi));
    fi.cbSize = sizeof(fi); fi.hwnd = g_wnd;
    fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG; fi.uCount = 5;
    FlashWindowEx(&fi);
    SetForegroundWindow(g_wnd);
}

/* "Reading Cyl 3  Head 0" (SAMdisk 3.8.12, from runner_run_tty's hidden
 * console) or "Cyl 40 Head 0:  ok": progress, it goes to the bar and not
 * to the log, which would otherwise fill up with identical lines */
static int progress_cyl(const char *s)
{
    while (*s == ' ') s++;
    if (!strncmp(s, "Reading ", 8)) s += 8;
    if (strncmp(s, "Cyl ", 4) || s[4] < '0' || s[4] > '9') return -1;
    return atoi(s + 4);
}

/* a scan track, "  4.0  1 2 3 ...": the cylinder goes to the bar, the line
 * stays in the log */
static int scan_cyl(const char *s)
{
    const char *p;
    while (*s == ' ') s++;
    for (p = s; *p >= '0' && *p <= '9'; p++) ;
    if (p == s || *p != '.' || p[1] < '0' || p[1] > '9' || p[2] != ' ') return -1;
    return atoi(s);
}

/* CDs and DVDs: the bar in thousandths and the time left, estimated from
 * how long has passed since the phase started */
static void show_opt(long done, long total)
{
    wchar_t w[64];
    DWORD el = GetTickCount() - g_t_phase;
    int pm = total > 0 ? (int)((double)done * OPT_MAX / total) : 0;
    if (pm > OPT_MAX) pm = OPT_MAX;
    SendMessageW(g_diskbar, PBM_SETPOS, pm, 0);
    if (pm >= 10 && pm < OPT_MAX && el > 15000) {
        DWORD left = (DWORD)((double)el * (OPT_MAX - pm) / pm / 1000.0);
        if (left >= 60) _snwprintf(w, 64, L"%d%%, ancora %lu min", pm / 10, (unsigned long)((left + 30) / 60));
        else _snwprintf(w, 64, L"%d%%, ancora %lu s", pm / 10, (unsigned long)left);
    } else {
        _snwprintf(w, 64, L"%d%%", pm / 10);
    }
    w[63] = 0;
    SetWindowTextW(g_cyltxt, w);
}

static void show_cyl(int cyl)
{
    wchar_t w[64];
    SendMessageW(g_diskbar, PBM_SETPOS, cyl > CYL_MAX ? CYL_MAX : cyl, 0);
    _snwprintf(w, 64, L"Cilindro %d di %d", cyl, CYL_MAX); w[63] = 0;
    SetWindowTextW(g_cyltxt, w);
}

/* the core's texts are meant for the XP console: "c'e'", "unita'". In the
 * window, vowels with an apostrophe at the end of a word get their accent
 * back (the closed e of "perche'" becomes an open one: better than the
 * apostrophe) */
static void accents(wchar_t *s)
{
    static const wchar_t plain[] = L"aeiouAEIOU", acc[] = L"àèìòùÀÈÌÒÙ";
    wchar_t *p, *q;
    for (p = s, q = s; *p; p++) {
        const wchar_t *v = wcschr(plain, *p);
        if (v && *v && p[1] == L'\'' && (!p[2] || wcschr(L" .,:;)!?\n", p[2]))) {
            *q++ = acc[v - plain];
            p++;                        /* the apostrophe is no longer needed */
            continue;
        }
        *q++ = *p;
    }
    *q = 0;
}

/* two core notices change the banner and the expected phases; the text is
 * session.c's, and if it changes only the colour is lost */
static void notice(const char *s)
{
    /* a CD not found in redump: second read and comparison */
    if (strstr(s, "non e' verificato da redump")) {
        g_reread = 1;
        pills_reread();
        work_banner();
    } else if (!strncmp(s, "Le letture non coincidono", 25)) {
        g_reread = 1;
        pills_reread();
        work_banner();
    } else if (!strncmp(s, "La ricognizione ha trovato settori difettosi", 44)) {
        g_fragile = 1;
        if (g_pcur >= 0) { pills_future(); repaint(g_pills); }
        work_banner();
    } else if (!strncmp(s, "Nel lettore non c'e' un ", 24) || !strncmp(s, "DiscImageCreator si e' fermato", 30) ||
               !strncmp(s, "DiscImageCreator non apre", 25) || !strncmp(s, "DiscImageCreator ha finito", 26)) {
        /* why a CD read failed (optical.c): it goes into the Riprova banner */
        to_w(s, g_why, 600);
        accents(g_why);
    }
}

static void on_phase(int disk, const wchar_t *t)
{
    wchar_t w[1100], who[32];
    wcsncpy(g_phase_name, t, 127); g_phase_name[127] = 0;
    if (!wcscmp(t, L"Scansione") || !wcscmp(t, L"TOC")) g_reread = g_fragile = 0;
    g_cur = disk;
    g_t_phase = GetTickCount();
    /* floppy: cylinders; CD and DVD: thousandths of the read */
    SendMessageW(g_diskbar, PBM_SETRANGE32, 0, cur_optical() ? OPT_MAX : CYL_MAX);
    SendMessageW(g_diskbar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_cyltxt, L"");
    SetWindowTextW(g_elapsed, L"0:00");
    pills_phase(t);
    work_banner();
    sbar_update();
    slot_name(disk, 0, who, 32);
    _snwprintf(w, 1100, L"--- %ls: %ls ---", who, t); w[1099] = 0;
    log_append(w);
}

static void on_disk_done(const gui_event *e, const wchar_t *t)
{
    wchar_t w[1100], who[32];
    int n = e->disk;
    g_last_done = n;
    g_phase_name[0] = 0;        /* between one disk and the next no phase is running */
    if (n >= 1 && n <= META_MAXDISK) {
        g_disk[n] = e->info;
        list_row(n);
    }
    pills_done();
    box_progress();
    sbar_update();
    slot_name(n, 1, who, 32);
    _snwprintf(w, 1100, L"%ls: %ls", who, t); w[1099] = 0;
    log_append(w);
}

/* a batch of lines: into the log with a single EM_REPLACESEL, except the
 * progress ones, of which only the last matters, for the bar */
static void on_lines(char *text, size_t len)
{
    wchar_t *w = (wchar_t *)malloc((2 * len + 1) * sizeof(wchar_t));
    size_t k = 0;
    int cyl = -1, c, opt = cur_optical(), prog = 0;
    long done = 0, total = 0, a, b;
    char *s, *nl;
    for (s = text; s < text + len; s = nl + 1) {
        nl = strchr(s, '\n');           /* the batch always ends with '\n' */
        if (!opt && (c = progress_cyl(s)) >= 0) { cyl = c; continue; }
        if (!opt && (c = scan_cyl(s)) >= 0) cyl = c;
        if (opt) {
            char save = *nl;
            int is = (*nl = 0, optical_progress(s, &a, &b));
            *nl = save;
            /* "Creating .scm (LBA) 1234/300000": the bar only; "Remains 3 c2
             * error ... 20/20" also in the log */
            if (is) { done = a; total = b; prog = 1; if (!strstr(s, "error")) continue; }
        }
        if (w) {
            k += MultiByteToWideChar(CP_UTF8, 0, s, (int)(nl - s), w + k, (int)(nl - s));
            w[k++] = L'\r'; w[k++] = L'\n';
        }
    }
    if (w && k) { w[k] = 0; log_block(w); }
    free(w);
    if (cyl >= 0) show_cyl(cyl);
    if (prog) show_opt(done, total);
    /* the notices after the log, as when lines arrived one at a time */
    for (s = text; s < text + len; s = nl + 1) {
        nl = strchr(s, '\n');
        *nl = 0;
        notice(s);
    }
}

static void on_event(gui_event *e)
{
    wchar_t w[1100], t[1024];
    const char *s = e->text ? e->text : "";
    if (e->k == EV_LINE) { if (e->text) on_lines(e->text, e->len); return; }
    to_w(s, t, 1024);
    switch (e->k) {
    case EV_PHASE:
        on_phase(e->disk, t);
        break;
    case EV_LINE:
        break;
    case EV_DISK_DONE:
        on_disk_done(e, t);
        break;
    case EV_BOX_DONE:
        g_bag_closed = 1;
        g_bag_complete = !strcmp(s, "completo");
        _snwprintf(w, 1100, L"Cofanetto %ls: pacchetto chiuso e verificato.", t); w[1099] = 0;
        log_append(w);
        sbar_update();
        break;
    case EV_ERROR:
        wcsncpy(g_last_err, t, 599); g_last_err[599] = 0;
        _snwprintf(w, 1100, L"ERRORE: %ls", t); w[1099] = 0;
        log_append(w);
        if (g_view == V_WORK) banner(B_RED, L"!", 0, L"Errore", t);
        sound(SND_ERROR);
        g_errbeep = 1;
        break;
    }
}

/* takes the whole queue and frees it; show is 0 when the window is closing */
static void queue_drain(int show)
{
    gui_event *e, *next;
    EnterCriticalSection(&g_qlock);
    e = g_qhead;
    g_qhead = g_qtail = NULL;
    LeaveCriticalSection(&g_qlock);
    for (; e; e = next) {
        next = e->next;
        if (show) on_event(e);
        free(e->text);
        free(e);
    }
}

/* the turn of a CD or DVD while their reading is not available: blue
 * banner, Salta instead of Leggi */
static void wait_unreadable(const disk_turn *t)
{
    wchar_t w[160], text[400];
    const wchar_t *k = kind_w[t->kind];
    _snwprintf(w, 160, L"%ls %d di %d: la lettura dei %ls non c'è ancora", kind_cap[t->kind], t->num, t->count, k);
    w[159] = 0;
    _snwprintf(text, 400, L"Il %ls resta da leggere e il cofanetto aperto: lo riprenderai con Riprendi quando "
                          L"RetroArchiver saprà leggere i %ls. Premi Salta per continuare.", k, k);
    text[399] = 0;
    banner(B_BLUE, NULL, ICON_DISC, w, text);
}

/* waiting for a disk: a new one, or the same after a failed read */
static void on_wait(void)
{
    wchar_t w[160], lab[300];
    int n = g_turn_in.n, total = g_turn_in.total;
    const char *last = (n >= 1 && n <= META_MAXDISK) ? g_disk[n].status : "";
    int retry = (n == g_last_done) &&
                (!strcmp(last, "lettura fallita") || !strcmp(last, "interrotto"));

    /* close confirmed between the check in host_wait and the PostMessage:
     * answer at once, the core's label and note stay */
    if (g_closing) { g_cmd = CMD_QUIT; SetEvent(g_wake); return; }
    g_turn = g_turn_in;
    if (total > 0 && total != g_total) { g_total = total; list_rebuild(); box_progress(); }
    g_cur = n;
    /* Salta is the Enter button when Leggi is absent */
    SendDlgItemMessageW(g_wnd, ID_READ, BM_SETSTYLE, g_turn.readable ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
    SendDlgItemMessageW(g_wnd, ID_SKIP, BM_SETSTYLE, g_turn.readable ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON, TRUE);
    if (!g_turn.readable) {
        wait_unreadable(&g_turn);
    } else if (retry) {
        _snwprintf(w, 160, L"%ls %d: lettura %ls", kind_cap[g_turn.kind], g_turn.num,
                   !strcmp(last, "interrotto") ? L"interrotta" : L"non riuscita");
        w[159] = 0;
        /* after Interrompi the user often wants to stop: say how; a CD says
         * why DiscImageCreator did not read it (not visible in the log) */
        if (!strcmp(last, "interrotto"))
            banner(B_ORANGE, L"!", 0, w, L"Premi Riprova per rileggerlo, Salta per passare al successivo, oppure "
                                         L"chiudi la finestra: il cofanetto resta aperto e lo riprendi dall'archivio");
        else if (g_turn.kind == MEDIA_FLOPPY)
            banner(B_ORANGE, L"!", 0, w, L"Controlla il dischetto, rimettilo nel lettore e premi Riprova (F5), "
                                         L"oppure Salta (Alt+S).");
        else {
            wchar_t why[700];
            if (g_why[0]) _snwprintf(why, 700, L"%ls\nPoi premi Riprova (F5), oppure Salta (Alt+S).", g_why);
            else _snwprintf(why, 700, L"Controlla il disco (il tipo giusto, pulito), rimettilo nel lettore e "
                                      L"premi Riprova (F5), oppure Salta (Alt+S).");
            why[699] = 0;
            banner(B_ORANGE, L"!", 0, w, why);
        }
    } else if (g_turn.kind != MEDIA_FLOPPY) {
        wchar_t sub[300];
        if (g_turn.count > 1)
            _snwprintf(w, 160, L"Inserisci il %ls %d di %d nel lettore %ls", kind_w[g_turn.kind], g_turn.num,
                       g_turn.count, g_drive_w);
        else
            _snwprintf(w, 160, L"Inserisci il %ls nel lettore %ls", kind_w[g_turn.kind], g_drive_w);
        w[159] = 0;
        _snwprintf(sub, 300, L"Chiudi il lettore, aspetta che la spia smetta di lampeggiare, poi premi Invio o "
                             L"Leggi (F5); F2 rilegge l'etichetta, Alt+S salta. La lettura con "
                             L"DiscImageCreator dura da 10 a 30 minuti. Etichetta del %ls "
                             L"(facoltativa):", kind_w[g_turn.kind]);
        sub[299] = 0;
        banner(B_YELLOW, NULL, ICON_DISC, w, sub);
    } else {
        _snwprintf(w, 160, L"Inserisci il %ls %d di %d", kind_w[g_turn.kind], g_turn.num, g_turn.count);
        w[159] = 0;
        banner_num(B_YELLOW, g_turn.num, w, L"Poi premi Invio o Leggi (F5); F2 rilegge l'etichetta, Alt+S salta "
                                            L"il disco. Etichetta del disco (facoltativa):");
    }
    SetDlgItemTextW(g_wnd, ID_READ, retry ? L"Riprova" : L"Leggi");
    /* what the core already has; on retry, what was typed stays */
    to_w(g_label_u8, lab, 128);
    if (lab[0] || !retry) SetWindowTextW(g_label, lab);
    to_w(g_note_u8, lab, 300);
    if (lab[0] || !retry) SetWindowTextW(g_note, lab);
    SendMessageW(g_diskbar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_cyltxt, L"");
    g_lb_msg[0] = 0;            /* the magnifier's outcome was for the previous disk */
    g_waiting = 1;
    view(V_WAIT);
    list_select(n);
    focus_to(g_label);
    SendMessageW(g_label, EM_SETSEL, 0, -1);
    attention(retry);
    /* CD or DVD already in the drive: the label at once, without spinning anything */
    if (g_turn.kind != MEDIA_FLOPPY && GetWindowTextLengthW(g_label) == 0) label_read();
}

/* the final banner: green complete, orange incomplete or open, red error */
static void end_banner(int rc)
{
    wchar_t title[200], text[700], sum[200], nd[64], later[200];
    int pul, mar, err, alt, opt[MEDIA_KINDS];
    counts(&pul, &mar, &err, &alt, opt);
    summary(sum, 200);
    plural(nd, 64, pul + mar + err, noun(g_count, 0), noun(g_count, 1));
    /* only CDs or DVDs are left: resume once their reading is available */
    later[0] = 0;
    if (!alt && (opt[MEDIA_CD] || opt[MEDIA_DVD]))
        _snwprintf(later, 200, L"\nRiprendilo dall'archivio quando RetroArchiver saprà leggere %ls.",
                   opt[MEDIA_CD] && opt[MEDIA_DVD] ? L"i CD e i DVD" : opt[MEDIA_CD] ? L"i CD" : L"i DVD");
    later[199] = 0;
    if (rc == SESSION_CLOSED) {
        _snwprintf(title, 200, g_bag_complete ? L"Cofanetto completo: %ls su %d"
                                              : L"Cofanetto chiuso incompleto: %ls su %d", nd, g_total);
        _snwprintf(text, 700, L"%ls.\nIl pacchetto è chiuso e verificato.%ls", sum,
                   g_aside_n ? L" Fuori dai dati restano le letture messe da parte: guardale e, se non "
                               L"servono, cancellale." : L"");
        title[199] = 0; text[699] = 0;
        if (g_bag_complete) banner(B_GREEN, NULL, 1, title, text);
        else banner(B_ORANGE, L"!", 0, title, text);
    } else if (rc == SESSION_OPEN) {
        _snwprintf(text, 700, L"%ls%ls%ls.%ls", g_last_err, g_last_err[0] ? L"\n" : L"", sum,
                   later[0] ? later : L"\nPer riprenderlo: Archivio, poi doppio clic sul cofanetto.");
        text[699] = 0;
        banner(B_ORANGE, L"!", 0, L"Cofanetto lasciato aperto", text);
    } else {
        _snwprintf(text, 700, L"%ls%lsLeggi il registro qui sotto, correggi e riprova con Nuovo cofanetto.",
                   g_last_err, g_last_err[0] ? L"\n" : L"");
        text[699] = 0;
        banner(B_RED, L"!", 0, L"Archiviazione non riuscita", text);
    }
}

static void on_end(int rc)
{
    int n;
    g_running = 0;
    KillTimer(g_wnd, TIMER_ID);
    if (g_closing) { DestroyWindow(g_wnd); return; }
    /* closed with Fine cofanetto: the disks never asked for count as missing */
    if (rc == SESSION_CLOSED)
        for (n = 1; n <= g_total && n <= META_MAXDISK; n++)
            if (!g_disk[n].status[0]) { str_copy(g_disk[n].status, sizeof(g_disk[n].status), "mancante"); list_row(n); }
    if (rc == SESSION_CLOSED) sound(SND_DONE);
    else if (rc != SESSION_OPEN && !g_errbeep) sound(SND_ERROR);
    if (g_bag_closed) bag_fill();
    g_phase_name[0] = 0;
    pills_clear();
    end_banner(rc);
    box_progress();
    view(V_END);
    ShowWindow(GetDlgItem(g_wnd, ID_OPEN), u8_isdir(g_bagdir) ? SW_SHOW : SW_HIDE);
    focus_to(GetDlgItem(g_wnd, ID_NEW));
}

/* only Scansione and Lettura run inside SAMdisk and can be stopped; the
 * comparison and closing the bag finish by themselves in a few seconds */
static void stop(void)
{
    if (g_view != V_WORK) return;      /* Interrompi exists only during the read */
    if (!wcsncmp(g_phase_name, L"Scansione", 9) || !wcsncmp(g_phase_name, L"Lettura", 7) ||
        !wcscmp(g_phase_name, L"TOC") || !wcscmp(g_phase_name, L"Controllo C2")) {
        banner_num(B_ORANGE, g_cur, L"Interrompo la lettura...", cur_optical()
                   ? L"DiscImageCreator si ferma fra poco. La lettura a metà non resta nel pacchetto."
                   : L"SAMdisk si ferma fra poco.");
        runner_cancel();
    } else if (!wcsncmp(g_phase_name, L"Confronto", 9) || !wcscmp(g_phase_name, L"Redump")) {
        /* on the last disk there is no next one: once the comparison ends
         * the bag closes, or it re-reads (seen on the VM with a slowed-down
         * comparison) */
        banner_num(B_BLUE, g_cur, L"Il confronto finisce da solo fra poco", g_cur >= g_total
                   ? L"È l'ultimo disco del cofanetto: aspetta qui l'esito, non c'è un disco successivo da chiedere"
                   : L"Quando ti chiede il disco successivo puoi chiudere la finestra: "
                     L"il cofanetto resta aperto e lo riprendi dall'archivio");
    } else {
        banner(B_BLUE, L"...", 0, L"Qui non c'è una lettura da fermare", L"Aspetta qualche secondo.");
    }
}

/* Fine cofanetto with media never read: a closed bag can no longer be
 * resumed, so ask for confirmation; like mark_missing, counts those
 * without reads */
static int finish_ok(void)
{
    wchar_t w[900], list[400], who[32];
    int n, miss = 0;
    if (!g_waiting) return 0;
    list[0] = 0;
    for (n = 1; n <= g_total && n <= META_MAXDISK; n++) {
        if (is_done(g_disk[n].status) || g_disk[n].nreads) continue;
        slot_name(n, 0, who, 32);
        if (miss++) wcsncat(list, L", ", 399 - wcslen(list));
        wcsncat(list, who, 399 - wcslen(list));
    }
    if (!miss) return 1;
    _snwprintf(w, 900, L"%ls ancora: %ls.\n\n"
                       L"Se chiudo il cofanetto adesso risulterà incompleto, con %ls, e non si potrà "
                       L"più riprendere dall'archivio.\n\nChiudo il cofanetto?",
               miss == 1 ? L"Manca" : L"Mancano", list, miss == 1 ? L"quel supporto mancante" : L"quei supporti mancanti");
    w[899] = 0;
    return MessageBoxW(g_wnd, w, L"RetroArchiver", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

static void open_folder(void)
{
    wchar_t w[700];
    to_w(g_bagdir, w, 700);
    ShellExecuteW(g_wnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

static void tick(void)
{
    wchar_t w[32];
    if (g_view == V_WORK && g_phase_name[0]) {
        fmt_time(GetTickCount() - g_t_phase, w, 32);
        SetWindowTextW(g_elapsed, w);
    }
    if (g_running) sbar_update();
}

static int on_command(HWND h, WPARAM wp)
{
    int id = LOWORD(wp);
    /* the media counts start at 0: entering the field selects everything,
     * so the number can be typed without deleting the 0 (after the click:
     * hence Post) */
    if (HIWORD(wp) == EN_SETFOCUS && (id == ID_COUNT || id == ID_CD || id == ID_DVD)) {
        PostMessageW(GetDlgItem(h, id), EM_SETSEL, 0, -1);
        return 1;
    }
    if (HIWORD(wp) == EN_CHANGE) {
        if (id == ID_TITLE || id == ID_DIR || id == ID_COUNT || id == ID_CD || id == ID_DVD) bagpath_update();
        if (id == ID_ADIR) SetTimer(h, TIMER_ADIR, 600, NULL);     /* re-read once typing stops */
        if (id == ID_ASEARCH) archive_fill(pick_dir());
        if (id == ID_SAMDISK) {             /* checked once typing stops */
            samdisk_pending();
            SetTimer(h, TIMER_SAMDISK, 600, NULL);
        }
        if (id == ID_DIC) {
            dic_pending();
            SetTimer(h, TIMER_DIC, 600, NULL);
        }
        if ((id == ID_SAMDISK || id == ID_SOURCE || id == ID_COUNT) && g_view == V_SETUP && !g_running) {
            floppy_pending();
            SetTimer(h, TIMER_FLOPPY, 600, NULL);
        }
        if (g_view == V_SETUP) sbar_setup();
        return 1;
    }
    if (id == ID_PLATFORM && (HIWORD(wp) == CBN_EDITCHANGE || HIWORD(wp) == CBN_SELCHANGE)) {
        /* during CBN_SELCHANGE the text is still the old one: recompute afterwards */
        PostMessageW(h, WM_APP_READY, 0, 0);
        return 1;
    }
    if (id == ID_APLAT) {
        if (HIWORD(wp) == CBN_SELCHANGE) archive_fill(pick_dir());
        return 1;
    }
    switch (id) {
    case IDOK:
        if (g_waiting) answer(g_turn.readable ? CMD_READ : CMD_SKIP);
        else if (g_view == V_END) SendMessageW(h, WM_COMMAND, ID_TOARCH, 0);
        else if (!g_running && g_view == V_SETUP) start();
        else if (g_view == V_ARCHIVE) {
            /* Enter on the list resumes an open box; in the fields it does
             * nothing; on a button it presses it: IsDialogMessage, in a window
             * that is not a dialog, sends IDOK even when the focus is there */
            HWND f = GetFocus();
            wchar_t cls[16];
            if (f && IsChild(h, f) && GetClassNameW(f, cls, 16) && !wcscmp(cls, L"Button"))
                SendMessageW(h, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(f), BN_CLICKED), (LPARAM)f);
            else if (f == g_alist) { if (pick_open()) resume_pick(); }
            else if (GetFocus() == GetDlgItem(h, ID_ADIR)) archive_dir_changed();
        }
        return 1;
    case ID_BACK:    archive_show(NULL); return 1;
    case ID_TOARCH:  archive_show(g_bagdir); return 1;
    case ID_ABROWSE:
        browse(ID_ADIR, L"Cartella dell'archivio dei cofanetti");
        archive_dir_changed();
        return 1;
    case ID_ARESUME: resume_pick(); return 1;
    case ID_AOPEN:   open_pick(); return 1;
    case ID_AVERIFY: verify_pick(); return 1;
    case ID_ANEW:    go_setup(1); return 1;
    case ID_START:  start(); return 1;
    case ID_RESUME: resume(); return 1;
    case ID_BROWSE: browse(ID_DIR, L"Cartella dove salvare i cofanetti"); return 1;
    case ID_READ:   answer(CMD_READ); return 1;
    case ID_SKIP:   answer(CMD_SKIP); return 1;
    case ID_LABELREAD: label_read(); return 1;
    case ID_FINISH: if (finish_ok()) answer(CMD_FINISH); return 1;
    case ID_STOP:   stop(); return 1;
    case ID_OPEN:   open_folder(); return 1;
    case ID_NEW:    go_setup(0); return 1;
    }
    return 0;
}

static void min_size(MINMAXINFO *mm)
{
    RECT r = { 0, 0, MIN_CW, MIN_CH };
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT);
    mm->ptMinTrackSize.x = r.right - r.left;
    mm->ptMinTrackSize.y = r.bottom - r.top;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        if (on_command(h, wp)) return 0;
        break;
    /* the worker thread queues everything before posting these three:
     * draining it first keeps lines and events in the order they were born */
    case WM_APP_EVENT:
        queue_drain(1);
        return 0;
    case WM_APP_WAIT:
        queue_drain(1);
        on_wait();
        return 0;
    case WM_APP_END:
        queue_drain(1);
        KillTimer(h, TIMER_LINES);
        on_end((int)wp);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_LINES) queue_drain(1);
        else if (wp == TIMER_ID) tick();
        else if (wp == TIMER_SAMDISK) samdisk_check();
        else if (wp == TIMER_DIC) dic_check();
        else if (wp == TIMER_FLOPPY) floppy_check();
        else if (wp == TIMER_ADIR) archive_dir_changed();
        return 0;
    case WM_APP_ARCHIVE:
        on_archive(wp, (arc_result *)lp);
        return 0;
    case WM_APP_VERIFY:
        on_verify((vf_result *)lp);
        return 0;
    case WM_APP_SAMDISK:
        on_samdisk(wp, (sd_result *)lp);
        return 0;
    case WM_APP_DIC:
        on_dic(wp, (dic_result *)lp);
        return 0;
    case WM_APP_LABEL:
        on_label(wp, (lb_result *)lp);
        return 0;
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL) on_media_arrival((const DEV_BROADCAST_HDR *)lp);
        return TRUE;
    case WM_APP_FLOPPY:
        on_floppy(wp, (fd_result *)lp);
        return 0;
    case WM_APP_READY:
        if (g_view == V_SETUP) sbar_setup();
        return 0;
    case WM_APP_FOCUS:
        /* only if nobody picked another control in the meantime */
        if ((HWND)lp == g_focus && can_focus(g_focus) && GetFocus() != g_focus) SetFocus(g_focus);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) focus_save();
        else if (!HIWORD(wp)) focus_restore();      /* not when minimized */
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) layout();
        return 0;
    case WM_GETMINMAXINFO:
        min_size((MINMAXINFO *)lp);
        return 0;
    case WM_DRAWITEM:
        if (wp == ID_BANNER || wp == ID_PILLS || wp == ID_ASUM || wp == ID_ADETAIL ||
            wp == ID_ICFLOP || wp == ID_ICCD || wp == ID_ICDVD) { draw_item((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->hwndFrom == g_list && ((NMHDR *)lp)->code == NM_CUSTOMDRAW)
            return list_draw((NMLVCUSTOMDRAW *)lp);
        if (((NMHDR *)lp)->hwndFrom == g_alist) {
            NMLISTVIEW *nm = (NMLISTVIEW *)lp;
            if (nm->hdr.code == LVN_ITEMCHANGED && (nm->uChanged & LVIF_STATE) &&
                (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
                on_pick((int)nm->lParam);
            else if (nm->hdr.code == NM_DBLCLK && pick_open())
                resume_pick();              /* double click on an open box: Riprendi */
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_running) {
            if (MessageBoxW(h, L"C'è un cofanetto in corso. Lo lascio aperto (si riprende dall'archivio) e chiudo?",
                            L"RetroArchiver", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            /* while the question was open the session may have ended */
            if (!g_running) { DestroyWindow(h); return 0; }
            g_closing = 1;
            runner_cancel();
            if (g_waiting) quit_waiting();
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_log) {        /* the log is read-only: it lands here */
            SetTextColor((HDC)wp, RGB(0xc8, 0xc8, 0xc8));
            SetBkColor((HDC)wp, RGB(0, 0, 0));
            return (LRESULT)g_black;
        }
        if (GetDlgCtrlID((HWND)lp) == ID_BAGPATH || GetDlgCtrlID((HWND)lp) == ID_AHINT) {
            SetBkMode((HDC)wp, TRANSPARENT);
            SetTextColor((HDC)wp, RGB(0x70, 0x70, 0x70));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_PAINT:
        paint_groups();
        return 0;
    case WM_THEMECHANGED:
        InvalidateRect(h, NULL, TRUE);
        break;
    case WM_DESTROY:
        place_save();
        KillTimer(h, TIMER_ID);
        KillTimer(h, TIMER_LINES);
        queue_drain(0);             /* the worker thread has already ended: nothing stays in memory */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ---- startup ---- */

static HFONT font_like(const LOGFONTW *base, int height, int weight, const wchar_t *face)
{
    LOGFONTW lf = *base;
    if (height) lf.lfHeight = height;
    lf.lfWeight = weight;
    if (face) { wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1); lf.lfFaceName[LF_FACESIZE - 1] = 0; }
    return CreateFontIndirectW(&lf);
}

/* XP has Lucida Console; if missing, Courier New */
static HFONT mono_font(void)
{
    wchar_t face[LF_FACESIZE];
    HFONT f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0,
                          FIXED_PITCH | FF_MODERN, L"Lucida Console");
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, f);
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    if (!_wcsicmp(face, L"Lucida Console")) return f;
    DeleteObject(f);
    return CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0,
                       FIXED_PITCH | FF_MODERN, L"Courier New");
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);    /* with WINVER 0x0501 this is already the XP size */
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0)) lf = ncm.lfMessageFont;
    else GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    g_font = CreateFontIndirectW(&lf);
    g_bold = font_like(&lf, 0, FW_BOLD, NULL);
    g_big = font_like(&lf, -17, FW_BOLD, L"Tahoma");
    g_num = font_like(&lf, -19, FW_BOLD, L"Tahoma");
    g_mid = font_like(&lf, -13, FW_BOLD, L"Tahoma");
    g_mono = mono_font();
    g_black = CreateSolidBrush(RGB(0, 0, 0));
}

static void free_fonts(void)
{
    DeleteObject(g_font); DeleteObject(g_bold); DeleteObject(g_big);
    DeleteObject(g_num); DeleteObject(g_mono); DeleteObject(g_black); DeleteObject(g_mid);
}

static void register_class(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    /* the 16 for the title bar is drawn specifically: request it by size */
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"RetroArchiver";
    RegisterClassExW(&wc);
}

/* 720x560 client area, reduced if the screen is smaller; centred */
static void default_rect(RECT *out)
{
    RECT wa, r = { 0, 0, 720, 560 };
    int w, h;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT);
    w = r.right - r.left; h = r.bottom - r.top;
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
    SetRect(out, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, 0, 0);
    out->right = out->left + w;
    out->bottom = out->top + h;
}

/* Shortcuts while waiting for a disk, for long sessions with many disks:
 * F5 or Alt+R reads (or retries), F2 or Alt+L reads the volume label again,
 * Alt+S skips. Plain letters would end up in the Label field, which has
 * the focus. Handled before IsDialogMessage; 1 if the key was used. */
static int wait_key(const MSG *m)
{
    int alt;
    if (!g_waiting || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)) return 0;
    if (m->hwnd != g_wnd && !IsChild(g_wnd, m->hwnd)) return 0;
    alt = m->message == WM_SYSKEYDOWN;
    if (m->wParam == VK_F5 || (alt && m->wParam == 'R')) {
        if (g_turn.readable) answer(CMD_READ);
        return 1;
    }
    if (m->wParam == VK_F2 || (alt && m->wParam == 'L')) { label_read(); return 1; }
    if (alt && m->wParam == 'S') { answer(CMD_SKIP); return 1; }
    return 0;
}

/* "RetroArchiver.exe --tty PROGRAM ARG...": the program launched by itself
 * as runner_run_tty's intermediary, with no window. -1 if the command line
 * is not that, otherwise the program's exit code */
static int tty_mode(void)
{
    int argc, i, rc; wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **u8;
    if (!wv) return -1;
    if (argc < 3 || wcscmp(wv[1], L"--tty")) { LocalFree(wv); return -1; }
    u8 = (char **)calloc((size_t)argc, sizeof(char *));
    for (i = 2; u8 && i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL);
        if ((u8[i - 2] = (char *)malloc((size_t)n)) != NULL)
            WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, u8[i - 2], n, NULL, NULL);
    }
    rc = u8 ? runner_tty_host((const char *const *)u8) : 255;
    LocalFree(wv);
    return rc;
}

/* this very program acts as the intermediary for SAMdisk */
static void set_tty_helper(void)
{
    wchar_t w[MAX_PATH]; char u8[MAX_PATH * 3];
    if (!GetModuleFileNameW(NULL, w, MAX_PATH)) return;
    to_u8(w, u8, sizeof(u8));
    runner_set_tty_helper(u8);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show_cmd)
{
    MSG msg; INITCOMMONCONTROLSEX icc; RECT r; int rc;
    (void)prev; (void)cmd;

    if ((rc = tty_mode()) >= 0) return rc;
    set_tty_helper();

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitialize(NULL);
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&g_qlock);
    make_fonts();
    load_uxtheme();
    register_class(inst);

    default_rect(&r);
    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"RetroArchiver", L"RetroArchiver - archiviazione",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            r.left, r.top, r.right - r.left, r.bottom - r.top,
                            NULL, NULL, inst, NULL);
    build_setup();
    build_work();
    build_archive();
    apply_limits();
    ini_load();
    samdisk_check();
    dic_check();
    bagpath_update();
    archive_show(NULL);                 /* start from the archive */
    if (!place_load(show_cmd)) ShowWindow(g_wnd, show_cmd);
    SetFocus(g_alist);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (wait_key(&msg)) continue;
        if (IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    free_fonts();
    CoUninitialize();
    return 0;
}
