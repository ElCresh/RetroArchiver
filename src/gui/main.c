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
 *
 * The texts come from lang.c, tr(T_...): the language chosen in setup or
 * Windows' one, English compiled in as the fallback. The core's texts (log
 * lines, the archive's outcomes and advice) stay as the core writes them.
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
#include "gui/lang.h"

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
#define WM_APP_DISKIN (WM_APP + 12) /* wParam: generation, lParam: FP_... from the floppy watcher */
#define OPT_MAX   1000              /* disk bar for CDs and DVDs: thousandths */

#define TIMER_ID  1
#define TIMER_SAMDISK 2             /* SAMdisk check ~600 ms after the last keystroke */
#define TIMER_ADIR 3                /* archive re-read ~600 ms after the last keystroke */
#define TIMER_LINES 4               /* SAMdisk lines: the window takes them in batches */
#define TIMER_DIC 5                 /* DiscImageCreator check ~600 ms after the last keystroke */
#define TIMER_FLOPPY 6              /* floppy drive check ~600 ms after the last keystroke */
#define TIMER_AUTO 7                /* automatic mode: the countdown before the read, every second */
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
    ID_HINT, ID_DIR, ID_BROWSE, ID_MAXREAD, ID_SAMDISK, ID_SOURCE, ID_DIC, ID_DRIVE, ID_SOUNDS, ID_AUTO, ID_BAGPATH,
    ID_UILANG, ID_PROT, ID_BACK, ID_START,
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
    int  twin;                  /* duplicate_of: the disk its read was the same as */
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
static int      g_aside_n;              /* files set aside in the closed bag */
static wchar_t  g_phase_name[128], g_last_err[600];
static wchar_t  g_drive_w[8];           /* the CD and DVD drive, "D:", for the texts */
static wchar_t  g_why[600];             /* why DiscImageCreator did not read the disc */
static DWORD    g_t_sess, g_t_phase;

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

/* "1 clean" / "2 clean": the two texts are T_... ids */
static void plural(wchar_t *out, int n, int k, int one, int many)
{
    _snwprintf(out, n, L"%d %ls", k, tr(k == 1 ? one : many));
    out[n - 1] = 0;
}

/* ---- media kinds ---- */

/* the name of a kind: "floppy", "floppies" (KN_MANY), "Floppy" (KN_CAP,
 * at the start of a sentence) */
enum { KN_ONE, KN_MANY, KN_CAP };
static const int kind_ids[MEDIA_KINDS][3] = {
    { T_KIND_FLOPPY, T_KIND_FLOPPY_PL, T_KIND_FLOPPY_CAP },
    { T_KIND_CD, T_KIND_CD_PL, T_KIND_CD_CAP },
    { T_KIND_DVD, T_KIND_DVD_PL, T_KIND_DVD_CAP }
};

static const wchar_t *kind_name(int kind, int form)
{
    return tr(kind_ids[kind][form]);
}

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
    _snwprintf(out, len, L"%ls %d", kind_name(kind, cap ? KN_CAP : KN_ONE), num);
    out[len - 1] = 0;
}

/* "3 floppies + 1 CD"; empty if there are no media */
static void comp_text(const int c[MEDIA_KINDS], wchar_t *out, int len)
{
    wchar_t p[64]; int k;
    out[0] = 0;
    for (k = 0; k < MEDIA_KINDS; k++) {
        if (!c[k]) continue;
        _snwprintf(p, 64, L"%ls%d %ls", out[0] ? L" + " : L"", c[k], kind_name(k, c[k] == 1 ? KN_ONE : KN_MANY));
        p[63] = 0;
        wcsncat(out, p, len - wcslen(out) - 1);
    }
}

/* all of the same kind: its name ("floppy"); otherwise "medium" or "media" */
static const wchar_t *noun(const int c[MEDIA_KINDS], int many)
{
    int k, kinds = 0, last = MEDIA_FLOPPY;
    for (k = 0; k < MEDIA_KINDS; k++) if (c[k]) { kinds++; last = k; }
    if (kinds > 1) return tr(many ? T_KIND_MIXED_PL : T_KIND_MIXED);
    return kind_name(last, many ? KN_MANY : KN_ONE);
}

/* the core names the phases in English ("Scan", "Read 2"): the window
 * keeps those names and shows them in its language */
static void phase_text(const wchar_t *p, wchar_t *out, int n)
{
    static const struct { const wchar_t *core; int id; } names[] = {
        { L"Scan", T_PHASE_SCAN }, { L"Compare", T_PHASE_COMPARE }, { L"C2 check", T_PHASE_C2 },
        { L"Redump", T_PHASE_REDUMP }, { L"TOC", T_PHASE_TOC }, { L"Outcome", T_PHASE_RESULT }, { NULL, 0 }
    };
    int i;
    if (!wcsncmp(p, L"Read ", 5)) _snwprintf(out, n, tr(T_PHASE_READ), _wtoi(p + 5));
    else {
        for (i = 0; names[i].core && wcscmp(p, names[i].core); i++) ;
        wcsncpy(out, names[i].core ? tr(names[i].id) : p, n);
    }
    out[n - 1] = 0;
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
    o->twin = d->duplicate_of;
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

/* the controls with a fixed text, set again when the language changes */
#define MAX_FIXED 80
static struct { int ctl, text; } g_fixed[MAX_FIXED];
static int g_nfixed;

static void fixed_add(int ctl, int text)
{
    if (g_nfixed >= MAX_FIXED) return;
    g_fixed[g_nfixed].ctl = ctl;
    g_fixed[g_nfixed].text = text;
    g_nfixed++;
}

/* make, with the text in the current language */
static HWND make_t(const wchar_t *cls, int text, DWORD style, int x, int y, int w, int h, int id)
{
    fixed_add(id, text);
    return make(cls, tr(text), style, x, y, w, h, id);
}

static void fixed_texts(void)
{
    int i;
    for (i = 0; i < g_nfixed; i++) SetDlgItemTextW(g_wnd, g_fixed[i].ctl, tr(g_fixed[i].text));
}

/* label (a program's name, the same in every language) and edit field */
static void field_w(const wchar_t *label, int y, int id, const wchar_t *value, int w)
{
    make(L"STATIC", label, 0, 12, y + 3, 110, 18, id + LBL);
    make(L"EDIT", value, WS_TABSTOP | ES_AUTOHSCROLL, 125, y, w, 21, id);
}

static void field(int label, int y, int id, const wchar_t *value, int w)
{
    field_w(tr(label), y, id, value, w);
    fixed_add(id + LBL, label);
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

/* the box's copy protection scan, META_PROT_...: the last choice, from the
 * ini, or the resumed box's */
static int g_prot = META_PROT_AUTO;

/* the menu in the current language, in the order of META_PROT_... */
static void prot_fill(void)
{
    HWND cb = GetDlgItem(g_wnd, ID_PROT);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)tr(T_SETUP_PROT_AUTO));
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)tr(T_SETUP_PROT_ON));
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)tr(T_SETUP_PROT_OFF));
    SendMessageW(cb, CB_SETCURSEL, g_prot, 0);
}

static int prot_chosen(void)
{
    int k = (int)SendDlgItemMessageW(g_wnd, ID_PROT, CB_GETCURSEL, 0, 0);
    return k >= META_PROT_AUTO && k <= META_PROT_OFF ? k : META_PROT_AUTO;
}

/* setup controls: ids from ID_TITLE to ID_START, labels + LBL.
 * Positions are those at 640 points; layout_setup widens the long fields */
static void build_setup(void)
{
    HWND cb; int i;
    wchar_t arc[MAX_PATH];

    field(T_SETUP_TITLE, 24, ID_TITLE, L"", 380);
    make_t(L"BUTTON", T_SETUP_RESUME, WS_TABSTOP, 520, 23, 95, 23, ID_RESUME);
    make_t(L"STATIC", T_SETUP_PLATFORM, 0, 12, 55, 110, 18, ID_PLATFORM + LBL);
    cb = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL, 125, 52, 200, 200, ID_PLATFORM);
    for (i = 0; platforms[i]; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)platforms[i]);
    make_t(L"STATIC", T_SETUP_YEAR, 0, 345, 55, 60, 18, ID_YEAR + LBL);
    make(L"EDIT", L"", WS_TABSTOP | ES_NUMBER, 410, 52, 50, 21, ID_YEAR);
    field(T_SETUP_PUBLISHER, 82, ID_PUBLISHER, L"", 200);
    make_t(L"STATIC", T_SETUP_LANGUAGE, 0, 345, 85, 60, 18, ID_LANGUAGE + LBL);
    make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 410, 82, 210, 21, ID_LANGUAGE);
    field(T_SETUP_EDITION, 110, ID_EDITION, L"", 200);
    field(T_SETUP_NOTES, 138, ID_NOTES, L"", 495);
    /* the make-up: how many floppies, CDs and DVDs, each with its kind's symbol */
    make_t(L"STATIC", T_SETUP_MEDIA, 0, 12, 169, 110, 18, ID_MEDIA);
    make(L"STATIC", L"", SS_OWNERDRAW, 125, 169, 14, 14, ID_ICFLOP);
    make_t(L"STATIC", T_KIND_FLOPPY_CAP, 0, 143, 169, 40, 18, ID_COUNT + LBL);
    make(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER, 185, 166, 36, 21, ID_COUNT);
    make(L"STATIC", L"", SS_OWNERDRAW, 239, 169, 14, 14, ID_ICCD);
    make_t(L"STATIC", T_KIND_CD_CAP, 0, 257, 169, 22, 18, ID_CD + LBL);
    make(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER, 281, 166, 36, 21, ID_CD);
    make(L"STATIC", L"", SS_OWNERDRAW, 335, 169, 14, 14, ID_ICDVD);
    make_t(L"STATIC", T_KIND_DVD_CAP, 0, 353, 169, 28, 18, ID_DVD + LBL);
    make(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER, 383, 166, 36, 21, ID_DVD);
    make_t(L"STATIC", T_SETUP_HINT, SS_NOPREFIX, 12, 198, 610, 18, ID_HINT);

    set_default_archive(arc, MAX_PATH);         /* with no ini at all: C:\RetroArchive */
    field(T_SETUP_FOLDER, 254, ID_DIR, arc, 460);
    make(L"BUTTON", L"...", WS_TABSTOP, 590, 254, 30, 21, ID_BROWSE);
    field(T_SETUP_MAXREAD, 282, ID_MAXREAD, L"3", 40);
    /* the program's own language, on the right of the short field */
    make_t(L"STATIC", T_SETUP_UILANG, SS_RIGHT, 330, 285, 140, 18, ID_UILANG + LBL);
    make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 474, 282, 150, 200, ID_UILANG);
    /* the two programs, each with where it reads from: same row, same label */
    field_w(L"SAMdisk", 310, ID_SAMDISK, L"samdisk", 300);
    make_t(L"STATIC", T_SETUP_READFROM, 0, 435, 313, 60, 18, ID_SOURCE + LBL);
    make(L"EDIT", L"a:", WS_TABSTOP | ES_AUTOHSCROLL, 500, 310, 120, 21, ID_SOURCE);
    field_w(L"DiscImageCreator", 338, ID_DIC, L"DiscImageCreator", 300);
    make_t(L"STATIC", T_SETUP_READFROM, 0, 435, 341, 60, 18, ID_DRIVE + LBL);
    make(L"EDIT", L"D:", WS_TABSTOP | ES_AUTOHSCROLL, 500, 338, 120, 21, ID_DRIVE);
    make(L"STATIC", L"", SS_NOPREFIX | SS_PATHELLIPSIS, 12, 369, 610, 18, ID_BAGPATH);

    make_t(L"BUTTON", T_SETUP_SOUNDS, WS_TABSTOP | BS_AUTOCHECKBOX, 12, 428, 140, 20, ID_SOUNDS);
    make_t(L"BUTTON", T_SETUP_AUTO, WS_TABSTOP | BS_AUTOCHECKBOX, 160, 428, 260, 20, ID_AUTO);
    /* the box's copy protection scan, under Automatico */
    make_t(L"STATIC", T_SETUP_PROT, 0, 160, 457, 125, 18, ID_PROT + LBL);
    make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 289, 454, 130, 200, ID_PROT);
    prot_fill();
    make_t(L"BUTTON", T_SETUP_ARCHIVE, WS_TABSTOP, 426, 423, 100, 28, ID_BACK);
    make_t(L"BUTTON", T_SETUP_START, WS_TABSTOP | BS_DEFPUSHBUTTON, 534, 423, 100, 28, ID_START);
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

static void add_column(HWND lv, int i, int name, int w, int fmt)
{
    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = fmt;
    c.cx = w;
    c.pszText = (wchar_t *)tr(name);
    SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
}

