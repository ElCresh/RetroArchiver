/* runner - launches an external program and passes on its output (stdout
 * and stderr together) one line at a time, while the program works.
 * SAMdisk updates the same line with '\r': that also ends a line. */
#ifndef RUNNER_H
#define RUNNER_H
#include <stddef.h>

typedef void (*runner_line_fn)(void *ctx, const char *line);

#define RUNNER_DONE      0   /* the program has finished: exit_code is valid */
#define RUNNER_NOSTART  -1   /* not found or not startable */
#define RUNNER_CANCELED  1   /* stopped by runner_cancel */

/* argv ends with NULL; argv[0] is the program, searched in the PATH */
int  runner_run(const char *const *argv, runner_line_fn on_line, void *ctx, int *exit_code);
/* like runner_run, for programs that write their progress only to a real
 * console (SAMdisk 3.8.12: on a pipe "copy" writes nothing).
 * On Windows, from a process without a console (the window) and with the
 * helper set, the program runs in a hidden console whose screen is read
 * back: finished lines arrive and, when it changes, the progress line
 * under the cursor ("Reading Cyl 3  Head 0", or a DiscImageCreator line
 * ending with a count, "(LBA) 1234/300000"). Input comes from NUL.
 * Otherwise (dskrun already has its console, Linux) it is runner_run */
int  runner_run_tty(const char *const *argv, runner_line_fn on_line, void *ctx, int *exit_code);
/* runner_run_tty with the program's input read from the file input instead
 * of NUL: the answers to the questions it asks (DiscImageCreator asks for
 * the drive's read offset). After the end of the file it behaves as with
 * NUL. input NULL or "" is NUL, as in runner_run_tty */
int  runner_run_tty_input(const char *const *argv, const char *input,
                          runner_line_fn on_line, void *ctx, int *exit_code);
/* the runner_run_tty helper: a program that, launched with
 * "--tty PROGRAM ARG...", calls runner_tty_host (the window itself) */
void runner_set_tty_helper(const char *exe);
/* Windows only: the helper's job. argv is the program to launch with its
 * arguments; returns its exit code. The program reads the helper's own
 * input: NUL, or runner_run_tty_input's file */
int  runner_tty_host(const char *const *argv);
/* from another thread: stops the running program */
void runner_cancel(void);
/* the cancellation stays in effect until cleared: pressed between two
 * programs, it stops the next one before it starts */
int  runner_stopped(void);
void runner_clear(void);

#define RUNNER_TIMEOUT   2   /* runner_capture: stopped after the maximum time */

/* launches argv and collects its output (stdout and stderr together, as
 * it arrives) in out, at most n-1 bytes; after timeout_ms it stops it.
 * Does not touch runner_run's state, cancellation included: it is for
 * short checks (the floppy drive's spin) that run in another thread
 * while a session may start. RUNNER_DONE (exit_code valid),
 * RUNNER_TIMEOUT (out holds what arrived before) or RUNNER_NOSTART */
int  runner_capture(const char *const *argv, char *out, size_t n, int timeout_ms, int *exit_code);

#endif
