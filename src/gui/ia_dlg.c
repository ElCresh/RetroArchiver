/* Upload to Archive.org: one window with two tabs (Item, Files), the ocra
 * box with the public warning and the "I understand" box that enables
 * Upload, and then the same window as the progress.
 *
 * The upload runs in a thread of its own, below normal priority so that a
 * read of another box does not suffer, and talks to the window only with
 * PostMessage, as the session does. The English lines of the core go to
 * the main window's black log. The keys live in the job and nowhere else,
 * and are wiped when it ends. */
#include "gui/dialogs.h"
#include "gui/resource.h"
#include "gui/lang.h"
#include "gui/winutil.h"
#include "core/fsu8.h"
#include "core/iaupload.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#define WM_APP_IU_PROG (WM_APP + 1)      /* lParam: ia_progress* (a copy) */
#define WM_APP_IU_DONE (WM_APP + 2)      /* lParam: ia_result* */
#define WM_APP_IU_ID   (WM_APP + 3)      /* lParam: ia_result* of the identifier check */

enum { IU_ITEM, IU_FILES, IU_PAGES };

typedef struct {
    char bagdir[700];
    wchar_t collection[64], prefix[64], userini[MAX_PATH];
    const settings *set;
    HWND logwnd;
    UINT logmsg, donemsg;
    HWND dlg, tabs, pages[IU_PAGES], list;
    ia_meta proposed;                    /* what the box proposes: Restore goes back to it */
    ia_meta sent;                        /* what the upload is given */
    ia_files plan;
    ia_record rec;
    int has_rec;
    int missing;                         /* files an interrupted upload still has to send */
    long long missing_bytes;
    char cacert[3 * MAX_PATH];
    ia_keys keys;
    int running, asking, closing, uploaded;
    volatile LONG cancel;
    char id[IA_ID_MAX];
} iudlg;

static const int page_dlg[IU_PAGES] = { IDD_IA_ITEM, IDD_IA_FILES };
static const int page_title[IU_PAGES] = { T_IAUP_TAB_ITEM, T_IAUP_TAB_FILES };

static const struct { int page, id, text; } texts[] = {
    { IU_ITEM, IDC_IU_ID_L,      T_IAUP_ID },
    { IU_ITEM, IDC_IU_CHECK,     T_IAUP_CHECK },
    { IU_ITEM, IDC_IU_COLL_L,    T_IAUP_COLL },
    { IU_ITEM, IDC_IU_TITLE_L,   T_IAUP_ITITLE },
    { IU_ITEM, IDC_IU_CREATOR_L, T_IAUP_CREATOR },
    { IU_ITEM, IDC_IU_PUB_L,     T_IAUP_PUBLISHER },
    { IU_ITEM, IDC_IU_DATE_L,    T_IAUP_DATE },
    { IU_ITEM, IDC_IU_LANG_L,    T_IAUP_LANGS },
    { IU_ITEM, IDC_IU_SUBJ_L,    T_IAUP_SUBJECTS },
    { IU_ITEM, IDC_IU_DESC_L,    T_IAUP_DESC },
    { IU_ITEM, IDC_IU_RIGHTS_L,  T_IAUP_RIGHTS },
    { IU_ITEM, IDC_IU_RESET,     T_IAUP_RESET },
    { -1, 0, 0 }
};

/* ---- small helpers ---- */

static void put_w(wchar_t *out, int n, const wchar_t *s)
{
    wcsncpy(out, s, (size_t)n);
    out[n - 1] = 0;
}

static void mb_text(long long bytes, wchar_t *out, int n)
{
    wchar_t num[48];
    lang_number((double)bytes / (1024.0 * 1024.0), 1, num, 48);
    _snwprintf(out, (size_t)n, L"%ls MB", num);
    out[n - 1] = 0;
}

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

static void set_u8(HWND w, int id, const char *s)
{
    wchar_t t[IA_DESC_MAX];
    to_w(s, t, IA_DESC_MAX);
    SetDlgItemTextW(w, id, t);
}

static void get_u8(HWND w, int id, char *out, int n)
{
    wchar_t *t = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!t) { out[0] = 0; return; }
    GetDlgItemTextW(w, id, t, n);
    to_u8(t, out, n);
    free(t);
}

/* free text with line breaks: the core writes "\n", a Windows EDIT wants
 * "\r\n" and shows a lone "\n" as a box; back to "\n" when read */
static void set_multi(HWND w, int id, const char *s)
{
    size_t n = strlen(s), k = 0, i;
    char *t = (char *)malloc(n * 2 + 1);
    if (!t) { set_u8(w, id, s); return; }
    for (i = 0; i < n; i++) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) t[k++] = '\r';
        t[k++] = s[i];
    }
    t[k] = 0;
    set_u8(w, id, t);
    free(t);
}