/* the column titles of the two lists, for a language change */
static const int list_cols[] = { T_COL_MEDIUM, T_COL_LABEL, T_COL_READS, T_COL_OUTCOME, -1 };
static const int arc_cols[] = { T_COL_TITLE, T_COL_PLATFORM, T_COL_YEAR, T_COL_MEDIA, T_COL_OUTCOME,
                                T_COL_ARCHIVED, -1 };

static void column_titles(HWND lv, const int *ids)
{
    LVCOLUMNW c;
    int i;
    for (i = 0; ids[i] >= 0; i++) {
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT;
        c.pszText = (wchar_t *)tr(ids[i]);
        SendMessageW(lv, LVM_SETCOLUMNW, i, (LPARAM)&c);
    }
}

static void build_list(void)
{
    g_list = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL |
                  LVS_NOSORTHEADER, 8, 100, 600, 200, ID_LIST);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SendMessageW(g_list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)make_dots());
    add_column(g_list, 0, list_cols[0], 130, LVCFMT_LEFT);
    add_column(g_list, 1, list_cols[1], 200, LVCFMT_LEFT);
    add_column(g_list, 2, list_cols[2], 60, LVCFMT_RIGHT);
    add_column(g_list, 3, list_cols[3], 190, LVCFMT_LEFT);
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
    make_t(L"STATIC", T_ARC_FOLDER, 0, 8, 0, 52, 18, ID_ADIR + LBL);
    make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 62, 0, 300, 21, ID_ADIR);
    make(L"BUTTON", L"...", WS_TABSTOP, 0, 0, 30, 21, ID_ABROWSE);
    make_t(L"STATIC", T_ARC_SEARCH, 0, 0, 0, 34, 18, ID_ASEARCH + LBL);
    make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 150, 21, ID_ASEARCH);
    make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL, 0, 0, 150, 240, ID_APLAT);
    g_asum = make(L"STATIC", L"", SS_OWNERDRAW, 8, 0, 600, 22, ID_ASUM);
    g_alist = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL |
                   LVS_NOSORTHEADER, 8, 0, 600, 200, ID_ALIST);
    SendMessageW(g_alist, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SendMessageW(g_alist, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)make_dots());
    add_column(g_alist, 0, arc_cols[0], 220, LVCFMT_LEFT);
    add_column(g_alist, 1, arc_cols[1], 92, LVCFMT_LEFT);
    add_column(g_alist, 2, arc_cols[2], 44, LVCFMT_LEFT);
    add_column(g_alist, 3, arc_cols[3], 132, LVCFMT_LEFT);
    add_column(g_alist, 4, arc_cols[4], 168, LVCFMT_LEFT);
    add_column(g_alist, 5, arc_cols[5], 76, LVCFMT_LEFT);
    g_adetail = make(L"STATIC", L"", SS_OWNERDRAW, 0, 0, 400, 140, ID_ADETAIL);
    make_t(L"BUTTON", T_ARC_RESUME, WS_TABSTOP, 0, 0, 130, 26, ID_ARESUME);
    make_t(L"BUTTON", T_ARC_OPEN, WS_TABSTOP, 0, 0, 130, 26, ID_AOPEN);
    make_t(L"BUTTON", T_ARC_VERIFY, WS_TABSTOP, 0, 0, 130, 26, ID_AVERIFY);
    make_t(L"STATIC", T_ARC_HINT, SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 400, 18, ID_AHINT);
    make_t(L"BUTTON", T_ARC_NEW, WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 150, 28, ID_ANEW);
    SendDlgItemMessageW(g_wnd, ID_ARESUME, WM_SETFONT, (WPARAM)g_bold, TRUE);
    SendDlgItemMessageW(g_wnd, ID_ANEW, WM_SETFONT, (WPARAM)g_bold, TRUE);
}

/* the square magnifier button next to the Etichetta field: the magnifier
 * is the Webdings "L", which XP has too; the tooltip says what it does */
static HWND g_tip;

static void label_tip(void)
{
    TOOLINFOW ti;
    if (!g_tip) return;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = TTTOOLINFOW_V2_SIZE;        /* works with common controls 5 too */
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_wnd;
    ti.uId = (UINT_PTR)GetDlgItem(g_wnd, ID_LABELREAD);
    ti.lpszText = (wchar_t *)tr(T_WAIT_LABEL_TIP);
    SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

static void label_button(void)
{
    HWND b = make(L"BUTTON", L"L", WS_TABSTOP, 0, 0, 24, 21, ID_LABELREAD);
    TOOLINFOW ti;
    HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, SYMBOL_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Webdings");
    if (f) SendMessageW(b, WM_SETFONT, (WPARAM)f, TRUE);
    g_tip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0,
                            g_wnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_tip) return;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_wnd;
    ti.uId = (UINT_PTR)b;
    ti.lpszText = (wchar_t *)tr(T_WAIT_LABEL_TIP);
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

/* running session: banner, phases, bars, list, log, buttons */
static void build_work(void)
{
    g_banner = make(L"STATIC", L"", SS_OWNERDRAW, 8, 8, 600, 64, ID_BANNER);
    g_pills = make(L"STATIC", L"", SS_OWNERDRAW, 8, 80, 600, 24, ID_PILLS);
    make_t(L"STATIC", T_WAIT_LABEL, 0, 8, 0, 60, 18, ID_LABEL + LBL);
    g_label = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 70, 0, 200, 21, ID_LABEL);
    label_button();
    make_t(L"STATIC", T_WAIT_NOTE, 0, 280, 0, 40, 18, ID_NOTE + LBL);
    g_note = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 320, 0, 280, 21, ID_NOTE);
    build_list();
    make_t(L"BUTTON", T_BTN_FINISH, WS_TABSTOP, 0, 0, 110, 26, ID_FINISH);
    make_t(L"BUTTON", T_BTN_SKIP, WS_TABSTOP, 0, 0, 90, 26, ID_SKIP);
    make_t(L"BUTTON", T_BTN_READ, WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 90, 26, ID_READ);
    make_t(L"BUTTON", T_BTN_STOP, WS_TABSTOP, 0, 0, 100, 26, ID_STOP);
    make_t(L"BUTTON", T_BTN_OPEN, WS_TABSTOP, 0, 0, 110, 26, ID_OPEN);
    make_t(L"BUTTON", T_BTN_NEW, WS_TABSTOP, 0, 0, 120, 26, ID_NEW);
    make_t(L"BUTTON", T_BTN_ARCHIVE, WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 100, 26, ID_TOARCH);

    make_t(L"STATIC", T_BAR_DISK, 0, 0, 0, 60, 18, ID_DISKBAR + LBL);
    g_diskbar = make(PROGRESS_CLASSW, L"", 0, 0, 0, 300, 16, ID_DISKBAR);
    SendMessageW(g_diskbar, PBM_SETRANGE32, 0, CYL_MAX);
    g_cyltxt = make(L"STATIC", L"", SS_LEFT, 0, 0, 120, 18, ID_CYLTXT);
    g_elapsed = make(L"STATIC", L"", SS_RIGHT, 0, 0, 52, 18, ID_ELAPSED);
    make_t(L"STATIC", T_BAR_BOX, 0, 0, 0, 60, 18, ID_BOXBAR + LBL);
    g_boxbar = make(PROGRESS_CLASSW, L"", 0, 0, 0, 300, 16, ID_BOXBAR);
    g_boxtxt = make(L"STATIC", L"", SS_LEFT, 0, 0, 180, 18, ID_BOXTXT);

    g_log = make(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                 8, 300, 600, 150, ID_LOG);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_mono, TRUE);
    /* without this the EDIT stops at about 32K characters and rejects the rest */
    SendMessageW(g_log, EM_SETLIMITTEXT, LOG_LIMIT, 0);

    make_t(L"STATIC", T_BAG_FOLDER, 0, 0, 0, 80, 18, ID_BAGDIR + LBL);
    make(L"STATIC", L"", SS_NOPREFIX | SS_PATHELLIPSIS, 0, 0, 400, 18, ID_BAGDIR);
    make_t(L"STATIC", T_BAG_CONTENTS, 0, 0, 0, 80, 18, ID_BAGCONT + LBL);
    make(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 400, 18, ID_BAGCONT);
    make_t(L"STATIC", T_BAG_HASHES, 0, 0, 0, 80, 18, ID_BAGHASH + LBL);
    make(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 400, 18, ID_BAGHASH);
    make_t(L"STATIC", T_BAG_ASIDE, 0, 0, 0, 80, 18, ID_BAGASIDE + LBL);
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
    /* a translation may start with the medium's name ("%ls is identical..."):
     * the first letter of a sentence is a capital */
    CharUpperBuffW(g_ban.title, 1);
    if (g_ban.text[0]) CharUpperBuffW(g_ban.text, 1);
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
        if (!wcsncmp(g_pill[i], L"Read ", 5) && _wtoi(g_pill[i] + 5) > k) k = _wtoi(g_pill[i] + 5);
    return k;
}

/* the phases expected after the running one: corrected as they arrive */
static int cur_optical(void);
static int cur_cd(void);

/* CDs and DVDs: TOC, Read 1, C2 check (CD only), Redump; if redump
 * does not know it, Read 2, C2 check, Compare */
static void pills_future_optical(void)
{
    const wchar_t *p = g_pill[g_pcur];
    int reads = last_read();
    g_npill = g_pcur + 1;
    if (!wcscmp(p, L"TOC")) {
        pill_add(L"Read 1");
        if (cur_cd()) pill_add(L"C2 check");
        pill_add(L"Redump");
    } else if (!wcsncmp(p, L"Read ", 5)) {
        if (cur_cd()) pill_add(L"C2 check");
        pill_add(_wtoi(p + 5) == 1 ? L"Redump" : L"Compare");
    } else if (!wcscmp(p, L"C2 check")) {
        pill_add(reads <= 1 ? L"Redump" : L"Compare");
    }
    pill_add(L"Outcome");
}

static void pills_future(void)
{
    const wchar_t *p = g_pill[g_pcur];
    if (cur_optical()) { pills_future_optical(); return; }
    g_npill = g_pcur + 1;
    if (!wcscmp(p, L"Scan")) {
        pill_add(L"Read 1");
        if (!g_fragile) { pill_add(L"Read 2"); pill_add(L"Compare"); }
    } else if (!wcsncmp(p, L"Read ", 5) && !g_fragile) {
        if (_wtoi(p + 5) == 1) pill_add(L"Read 2");
        pill_add(L"Compare");
    }
    pill_add(L"Outcome");
}

static void pills_phase(const wchar_t *name)
{
    if (!wcscmp(name, L"Scan") || !wcscmp(name, L"TOC") || g_pcur < 0) g_npill = g_pcur = 0;
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
    _snwprintf(w, 24, L"Read %d", last_read() + 1); w[23] = 0;
    pill_add(w);
    if (cur_cd()) pill_add(L"C2 check");
    pill_add(L"Compare");
    pill_add(L"Outcome");
    repaint(g_pills);
}

static void pills_done(void)
{
    if (g_pcur < 0) return;
    if (g_pcur < MAXPILL - 1) g_pcur++;
    wcscpy(g_pill[g_pcur], L"Outcome");
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
    SIZE sz; wchar_t t[40];
    phase_text(g_pill[i], t, 40);
    SelectObject(dc, i == g_pcur ? g_bold : g_font);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    return sz.cx + 22;
}

