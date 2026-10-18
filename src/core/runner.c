#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runner.h"
#include "strbuf.h"

/* collects the bytes and calls on_line at every \n or \r */
typedef struct { strbuf line; runner_line_fn fn; void *ctx; } splitter;

static void feed(splitter *s, const char *buf, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            if (s->line.len && s->fn) s->fn(s->ctx, s->line.s);
            sb_clear(&s->line);
        } else {
            char c[2]; c[0] = buf[i]; c[1] = 0;
            sb_puts(&s->line, c);
        }
    }
}

static void flush(splitter *s)
{
    if (s->line.len && s->fn) s->fn(s->ctx, s->line.s);
    sb_clear(&s->line);
}

#ifdef _WIN32
#include <windows.h>

/* copy of the running process's handle: whoever takes it with the atomic
 * exchange closes it, so runner_cancel never uses an already closed handle */
static PVOID volatile running = NULL;
static volatile LONG canceled = 0;

/* SAMdisk writes in the OEM code page, the program's internal text is UTF-8 */
typedef struct { runner_line_fn fn; void *ctx; } oem_fwd;

static void oem_to_utf8(void *c, const char *line)
{
    oem_fwd *f = (oem_fwd *)c; wchar_t *w; char *u = NULL; int nw, nu;
    nw = MultiByteToWideChar(CP_OEMCP, 0, line, -1, NULL, 0);
    w = nw > 0 ? (wchar_t *)malloc((size_t)nw * sizeof(wchar_t)) : NULL;
    if (w && MultiByteToWideChar(CP_OEMCP, 0, line, -1, w, nw) > 0) {
        nu = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        u = nu > 0 ? (char *)malloc((size_t)nu) : NULL;
        if (u && WideCharToMultiByte(CP_UTF8, 0, w, -1, u, nu, NULL, NULL) <= 0) { free(u); u = NULL; }
    }
    f->fn(f->ctx, u ? u : line);
    free(u); free(w);
}

/* quotes an argument the way the Windows C library parses it back */
static void quote(strbuf *cmd, const char *a)
{
    const char *p; int bs = 0, k;
    if (*a && !strpbrk(a, " \t\"")) { sb_puts(cmd, a); return; }
    sb_puts(cmd, "\"");
    for (p = a; ; p++) {
        if (*p == '\\') { bs++; continue; }
        if (*p == '"' || *p == 0) {
            int n = bs * 2 + (*p == '"');
            for (k = 0; k < n; k++) sb_puts(cmd, "\\");
            bs = 0;
            if (*p == 0) break;
            sb_puts(cmd, "\"");
            continue;
        }
        for (; bs; bs--) sb_puts(cmd, "\\");
        { char c[2]; c[0] = *p; c[1] = 0; sb_puts(cmd, c); }
    }
    sb_puts(cmd, "\"");
}

/* starts argv with stdin from NUL and stdout/stderr on a pipe, returning
 * its read end in *rd; 0 if it does not start */
static int spawn(const char *const *argv, PROCESS_INFORMATION *pi, HANDLE *rd)
{
    strbuf cmd; SECURITY_ATTRIBUTES sa; STARTUPINFOW si;
    HANDLE wr = NULL, nul; wchar_t *wcmd; int i, n, ok;

    sb_init(&cmd);
    for (i = 0; argv[i]; i++) { if (i) sb_puts(&cmd, " "); quote(&cmd, argv[i]); }
    n = MultiByteToWideChar(CP_UTF8, 0, cmd.s, -1, NULL, 0);
    wcmd = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!wcmd) { sb_free(&cmd); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, cmd.s, -1, wcmd, n);
    sb_free(&cmd);

    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(rd, &wr, &sa, 0)) { free(wcmd); return 0; }
    SetHandleInformation(*rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(*rd); CloseHandle(wr); free(wcmd);
        return 0;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    /* SW_HIDE: also the console that the runner_run_tty helper creates */
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    memset(pi, 0, sizeof(*pi));
    /* CREATE_NO_WINDOW: no console must flash from the GUI */
    ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, pi);
    free(wcmd);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) { CloseHandle(*rd); return 0; }
    return 1;
}

/* oem: the output is in the OEM code page (console programs) and must be
 * converted to UTF-8; 0 if it is already UTF-8 (the runner_run_tty helper) */