static void get_multi(HWND w, int id, char *out, int n)
{
    char *r, *p;
    get_u8(w, id, out, n);
    for (r = p = out; *r; r++) if (*r != '\r') *p++ = *r;
    *p = 0;
}

/* one value per line, in a multiline field */
static void set_list(HWND w, int id, const char (*v)[80], int n, int len)
{
    char buf[IA_MAX_SUBJECT * 84 + 4];
    int i;
    buf[0] = 0;
    for (i = 0; i < n; i++) {
        if (i) strcat(buf, "\r\n");
        strncat(buf, v[i], (size_t)len - 1);
    }
    set_u8(w, id, buf);
}

static void set_list16(HWND w, int id, const char (*v)[24], int n)
{
    char buf[IA_MAX_LANG * 28 + 4];
    int i;
    buf[0] = 0;
    for (i = 0; i < n; i++) {
        if (i) strcat(buf, "\r\n");
        strncat(buf, v[i], 23);
    }
    set_u8(w, id, buf);
}

/* the lines of a multiline field back into the array; blanks dropped */
static int get_lines(HWND w, int id, char *dst, int stride, int max, int len)
{
    char text[IA_MAX_SUBJECT * 84 + 64], *p;
    int n = 0;
    get_u8(w, id, text, (int)sizeof(text));
    for (p = text; *p && n < max; ) {
        char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        while (e > p && e[-1] == ' ') e--;
        if (e > p) {
            int k = (int)(e - p) < len - 1 ? (int)(e - p) : len - 1;
            memcpy(dst + (size_t)n * (size_t)stride, p, (size_t)k);
            dst[(size_t)n * (size_t)stride + (size_t)k] = 0;
            n++;
        }
        p = *e ? e + 1 : e;
        while (*p == '\n' || *p == '\r') p++;
    }
    return n;
}

/* ---- the Item tab ---- */

static void fields_set(iudlg *d, const ia_meta *m)
{
    HWND p = d->pages[IU_ITEM];
    if (!p) return;
    set_u8(p, IDC_IU_COLL, m->collection);
    set_u8(p, IDC_IU_TITLE, m->title);
    set_u8(p, IDC_IU_CREATOR, m->creator);
    set_u8(p, IDC_IU_PUB, m->publisher);
    set_u8(p, IDC_IU_DATE, m->date);
    set_multi(p, IDC_IU_RIGHTS, m->rights);
    set_multi(p, IDC_IU_DESC, m->description);
    set_list(p, IDC_IU_SUBJ, m->subject, m->nsubj, 80);
    set_list16(p, IDC_IU_LANG, m->language, m->nlang);
}

static void fields_get(iudlg *d, ia_meta *m)
{
    HWND p = d->pages[IU_ITEM];
    *m = d->proposed;                    /* mediatype, collection and the rest stay */
    if (!p) return;
    get_u8(p, IDC_IU_TITLE, m->title, (int)sizeof(m->title));
    get_u8(p, IDC_IU_CREATOR, m->creator, (int)sizeof(m->creator));
    get_u8(p, IDC_IU_PUB, m->publisher, (int)sizeof(m->publisher));
    get_u8(p, IDC_IU_DATE, m->date, (int)sizeof(m->date));
    get_multi(p, IDC_IU_RIGHTS, m->rights, (int)sizeof(m->rights));
    get_multi(p, IDC_IU_DESC, m->description, (int)sizeof(m->description));
    m->nsubj = get_lines(p, IDC_IU_SUBJ, m->subject[0], (int)sizeof(m->subject[0]),
                         IA_MAX_SUBJECT, (int)sizeof(m->subject[0]));
    m->nlang = get_lines(p, IDC_IU_LANG, m->language[0], (int)sizeof(m->language[0]),
                         IA_MAX_LANG, (int)sizeof(m->language[0]));
}

static void id_status(iudlg *d, const wchar_t *s)
{
    if (d->pages[IU_ITEM]) SetDlgItemTextW(d->pages[IU_ITEM], IDC_IU_ID_ST, s);
}

/* while it is typed: only the local rules, no question asked */
static void id_changed(iudlg *d)
{
    char id[IA_ID_MAX];
    get_u8(d->pages[IU_ITEM], IDC_IU_ID, id, (int)sizeof(id));
    str_copy(d->id, sizeof(d->id), id);
    id_status(d, ia_ident_ok(id) ? L"" : tr(T_IAUP_ID_BAD));
    EnableWindow(GetDlgItem(d->dlg, IDC_IU_UPLOAD),
                 ia_ident_ok(id) && !d->running &&
                 IsDlgButtonChecked(d->dlg, IDC_IU_UNDERSTOOD) == BST_CHECKED);
}