static void paint_pill(HDC dc, int i, int x, int w, int h)
{
    COLORREF bg, edge, fg;
    HBRUSH br; HPEN pen; HGDIOBJ op, ob;
    RECT r = { x, 0, x + w, h };
    wchar_t t[40];
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
    phase_text(g_pill[i], t, 40);
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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

static const int group_title[G_COUNT] = { T_GROUP_BOX, T_GROUP_ARCHIVING, T_GROUP_BAG, T_GROUP_SELECTED };
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
    const wchar_t *t = tr(group_title[i]);
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
    _snwprintf(out, n, tr(g_arc_missing ? T_EMPTY_MISSING : T_EMPTY_NONE), dir);
    out[n - 1] = 0;
}

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
    DrawTextW(dc, tr(T_EMPTY_TITLE), -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(0x33, 0x33, 0x33));
    empty_text(t, 900);
    SetRect(&r, cx - EMPTY_TEXT_W / 2, g_emp.text_y, cx + EMPTY_TEXT_W / 2, g_emp.text_y + g_emp.text_h);
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    SetTextColor(dc, RGB(0x44, 0x44, 0x44));
    SetRect(&r, g_empty.left + 8, g_emp.hint_y, g_empty.right - 8, g_emp.hint_y + 16);
    DrawTextW(dc, tr(T_EMPTY_HINT), -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
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
    put(ID_UILANG + LBL, R - 150 - 8 - 160, 285, 160, 18);
    put(ID_UILANG, R - 150, 282, 150, 200);
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
    put(ID_SOUNDS, 12, 411, 140, 20);
    put(ID_AUTO, 160, 411, 260, 20);
    put(ID_PROT + LBL, 160, 440, 125, 18);
    put(ID_PROT, 289, 437, 130, 200);
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

/* the width of a label's text, plus a little margin */
static int text_width(int id)
{
    wchar_t t[128]; SIZE sz; HDC dc; HGDIOBJ o;
    GetDlgItemTextW(g_wnd, id, t, 128);
    dc = GetDC(g_wnd);
    o = SelectObject(dc, g_font);
    if (!GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz)) sz.cx = 34;
    SelectObject(dc, o);
    ReleaseDC(g_wnd, dc);
    return sz.cx + 4;
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
    /* folder | ... | Cerca | platform: only the folder widens; the label
     * is as wide as its text, which changes with the language */
    put(ID_APLAT, right - 150, y, 150, 240);
    put(ID_ASEARCH, right - 306, y, 150, 21);
    k = text_width(ID_ASEARCH + LBL);
    x = right - 306 - 4 - k;
    put(ID_ASEARCH + LBL, x, y + 3, k, 18);
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

static const struct { const wchar_t *sec, *key; int id; } ini_keys[] = {
    { SEC_GENERAL, KEY_FOLDER, ID_DIR }, { SEC_GENERAL, KEY_MAXREADS, ID_MAXREAD },
    { SEC_FLOPPY, KEY_SAMDISK, ID_SAMDISK }, { SEC_FLOPPY, KEY_READFROM, ID_SOURCE },
    { SEC_BOX, KEY_PLATFORM, ID_PLATFORM },
    { SEC_OPTICAL, KEY_DIC, ID_DIC }, { SEC_OPTICAL, KEY_DRIVE, ID_DRIVE },
    { SEC_BOX, KEY_LANGUAGE, ID_LANGUAGE },
    { NULL, NULL, 0 }
};

/* the file naming patterns, per kind: FLOPPY01.dsk, CD01\, DVD01\ */
static const wchar_t *const name_secs[MEDIA_KINDS] = { SEC_FLOPPY, SEC_OPTICAL, SEC_OPTICAL };
static const wchar_t *const name_keys[MEDIA_KINDS] = { KEY_FLOPPYNAME, KEY_CDNAME, KEY_DVDNAME };
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
    char prot_key[16];
    int i;
    set_init(&g_set);
    for (i = 0; ini_keys[i].key; i++) {
        GetDlgItemTextW(g_wnd, ini_keys[i].id, cur, 512);   /* the default stays the current one */
        wcscpy(g_builtin[i], cur);
        set_get(&g_set, ini_keys[i].sec, ini_keys[i].key, cur, v, 512);
        SetDlgItemTextW(g_wnd, ini_keys[i].id, v);
    }
    tool_fallback(ID_SAMDISK, SET_TOOL_SAMDISK);
    tool_fallback(ID_DIC, SET_TOOL_DIC);
    /* the checkbox is not a text: Suoni=1 or 0 */
    CheckDlgButton(g_wnd, ID_SOUNDS,
                   set_int(&g_set, SEC_GENERAL, KEY_SOUNDS, 0) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_AUTO,
                   set_int(&g_set, SEC_AUTO, KEY_ENABLED, 0) ? BST_CHECKED : BST_UNCHECKED);
    /* the copy protection scan: auto, on, off */
    set_get(&g_set, SEC_BOX, KEY_COPYPROT, L"auto", v, 512);
    to_u8(v, prot_key, sizeof(prot_key));
    g_prot = meta_prot_parse(prot_key) < 0 ? META_PROT_AUTO : meta_prot_parse(prot_key);
    SendDlgItemMessageW(g_wnd, ID_PROT, CB_SETCURSEL, g_prot, 0);
    /* the file names are not in the window: they change only here */
    for (i = 0; i < MEDIA_KINDS; i++) {
        set_get(&g_set, name_secs[i], name_keys[i], name_defaults[i], v, 512);
        to_u8(v[0] ? v : name_defaults[i], g_ini_names[i], sizeof(g_ini_names[i]));
    }
}

/* a window field into the user's ini, if different from the default */
static void ini_store(int i)
{
    wchar_t v[512];
    GetDlgItemTextW(g_wnd, ini_keys[i].id, v, 512);
    set_save(&g_set, ini_keys[i].sec, ini_keys[i].key, g_builtin[i], v);
}

static void ini_save(void)
{
    wchar_t v[512];
    int i;
    for (i = 0; ini_keys[i].key; i++) ini_store(i);
    set_save(&g_set, SEC_GENERAL, KEY_SOUNDS, L"0",
             IsDlgButtonChecked(g_wnd, ID_SOUNDS) == BST_CHECKED ? L"1" : L"0");
    set_save(&g_set, SEC_AUTO, KEY_ENABLED, L"0",
             IsDlgButtonChecked(g_wnd, ID_AUTO) == BST_CHECKED ? L"1" : L"0");
    g_prot = prot_chosen();
    to_w(meta_prot_key(g_prot), v, 512);
    set_save(&g_set, SEC_BOX, KEY_COPYPROT, L"auto", v);
    /* the names change only by hand in the ini: one that differs from the default stays */
    for (i = 0; i < MEDIA_KINDS; i++) {
        to_w(g_ini_names[i], v, 512);
        set_save(&g_set, name_secs[i], name_keys[i], name_defaults[i], v);
    }
}

/* window position and size, in work-area coordinates as
 * GetWindowPlacement gives them */
static void place_save(void)
{
    WINDOWPLACEMENT wp;
    RECT *r = &wp.rcNormalPosition;
    wp.length = sizeof(wp);
    if (!g_set.user[0] || !GetWindowPlacement(g_wnd, &wp)) return;
    set_put_int(&g_set, SEC_WINDOW, L"X", r->left);
    set_put_int(&g_set, SEC_WINDOW, L"Y", r->top);
    set_put_int(&g_set, SEC_WINDOW, L"Width", r->right - r->left);
    set_put_int(&g_set, SEC_WINDOW, L"Height", r->bottom - r->top);
    set_put_int(&g_set, SEC_WINDOW, L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED ||
                (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED)));
}

/* the saved position applies only if the window fits entirely on one screen
 * (a monitor removed, a lower resolution): otherwise the default one */
static int place_load(int show_cmd)
{
    WINDOWPLACEMENT wp; MONITORINFO mi; HMONITOR mon; RECT r, wa;
    int x = set_int(&g_set, SEC_WINDOW, L"X", -32000);
    int y = set_int(&g_set, SEC_WINDOW, L"Y", -32000);
    int w = set_int(&g_set, SEC_WINDOW, L"Width", 0);
    int h = set_int(&g_set, SEC_WINDOW, L"Height", 0);
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
    wp.showCmd = set_int(&g_set, SEC_WINDOW, L"Maximized", 0) ? SW_SHOWMAXIMIZED : show_cmd;
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
    case SD_CHECKING: wcsncpy(out, tr(T_SD_CHECKING), n); break;
    case SD_OK:       _snwprintf(out, n, tr(T_SD_OK), g_sd_ver); break;
    case SD_NOTFOUND: wcsncpy(out, tr(T_SD_NOTFOUND), n); break;
    case SD_V4:       wcsncpy(out, tr(T_SD_V4), n); break;
    default:          wcsncpy(out, tr(T_SD_NOTSAM), n); break;
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
    case FD_CHECKING: _snwprintf(out, n, tr(T_FD_CHECKING), g_fd_drive); break;
    case FD_OK:       _snwprintf(out, n, tr(T_FD_OK), g_fd_drive, g_fd_rpm); break;
    case FD_DEAD:     _snwprintf(out, n, tr(T_FD_DEAD), g_fd_drive); break;
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

typedef struct { LONG gen; int remount; wchar_t root[8]; char file[512]; } lb_job;
typedef struct { int state, has_serial; DWORD serial; wchar_t label[64]; wchar_t root[8]; } lb_result;

static volatile LONG g_lb_gen;
static wchar_t g_lb_msg[200];          /* the outcome, in the status bar while waiting for the disk */

static void sbar_text(int part, const wchar_t *t);
static void focus_to(HWND h);

static int g_auto;                      /* the running session is in automatic mode */

/* the automatic mode's questions to fdrawcmd clear the drive's disk-change
 * line, so Windows does not know the floppy was swapped and answers with
 * the label and serial number of the previous one (seen on the VM). A
 * dismounted volume is mounted again, from the disk, on the next access */
static void volume_remount(const wchar_t *root)
{
    wchar_t dev[8]; HANDLE h; DWORD got;
    _snwprintf(dev, 8, L"\\\\.\\%c:", root[0]); dev[7] = 0;
    h = CreateFileW(dev, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL);
    CloseHandle(h);
}

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
            if (job->remount) volume_remount(job->root);
            if (GetVolumeInformationW(job->root, r->label, 64, &r->serial, NULL, NULL, NULL, 0)) {
                r->state = r->label[0] ? LB_FOUND : LB_NONE;
                r->has_serial = 1;
            } else {
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

static void watch_pause(void);

static void label_read(void)
{
    lb_job *job; char src[512]; HANDLE th; const wchar_t *d;
    if (!g_waiting || !(job = (lb_job *)calloc(1, sizeof(lb_job)))) return;
    watch_pause();               /* Windows cannot reach A: while fdrawcmd holds the controller */
    job->gen = InterlockedIncrement(&g_lb_gen);
    if (g_turn.kind == MEDIA_FLOPPY) {
        get_u8_trim(ID_SOURCE, src, sizeof(src));
        if (!src[0]) str_copy(src, sizeof(src), "a:");
        if (floppy_source_is_drive(src)) {
            while (*src == ' ') memmove(src, src + 1, strlen(src));
            _snwprintf(job->root, 8, L"%c:\\", (wchar_t)toupper((unsigned char)src[0]));
            job->remount = g_auto;
        } else {
            str_copy(job->file, sizeof(job->file), src);
            wcscpy(job->root, L"A:\\");
        }
    } else {
        d = g_drive_w[0] ? g_drive_w : L"D:";
        _snwprintf(job->root, 8, L"%c:\\", d[0]);
    }
    job->root[7] = 0;
    wcsncpy(g_lb_msg, tr(T_LB_READING), 199); g_lb_msg[199] = 0;
    sbar_text(0, g_lb_msg);
    th = CreateThread(NULL, 0, label_worker, job, 0, NULL);
    if (th) CloseHandle(th); else free(job);
}

/* ---- automatic mode ----
 * With the Automatico checkbox the window starts the read by itself when a
 * new disk is in the drive: label, a countdown of AUTO_SECONDS that Esc
 * stops, then Leggi. A new disk, not just a disk: after a read the same one
 * stays in the drive, so a change is needed (removed and inserted, or
 * swapped). Floppies: a thread asks fdrawcmd.sys (IOCTL_FD_CHECK_DISK, tried
 * first with tests/xp/fdcheck.c), only while waiting, never while SAMdisk
 * reads. CDs and DVDs: Windows' WM_DEVICECHANGE. Before reading, the volume
 * serial number: the same as a disk already read in this box stops the
 * automatic start and asks. */

#define AUTO_SECONDS 5
/* a "present" answer slower than this found the disk-change line raised:
 * the driver seeks to clear it, so the disk was swapped in the meantime */
#define FD_SWAP_MS 25
#define FD_POLL_PRESENT 1000            /* waiting for the disk just read to be removed */
#define FD_POLL_ABSENT 2000             /* empty drive: each question may move the head */

#define FD_CTL_CODE(i, m) CTL_CODE(FILE_DEVICE_UNKNOWN, i, m, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_FD_CHECK_DISK FD_CTL_CODE(0x917, METHOD_BUFFERED)   /* fdrawcmd.sys 1.0.1.10 and later */

enum { FP_NEW, FP_GONE, FP_NODRIVER, FP_SLOWDRIVE };
/* off for this turn, waiting for a new disk, reading its label, counting
 * down, asking because it looks like a disk already read */
enum { AUTO_OFF, AUTO_WATCH, AUTO_LABEL, AUTO_COUNT, AUTO_ASK };

typedef struct { LONG gen; int drive, armed, prev; HANDLE stop; } fp_job;
typedef struct { int ok; DWORD serial; wchar_t label[64]; } vol_id;

static int      g_auto_state = AUTO_OFF, g_auto_left;
static int      g_fd_index = -1;        /* fdraw number of "Leggi da"; -1 for a .dsk file */
static int      g_fd_used, g_opt_used;  /* a disk of that kind was read since the program started */
static volatile LONG g_fp_gen;
static volatile LONG g_fd_slow;         /* every answer of the drive is slow: only removals count */
static HANDLE   g_fp_thread, g_fp_stop;
static LONG     g_lb_auto;              /* the label read started by the automatic mode */
static wchar_t  g_auto_label[64];       /* what it wrote in the field */
static vol_id   g_vol_turn;             /* the disk in the drive, from the last label read */
static vol_id   g_vol[META_MAXDISK + 1];        /* the disks read in this box */

static void answer(int cmd);
static void wait_banner(void);

static void auto_status(const wchar_t *t)
{
    wcsncpy(g_lb_msg, t, 199); g_lb_msg[199] = 0;
    sbar_text(0, g_lb_msg);
}

static double ms_now(void)
{
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&c)) return GetTickCount();
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* 1 present, 0 absent, -1 error; *ms how long the driver took */
static int fd_ask(HANDLE h, double *ms)
{
    DWORD got, e; double t0 = ms_now();
    BOOL ok = DeviceIoControl(h, IOCTL_FD_CHECK_DISK, NULL, 0, NULL, 0, &got, NULL);
    e = ok ? 0 : GetLastError();
    *ms = ms_now() - t0;
    if (ok) return 1;
    return e == ERROR_NO_MEDIA_IN_DRIVE ? 0 : -1;
}

static void fp_post(LONG gen, int what)
{
    PostMessageW(g_wnd, WM_APP_DISKIN, (WPARAM)gen, (LPARAM)what);
}

/* a slow "present" is a swap only if the question right after is fast: on
 * a drive whose every answer is slow the disk would always look new */
static int fd_swapped(HANDLE h, double ms, LONG gen)
{
    double again;
    if (ms <= FD_SWAP_MS || g_fd_slow) return 0;
    if (fd_ask(h, &again) != 1) return 0;
    if (again <= FD_SWAP_MS) return 1;
    InterlockedExchange(&g_fd_slow, 1);
    fp_post(gen, FP_SLOWDRIVE);
    return 0;
}

/* one question with the drive opened just for it: while \\.\fdrawN is
 * open fdrawcmd.sys holds the controller, and Windows' own floppy driver
 * (the label read on A:) waits until it is closed (seen on the VM).
 * may_swap: a slow answer can mean a swap. -2 if fdrawcmd.sys is not there */
static int fd_check(const wchar_t *path, int may_swap, int *swapped, int *fast, LONG gen)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    double ms; int st; DWORD e;
    *swapped = *fast = 0;
    if (h == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? -2 : -1;
    }
    st = fd_ask(h, &ms);
    if (st == 1) {
        *fast = ms <= FD_SWAP_MS;
        if (may_swap) *swapped = fd_swapped(h, ms, gen);
    }
    CloseHandle(h);
    return st;
}