static int run_pipe(const char *const *argv, runner_line_fn on_line, void *ctx, int *exit_code, int oem)
{
    splitter sp; HANDLE rd = NULL;
    PROCESS_INFORMATION pi;
    DWORD got, code = 255;
    char buf[4096]; oem_fwd fw; HANDLE dup;

    if (canceled) return RUNNER_CANCELED;
    if (!spawn(argv, &pi, &rd)) return RUNNER_NOSTART;
    if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup,
                        PROCESS_TERMINATE, FALSE, 0))
        InterlockedExchangePointer(&running, dup);
    /* canceled while starting: runner_cancel may not have seen the handle */
    if (InterlockedCompareExchange(&canceled, 0, 0)) TerminateProcess(pi.hProcess, 1);

    fw.fn = on_line; fw.ctx = ctx;
    sb_init(&sp.line);
    if (oem) { sp.fn = on_line ? oem_to_utf8 : NULL; sp.ctx = &fw; }
    else { sp.fn = on_line; sp.ctx = ctx; }
    /* exits only when ReadFile fails (pipe closed): a 0-byte read is not the end */
    while (ReadFile(rd, buf, sizeof(buf), &got, NULL)) {
        if (got > 0) feed(&sp, buf, got);
    }
    flush(&sp);
    sb_free(&sp.line);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    dup = InterlockedExchangePointer(&running, NULL);
    if (dup) CloseHandle(dup);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (exit_code) *exit_code = (int)code;
    return canceled ? RUNNER_CANCELED : RUNNER_DONE;
}

int runner_run(const char *const *argv, runner_line_fn on_line, void *ctx, int *exit_code)
{
    return run_pipe(argv, on_line, ctx, exit_code, 1);
}

/* ---- runner_run_tty: the hidden console ----
 * SAMdisk 3.8.12 writes its progress ("Reading Cyl 3  Head 0") only to a
 * real console. The window has no console, and attaching to another
 * process's console with AttachConsole is fragile on XP: one failed attach
 * (the console is not ready yet) makes all the following ones fail,
 * forever. So there is a helper, a new process for each launch (the
 * window itself with --tty): it creates its own hidden console, runs the
 * program in it, reads its screen and passes the lines on the pipe,
 * already in UTF-8. */

#define TTY_ROWS 3000           /* buffer rows: SAMdisk writes few of them */
#define TTY_COLS 160
#define TTY_NOSTART 0x4E53      /* helper exit code: the program does not start */

static char tty_helper[1024];   /* the helper: the window sets itself */

void runner_set_tty_helper(const char *exe)
{
    snprintf(tty_helper, sizeof(tty_helper), "%s", exe ? exe : "");
}

/* screen row y, without trailing spaces, in UTF-8 */
static void tty_row(HANDLE out, SHORT y, SHORT w, char *u8, size_t n)
{
    wchar_t buf[TTY_COLS + 1]; DWORD got = 0; COORD c; int k;
    c.X = 0; c.Y = y;
    if (w > TTY_COLS) w = TTY_COLS;
    if (!ReadConsoleOutputCharacterW(out, buf, (DWORD)w, c, &got)) got = 0;
    for (k = (int)got; k > 0 && (buf[k - 1] == L' ' || buf[k - 1] == 0); k--) ;
    buf[k] = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, (int)n, NULL, NULL)) u8[0] = 0;
}

/* a progress line: "Reading Cyl 3  Head 0", "Cyl 40 Head 0" */
static int is_progress(const char *s)
{
    const char *p = strstr(s, "Cyl "), *e;
    if (p && (p == s || p[-1] == ' ') && p[4] >= '0' && p[4] <= '9') return 1;
    /* DiscImageCreator: the line ends with a count, "... (LBA) 1234/300000"
     * (spaces allowed around the slash, as optical_progress accepts) */
    e = s + strlen(s);
    while (e > s && e[-1] == ' ') e--;
    if (e == s || e[-1] < '0' || e[-1] > '9') return 0;
    while (e > s && e[-1] >= '0' && e[-1] <= '9') e--;
    while (e > s && e[-1] == ' ') e--;
    if (e == s || e[-1] != '/') return 0;
    e--;
    while (e > s && e[-1] == ' ') e--;
    return e > s && e[-1] >= '0' && e[-1] <= '9';
}