/* ---- the Files tab ---- */

static void cell(HWND lv, int row, int col, const wchar_t *t)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.iSubItem = col;
    it.pszText = (wchar_t *)t;
    SendMessageW(lv, LVM_SETITEMTEXTW, row, (LPARAM)&it);
}

static const wchar_t *what_text(int what)
{
    static const int k[] = { T_IAUP_WHAT_SAME, T_IAUP_WHAT_NEW, T_IAUP_WHAT_CHANGED,
                             T_IAUP_WHAT_REMOVED };
    return tr(k[what >= IA_F_SAME && what <= IA_F_REMOVED ? what : IA_F_SAME]);
}

static void files_fill(iudlg *d)
{
    static const int head[] = { T_IAUP_COL_FILE, T_IAUP_COL_WHAT, T_IAUP_COL_SIZE };
    static const int width[] = { 180, 60, 68 };
    LVCOLUMNW c; LVITEMW it; wchar_t w[600]; int i;
    long long total = 0;

    d->list = GetDlgItem(d->pages[IU_FILES], IDC_IU_LIST);
    SendMessageW(d->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
    for (i = 0; i < 3; i++) {
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.pszText = (wchar_t *)tr(head[i]);
        c.cx = width[i];
        SendMessageW(d->list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
    for (i = 0; i < d->plan.n; i++) {
        const ia_file *f = &d->plan.f[i];
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        to_w(f->name, w, 600);
        it.pszText = w;
        SendMessageW(d->list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        cell(d->list, i, 1, what_text(f->what));
        mb_text(f->size, w, 600);
        cell(d->list, i, 2, w);
        if (f->what != IA_F_REMOVED) total += f->size;
    }
    mb_text(total, w, 600);
    {
        wchar_t t[700];
        _snwprintf(t, 700, tr(T_IAUP_TOTAL), d->plan.n, w);
        t[699] = 0;
        SetDlgItemTextW(d->pages[IU_FILES], IDC_IU_TOTAL, t);
    }
}

/* ---- what went wrong, in the window's words ---- */

static void when_text(long long t, wchar_t *out, int n)
{
    time_t tt = (time_t)t;
    struct tm *g = gmtime(&tt);
    char iso[40] = "";
    if (g) snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d", g->tm_year + 1900,
                    g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
    lang_date(iso, out, (size_t)n);
}

/* the same words as the Archive.org tab of the Settings */
static void tls_text(const iudlg *d, const tls_info *ti, wchar_t *out, int n)
{
    wchar_t a[64], b[64], c[64], dir[MAX_PATH];
    switch (ti->why) {
    case TLS_ERR_CLOCK:
        when_text(ti->now, a, 64);
        when_text(ti->not_before, b, 64);
        if (!ti->not_after) {
            _snwprintf(out, (size_t)n, tr(T_IA_ST_CLOCK_OLD), a, b);
        } else {
            when_text(ti->not_after, c, 64);
            _snwprintf(out, (size_t)n, tr(T_IA_ST_CLOCK), a, b, c);
        }
        break;
    case TLS_ERR_UNKNOWN_ROOT:
        user_ini_dir(d->userini, dir, MAX_PATH);
        _snwprintf(out, (size_t)n, tr(T_IA_ST_ROOT), dir);
        break;
    case TLS_ERR_BAD_NAME: put_w(out, n, tr(T_IA_ST_NAME)); break;
    case TLS_ERR_NET:      put_w(out, n, tr(T_IA_ST_NET)); break;
    default:               put_w(out, n, tr(T_IA_ST_TLS)); break;
    }
    out[n - 1] = 0;
}

static const wchar_t *box_why_text(int why)
{
    static const int k[] = { 0, T_IAUP_BOX_NOTBOX, T_IAUP_BOX_OPEN, T_IAUP_BOX_BROKEN,
                             T_IAUP_BOX_EDITING, T_IAUP_BOX_OLD };
    return tr(k[why > IA_BOX_OK && why <= IA_BOX_OLD ? why : IA_BOX_NOTBOX]);
}

static void result_text(const iudlg *d, const ia_result *r, wchar_t *out, int n)
{
    wchar_t file[400], detail[500], dir[MAX_PATH], id[IA_ID_MAX + 8];
    to_w(r->file, file, 400);
    to_w(r->detail, detail, 500);
    to_w(r->identifier, id, IA_ID_MAX + 8);
    switch (r->why) {
    case IA_OK:
        _snwprintf(out, (size_t)n, tr(T_IAUP_DONE_OK), r->files_sent);
        break;
    case IA_ERR_PENDING:
        _snwprintf(out, (size_t)n, tr(T_IAUP_DONE_PENDING), r->files_sent);
        break;
    case IA_ERR_VERIFY:
        _snwprintf(out, (size_t)n, tr(T_IAUP_DONE_VERIFY), r->mismatch, file, detail);
        break;
    case IA_ERR_CANCELED:
        _snwprintf(out, (size_t)n, tr(T_IAUP_DONE_STOPPED), r->files_sent);
        break;
    case IA_ERR_KEYS:    put_w(out, n, tr(T_IAUP_ERR_KEYS)); break;
    case IA_ERR_TAKEN:   _snwprintf(out, (size_t)n, tr(T_IAUP_ERR_TAKEN), id); break;
    case IA_ERR_IDENT:   put_w(out, n, tr(T_IAUP_ID_BAD)); break;
    case IA_ERR_SPAM:    put_w(out, n, tr(T_IAUP_ERR_SPAM)); break;
    case IA_ERR_CHANGED: _snwprintf(out, (size_t)n, tr(T_IAUP_ERR_CHANGED), file); break;
    case IA_ERR_FILE:    _snwprintf(out, (size_t)n, tr(T_IAUP_ERR_FILE), file); break;
    case IA_ERR_RECORD:  put_w(out, n, tr(T_IAUP_ERR_RECORD)); break;
    case IA_ERR_BOX:     _snwprintf(out, (size_t)n, tr(T_IAUP_ERR_BOX), box_why_text(r->box_why)); break;
    case IA_ERR_NO_CERTS:
        user_ini_dir(d->userini, dir, MAX_PATH);
        _snwprintf(out, (size_t)n, tr(T_IA_ST_NOCERTS), dir);
        break;
    case IA_ERR_TLS:     tls_text(d, &r->tls, out, n); break;
    case IA_ERR_NET:     put_w(out, n, tr(T_IA_ST_NET)); break;
    default:             _snwprintf(out, (size_t)n, tr(T_IAUP_ERR_HTTP), r->status, detail); break;
    }
    out[n - 1] = 0;
}

/* ---- the worker thread ---- */

typedef struct {
    HWND dlg;
    HWND logwnd;                         /* the main window's black log */
    UINT logmsg;
    char bagdir[700];
    char cacert[3 * MAX_PATH];
    char id[IA_ID_MAX];
    ia_keys keys;
    ia_meta meta;
    volatile LONG *cancel;
    int last_pct, last_file;
} iujob;

static void job_progress(void *ctx, const ia_progress *p)
{
    iujob *j = (iujob *)ctx;
    ia_progress *copy;
    int pct = p->total > 0 ? (int)((p->total_done + p->done) * 100 / p->total) : 0;
    /* one message per percent and per file: during a DVD there would be
     * thousands a second, and WM_PAINT would come much later */
    if (pct == j->last_pct && p->file == j->last_file && p->phase != IA_PH_RETRY &&
        p->phase != IA_PH_TASKS)
        return;
    j->last_pct = pct;
    j->last_file = p->file;
    if ((copy = (ia_progress *)malloc(sizeof(*copy))) == NULL) return;
    *copy = *p;
    copy->name = NULL;                   /* the name travels in the window's own copy */
    if (!PostMessageW(j->dlg, WM_APP_IU_PROG, (WPARAM)pct, (LPARAM)copy)) free(copy);
}

static void job_log(void *ctx, const char *line)
{
    iujob *j = (iujob *)ctx;
    wchar_t *w;
    size_t n = strlen(line) + 1;
    if (!j->logwnd || !j->logmsg) return;
    if ((w = (wchar_t *)malloc(n * sizeof(wchar_t))) == NULL) return;
    to_w(line, w, (int)n);
    if (!PostMessageW(j->logwnd, j->logmsg, 0, (LPARAM)w)) free(w);
}

static int job_canceled(void *ctx)
{
    iujob *j = (iujob *)ctx;
    return *j->cancel != 0;
}

static DWORD WINAPI upload_worker(LPVOID p)
{
    iujob *j = (iujob *)p;
    ia_result *r = (ia_result *)calloc(1, sizeof(ia_result));
    ia_config cfg;
    ia_host host;
    if (r) {
        const char *md = getenv("RETROARCHIVER_IA_URL");
        const char *s3 = getenv("RETROARCHIVER_IA_S3_URL");
        memset(&cfg, 0, sizeof(cfg));
        memset(&host, 0, sizeof(host));
        cfg.bagdir = j->bagdir;
        cfg.md_base = md && md[0] ? md : NULL;
        cfg.s3_base = s3 && s3[0] ? s3 : NULL;
        cfg.cacert = j->cacert;
        cfg.access = j->keys.access;
        cfg.secret = j->keys.secret;
        cfg.identifier = j->id;
        cfg.meta = &j->meta;
        host.ctx = j;
        host.progress = job_progress;
        host.log = job_log;
        host.canceled = job_canceled;
        ia_upload(&cfg, &host, r);
        if (!PostMessageW(j->dlg, WM_APP_IU_DONE, 0, (LPARAM)r)) free(r);
    }
    SecureZeroMemory(&j->keys, sizeof(j->keys));
    free(j);
    return 0;
}

/* the identifier check: a short thread of the same kind */
typedef struct { HWND dlg; char id[IA_ID_MAX], mine[IA_ID_MAX], cacert[3 * MAX_PATH]; ia_keys keys; } idjob;

static DWORD WINAPI id_worker(LPVOID p)
{
    idjob *j = (idjob *)p;
    ia_result *r = (ia_result *)calloc(1, sizeof(ia_result));
    ia_config cfg;
    if (r) {
        const char *md = getenv("RETROARCHIVER_IA_URL");
        memset(&cfg, 0, sizeof(cfg));
        cfg.md_base = md && md[0] ? md : NULL;
        cfg.cacert = j->cacert;
        cfg.access = j->keys.access;
        cfg.secret = j->keys.secret;
        ia_ident_check(&cfg, j->id, j->mine[0] ? j->mine : NULL, r);
        if (!PostMessageW(j->dlg, WM_APP_IU_ID, 0, (LPARAM)r)) free(r);
    }
    SecureZeroMemory(j, sizeof(*j));
    free(j);
    return 0;
}

static void id_check_start(iudlg *d)
{
    idjob *j;
    HANDLE th;
    if (d->asking || d->running || !ia_ident_ok(d->id)) return;
    if ((j = (idjob *)calloc(1, sizeof(idjob))) == NULL) return;
    j->dlg = d->dlg;
    str_copy(j->id, sizeof(j->id), d->id);
    if (d->has_rec) str_copy(j->mine, sizeof(j->mine), d->rec.identifier);
    str_copy(j->cacert, sizeof(j->cacert), d->cacert);
    j->keys = d->keys;
    if ((th = CreateThread(NULL, 0, id_worker, j, 0, NULL)) == NULL) {
        SecureZeroMemory(j, sizeof(*j));
        free(j);
        return;
    }
    CloseHandle(th);
    d->asking = 1;
    EnableWindow(GetDlgItem(d->pages[IU_ITEM], IDC_IU_CHECK), 0);
    id_status(d, tr(T_IAUP_ID_ASKING));
}

static void id_check_done(iudlg *d, ia_result *r)
{
    wchar_t w[800];
    d->asking = 0;
    EnableWindow(GetDlgItem(d->pages[IU_ITEM], IDC_IU_CHECK), 1);
    if (r->why == IA_OK)
        id_status(d, tr(r->ident == IA_ID_FREE ? T_IAUP_ID_FREE :
                        r->ident == IA_ID_OURS ? T_IAUP_ID_OURS : T_IAUP_ID_TAKEN));
    else {
        result_text(d, r, w, 800);
        id_status(d, w);
    }
    free(r);
}

/* ---- the upload ---- */

static void show_running(iudlg *d, int on)
{
    int i;
    d->running = on;
    for (i = 0; i < IU_PAGES; i++) if (d->pages[i]) EnableWindow(d->pages[i], !on);
    EnableWindow(GetDlgItem(d->dlg, IDC_IU_UNDERSTOOD), !on);
    ShowWindow(GetDlgItem(d->dlg, IDC_IU_WARN), on ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(d->dlg, IDC_IU_UNDERSTOOD), on ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(d->dlg, IDC_IU_PROG), on ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(d->dlg, IDC_IU_BAR_FILE), on ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(d->dlg, IDC_IU_BAR_TOTAL), on ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(d->dlg, IDC_IU_UPLOAD), !on);
    ShowWindow(GetDlgItem(d->dlg, IDC_IU_UPLOAD), on ? SW_HIDE : SW_SHOW);
    EnableWindow(GetDlgItem(d->dlg, IDC_IU_STOP), on);
    ShowWindow(GetDlgItem(d->dlg, IDC_IU_STOP), on ? SW_SHOW : SW_HIDE);
}

static void upload_start(iudlg *d)
{
    iujob *j;
    HANDLE th;
    if (d->running || !ia_ident_ok(d->id)) return;
    if ((j = (iujob *)calloc(1, sizeof(iujob))) == NULL) return;
    fields_get(d, &d->sent);
    j->dlg = d->dlg;
    j->logwnd = d->logwnd;
    j->logmsg = d->logmsg;
    j->last_pct = -1;
    j->last_file = -1;
    str_copy(j->bagdir, sizeof(j->bagdir), d->bagdir);
    str_copy(j->cacert, sizeof(j->cacert), d->cacert);
    str_copy(j->id, sizeof(j->id), d->id);
    j->keys = d->keys;
    j->meta = d->sent;
    d->cancel = 0;
    j->cancel = &d->cancel;
    if ((th = CreateThread(NULL, 0, upload_worker, j, 0, NULL)) == NULL) {
        SecureZeroMemory(j, sizeof(*j));
        free(j);
        return;
    }
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
    show_running(d, 1);
    SetDlgItemTextW(d->dlg, IDC_IU_PROG, tr(T_IAUP_PH_PREPARE));
    SendDlgItemMessageW(d->dlg, IDC_IU_BAR_FILE, PBM_SETRANGE32, 0, 1000);
    SendDlgItemMessageW(d->dlg, IDC_IU_BAR_TOTAL, PBM_SETRANGE32, 0, 1000);
}

static void on_progress(iudlg *d, int pct, ia_progress *p)
{
    wchar_t w[700], name[600];
    int fpct = p->size > 0 ? (int)(p->done * 1000 / p->size) : 0;
    name[0] = 0;
    if (p->file >= 1 && p->file <= d->plan.n) to_w(d->plan.f[p->file - 1].name, name, 600);
    switch (p->phase) {
    case IA_PH_HASH:    _snwprintf(w, 700, tr(T_IAUP_PH_HASH), name); break;
    case IA_PH_PUT:     _snwprintf(w, 700, tr(T_IAUP_PH_PUT), name); break;
    case IA_PH_META:    put_w(w, 700, tr(T_IAUP_PH_META)); break;
    case IA_PH_TASKS:   put_w(w, 700, tr(T_IAUP_PH_TASKS)); break;
    case IA_PH_VERIFY:  put_w(w, 700, tr(T_IAUP_PH_VERIFY)); break;
    case IA_PH_RETRY:   _snwprintf(w, 700, tr(T_IAUP_PH_RETRY), p->seconds); break;
    default:            put_w(w, 700, tr(T_IAUP_PH_PREPARE)); break;
    }
    w[699] = 0;
    SetDlgItemTextW(d->dlg, IDC_IU_PROG, w);
    SendDlgItemMessageW(d->dlg, IDC_IU_BAR_FILE, PBM_SETPOS, (WPARAM)fpct, 0);
    SendDlgItemMessageW(d->dlg, IDC_IU_BAR_TOTAL, PBM_SETPOS, (WPARAM)(pct * 10), 0);
    free(p);
}

static void on_done(iudlg *d, ia_result *r)
{
    wchar_t w[1200], url[400];
    show_running(d, 0);
    if (r->files_sent > 0 || r->why == IA_OK) d->uploaded = 1;
    result_text(d, r, w, 1200);
    SetDlgItemTextW(d->dlg, IDC_IU_PROG, w);
    ShowWindow(GetDlgItem(d->dlg, IDC_IU_PROG), SW_SHOW);
    if (r->why == IA_OK || r->why == IA_ERR_PENDING || r->why == IA_ERR_VERIFY) {
        to_w(r->url, url, 400);
        SetDlgItemTextW(d->dlg, IDC_IU_URL, url);
        ShowWindow(GetDlgItem(d->dlg, IDC_IU_URL), SW_SHOW);
        ShowWindow(GetDlgItem(d->dlg, IDC_IU_OPEN), SW_SHOW);
    }
    /* Upload becomes Resume when there is something left to send */
    SetDlgItemTextW(d->dlg, IDC_IU_UPLOAD,
                    tr(r->why == IA_OK ? T_IAUP_UPLOAD : T_IAUP_RESUME));
    free(r);
    if (d->closing) DestroyWindow(d->dlg);
}

/* ---- the dialog ---- */

static void show_page(iudlg *d, int pg)
{
    int i;
    for (i = 0; i < IU_PAGES; i++) ShowWindow(d->pages[i], i == pg ? SW_SHOW : SW_HIDE);
}

/* the controls of the tabs: the identifier, Check and Restore */
static INT_PTR CALLBACK page_proc(HWND page, UINT m, WPARAM wp, LPARAM lp)
{
    iudlg *d = (iudlg *)GetWindowLongPtrW(page, GWLP_USERDATA);
    switch (m) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(page, GWLP_USERDATA, lp);
        return FALSE;
    case WM_COMMAND:
        if (!d) return FALSE;
        if (LOWORD(wp) == IDC_IU_ID && HIWORD(wp) == EN_CHANGE) id_changed(d);
        else if (LOWORD(wp) == IDC_IU_CHECK) id_check_start(d);
        else if (LOWORD(wp) == IDC_IU_RESET) fields_set(d, &d->proposed);
        return TRUE;
    }
    return FALSE;
}

static void dlg_init(HWND dlg, const ia_dlg_args *a, iudlg *d)
{
    RECT r; TCITEMW it; wchar_t cap[600], t[400], w[800];
    strbuf err;
    meta_box *b;
    char coll[64];
    int i;

    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
    d->dlg = dlg;
    /* everything the caller gave is copied: the window outlives the call */
    str_copy(d->bagdir, sizeof(d->bagdir), a->bagdir ? a->bagdir : "");
    put_w(d->collection, 64, a->collection ? a->collection : L"");
    put_w(d->prefix, 64, a->prefix ? a->prefix : L"");
    put_w(d->userini, MAX_PATH, a->set && a->set->user[0] ? a->set->user : L"");
    d->set = a->set;
    d->logwnd = a->logwnd;
    d->logmsg = a->logmsg;
    d->donemsg = a->donemsg;
    sb_init(&err);
    to_u8(d->collection, coll, (int)sizeof(coll));
    ia_meta_from_box(d->bagdir, coll, &d->proposed, &err);
    ia_plan(d->bagdir, NULL, &d->plan, &err);
    d->has_rec = ia_record_load(d->bagdir, &d->rec);
    cacert_path(d->userini, d->cacert, (int)sizeof(d->cacert));
    if (d->set) set_ia_keys_get(d->set, &d->keys);

    to_w(d->proposed.title, t, 400);
    _snwprintf(cap, 600, tr(T_IAUP_TITLE), t);
    cap[599] = 0;
    SetWindowTextW(dlg, cap);
    SetDlgItemTextW(dlg, IDC_IU_WARN, tr(T_IAUP_WARN));
    SetDlgItemTextW(dlg, IDC_IU_UNDERSTOOD, tr(T_IAUP_UNDERSTOOD));
    SetDlgItemTextW(dlg, IDC_IU_UPLOAD, tr(T_IAUP_UPLOAD));
    SetDlgItemTextW(dlg, IDC_IU_STOP, tr(T_IAUP_STOP));
    SetDlgItemTextW(dlg, IDCANCEL, tr(T_IAUP_CLOSE));
    SetDlgItemTextW(dlg, IDC_IU_OPEN, tr(T_IAUP_OPENPAGE));
    ShowWindow(GetDlgItem(dlg, IDC_IU_PROG), SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_IU_BAR_FILE), SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_IU_BAR_TOTAL), SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_IU_STOP), SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_IU_URL), SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_IU_OPEN), SW_HIDE);
    EnableWindow(GetDlgItem(dlg, IDC_IU_UPLOAD), 0);

    d->tabs = GetDlgItem(dlg, IDC_TABS);
    for (i = 0; i < IU_PAGES; i++) {
        memset(&it, 0, sizeof(it));
        it.mask = TCIF_TEXT;
        it.pszText = (wchar_t *)tr(page_title[i]);
        SendMessageW(d->tabs, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    GetWindowRect(d->tabs, &r);
    MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
    TabCtrl_AdjustRect(d->tabs, FALSE, &r);
    for (i = IU_PAGES - 1; i >= 0; i--) {
        int k;
        d->pages[i] = CreateDialogParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(page_dlg[i]),
                                         dlg, page_proc, (LPARAM)d);
        if (!d->pages[i]) continue;
        theme_page(d->pages[i]);
        for (k = 0; texts[k].page >= 0; k++)
            if (texts[k].page == i) SetDlgItemTextW(d->pages[i], texts[k].id, tr(texts[k].text));
        SetWindowPos(d->pages[i], d->tabs, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE);
    }
    /* the identifier: the one of the item if the box already has one (it
     * never changes), otherwise the proposal from the box */
    if (d->has_rec && d->rec.identifier[0]) {
        str_copy(d->id, sizeof(d->id), d->rec.identifier);
        EnableWindow(GetDlgItem(d->pages[IU_ITEM], IDC_IU_ID), 0);
    } else if ((b = meta_new()) != NULL) {
        char data[800], prefix[64];
        u8_join(data, sizeof(data), d->bagdir, "data");
        to_u8(d->prefix, prefix, (int)sizeof(prefix));
        if (meta_load(b, data, &err) == 0) ia_ident_propose(b, prefix, d->id, sizeof(d->id));
        meta_delete(b);
    }
    set_u8(d->pages[IU_ITEM], IDC_IU_ID, d->id);
    SendDlgItemMessageW(d->pages[IU_ITEM], IDC_IU_ID, EM_LIMITTEXT, IA_ID_MAX - 1, 0);
    fields_set(d, &d->proposed);
    files_fill(d);
    show_page(d, IU_ITEM);
    id_changed(d);

    /* an upload left halfway: say what is missing, so Resume is a choice */
    {
        ia_files done;
        char tag[65];
        ia_tagmanifest_digest(d->bagdir, tag);
        if (ia_journal_read(d->bagdir, tag, &done) >= 0 && done.n) {
            long long left = 0;
            int miss = 0;
            for (i = 0; i < d->plan.n; i++)
                if (!ia_files_find(&done, d->plan.f[i].name)) { miss++; left += d->plan.f[i].size; }
            d->missing = miss;
            d->missing_bytes = left;
            mb_text(left, t, 400);
            _snwprintf(w, 800, tr(T_IAUP_RESUMABLE), miss, t);
            w[799] = 0;
            SetDlgItemTextW(dlg, IDC_IU_WARN, w);
            SetDlgItemTextW(dlg, IDC_IU_UPLOAD, tr(T_IAUP_RESUME));
        }
        ia_files_free(&done);
    }
    sb_free(&err);
}