/* armed: a disk already present counts as new (the first one of the run).
 * A slow answer counts as a swap only after a fast one of the same run:
 * the first question after SAMdisk or the label read is slow anyway, the
 * controller has just changed hands (seen on the VM: the countdown kept
 * starting again on the same disk) */
static DWORD WINAPI fd_watch(LPVOID p)
{
    fp_job *job = (fp_job *)p;
    wchar_t path[32];
    int st, prev = job->prev, armed = job->armed, errs = 0, swapped, fast = 0, was_fast = 0;
    _snwprintf(path, 32, L"\\\\.\\fdraw%d", job->drive); path[31] = 0;
    for (;;) {
        st = fd_check(path, was_fast, &swapped, &fast, job->gen);
        if (st == 1 && (armed || prev == 0 || swapped)) {
            fp_post(job->gen, FP_NEW);
            armed = 0;
        } else if (st == 0 && prev == 1) {
            fp_post(job->gen, FP_GONE);
        }
        /* no driver, or an older one without the question: it never answers */
        if (st == -2 || (st < 0 && ++errs >= 3)) { fp_post(job->gen, FP_NODRIVER); break; }
        if (st >= 0) { prev = st; errs = 0; was_fast = fast; }
        if (WaitForSingleObject(job->stop, st == 0 ? FD_POLL_ABSENT : FD_POLL_PRESENT) != WAIT_TIMEOUT) break;
    }
    free(job);
    return 0;
}

/* the drive must be free before SAMdisk opens it: waits for the thread */
static void watch_stop(void)
{
    InterlockedIncrement(&g_fp_gen);
    if (!g_fp_thread) return;
    SetEvent(g_fp_stop);
    WaitForSingleObject(g_fp_thread, 5000);
    CloseHandle(g_fp_thread);
    CloseHandle(g_fp_stop);
    g_fp_thread = g_fp_stop = NULL;
}

/* present: the disk is known to be in the drive (after a label read), only
 * its removal or a swap counts */
static void watch_start(int present)
{
    fp_job *job;
    watch_stop();
    if (g_fd_index < 0 || !(job = (fp_job *)calloc(1, sizeof(fp_job)))) return;
    if (!(g_fp_stop = CreateEventW(NULL, TRUE, FALSE, NULL))) { free(job); return; }
    job->gen = InterlockedIncrement(&g_fp_gen);
    job->drive = g_fd_index;
    job->armed = !g_fd_used && !present;
    job->prev = present ? 1 : -1;
    job->stop = g_fp_stop;
    if (!(g_fp_thread = CreateThread(NULL, 0, fd_watch, job, 0, NULL))) {
        free(job);
        CloseHandle(g_fp_stop);
        g_fp_stop = NULL;
    }
}

/* Windows' floppy driver waits for the controller while the watcher keeps
 * asking (even with the drive closed between questions: seen on the VM, the
 * label read hung until the watcher stopped). It pauses for the label read
 * and goes on after it, knowing the disk is in */
static int g_fp_paused;

static void watch_pause(void)
{
    if (!g_fp_thread) return;
    watch_stop();
    g_fp_paused = 1;
}

static void watch_resume(void)
{
    if (!g_fp_paused) return;
    g_fp_paused = 0;
    if (g_waiting && g_auto_state != AUTO_OFF && g_turn.kind == MEDIA_FLOPPY) watch_start(1);
}

/* the automatic mode applies to the disk being asked for */
static int auto_turn(void)
{
    if (!g_auto || !g_waiting || !g_turn.readable) return 0;
    return g_turn.kind != MEDIA_FLOPPY || g_fd_index >= 0;
}

/* the drive's letter, "A:" or "D:", for the texts */
static void auto_drive(wchar_t *out)
{
    out[0] = g_turn.kind == MEDIA_FLOPPY ? (wchar_t)(L'A' + g_fd_index) : (g_drive_w[0] ? g_drive_w[0] : L'D');
    out[1] = L':'; out[2] = 0;
}

/* a new disk is in the drive: its label and serial number first */
static void auto_found(void)
{
    KillTimer(g_wnd, TIMER_AUTO);
    g_auto_state = AUTO_LABEL;
    label_read();
    g_lb_auto = g_lb_gen;
}

/* back to waiting for a new disk: the disk was removed */
static void auto_rewatch(const wchar_t *why)
{
    int was = g_auto_state;
    KillTimer(g_wnd, TIMER_AUTO);
    g_auto_state = AUTO_WATCH;
    if (was != AUTO_WATCH) wait_banner();
    auto_status(why);
}

/* off for this disk: Leggi by hand */
static void auto_manual(const wchar_t *why)
{
    KillTimer(g_wnd, TIMER_AUTO);
    watch_stop();
    g_fp_paused = 0;
    if (g_auto_state == AUTO_OFF) return;
    g_auto_state = AUTO_OFF;
    wait_banner();
    auto_status(why);
}

/* leaving the wait (Leggi, Salta, closing): nothing keeps running */
static void auto_leave(void)
{
    KillTimer(g_wnd, TIMER_AUTO);
    watch_stop();
    g_fp_paused = 0;
    g_auto_state = AUTO_OFF;
}

/* entering the wait, from on_wait */
static void auto_begin(void)
{
    wchar_t w[200], d[3];
    auto_leave();
    memset(&g_vol_turn, 0, sizeof(g_vol_turn));
    g_auto_label[0] = 0;
    if (!auto_turn()) return;
    g_auto_state = AUTO_WATCH;
    auto_drive(d);
    if (g_turn.kind == MEDIA_FLOPPY) watch_start(0);
    else if (!g_opt_used) { auto_found(); return; }    /* the first disc may already be in */
    _snwprintf(w, 200, tr(T_AUTO_WAITING), d); w[199] = 0;
    auto_status(w);
}

/* another slot of the box read from a disk with the same serial number and
 * label: probably the same disk again. 0 if none, or no serial number */
static int vol_twin(void)
{
    int k;
    if (!g_vol_turn.ok) return 0;
    for (k = 1; k <= g_total && k <= META_MAXDISK; k++)
        if (k != g_cur && g_vol[k].ok && g_vol[k].serial == g_vol_turn.serial &&
            !wcscmp(g_vol[k].label, g_vol_turn.label)) return k;
    return 0;
}

static void count_banner(void)
{
    wchar_t t[160], s[500], who[32], lab[128];
    slot_name(g_cur, 0, who, 32);
    if (g_auto_left == 1) _snwprintf(t, 160, tr(T_AUTO_COUNT_ONE), who);
    else _snwprintf(t, 160, tr(T_AUTO_COUNT_MANY), who, g_auto_left);
    t[159] = 0;
    GetWindowTextW(g_label, lab, 128);
    s[0] = 0;
    if (lab[0]) _snwprintf(s, 500, tr(T_AUTO_COUNT_LABEL), lab);
    s[499] = 0;
    wcsncat(s, tr(T_AUTO_COUNT_TEXT), 499 - wcslen(s));
    if (g_turn.kind == MEDIA_FLOPPY) banner_num(B_YELLOW, g_turn.num, t, s);
    else banner(B_YELLOW, NULL, ICON_DISC, t, s);
}

static void ask_banner(int twin)
{
    wchar_t t[160], s[600], who[32], other[32];
    const wchar_t *what = tr(g_turn.kind == MEDIA_FLOPPY ? T_NOUN_DISKETTE : T_NOUN_DISC);
    slot_name(g_cur, 0, who, 32);
    slot_name(twin, 0, other, 32);
    _snwprintf(t, 160, tr(T_AUTO_ASK_TITLE), what, other); t[159] = 0;
    _snwprintf(s, 600, tr(T_AUTO_ASK_TEXT),
               (unsigned long)(g_vol_turn.serial >> 16), (unsigned long)(g_vol_turn.serial & 0xffff),
               g_vol_turn.label[0] ? tr(T_AUTO_ASK_LABEL) : L"", other, who, what);
    s[599] = 0;
    if (g_turn.kind == MEDIA_FLOPPY) banner(B_ORANGE, L"!", 0, t, s);
    else banner(B_ORANGE, NULL, ICON_DISC, t, s);
}

/* the label read started by auto_found has answered */
static void auto_labelled(int state)
{
    int twin;
    if (g_turn.kind != MEDIA_FLOPPY && state == LB_NODISK) {
        wchar_t w[120], d[3];
        auto_drive(d);
        _snwprintf(w, 120, tr(T_AUTO_WAITING), d); w[119] = 0;
        auto_rewatch(w);            /* no disc yet: Windows will say when it arrives */
        return;
    }
    if ((twin = vol_twin()) > 0) {
        g_auto_state = AUTO_ASK;
        ask_banner(twin);
        sound(SND_ERROR);
        return;
    }
    g_auto_state = AUTO_COUNT;
    g_auto_left = AUTO_SECONDS;
    SetTimer(g_wnd, TIMER_AUTO, 1000, NULL);
    count_banner();
}

static void auto_tick(void)
{
    if (g_auto_state != AUTO_COUNT || !g_waiting) { KillTimer(g_wnd, TIMER_AUTO); return; }
    if (--g_auto_left > 0) { count_banner(); return; }
    KillTimer(g_wnd, TIMER_AUTO);
    answer(CMD_READ);
}

/* what the floppy watcher saw */
static void on_diskin(WPARAM gen, LPARAM what)
{
    wchar_t w[200], d[3];
    if ((LONG)gen != g_fp_gen || !g_waiting || g_auto_state == AUTO_OFF) return;
    auto_drive(d);
    switch (what) {
    case FP_NEW:
        auto_found();
        break;
    case FP_GONE:
        _snwprintf(w, 200, tr(T_AUTO_GONE), d); w[199] = 0;
        auto_rewatch(w);
        break;
    case FP_SLOWDRIVE:
        auto_status(tr(T_AUTO_SLOW));
        break;
    default:
        _snwprintf(w, 200, tr(T_AUTO_NODRIVER), d); w[199] = 0;
        auto_manual(w);
        break;
    }
}