typedef struct { SHORT done; char status[TTY_COLS * 4]; } tty_state;

/* the rows above the cursor are finished: they are passed on only once.
 * The cursor row is the one SAMdisk rewrites: it is passed on only if it
 * is a progress line, otherwise a half-written line would end up twice in
 * the log. final: the program has exited, the cursor row counts too */
static void tty_poll(HANDLE out, tty_state *t, runner_line_fn fn, void *ctx, int final)
{
    CONSOLE_SCREEN_BUFFER_INFO bi; char line[TTY_COLS * 4]; SHORT y, end;
    if (!GetConsoleScreenBufferInfo(out, &bi)) return;
    end = bi.dwCursorPosition.Y + (final ? 1 : 0);
    if (t->done > bi.dwCursorPosition.Y) t->done = bi.dwCursorPosition.Y;  /* the buffer scrolled */
    for (y = t->done; y < end; y++) {
        tty_row(out, y, bi.dwSize.X, line, sizeof(line));
        if (line[0]) fn(ctx, line);
    }
    if (end > t->done) t->done = end;
    if (final) return;
    tty_row(out, bi.dwCursorPosition.Y, bi.dwSize.X, line, sizeof(line));
    if (is_progress(line) && strcmp(line, t->status)) {
        strcpy(t->status, line);
        fn(ctx, line);
    }
}

/* in the helper: one line towards the window */
static void to_pipe(void *c, const char *line)
{
    DWORD put;
    WriteFile((HANDLE)c, line, (DWORD)strlen(line), &put, NULL);
    WriteFile((HANDLE)c, "\n", 1, &put, NULL);
}

/* the program in the helper's console, inside a job: if the helper is
 * stopped ("Interrompi"), the job closes and stops it */
static int tty_child(const char *const *argv, HANDLE pipe, PROCESS_INFORMATION *pi, HANDLE *job)
{
    strbuf cmd; STARTUPINFOW si; wchar_t *wcmd; int i, n, ok;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; SECURITY_ATTRIBUTES sa; HANDLE in, out;
    sb_init(&cmd);
    for (i = 0; argv[i]; i++) { if (i) sb_puts(&cmd, " "); quote(&cmd, argv[i]); }
    n = MultiByteToWideChar(CP_UTF8, 0, cmd.s, -1, NULL, 0);
    wcmd = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!wcmd) { sb_free(&cmd); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, cmd.s, -1, wcmd, n);
    sb_free(&cmd);
    /* the program's input and output are the console, not the pipe the
     * helper received from the window: that one is not inherited */
    SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, 0);
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    /* input from NUL, as with runner_run: after an error DiscImageCreator
     * waits for "Press any key to continue", and with NUL it goes on */
    in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     &sa, OPEN_EXISTING, 0, NULL);
    out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in; si.hStdOutput = out; si.hStdError = out;
    memset(pi, 0, sizeof(*pi));
    ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, pi);
    free(wcmd);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return 0;
    *job = CreateJobObjectW(NULL, NULL);
    if (*job) {
        memset(&li, 0, sizeof(li));
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(*job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        AssignProcessToJobObject(*job, pi->hProcess);
    }
    ResumeThread(pi->hThread);
    return 1;
}

int runner_tty_host(const char *const *argv)
{
    HANDLE pipe = GetStdHandle(STD_OUTPUT_HANDLE);  /* before the console: the pipe */
    HANDLE out, job = NULL; PROCESS_INFORMATION pi; DWORD code = 255;
    tty_state t; COORD size; HWND w; int rc;
    /* a new console of our own: if we had one (launched from a console) we leave it */
    FreeConsole();
    if (!AllocConsole()) {
        /* no console: at least the normal output, without progress */
        rc = runner_run(argv, to_pipe, pipe, (int *)&code);
        return rc == RUNNER_NOSTART ? TTY_NOSTART : (int)code;
    }
    /* the helper's startup info already asks for SW_HIDE; just to be safe */
    if ((w = GetConsoleWindow()) != NULL) ShowWindow(w, SW_HIDE);
    out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
    /* long rows without wrapping and enough rows not to scroll */
    size.X = TTY_COLS; size.Y = TTY_ROWS;
    if (out != INVALID_HANDLE_VALUE) SetConsoleScreenBufferSize(out, size);
    if (!tty_child(argv, pipe, &pi, &job)) return TTY_NOSTART;
    memset(&t, 0, sizeof(t));
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT)
        if (out != INVALID_HANDLE_VALUE) tty_poll(out, &t, to_pipe, pipe, 0);
    if (out != INVALID_HANDLE_VALUE) tty_poll(out, &t, to_pipe, pipe, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (job) CloseHandle(job);
    return (int)code;
}