/* Closing during an upload is allowed (it is not a save): the upload
 * stops and can be resumed; the window stays until the thread answers,
 * so nothing it still reads is freed */
static void ask_close(iudlg *d)
{
    if (!d->running) { DestroyWindow(d->dlg); return; }
    if (MessageBoxW(d->dlg, tr(T_IAUP_CLOSE_ASK), L"RetroArchiver",
                    MB_ICONQUESTION | MB_YESNO) != IDYES) return;
    d->closing = 1;
    InterlockedExchange(&d->cancel, 1);
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp)
{
    iudlg *d = (iudlg *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (m) {
    case WM_INITDIALOG: {
        const ia_dlg_args *a = (const ia_dlg_args *)lp;
        iudlg *nd = (iudlg *)calloc(1, sizeof(iudlg));
        if (!nd) { DestroyWindow(dlg); return TRUE; }
        dlg_init(dlg, a, nd);
        return TRUE;
    }
    case WM_DESTROY:
        if (d) {
            if (d->logwnd && d->donemsg) PostMessageW(d->logwnd, d->donemsg, (WPARAM)d->uploaded, 0);
            ia_files_free(&d->plan);
            ia_record_free(&d->rec);
            SecureZeroMemory(&d->keys, sizeof(d->keys));
            SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
            free(d);
        }
        return TRUE;
    case WM_APP_IU_PROG:
        if (d) on_progress(d, (int)wp, (ia_progress *)lp); else free((void *)lp);
        return TRUE;
    case WM_APP_IU_DONE:
        if (d) on_done(d, (ia_result *)lp); else free((void *)lp);
        return TRUE;
    case WM_APP_IU_ID:
        if (d) id_check_done(d, (ia_result *)lp); else free((void *)lp);
        return TRUE;
    case WM_NOTIFY:
        if (d && ((NMHDR *)lp)->hwndFrom == d->tabs && ((NMHDR *)lp)->code == TCN_SELCHANGE)
            show_page(d, TabCtrl_GetCurSel(d->tabs));
        return TRUE;
    case WM_COMMAND:
        if (!d) return FALSE;
        switch (LOWORD(wp)) {
        case IDC_IU_UNDERSTOOD: id_changed(d); return TRUE;
        case IDC_IU_UPLOAD:     upload_start(d); return TRUE;
        case IDC_IU_STOP:       InterlockedExchange(&d->cancel, 1); return TRUE;
        case IDC_IU_OPEN: {
            wchar_t url[400];
            GetDlgItemTextW(dlg, IDC_IU_URL, url, 400);
            if (url[0]) ShellExecuteW(dlg, L"open", url, NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        case IDCANCEL: ask_close(d); return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        if (d) ask_close(d);
        return TRUE;
    }
    return FALSE;
}

HWND ia_upload_open(HWND owner, const ia_dlg_args *a)
{
    HWND dlg = CreateDialogParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_IAUPLOAD),
                                  owner, dlg_proc, (LPARAM)a);
    if (dlg) ShowWindow(dlg, SW_SHOW);
    return dlg;
}