static void on_label(WPARAM gen, lb_result *r)
{
    wchar_t *w = g_lb_msg, u[3], cur[64];
    int mine = (LONG)gen == g_lb_auto && g_auto_state == AUTO_LABEL;
    u[0] = r->root[0]; u[1] = L':'; u[2] = 0;
    if ((LONG)gen == g_lb_gen && g_waiting) {
        g_vol_turn.ok = r->has_serial;
        g_vol_turn.serial = r->serial;
        wcscpy(g_vol_turn.label, r->has_serial ? r->label : L"");
        switch (r->state) {
        case LB_FOUND:
            /* the automatic mode does not overwrite what the user typed */
            GetWindowTextW(g_label, cur, 64);
            if (mine && cur[0] && wcscmp(cur, g_auto_label)) {
                _snwprintf(w, 200, tr(T_LB_KEPT), r->label);
                break;
            }
            if (mine) wcscpy(g_auto_label, r->label);
            SetWindowTextW(g_label, r->label);
            focus_to(g_label);
            SendMessageW(g_label, EM_SETSEL, 0, -1);
            _snwprintf(w, 200, tr(T_LB_READ), r->label);
            break;
        case LB_NONE:    wcsncpy(w, tr(T_LB_NONE), 200); break;
        case LB_NODISK:  _snwprintf(w, 200, tr(T_LB_NODISK), u); break;
        default:         wcsncpy(w, tr(T_LB_UNKNOWN), 200); break;
        }
        w[199] = 0;
        sbar_text(0, w);
        if (mine) auto_labelled(r->state);
        watch_resume();
    }
    free(r);
}

/* the notice is about a CD or DVD in the awaited drive. A disc in a real
 * drive comes with DBTF_MEDIA; an image mounted in a virtual drive is a
 * whole new volume, without it: for the automatic mode both are a disc
 * (media_only for the label read by itself, as before) */
static int awaited_media(const DEV_BROADCAST_HDR *h, int media_only)
{
    const DEV_BROADCAST_VOLUME *v = (const DEV_BROADCAST_VOLUME *)h;
    int bit;
    if (!h || h->dbch_devicetype != DBT_DEVTYP_VOLUME || (media_only && !(v->dbcv_flags & DBTF_MEDIA))) return 0;
    if (!g_waiting || g_turn.kind == MEDIA_FLOPPY) return 0;
    bit = toupper(g_drive_w[0] ? g_drive_w[0] : L'D') - 'A';
    return bit >= 0 && bit < 26 && (v->dbcv_unitmask & (1UL << bit));
}

/* a CD or DVD just inserted in the awaited drive: label read by itself,
 * if the field is still empty; in automatic mode it is the new disk */
static void on_media_arrival(const DEV_BROADCAST_HDR *h)
{
    if (g_auto_state != AUTO_OFF) { if (awaited_media(h, 0)) auto_found(); }
    else if (awaited_media(h, 1) && GetWindowTextLengthW(g_label) == 0) label_read();
}

static void on_media_removal(const DEV_BROADCAST_HDR *h)
{
    wchar_t w[120], d[3];
    if (g_auto_state == AUTO_OFF || !awaited_media(h, 0)) return;
    auto_drive(d);
    _snwprintf(w, 120, tr(T_AUTO_DISC_GONE), d); w[119] = 0;
    auto_rewatch(w);
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
    const wchar_t *t = NULL;
    switch (g_dic_state) {
    case DIC_CHECKING: t = tr(T_DIC_CHECKING); break;
    case DIC_NOTFOUND: t = tr(brief ? T_DIC_NOTFOUND_B : T_DIC_NOTFOUND); break;
    case DIC_NODLL:    t = tr(brief ? T_DIC_NODLL_B : T_DIC_NODLL); break;
    case DIC_NOTDIC:   t = tr(brief ? T_DIC_NOTDIC_B : T_DIC_NOTDIC); break;
    }
    if (t) { wcsncpy(out, t, n); out[n - 1] = 0; return; }
    if (!g_has_dat) wcsncpy(dat, tr(brief ? T_DAT_NONE_B : T_DAT_NONE), 120);
    else if (dat_old()) _snwprintf(dat, 120, tr(brief ? T_DAT_OLD_B : T_DAT_OLD), g_dat_date);
    else _snwprintf(dat, 120, tr(brief ? T_DAT_OK_B : T_DAT_OK), g_dat_date[0] ? g_dat_date : L"?");
    dat[119] = 0;
    _snwprintf(out, n, tr(T_DIC_OK), g_dic_ver, dat);
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

/* the first thing missing before starting, or "Pronto"; the two archive
 * texts only in the archive view */
static const int READY_TEXTS[] = {
    T_READY_NOTITLE, T_READY_NOPLATFORM, T_READY_MEDIA, T_READY_NOFOLDER, T_SD_CHECKING, T_READY_SD_BAD,
    T_READY_OK, T_READY_ARC_LOADING, T_READY_VERIFYING, T_DIC_CHECKING, T_READY_DIC_BAD, -1
};

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
    if (!s[0]) return tr(T_READY_NOTITLE);
    get_u8_trim(ID_PLATFORM, s, sizeof(s));
    if (!s[0]) return tr(T_READY_NOPLATFORM);
    if (setup_counts(c)) return tr(T_READY_MEDIA);
    get_u8_trim(ID_DIR, s, sizeof(s));
    if (!s[0]) return tr(T_READY_NOFOLDER);
    /* SAMdisk is needed only with floppies, DiscImageCreator with CDs or DVDs */
    if (c[MEDIA_FLOPPY] && g_sd_state == SD_CHECKING) return tr(T_SD_CHECKING);
    if (c[MEDIA_FLOPPY] && g_sd_state != SD_OK) return tr(T_READY_SD_BAD);
    if ((c[MEDIA_CD] || c[MEDIA_DVD]) && g_dic_state == DIC_CHECKING) return tr(T_DIC_CHECKING);
    if ((c[MEDIA_CD] || c[MEDIA_DVD]) && g_dic_state != DIC_OK) return tr(T_READY_DIC_BAD);
    return tr(T_READY_OK);
}

/* the right part is as wide as the longest text it can hold */
static int ready_width(void)
{
    HDC dc = GetDC(g_sbar);
    HGDIOBJ o = SelectObject(dc, (HFONT)SendMessageW(g_sbar, WM_GETFONT, 0, 0));
    SIZE sz; int i, w = 0;
    for (i = 0; READY_TEXTS[i] >= 0; i++) {
        const wchar_t *t = tr(READY_TEXTS[i]);
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
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
    const wchar_t *ready = tr(T_READY_OK);
    tools_text(w, 420);
    sbar_text(0, w);
    to_w(g_arc_root, dir, 512);
    if (!g_arc_loaded) { _snwprintf(w, 700, tr(T_SBAR_READING), dir); ready = tr(T_READY_ARC_LOADING); }
    else if (g_arc_missing) _snwprintf(w, 700, tr(T_SBAR_NOFOLDER), dir);
    else if (!g_arc.n) _snwprintf(w, 700, tr(T_SBAR_NOBOXES), dir);
    else {
        plural(n, 64, g_arc.n, T_SBAR_BOX_ONE, T_SBAR_BOX_MANY);
        _snwprintf(w, 700, tr(T_SBAR_BOXES_FROM), n, dir);
    }
    w[699] = 0;
    sbar_text(1, w);
    if (g_verifying) ready = tr(T_READY_VERIFYING);
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
        _snwprintf(w, 200, tr(T_SBAR_SLOT), kind_name(kind, KN_CAP), num, g_count[kind]); w[199] = 0;
    }
    sbar_text(0, w);
    plural(w, 200, pul + mar + err, T_SBAR_ARCHIVED_ONE, T_SBAR_ARCHIVED_MANY);
    if (mar) { plural(a, 64, mar, T_SUM_MARGINAL_ONE, T_SUM_MARGINAL_MANY); wcscat(w, L", "); wcscat(w, a); }
    if (err) { _snwprintf(b, 64, tr(T_SBAR_ERRORS), err); b[63] = 0; wcsncat(w, b, 199 - wcslen(w)); }
    sbar_text(1, w);
    sbar_text(2, tr(g_bag_closed ? T_SBAR_BAG_CLOSED : T_SBAR_BAG_OPEN));
    fmt_time(GetTickCount() - g_t_sess, a, 64);
    _snwprintf(w, 200, tr(T_SBAR_SESSION), a); w[199] = 0;
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
    HWND c, f; wchar_t t[120];
    g_view = v;
    _snwprintf(t, 120, tr(v == V_ARCHIVE ? T_TITLE_ARCHIVE : T_TITLE_SESSION), L"" RA_VERSION); t[119] = 0;
    SetWindowTextW(g_wnd, t);
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
    if (!s[0]) wcsncpy(out, tr(T_OUT_TOREAD), len);
    else if (!strcmp(s, ST_CLEAN)) { wcsncpy(out, tr(T_OUT_CLEAN), len); dot = DOT_GREEN; }
    else if (!strcmp(s, ST_MARGINAL)) {
        if (d->agree >= 2) _snwprintf(out, len, tr(T_OUT_MARGINAL_AGREE), d->agree);
        else wcsncpy(out, tr(T_OUT_MARGINAL), len);
        dot = DOT_OCHRE;
    }
    else if (!strcmp(s, "fragile")) { wcsncpy(out, tr(T_OUT_FRAGILE), len); dot = DOT_OCHRE; }
    else if (!strcmp(s, ST_ERROR)) { wcsncpy(out, tr(T_OUT_ERROR), len); dot = DOT_RED; }
    else if (!strcmp(s, ST_REDUMP)) { wcsncpy(out, tr(T_OUT_REDUMP), len); dot = DOT_GREEN; }
    else if (!strcmp(s, ST_READ_ONCE)) { wcsncpy(out, tr(T_OUT_ONCE), len); dot = DOT_OCHRE; }
    else if (!strcmp(s, ST_UNSTABLE)) {
        if (d->agree >= 2) _snwprintf(out, len, tr(T_OUT_UNSTABLE_AGREE), d->agree);
        else wcsncpy(out, tr(T_OUT_UNSTABLE), len);
        dot = DOT_OCHRE;
    }
    else if (!strcmp(s, ST_ERRORS)) { wcsncpy(out, tr(T_OUT_ERRORS), len); dot = DOT_RED; }
    else if (!strcmp(s, ST_FAILED)) { wcsncpy(out, tr(T_OUT_FAILED), len); dot = DOT_RED; }
    else if (!strcmp(s, ST_INTERRUPTED)) wcsncpy(out, tr(T_OUT_INTERRUPTED), len);
    else if (!strcmp(s, ST_DUPLICATE)) wcsncpy(out, tr(T_OUT_DUPLICATE), len);
    else if (!strcmp(s, ST_MISSING)) wcsncpy(out, tr(T_OUT_SKIPPED), len);
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
    /* the same image as another disk: its reads are set aside, to read again
     * (or skipped, closed without it: the reason stays visible) */
    if (g_disk[n].twin && (!strcmp(g_disk[n].status, ST_DUPLICATE) || !strcmp(g_disk[n].status, ST_MISSING))) {
        wchar_t who[32];
        slot_name(g_disk[n].twin, 0, who, 32);
        if (!strcmp(g_disk[n].status, ST_DUPLICATE)) _snwprintf(w + 100, 100, tr(T_OUT_DUPLICATE_OF), who);
        else _snwprintf(w + 100, 100, tr(T_OUT_SKIPPED_DUP), who);
        w[199] = 0;
        it.iImage = DOT_OCHRE;
    }
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
    _snwprintf(w, 64, tr(T_BAR_PROGRESS), done, g_total, noun(g_count, g_total != 1)); w[63] = 0;
    SetWindowTextW(g_boxtxt, w);
}

/* one more part of the summary: after a comma, or glued to the previous one */
static void sum_add(wchar_t *out, int len, const wchar_t *part, int glued)
{
    if (out[0] && !glued) wcsncat(out, L", ", len - wcslen(out) - 1);
    wcsncat(out, part, len - wcslen(out) - 1);
}