int runner_run_tty(const char *const *argv, runner_line_fn on_line, void *ctx, int *exit_code)
{
    const char **hv; int i, n, rc, code = 0;
    /* whoever already has a console (dskrun) or has no helper uses the pipe */
    if (!tty_helper[0] || GetConsoleWindow()) return runner_run(argv, on_line, ctx, exit_code);
    for (n = 0; argv[n]; n++) ;
    hv = (const char **)malloc((size_t)(n + 3) * sizeof(*hv));
    if (!hv) return runner_run(argv, on_line, ctx, exit_code);
    hv[0] = tty_helper; hv[1] = "--tty";
    for (i = 0; i <= n; i++) hv[i + 2] = argv[i];
    rc = run_pipe(hv, on_line, ctx, &code, 0);
    free(hv);
    if (rc == RUNNER_NOSTART) return runner_run(argv, on_line, ctx, exit_code);
    if (rc == RUNNER_DONE && code == TTY_NOSTART) return RUNNER_NOSTART;
    if (exit_code) *exit_code = code;
    return rc;
}

void runner_cancel(void)
{
    HANDLE h;
    InterlockedExchange(&canceled, 1);      /* first the flag, then the handle */
    h = InterlockedExchangePointer(&running, NULL);
    if (h) { TerminateProcess(h, 1); CloseHandle(h); }
}

int runner_stopped(void) { return InterlockedCompareExchange(&canceled, 0, 0) != 0; }
void runner_clear(void) { InterlockedExchange(&canceled, 0); }

/* what is in the pipe right now, without waiting for more: a child of the
 * program may keep it open even after the program has been stopped */
static void drain(HANDLE rd, char *out, size_t n, size_t *len)
{
    DWORD avail, got, room; char sink[512], *dst;
    while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) {
        if (*len + 1 < n) { dst = out + *len; room = (DWORD)(n - 1 - *len); }
        else { dst = sink; room = sizeof(sink); }
        if (room > avail) room = avail;
        if (!ReadFile(rd, dst, room, &got, NULL) || !got) break;
        if (dst != sink) { *len += got; out[*len] = 0; }
    }
}

int runner_capture(const char *const *argv, char *out, size_t n, int timeout_ms, int *exit_code)
{
    PROCESS_INFORMATION pi; HANDLE rd; size_t len = 0; DWORD t0, code = 255; int rc = RUNNER_DONE;
    out[0] = 0;
    if (!spawn(argv, &pi, &rd)) return RUNNER_NOSTART;
    t0 = GetTickCount();
    for (;;) {
        drain(rd, out, n, &len);
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        if (GetTickCount() - t0 >= (DWORD)timeout_ms) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            rc = RUNNER_TIMEOUT;
            break;
        }
    }
    drain(rd, out, n, &len);
    if (rc == RUNNER_DONE && GetExitCodeProcess(pi.hProcess, &code) && exit_code) *exit_code = (int)code;
    CloseHandle(rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return rc;
}

#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile pid_t running = 0;
static volatile int canceled = 0;

