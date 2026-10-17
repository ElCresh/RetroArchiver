#include "unit.h"
#include "core/runner.h"
#include "core/strbuf.h"
#include <time.h>

typedef struct { char lines[8][64]; int n; } got_lines;

static void grab(void *ctx, const char *line)
{
    got_lines *g = (got_lines *)ctx;
    if (g->n < 8) str_copy(g->lines[g->n++], 64, line);
}

/* runner_capture: whole output, timeout, and no tie to the cancellation
 * of runner_run (the session can start while it runs) */
static void test_capture(void)
{
    char out[256]; int code = -1; time_t t0;
#ifdef _WIN32
    const char *quick[] = { "cmd.exe", "/c", "echo 300.0 rpm& exit 2", NULL };
    /* ping outlives cmd and holds the pipe: do not wait for it */
    const char *slow[] = { "cmd.exe", "/c", "ping -n 6 127.0.0.1 >nul & echo tardi", NULL };
#else
    const char *quick[] = { "sh", "-c", "echo 300.0 rpm; exit 2", NULL };
    const char *slow[] = { "sh", "-c", "sleep 6; echo tardi", NULL };
#endif
    const char *nope[] = { "programma-che-non-esiste-xyz", NULL };

    runner_cancel();
    CHECK(runner_capture(quick, out, sizeof(out), 5000, &code) == RUNNER_DONE);
    CHECK(code == 2 && strstr(out, "300.0 rpm") != NULL);
    CHECK(runner_stopped());            /* did not touch it */
    runner_clear();

    CHECK(runner_capture(quick, out, 6, 5000, &code) == RUNNER_DONE);
    CHECK(strlen(out) == 5);            /* cut to n-1 bytes */

    t0 = time(NULL);
    code = -1;
    CHECK(runner_capture(slow, out, sizeof(out), 700, &code) == RUNNER_TIMEOUT);
    CHECK(time(NULL) - t0 <= 3);
    CHECK(code == -1 && !strstr(out, "tardi"));
    CHECK(!runner_stopped());

    CHECK(runner_capture(nope, out, sizeof(out), 1000, &code) == RUNNER_NOSTART);
}

void test_runner(void)
{
    got_lines g; int code = -1;
#ifdef _WIN32
    const char *argv[] = { "cmd.exe", "/c", "echo uno& echo due& exit 3", NULL };
#else
    const char *argv[] = { "sh", "-c",
        "printf 'uno\\ndue\\rtre\\n'; printf 'err\\n' >&2; exit 3", NULL };
    const char *spaced[] = { "sh", "-c", "printf '%s' \"$1\"", "sh", "a b \"c\"", NULL };
    const char *crlf[] = { "sh", "-c", "printf 'a\\r\\n\\r\\nb'", NULL };
#endif
    const char *nope[] = { "programma-che-non-esiste-xyz", NULL };

    memset(&g, 0, sizeof(g));
    CHECK(runner_run(argv, grab, &g, &code) == RUNNER_DONE);
    CHECK(code == 3);
    CHECK_STR(g.lines[0], "uno");
    CHECK_STR(g.lines[1], "due");
#ifndef _WIN32
    CHECK_STR(g.lines[2], "tre");
    CHECK_STR(g.lines[3], "err");
    CHECK(g.n == 4);
    memset(&g, 0, sizeof(g));
    CHECK(runner_run(spaced, grab, &g, &code) == RUNNER_DONE);
    CHECK_STR(g.lines[0], "a b \"c\"");
    memset(&g, 0, sizeof(g));
    CHECK(runner_run(crlf, grab, &g, &code) == RUNNER_DONE);
    CHECK(g.n == 2);
    CHECK_STR(g.lines[0], "a");
    CHECK_STR(g.lines[1], "b");
#endif
    CHECK(runner_run(nope, grab, &g, &code) == RUNNER_NOSTART);
    CHECK(runner_run(argv, NULL, NULL, &code) == RUNNER_DONE);

    /* cancelled between two programs: the next one does not start until reset */
    CHECK(!runner_stopped());
    runner_cancel();
    CHECK(runner_stopped());
    memset(&g, 0, sizeof(g)); code = -1;
    CHECK(runner_run(argv, grab, &g, &code) == RUNNER_CANCELED);
    CHECK(g.n == 0 && code == -1);
    CHECK(runner_stopped());
    runner_clear();
    CHECK(!runner_stopped());
    CHECK(runner_run(argv, grab, &g, &code) == RUNNER_DONE);
    CHECK(code == 3);
    CHECK_STR(g.lines[0], "uno");
    test_capture();
}