/* "2 clean, 1 marginal, 1 skipped or to redo" */
static void summary(wchar_t *out, int len)
{
    wchar_t part[64];
    int pul, mar, err, alt, opt[MEDIA_KINDS], k, red = 0, n;
    counts(&pul, &mar, &err, &alt, opt);
    for (n = 1; n <= g_total && n <= META_MAXDISK; n++) red += !strcmp(g_disk[n].status, ST_REDUMP);
    out[0] = 0;
    if (pul) { plural(part, 64, pul, T_SUM_CLEAN_ONE, T_SUM_CLEAN_MANY); sum_add(out, len, part, 0); }
    if (red) {              /* "2 clean (1 verified with redump)" */
        _snwprintf(part, 64, tr(red == 1 ? T_SUM_REDUMP_ONE : T_SUM_REDUMP_MANY), red); part[63] = 0;
        sum_add(out, len, part, 1);
    }
    if (mar) { plural(part, 64, mar, T_SUM_MARGINAL_ONE, T_SUM_MARGINAL_MANY); sum_add(out, len, part, 0); }
    if (err) { _snwprintf(part, 64, tr(T_SUM_ERRORS), err); part[63] = 0; sum_add(out, len, part, 0); }
    if (alt) { plural(part, 64, alt, T_SUM_OTHER_ONE, T_SUM_OTHER_MANY); sum_add(out, len, part, 0); }
    for (k = 0; k < MEDIA_KINDS; k++) {     /* "1 CD to read" */
        if (!opt[k]) continue;
        _snwprintf(part, 64, tr(T_SUM_TOREAD), opt[k], kind_name(k, opt[k] == 1 ? KN_ONE : KN_MANY)); part[63] = 0;
        sum_add(out, len, part, 0);
    }
    if (!out[0]) { wcsncpy(out, tr(T_SUM_NOTHING), len); out[len - 1] = 0; }
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
            /* the disks to redo are asked for again: for now they are ST_TO_READ */
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
 * "set-aside\FLOPPY01.dsk.damaged, ...", relative to the Folder;
 * the log has them with the full path */
static void aside_fill(void)
{
    strbuf s; strbuf t; char *p, *nl; wchar_t w[1100];
    sb_init(&s); sb_init(&t);
    g_aside_n = session_aside_list(g_bagdir, &s);
    for (p = s.s; g_aside_n && *p; p = nl + 1) {
        nl = strchr(p, '\n');
        *nl = 0;
        sb_printf(&t, "%s%s\\%s", p == s.s ? "" : ", ", session_aside_dir(g_bagdir), p);
    }
    to_w(t.len ? t.s : "", w, 1100);
    SetDlgItemTextW(g_wnd, ID_BAGASIDE, w);
    sb_free(&s); sb_free(&t);
}

static void bag_fill(void)
{
    char data[640]; bag_count c; wchar_t w[700], a[64], b[64], mb[32];
    to_w(g_bagdir, w, 700);
    SetDlgItemTextW(g_wnd, ID_BAGDIR, w);
    memset(&c, 0, sizeof(c));
    u8_join(data, sizeof(data), g_bagdir, "data");
    c.dir = data;
    u8_listdir(data, count_file, &c);
    plural(a, 64, c.img, T_BAG_IMAGE_ONE, T_BAG_IMAGE_MANY);
    plural(b, 64, c.scan, T_BAG_SCAN_ONE, T_BAG_SCAN_MANY);
    lang_number((double)c.bytes / (1024.0 * 1024.0), 1, mb, 32);
    _snwprintf(w, 700, L"%ls, %ls%ls - %ls MB", a, b, c.meta ? L", metadata.json" : L"", mb); w[699] = 0;
    SetDlgItemTextW(g_wnd, ID_BAGCONT, w);
    /* EV_BOX_DONE arrives only after bag_close has recomputed and compared the hashes */
    SetDlgItemTextW(g_wnd, ID_BAGHASH, tr(T_BAG_HASHES_OK));
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
    _snwprintf(out, len, tr(b->media_expected == 1 ? T_RESUME_ITEM_ONE : T_RESUME_ITEM_MANY), esc, done,
               b->media_expected, noun(c, b->media_expected != 1));
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
    /* the box keeps its choice: changed here, it changes for the rest of the box */
    g_prot = b->copy_protection;
    SendDlgItemMessageW(g_wnd, ID_PROT, CB_SETCURSEL, g_prot, 0);
    to_w(b->title, t, 300);
    _snwprintf(w, 400, tr(T_RESUME_HINT), t); w[399] = 0;
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
        _snwprintf(w, 700, tr(T_RESUME_NONE), wr);
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
    wcsncat(out, tr(T_FILES_PREFIX), len - wcslen(out) - 1);
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
        if (done > 1) wcsncat(out, done == parts ? tr(T_FILES_AND) : L", ", len - wcslen(out) - 1);
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
        _snwprintf(w, 900, tr(T_BAGPATH), wb, f);
    } else {
        to_w(dir, wb, 800);
        _snwprintf(w, 900, tr(T_BAGPATH_NOTITLE), wb,
                   wb[0] && wb[wcslen(wb) - 1] != L'\\' ? L"\\" : L"", f);
    }
    w[899] = 0;
    SetDlgItemTextW(g_wnd, ID_BAGPATH, w);
}

/* NULL if the required fields are there; otherwise what is missing, and the focus goes there */
static const wchar_t *missing(int bad_counts)
{
    int id = 0; const wchar_t *msg = NULL;
    if (!g_box->title[0])         { id = ID_TITLE; msg = tr(T_MISS_TITLE); }
    else if (!g_box->platform[0]) { id = ID_PLATFORM; msg = tr(T_MISS_PLATFORM); }
    else if (bad_counts)          { id = ID_COUNT; msg = tr(T_MISS_MEDIA); }
    else if (!GetWindowTextLengthW(GetDlgItem(g_wnd, ID_DIR))) { id = ID_DIR; msg = tr(T_MISS_FOLDER); }
    /* a path chosen with "..." or read from the ini bypasses the typing limit */
    else if (u8_len(ID_DIR) >= (int)sizeof(g_outdir)) { id = ID_DIR; msg = tr(T_MISS_TOOLONG); }
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
    memset(g_vol, 0, sizeof(g_vol));        /* serial numbers are compared within one box */
    g_bag_closed =g_bag_complete = g_reread = g_fragile = g_aside_n = 0;
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
        MessageBoxW(g_wnd, tr(T_DIC_WAIT), L"RetroArchiver", MB_ICONINFORMATION);
        return 0;
    }
    dic_text(t, 200, 0);
    _snwprintf(w, 700, tr(T_DIC_FIX), t);
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
        MessageBoxW(g_wnd, tr(T_SD_WAIT), L"RetroArchiver", MB_ICONINFORMATION);
        return 0;
    }
    samdisk_text(t, 200);
    _snwprintf(w, 600, tr(T_SD_FIX), t);
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
    if (!g_box) { MessageBoxW(g_wnd, tr(T_ERR_MEMORY), L"RetroArchiver", MB_ICONERROR); return; }
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
    {   /* a: -> \\.\fdraw0, b: -> \\.\fdraw1; a .dsk file has no drive to watch */
        const char *q = buf[0] ? buf : "a:";
        while (*q == ' ') q++;
        g_fd_index = floppy_source_is_drive(q) && (toupper((unsigned char)*q) == 'A' || toupper((unsigned char)*q) == 'B')
                   ? toupper((unsigned char)*q) - 'A' : -1;
    }
    g_auto = IsDlgButtonChecked(g_wnd, ID_AUTO) == BST_CHECKED;
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
    {   /* the CD drive's read offset, for DIC's question when it does not
         * know the drive: in the ini only, no field in the window yet */
        wchar_t off[64], w[600];
        set_get(&g_set, SEC_OPTICAL, KEY_DRIVEOFFSET, L"", off, 64);
        to_u8(off, buf, sizeof(buf));
        if (optical_set_drive_offset(buf)) {
            optical_set_drive_offset("");
            if (c[MEDIA_CD]) {
                _snwprintf(w, 600, tr(T_OFFSET_BAD), off);
                w[599] = 0;
                MessageBoxW(g_wnd, w, L"RetroArchiver", MB_ICONEXCLAMATION);
                return;
            }
        }
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
    g_cfg.copy_protection = meta_prot_key(g_prot);
    /* copies for the window: while the session runs g_box belongs to the worker thread */
    str_copy(g_title, sizeof(g_title), g_box->title);
    session_bagdir(g_bagdir, sizeof(g_bagdir), g_outdir, g_box->title);

    reset_state();
    g_t_sess = GetTickCount();
    SetTimer(g_wnd, TIMER_ID, 1000, NULL);
    SetTimer(g_wnd, TIMER_LINES, LINES_MS, NULL);
    banner(B_BLUE, L"...", 0, tr(T_START_TITLE), tr(T_START_TEXT));
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
        MessageBoxW(g_wnd, tr(T_ERR_NOSTART), L"RetroArchiver", MB_ICONERROR);
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
    ini_store(0);                       /* ArchiveFolder */
}

/* ---- archive: texts ---- */

static void fmt_mb(long long bytes, wchar_t *out, int n)
{
    lang_number((double)bytes / (1024.0 * 1024.0), 1, out, n);
}