int runner_run(const char *const *argv, runner_line_fn on_line, void *ctx, int *exit_code)
{
    int fd[2], ok[2], st = 0; pid_t pid; splitter sp; char buf[4096], c;
    ssize_t got, r;

    if (canceled) return RUNNER_CANCELED;
    if (pipe(fd)) return RUNNER_NOSTART;
    if (pipe(ok)) { close(fd[0]); close(fd[1]); return RUNNER_NOSTART; }
    /* ok[] stays open only if exec fails: that tells whether the program exists */
    fcntl(ok[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        close(fd[0]); close(fd[1]); close(ok[0]); close(ok[1]);
        return RUNNER_NOSTART;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) {
            dup2(nul, 0);
            if (nul > 0) close(nul);
        }
        dup2(fd[1], 1); dup2(fd[1], 2);
        close(fd[0]); close(fd[1]); close(ok[0]);
        execvp(argv[0], (char *const *)argv);
        c = 1;
        if (write(ok[1], &c, 1) < 0) { /* nothing to do */ }
        _exit(127);
    }
    close(fd[1]); close(ok[1]);
    /* only 1 byte read means exec failed; EINTR means nothing */
    do r = read(ok[0], &c, 1); while (r < 0 && errno == EINTR);
    if (r == 1) {
        close(ok[0]); close(fd[0]);
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
        return RUNNER_NOSTART;
    }
    close(ok[0]);
    running = pid;
    if (canceled) kill(pid, SIGTERM);   /* canceled while starting */

    sb_init(&sp.line); sp.fn = on_line; sp.ctx = ctx;
    for (;;) {
        got = read(fd[0], buf, sizeof(buf));
        if (got > 0) { feed(&sp, buf, (size_t)got); continue; }
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    flush(&sp);
    sb_free(&sp.line);
    close(fd[0]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    running = 0;
    if (exit_code) *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    return canceled ? RUNNER_CANCELED : RUNNER_DONE;
}

void runner_set_tty_helper(const char *exe) { (void)exe; }

int runner_run_tty(const char *const *argv, runner_line_fn on_line, void *ctx, int *exit_code)
{
    return runner_run(argv, on_line, ctx, exit_code);
}

void runner_cancel(void)
{
    pid_t p = running;
    canceled = 1;
    if (p > 0) kill(p, SIGTERM);
}

int runner_stopped(void) { return canceled != 0; }
void runner_clear(void) { canceled = 0; }

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* waits at most wait ms for something to arrive and takes it; 0 at end of pipe */
static int take(int fd, int wait, char *out, size_t n, size_t *len)
{
    struct pollfd p; char sink[512]; ssize_t got;
    p.fd = fd; p.events = POLLIN; p.revents = 0;
    if (poll(&p, 1, wait) <= 0) return 1;
    if (*len + 1 < n) got = read(fd, out + *len, n - 1 - *len);
    else got = read(fd, sink, sizeof(sink));
    if (got == 0) return 0;
    if (got > 0 && *len + 1 < n) { *len += (size_t)got; out[*len] = 0; }
    return 1;
}

int runner_capture(const char *const *argv, char *out, size_t n, int timeout_ms, int *exit_code)
{
    int fd[2], ok[2], st = 0, open_pipe = 1, rc = RUNNER_DONE; pid_t pid; ssize_t r; char c;
    size_t len = 0; long t0;

    out[0] = 0;
    if (pipe(fd)) return RUNNER_NOSTART;
    if (pipe(ok)) { close(fd[0]); close(fd[1]); return RUNNER_NOSTART; }
    fcntl(ok[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        close(fd[0]); close(fd[1]); close(ok[0]); close(ok[1]);
        return RUNNER_NOSTART;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);
        setpgid(0, 0);                  /* stopped together with its children */
        if (nul >= 0) { dup2(nul, 0); if (nul > 0) close(nul); }
        dup2(fd[1], 1); dup2(fd[1], 2);
        close(fd[0]); close(fd[1]); close(ok[0]);
        execvp(argv[0], (char *const *)argv);
        c = 1;
        if (write(ok[1], &c, 1) < 0) { /* nothing to do */ }
        _exit(127);
    }
    close(fd[1]); close(ok[1]);
    do r = read(ok[0], &c, 1); while (r < 0 && errno == EINTR);
    close(ok[0]);
    if (r == 1) {
        close(fd[0]);
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
        return RUNNER_NOSTART;
    }
    t0 = now_ms();
    for (;;) {
        if (open_pipe) open_pipe = take(fd[0], 50, out, n, &len);
        else poll(NULL, 0, 50);
        if (waitpid(pid, &st, WNOHANG) == pid) break;
        if (now_ms() - t0 >= timeout_ms) {
            kill(-pid, SIGKILL); kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
            rc = RUNNER_TIMEOUT;
            break;
        }
    }
    while (open_pipe && take(fd[0], 0, out, n, &len)) {
        struct pollfd p; p.fd = fd[0]; p.events = POLLIN; p.revents = 0;
        if (poll(&p, 1, 0) <= 0) break;     /* nothing else right now */
    }
    close(fd[0]);
    if (rc == RUNNER_DONE && exit_code) *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    return rc;
}
#endif
