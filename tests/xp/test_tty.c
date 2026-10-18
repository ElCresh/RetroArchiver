/* test_tty - runner_run_tty on Windows, from a process without a console
 * like the window: the fake SAMdisk writes progress ("Cyl 20 Head 0")
 * only on a console, like the real SAMdisk 3.8.12, and here it must arrive.
 * With runner_run (the pipe) it must not.
 *
 *   tty_test.exe FAKE_SAMDISK FIXTURES [SAMDISK IMAGE.dsk]
 *   (make test runs it from build/; on the VM also with the real SAMdisk) */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "core/runner.h"

static int fails;

static void ok(const char *what, int cond)
{
    printf("  %s   %s\n", cond ? "ok" : "NO", what);
    if (!cond) fails++;
}

typedef struct { int cyl, done, lines; char last_cyl[64]; char first[4][160]; } seen;

/* the first lines received, to show when a check fails */
static void show(const seen *s, const char *what, int rc, int code)
{
    int i;
    printf("       %s: rc %d, code %d, %d lines\n", what, rc, code, s->lines);
    for (i = 0; i < 4 && i < s->lines; i++) printf("       [%s]\n", s->first[i]);
}

static void on_line(void *c, const char *line)
{
    seen *s = (seen *)c;
    if (s->lines < 4) snprintf(s->first[s->lines], sizeof(s->first[0]), "%s", line);
    s->lines++;
    /* "Cyl 20 Head 0" from the fake, "Reading Cyl 3  Head 0" from the real SAMdisk */
    if (!strncmp(line, "Cyl ", 4) || !strncmp(line, "Reading Cyl ", 12)) {
        /* the same progress line must not arrive twice in a row */
        if (strcmp(line, s->last_cyl)) s->cyl++;
        snprintf(s->last_cyl, sizeof(s->last_cyl), "%s", line);
    }
    if (strstr(line, "fake_samdisk: copied")) s->done++;
}

int main(int argc, char **argv)
{
    const char *cmd[5]; seen s; int code = -1, rc; FILE *f;
    char out[MAX_PATH], script[MAX_PATH + 16], pos[MAX_PATH + 32], self[MAX_PATH];
    /* like the window: launched with --tty it acts as an intermediary */
    if (argc >= 3 && !strcmp(argv[1], "--tty")) return runner_tty_host((const char *const *)argv + 2);
    if (argc < 3) { printf("usage: tty_test.exe FAKE_SAMDISK FIXTURES\n"); return 2; }
    GetModuleFileNameA(NULL, self, sizeof(self));
    runner_set_tty_helper(self);
    /* like the window: no console. Launched from make, the output stays
     * on the pipe and the exit code arrives */
    FreeConsole();
    SetEnvironmentVariableA("FAKE_SAMDISK_DIR", argv[2]);
    GetTempPathA(sizeof(out), out);
    snprintf(script, sizeof(script), "%stty_test.txt", out);
    snprintf(pos, sizeof(pos), "%s.pos", script);
    if (!(f = fopen(script, "w"))) { printf("cannot write %s\n", script); return 2; }
    fputs("ident_a\nident_a\n", f);         /* one copy with the console, one with the pipe */
    fclose(f);
    DeleteFileA(pos);
    SetEnvironmentVariableA("FAKE_SAMDISK_SCRIPT", script);
    strncat(out, "tty_test.dsk", sizeof(out) - strlen(out) - 1);
    cmd[0] = argv[1]; cmd[1] = "copy"; cmd[2] = "a:"; cmd[3] = out; cmd[4] = NULL;

    printf("\nrunner_run_tty\n");
    ok("the process has no console (like the window)", GetConsoleWindow() == NULL);
    memset(&s, 0, sizeof(s));
    rc = runner_run_tty(cmd, on_line, &s, &code);
    if (rc != RUNNER_DONE || code || s.cyl < 2) show(&s, "console", rc, code);
    ok("console: the program exits with 0", rc == RUNNER_DONE && code == 0);
    ok("console: progress lines arrive", s.cyl >= 2);
    ok("console: the final line arrives only once", s.done == 1);
    ok("console: no console left attached", GetConsoleWindow() == NULL);

    memset(&s, 0, sizeof(s));
    rc = runner_run(cmd, on_line, &s, &code);
    if (rc != RUNNER_DONE || s.done != 1) show(&s, "pipe", rc, code);
    ok("pipe: no progress, like the real SAMdisk", rc == RUNNER_DONE && s.cyl == 0 && s.done == 1);

    cmd[0] = "non_esiste_davvero.exe";
    rc = runner_run_tty(cmd, on_line, &s, &code);
    ok("console: missing program = RUNNER_NOSTART", rc == RUNNER_NOSTART);

    /* the answer to a question (DiscImageCreator's drive offset) from a
     * file, through the hidden console */
    {
        const char *ask[] = { "cmd.exe", "/c", "set /p X=& call echo got [%X%]", NULL };
        char in[MAX_PATH + 16];
        GetTempPathA(MAX_PATH, in);
        strncat(in, "tty_input.txt", sizeof(in) - strlen(in) - 1);
        if ((f = fopen(in, "wb")) != NULL) { fputs("+6\r\n", f); fclose(f); }
        memset(&s, 0, sizeof(s));
        rc = runner_run_tty_input(ask, in, on_line, &s, &code);
        if (rc != RUNNER_DONE || s.lines < 1 || strcmp(s.first[0], "got [+6]")) show(&s, "input", rc, code);
        ok("console: the program reads its answer from the file", rc == RUNNER_DONE && s.lines >= 1 &&
           !strcmp(s.first[0], "got [+6]"));
        memset(&s, 0, sizeof(s));
        rc = runner_run_tty(ask, on_line, &s, &code);
        ok("console: without a file the input is NUL", rc == RUNNER_DONE && s.lines >= 1 &&
           !strncmp(s.first[0], "got [", 5) && !strstr(s.first[0], "+6"));
        DeleteFileA(in);
    }

    /* on the VM: the real SAMdisk (argument 3) with "scan SOURCE" (argument 4),
     * which surely writes, and with "copy SOURCE" (on a: it writes "Cyl") */
    if (argc >= 5) {
        const char *sc[4];
        sc[0] = argv[3]; sc[1] = "scan"; sc[2] = argv[4]; sc[3] = NULL;
        memset(&s, 0, sizeof(s));
        rc = runner_run_tty(sc, on_line, &s, &code);
        show(&s, "real SAMdisk, scan", rc, code);
        ok("real SAMdisk: with the console the scan output arrives", rc == RUNNER_DONE && s.lines > 0);
        cmd[0] = argv[3]; cmd[2] = argv[4];
        memset(&s, 0, sizeof(s));
        rc = runner_run_tty(cmd, on_line, &s, &code);
        show(&s, "real SAMdisk", rc, code);
        printf("       real SAMdisk: code %d, %d lines, %d progress (last: %s)\n",
               code, s.lines, s.cyl, s.last_cyl);
        ok("real SAMdisk: with the console progress arrives", rc == RUNNER_DONE && code == 0 && s.cyl >= 2);
    }

    DeleteFileA(out); DeleteFileA(script); DeleteFileA(pos);
    printf("\ntty: %d failed\n", fails);
    return fails ? 1 : 0;
}