/* "2026-10-14T21:20:11" -> "14/10/2026", as the language writes dates */
static void fmt_date(const char *d, wchar_t *out, int n)
{
    lang_date(d, out, n);
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
    SendMessageW(cb, CB_INSERTSTRING, 0, (LPARAM)tr(T_ARC_ALLPLAT));
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
        DrawTextW(dc, tr(T_READY_ARC_LOADING), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
    plural(a, 64, g_nrows, T_ARC_BOX_ONE, T_ARC_BOX_MANY);
    comp_text(disks, b, 64);                /* the archived media, per kind */
    if (!b[0]) { wcsncpy(b, tr(T_ARC_NOMEDIA), 64); b[63] = 0; }
    fmt_mb(bytes, mb, 32);
    _snwprintf(t, 200, L"%ls, %ls, %ls MB", a, b, mb); t[199] = 0;
    SelectObject(dc, g_mid);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    x = sz.cx + 10;
    SelectObject(dc, g_font);
    if (comp) { plural(t, 200, comp, T_ARC_COMPLETE_ONE, T_ARC_COMPLETE_MANY); x = pill(dc, x, h, t, B_GREEN); }
    if (inc)  { plural(t, 200, inc, T_ARC_INCOMPLETE_ONE, T_ARC_INCOMPLETE_MANY); x = pill(dc, x, h, t, B_ORANGE); }
    if (open) { plural(t, 200, open, T_ARC_OPEN_ONE, T_ARC_OPEN_MANY); x = pill(dc, x, h, t, B_BLUE); }
    /* disks with errors are always reported, even when there are none */
    if (err) { plural(t, 200, err, T_ARC_ERRDISK_ONE, T_ARC_ERRDISK_MANY); x = pill(dc, x, h, t, B_RED); }
    else x = pill(dc, x, h, tr(T_ARC_NOERRORS), PILL_GREY);
    if (broken) { plural(t, 200, broken, T_ARC_BROKEN_ONE, T_ARC_BROKEN_MANY); pill(dc, x, h, t, B_RED); }
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
        wcsncat(out, tr(T_DETAIL_VERSION), n - wcslen(out) - 1);
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
    SIZE sz, ts; RECT r; wchar_t num[24];
    const wchar_t *label = tr(T_DETAIL_MEDIA);
    int i, x, cw, kind, n;
    SetTextColor(dc, RGB(0x44, 0x44, 0x44));
    GetTextExtentPoint32W(dc, label, (int)wcslen(label), &sz);
    SetRect(&r, 0, y, sz.cx, y + 18);
    DrawTextW(dc, label, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
        if (kind == MEDIA_FLOPPY) _snwprintf(num, 24, L"%d", n);
        else if (b->count[kind] > 1) _snwprintf(num, 24, L"%ls%d", kind_name(kind, KN_CAP), n);
        else _snwprintf(num, 24, L"%ls", kind_name(kind, KN_CAP));
        num[23] = 0;
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
        DrawTextW(dc, tr(!g_arc_loaded ? T_READY_ARC_LOADING : T_ARC_NOMATCH),
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
    if (b->notes[0]) { to_w(b->notes, n, 600); _snwprintf(t, 1200, tr(T_DETAIL_NOTES), n); }
    else wcsncpy(t, tr(T_DETAIL_NONOTES), 1200);
    t[1199] = 0;
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
    SetDlgItemTextW(g_wnd, ID_HINT, tr(T_SETUP_HINT));
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

static void add_count(wchar_t *out, int n, int k, int one, int many)
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
    add_count(what, 300, r->c.changed, T_VF_CHANGED_ONE, T_VF_CHANGED_MANY);
    add_count(what, 300, r->c.lost, T_VF_LOST_ONE, T_VF_LOST_MANY);
    add_count(what, 300, r->c.extra, T_VF_EXTRA_ONE, T_VF_EXTRA_MANY);
    if (r->rc) {
        to_w(r->err, what, 300);
        _snwprintf(w, 1200, tr(T_VF_CANNOT), title, what);
        icon = MB_ICONEXCLAMATION;
    } else if (r->open && !r->c.files) {
        _snwprintf(w, 1200, tr(T_VF_EMPTY), title);
    } else if (!bad && !r->c.extra) {
        _snwprintf(w, 1200, tr(r->open ? T_VF_OK_OPEN : T_VF_OK_CLOSED), title, r->c.files);
    } else {
        _snwprintf(w, 1200, tr(T_VF_BAD), title, what, first,
                   tr(r->open ? T_VF_BAD_OPEN : bad ? T_VF_BAD_CLOSED : T_VF_EXTRA_ONLY));
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
    wchar_t sub[400], title[160], phase[64], tail[80];
    int kind, num;
    slot(g_cur, &kind, &num);
    phase_text(g_phase_name, phase, 64);
    if (kind == MEDIA_FLOPPY) {
        /* "Floppy 2 of 3: Read 1." */
        _snwprintf(tail, 80, L"%ls%ls", phase[0] ? L": " : L"", phase); tail[79] = 0;
        _snwprintf(sub, 400, tr(T_WORK_FLOPPY_SUB), kind_name(kind, KN_CAP), num, g_count[kind], tail);
        sub[399] = 0;
        if (g_reread) banner_num(B_ORANGE, g_cur, tr(T_WORK_REREAD), sub);
        else if (g_fragile) banner_num(B_ORANGE, g_cur, tr(T_WORK_FRAGILE), sub);
        else banner_num(B_BLUE, g_cur, tr(T_WORK_READING), sub);
        return;
    }
    /* CDs and DVDs: DiscImageCreator, 10 to 30 minutes per read; "CD 1 of 1 in drive D: (TOC)." */
    if (phase[0]) _snwprintf(tail, 80, L" (%ls)", phase); else tail[0] = 0;
    tail[79] = 0;
    _snwprintf(sub, 400, tr(T_WORK_OPT_SUB), kind_name(kind, KN_CAP), num, g_count[kind], g_drive_w, tail);
    sub[399] = 0;
    _snwprintf(title, 160, tr(g_reread ? T_WORK_OPT_REREAD : T_WORK_OPT_READING), kind_name(kind, KN_ONE));
    title[159] = 0;
    banner(g_reread ? B_ORANGE : B_BLUE, NULL, ICON_DISC, title, sub);
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
    auto_leave();               /* the floppy watcher lets go of the drive before SAMdisk opens it */
    if (cmd == CMD_READ && g_turn.readable) {
        /* the next disk of this kind must be a new one */
        if (g_turn.kind == MEDIA_FLOPPY) g_fd_used = 1; else g_opt_used = 1;
        if (g_cur >= 1 && g_cur <= META_MAXDISK) g_vol[g_cur] = g_vol_turn;
    }
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
        _snwprintf(w, 300, tr(g_turn.readable ? T_ANSWER_SKIP : T_ANSWER_LATER), who); w[299] = 0;
        banner(B_BLUE, L"...", 0, w, NULL);
    }
    else if (cmd == CMD_FINISH)
        banner(B_BLUE, L"...", 0, tr(T_ANSWER_CLOSING), tr(T_ANSWER_CLOSING_TEXT));
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
    auto_leave();
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
        if (left >= 60) _snwprintf(w, 64, tr(T_BAR_OPT_MIN), pm / 10, (unsigned long)((left + 30) / 60));
        else _snwprintf(w, 64, tr(T_BAR_OPT_SEC), pm / 10, (unsigned long)left);
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
    _snwprintf(w, 64, tr(T_BAR_CYL), cyl, CYL_MAX); w[63] = 0;
    SetWindowTextW(g_cyltxt, w);
}

static int starts(const char *s, const char *head)
{
    return !strncmp(s, head, strlen(head));
}

/* two core notices change the banner and the expected phases; the text is
 * session.c's and optical.c's, in English: if it changes only the colour is lost */
static void notice(const char *s)
{
    /* a CD not found in redump: second read and comparison */
    if (strstr(s, "is not verified by redump")) {
        g_reread = 1;
        pills_reread();
        work_banner();
    } else if (starts(s, "The reads do not match: making another read")) {
        g_reread = 1;
        pills_reread();
        work_banner();
    } else if (starts(s, "The survey found bad sectors")) {
        g_fragile = 1;
        if (g_pcur >= 0) { pills_future(); repaint(g_pills); }
        work_banner();
    } else if (starts(s, "The drive does not hold a ") || starts(s, "DiscImageCreator stopped") ||
               starts(s, "DiscImageCreator cannot open") || starts(s, "DiscImageCreator finished") ||
               starts(s, "The drive's read offset is not set") || starts(s, "The drive was reset ")) {
        /* why a CD read failed (optical.c): it goes into the Riprova banner */
        to_w(s, g_why, 600);
    }
}

static void on_phase(int disk, const wchar_t *t)
{
    wchar_t w[1100], who[32], phase[64];
    wcsncpy(g_phase_name, t, 127); g_phase_name[127] = 0;
    if (!wcscmp(t, L"Scan") || !wcscmp(t, L"TOC")) g_reread = g_fragile = 0;
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
    phase_text(t, phase, 64);
    _snwprintf(w, 1100, L"--- %ls: %ls ---", who, phase); w[1099] = 0;
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
        /* the serial number read before was the wrong disk's */
        if (!strcmp(g_disk[n].status, ST_DUPLICATE)) memset(&g_vol[n], 0, sizeof(g_vol[n]));
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
        g_bag_complete = bag_status_complete(s);
        _snwprintf(w, 1100, tr(T_LOG_BOX_DONE), tr(g_bag_complete ? T_LOG_BOX_COMPLETE : T_LOG_BOX_INCOMPLETE));
        w[1099] = 0;
        log_append(w);
        sbar_update();
        break;
    case EV_ERROR:
        wcsncpy(g_last_err, t, 599); g_last_err[599] = 0;
        _snwprintf(w, 1100, tr(T_LOG_ERROR), t); w[1099] = 0;
        log_append(w);
        if (g_view == V_WORK) banner(B_RED, L"!", 0, tr(T_BANNER_ERROR), t);
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
    _snwprintf(w, 160, tr(T_UNREAD_TITLE), kind_name(t->kind, KN_CAP), t->num, t->count,
               kind_name(t->kind, KN_MANY));
    w[159] = 0;
    _snwprintf(text, 400, tr(T_UNREAD_TEXT), kind_name(t->kind, KN_ONE), kind_name(t->kind, KN_MANY));
    text[399] = 0;
    banner(B_BLUE, NULL, ICON_DISC, w, text);
}

/* the turn being waited for is a retry: 0 no, 1 failed read, 2 interrupted,
 * 3 duplicate of another disk, 4 again with the copy protection scan */
static int g_retry;

/* the core proposes to read the CD again with the copy protection scan
 * (the last read, made without it, failed or has errors) or without it
 * (with it the drive reset itself); the reason from DIC when it failed */
static void wait_protection(void)
{
    wchar_t w[160], text[900];
    const char *st = g_cur >= 1 && g_cur <= META_MAXDISK ? g_disk[g_cur].status : "";
    int off = g_turn.protection_retry == 2;    /* with the scan the drive reset itself */
    _snwprintf(w, 160, tr(off ? T_PROTOFF_TITLE : T_PROT_TITLE), kind_name(g_turn.kind, KN_CAP), g_turn.num);
    w[159] = 0;
    if (!strcmp(st, ST_FAILED))
        _snwprintf(text, 900, L"%ls%ls%ls", g_why, g_why[0] ? L"\n" : L"",
                   tr(off ? T_PROTOFF_TEXT : T_PROT_TEXT_FAILED));
    else
        _snwprintf(text, 900, L"%ls", tr(off ? T_PROTOFF_TEXT : T_PROT_TEXT_ERRORS));
    text[899] = 0;
    banner(B_ORANGE, NULL, ICON_DISC, w, text);
}

/* the yellow banner while waiting, or the orange one of a retry */
static void wait_banner(void)
{
    wchar_t w[160];
    int retry = g_retry != 0;
    const char *last = g_retry == 2 ? ST_INTERRUPTED : ST_FAILED;
    int autom = g_auto_state != AUTO_OFF, again = g_turn.kind == MEDIA_FLOPPY ? g_fd_used : g_opt_used;
    if (!g_turn.readable) {
        wait_unreadable(&g_turn);
    } else if (g_retry == 4) {
        wait_protection();
    } else if (g_retry == 3) {
        wchar_t who[32], other[64], text[500];
        const wchar_t *what = tr(g_turn.kind == MEDIA_FLOPPY ? T_NOUN_DISKETTE : T_NOUN_DISC);
        int t = g_cur >= 1 && g_cur <= META_MAXDISK ? g_disk[g_cur].twin : 0;
        slot_name(g_cur, 0, who, 32);
        if (t) slot_name(t, 0, other, 64); else { wcsncpy(other, tr(T_DUP_OTHER), 64); other[63] = 0; }
        _snwprintf(w, 160, tr(T_DUP_TITLE), who, other); w[159] = 0;
        _snwprintf(text, 500, tr(autom ? T_DUP_TEXT_AUTO : T_DUP_TEXT), what, who);
        text[499] = 0;
        if (g_turn.kind == MEDIA_FLOPPY) banner(B_ORANGE, L"!", 0, w, text);
        else banner(B_ORANGE, NULL, ICON_DISC, w, text);
    } else if (retry) {
        _snwprintf(w, 160, tr(!strcmp(last, ST_INTERRUPTED) ? T_RETRY_STOPPED : T_RETRY_FAILED),
                   kind_name(g_turn.kind, KN_CAP), g_turn.num);
        w[159] = 0;
        /* after Interrompi the user often wants to stop: say how; a CD says
         * why DiscImageCreator did not read it (not visible in the log) */
        if (!strcmp(last, ST_INTERRUPTED))
            banner(B_ORANGE, L"!", 0, w, tr(T_RETRY_TEXT_STOPPED));
        else if (g_turn.kind == MEDIA_FLOPPY)
            banner(B_ORANGE, L"!", 0, w, tr(autom ? T_RETRY_TEXT_AUTO : T_RETRY_TEXT_FLOPPY));
        else {
            wchar_t why[700];
            if (g_why[0]) _snwprintf(why, 700, tr(T_RETRY_TEXT_WHY), g_why);
            else wcsncpy(why, tr(T_RETRY_TEXT_DISC), 700);
            why[699] = 0;
            banner(B_ORANGE, L"!", 0, w, why);
        }
    } else if (g_turn.kind != MEDIA_FLOPPY) {
        wchar_t sub[500];
        const wchar_t *k = kind_name(g_turn.kind, KN_ONE);
        if (g_turn.count > 1) _snwprintf(w, 160, tr(T_INS_OPT_N), k, g_turn.num, g_turn.count, g_drive_w);
        else _snwprintf(w, 160, tr(T_INS_OPT_ONE), k, g_drive_w);
        w[159] = 0;
        if (autom) _snwprintf(sub, 500, tr(T_INS_OPT_AUTO), tr(again ? T_INS_OPT_AGAIN : T_INS_OPT_FIRST), k);
        else _snwprintf(sub, 500, tr(T_INS_OPT_MANUAL), k);
        sub[499] = 0;
        banner(B_YELLOW, NULL, ICON_DISC, w, sub);
    } else {
        _snwprintf(w, 160, tr(T_INS_FLOPPY), kind_name(g_turn.kind, KN_ONE), g_turn.num, g_turn.count);
        w[159] = 0;
        banner_num(B_YELLOW, g_turn.num, w,
                   tr(!autom ? T_INS_FLOPPY_MANUAL : again ? T_INS_FLOPPY_AGAIN : T_INS_FLOPPY_AUTO));
    }
}

/* waiting for a disk: a new one, or the same after a failed read */
static void on_wait(void)
{
    wchar_t lab[300];
    int n = g_turn_in.n, total = g_turn_in.total;
    const char *last = (n >= 1 && n <= META_MAXDISK) ? g_disk[n].status : "";
    int retry = (n == g_last_done) &&
                (!strcmp(last, ST_FAILED) || !strcmp(last, ST_INTERRUPTED) || !strcmp(last, ST_DUPLICATE));
    int prot = g_turn_in.protection_retry;

    /* close confirmed between the check in host_wait and the PostMessage:
     * answer at once, the core's label and note stay */
    if (g_closing) { g_cmd = CMD_QUIT; SetEvent(g_wake); return; }
    g_turn = g_turn_in;
    if (total > 0 && total != g_total) { g_total = total; list_rebuild(); box_progress(); }
    g_cur = n;
    g_retry = prot ? 4 : !retry ? 0 : !strcmp(last, ST_INTERRUPTED) ? 2 : !strcmp(last, ST_DUPLICATE) ? 3 : 1;
    retry = retry || prot;
    /* Salta is the Enter button when Leggi is absent */
    SendDlgItemMessageW(g_wnd, ID_READ, BM_SETSTYLE, g_turn.readable ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
    SendDlgItemMessageW(g_wnd, ID_SKIP, BM_SETSTYLE, g_turn.readable ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON, TRUE);
    SetDlgItemTextW(g_wnd, ID_READ, tr(g_retry == 3 ? T_BTN_REREAD : retry ? T_BTN_RETRY : T_BTN_READ));
    /* what the core already has; on retry, what was typed stays */
    to_w(g_label_u8, lab, 128);
    if (lab[0] || !retry) SetWindowTextW(g_label, lab);
    to_w(g_note_u8, lab, 300);
    if (lab[0] || !retry) SetWindowTextW(g_note, lab);
    SendMessageW(g_diskbar, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_cyltxt, L"");
    g_lb_msg[0] = 0;            /* the magnifier's outcome was for the previous disk */
    g_waiting = 1;
    auto_begin();
    wait_banner();
    view(V_WAIT);
    list_select(n);
    focus_to(g_label);
    SendMessageW(g_label, EM_SETSEL, 0, -1);
    attention(retry);
    /* CD or DVD already in the drive: the label at once, without spinning anything */
    if (g_auto_state == AUTO_OFF && g_turn.kind != MEDIA_FLOPPY && GetWindowTextLengthW(g_label) == 0) label_read();
}

/* the final banner: green complete, orange incomplete or open, red error */
static void end_banner(int rc)
{
    wchar_t title[200], text[700], sum[200], nd[64], later[200];
    int pul, mar, err, alt, opt[MEDIA_KINDS], k;
    counts(&pul, &mar, &err, &alt, opt);
    summary(sum, 200);
    k = pul + mar + err;
    _snwprintf(nd, 64, L"%d %ls", k, noun(g_count, k != 1)); nd[63] = 0;
    /* only CDs or DVDs are left: resume once their reading is available */
    later[0] = 0;
    if (!alt && (opt[MEDIA_CD] || opt[MEDIA_DVD]))
        _snwprintf(later, 200, tr(T_END_LATER), tr(opt[MEDIA_CD] && opt[MEDIA_DVD] ? T_END_LATER_BOTH
                                                   : opt[MEDIA_CD] ? T_END_LATER_CD : T_END_LATER_DVD));
    later[199] = 0;
    if (rc == SESSION_CLOSED) {
        _snwprintf(title, 200, tr(g_bag_complete ? T_END_COMPLETE : T_END_INCOMPLETE), nd, g_total);
        _snwprintf(text, 700, tr(T_END_CLOSED_TEXT), sum, g_aside_n ? tr(T_END_ASIDE) : L"");
        title[199] = 0; text[699] = 0;
        if (g_bag_complete) banner(B_GREEN, NULL, 1, title, text);
        else banner(B_ORANGE, L"!", 0, title, text);
    } else if (rc == SESSION_OPEN) {
        _snwprintf(text, 700, L"%ls%ls%ls.%ls", g_last_err, g_last_err[0] ? L"\n" : L"", sum,
                   later[0] ? later : tr(T_END_OPEN_HOW));
        text[699] = 0;
        banner(B_ORANGE, L"!", 0, tr(T_END_OPEN), text);
    } else {
        _snwprintf(text, 700, tr(T_END_FAILED_TEXT), g_last_err, g_last_err[0] ? L"\n" : L"");
        text[699] = 0;
        banner(B_RED, L"!", 0, tr(T_END_FAILED), text);
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
            if (!g_disk[n].status[0]) { str_copy(g_disk[n].status, sizeof(g_disk[n].status), ST_MISSING); list_row(n); }
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

/* only Scan and Read run inside SAMdisk and can be stopped; the
 * comparison and closing the bag finish by themselves in a few seconds */
static void stop(void)
{
    if (g_view != V_WORK) return;      /* Interrompi exists only during the read */
    if (!wcsncmp(g_phase_name, L"Scan", 4) || !wcsncmp(g_phase_name, L"Read", 4) ||
        !wcscmp(g_phase_name, L"TOC") || !wcscmp(g_phase_name, L"C2 check")) {
        banner_num(B_ORANGE, g_cur, tr(T_STOP_TITLE), tr(cur_optical() ? T_STOP_DIC : T_STOP_SAMDISK));
        runner_cancel();
    } else if (!wcsncmp(g_phase_name, L"Compare", 7) || !wcscmp(g_phase_name, L"Redump")) {
        /* on the last disk there is no next one: once the comparison ends
         * the bag closes, or it re-reads (seen on the VM with a slowed-down
         * comparison) */
        banner_num(B_BLUE, g_cur, tr(T_STOP_COMPARE), tr(g_cur >= g_total ? T_STOP_COMPARE_LAST : T_STOP_COMPARE_NEXT));
    } else {
        banner(B_BLUE, L"...", 0, tr(T_STOP_NONE), tr(T_STOP_NONE_TEXT));
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
    _snwprintf(w, 900, tr(miss == 1 ? T_FINISH_ASK_ONE : T_FINISH_ASK_MANY), list);
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

/* ---- the program's language ----
 * UILanguage in [General]: a code (it, en...), the name of a file in
 * lang\ next to the exe, or empty to follow Windows' interface language.
 * The installer writes the language chosen during setup in the defaults;
 * the menu in setup writes the user's choice. A language without its file
 * is the English compiled in. */

#define UILANG_SEC SEC_GENERAL
#define UILANG_KEY KEY_UILANG
#define MAX_LANGS  16
#define CODE_LEN   16

static wchar_t g_uilang[CODE_LEN];              /* the choice; "" follows Windows */
static wchar_t g_langs[MAX_LANGS][CODE_LEN];    /* the menu's languages, after "Same as Windows" */
static int     g_nlangs;

/* letters, digits, - and _: a file name in lang\, never a path */
static int code_ok(const wchar_t *c)
{
    int i;
    for (i = 0; c[i]; i++)
        if (i >= CODE_LEN - 1 || !(iswalnum(c[i]) || c[i] == L'-' || c[i] == L'_')) return 0;
    return i > 0;
}

/* <exe folder>\lang\<name>, in UTF-8 for lang.c */
static void lang_dir_path(const wchar_t *name, wchar_t *out)
{
    wchar_t *p;
    out[0] = 0;
    if (!GetModuleFileNameW(NULL, out, MAX_PATH) || !(p = wcsrchr(out, L'\\'))) { out[0] = 0; return; }
    _snwprintf(p + 1, MAX_PATH - (p + 1 - out), L"%ls\\%ls", LANG_DIR_W, name);
    out[MAX_PATH - 1] = 0;
}

static void lang_file(const wchar_t *code, char *out, int n)
{
    wchar_t name[CODE_LEN + 8], w[MAX_PATH];
    _snwprintf(name, CODE_LEN + 8, L"%ls%ls", code, LANG_EXT_W); name[CODE_LEN + 7] = 0;
    lang_dir_path(name, w);
    to_u8(w, out, n);
}

/* Windows' interface language as an ISO 639 code: "it", "en"... */
static void system_lang(wchar_t *out, int n)
{
    LCID id = MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT);
    if (!GetLocaleInfoW(id, LOCALE_SISO639LANGNAME, out, n) || !code_ok(out))
        _snwprintf(out, n, L"%ls", LANG_BUILTIN_W);
    out[n - 1] = 0;
}

/* the chosen language, or Windows' one; English if its file is missing */
static void lang_choose(void)
{
    wchar_t code[CODE_LEN]; char path[MAX_PATH * 3];
    if (code_ok(g_uilang)) wcscpy(code, g_uilang);
    else system_lang(code, CODE_LEN);
    lang_file(code, path, sizeof(path));
    if (!path[0] || lang_load_file(path, NULL)) lang_reset();
}

static int code_cmp(const void *a, const void *b)
{
    return wcscmp((const wchar_t *)a, (const wchar_t *)b);
}

/* the languages in lang\, and English, which is always there */
static void lang_list(void)
{
    WIN32_FIND_DATAW fd; HANDLE h; wchar_t pat[MAX_PATH], *dot;
    int i, en = 0;
    g_nlangs = 0;
    lang_dir_path(L"*" LANG_EXT_W, pat);
    if (pat[0] && (h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (!(dot = wcsrchr(fd.cFileName, L'.')) || _wcsicmp(dot, LANG_EXT_W)) continue;
            *dot = 0;
            if (!code_ok(fd.cFileName) || g_nlangs >= MAX_LANGS) continue;
            CharLowerBuffW(fd.cFileName, (DWORD)wcslen(fd.cFileName));
            wcscpy(g_langs[g_nlangs++], fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (i = 0; i < g_nlangs; i++) en |= !wcscmp(g_langs[i], LANG_BUILTIN_W);
    if (!en && g_nlangs < MAX_LANGS) wcscpy(g_langs[g_nlangs++], LANG_BUILTIN_W);
    qsort(g_langs, (size_t)g_nlangs, sizeof(g_langs[0]), code_cmp);
}

/* the menu: "Same as Windows", then each language with its own name */
static void uilang_fill(void)
{
    HWND cb = GetDlgItem(g_wnd, ID_UILANG);
    wchar_t name[64]; char path[MAX_PATH * 3];
    int i, sel = 0;
    lang_list();
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)tr(T_SETUP_UILANG_SYS));
    for (i = 0; i < g_nlangs; i++) {
        lang_file(g_langs[i], path, sizeof(path));
        if (!lang_peek_file(path, "language.name", name, 64) || !name[0]) {
            if (!wcscmp(g_langs[i], LANG_BUILTIN_W)) wcsncpy(name, lang_builtin(T_LANG_NAME), 64);
            else wcsncpy(name, g_langs[i], 64);
            name[63] = 0;
        }
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)name);
        if (!_wcsicmp(g_langs[i], g_uilang)) sel = i + 1;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

static void search_cue(void)
{
    /* with the comctl32 6 manifest the grey placeholder text works on XP too */
    SendDlgItemMessageW(g_wnd, ID_ASEARCH, EM_SETCUEBANNER, 0, (LPARAM)tr(T_ARC_SEARCH_CUE));
}

/* every text on screen again, in the new language: no restart needed.
 * Only from setup, with no session running */
static void lang_refresh(void)
{
    fixed_texts();
    column_titles(g_list, list_cols);
    column_titles(g_alist, arc_cols);
    label_tip();
    search_cue();
    uilang_fill();
    platforms_fill();
    prot_fill();
    archive_fill(pick_dir());           /* the dates and the platform filter */
    bagpath_update();
    view(g_view);                       /* title, layout, status bar */
}

static void uilang_changed(void)
{
    int k = (int)SendDlgItemMessageW(g_wnd, ID_UILANG, CB_GETCURSEL, 0, 0);
    if (k <= 0 || k > g_nlangs) g_uilang[0] = 0;
    else wcscpy(g_uilang, g_langs[k - 1]);
    set_save(&g_set, UILANG_SEC, UILANG_KEY, L"", g_uilang);
    lang_choose();
    lang_refresh();
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
    if (id == ID_UILANG) {
        if (HIWORD(wp) == CBN_SELCHANGE && !g_running) uilang_changed();
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
        browse(ID_ADIR, tr(T_ARC_BROWSE));
        archive_dir_changed();
        return 1;
    case ID_ARESUME: resume_pick(); return 1;
    case ID_AOPEN:   open_pick(); return 1;
    case ID_AVERIFY: verify_pick(); return 1;
    case ID_ANEW:    go_setup(1); return 1;
    case ID_START:  start(); return 1;
    case ID_RESUME: resume(); return 1;
    case ID_BROWSE: browse(ID_DIR, tr(T_BROWSE_DIR)); return 1;
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
        else if (wp == TIMER_AUTO) auto_tick();
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
    case WM_APP_DISKIN:
        on_diskin(wp, lp);
        return 0;
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL) on_media_arrival((const DEV_BROADCAST_HDR *)lp);
        else if (wp == DBT_DEVICEREMOVECOMPLETE) on_media_removal((const DEV_BROADCAST_HDR *)lp);
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
            if (MessageBoxW(h, tr(T_CLOSE_ASK),
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
        watch_stop();
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
    /* automatic mode: Esc goes back to Leggi by hand for this disk */
    if (m->wParam == VK_ESCAPE && g_auto_state != AUTO_OFF) {
        auto_manual(tr(T_AUTO_ESC));
        return 1;
    }
    /* typing the label during the countdown: the read waits for Enter */
    if (g_auto_state == AUTO_COUNT && !alt && m->wParam != VK_RETURN && m->wParam != VK_TAB &&
        m->wParam != VK_SHIFT && m->wParam != VK_CONTROL && m->wParam != VK_MENU)
        auto_manual(tr(T_AUTO_TYPING));
    return 0;
}

/* "RetroArchiver.exe --tty PROGRAM ARG...": the program launched by itself
 * as runner_run_tty's intermediary, with no window. -1 if the command line
 * is not that, otherwise the program's exit code */
static int tty_mode(void);

/* "RetroArchiver.exe --convert-ini FILE": the setup turns an ini of 261017
 * into the English keys with the same code as the window, with no window.
 * -1 if the command line is not that, otherwise the keys converted (255
 * if the file cannot be written) */
static int convert_mode(void)
{
    int argc, rc; wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wv) return -1;
    if (argc != 3 || wcscmp(wv[1], L"--convert-ini")) { LocalFree(wv); return -1; }
    rc = set_convert(wv[2]);
    LocalFree(wv);
    return rc < 0 ? 255 : rc;
}

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
    if ((rc = convert_mode()) >= 0) return rc;
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

    /* the language first: the controls are created with its texts */
    set_init(&g_set);
    /* the user's ini of 261017, with Italian keys, becomes English once: the
     * defaults next to the exe are converted by the setup (a normal user
     * cannot write in Program Files), until then they are read as they are */
    set_convert(g_set.user);
    set_get(&g_set, UILANG_SEC, UILANG_KEY, L"", g_uilang, CODE_LEN);
    lang_choose();

    default_rect(&r);
    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"RetroArchiver", L"RetroArchiver",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            r.left, r.top, r.right - r.left, r.bottom - r.top,
                            NULL, NULL, inst, NULL);
    build_setup();
    build_work();
    build_archive();
    search_cue();
    uilang_fill();
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
