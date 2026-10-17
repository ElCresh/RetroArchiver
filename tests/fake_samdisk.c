/* fake_samdisk - fake SAMdisk to test dskrun and the GUI without a
 * floppy drive. Understands "rpm a:", "scan a:" and "copy a: FILE".
 *
 *   FAKE_SAMDISK_DIR      folder of the images (tests/fixtures)
 *   FAKE_SAMDISK_SCRIPT   file with one line per "copy": the name of an
 *                         image without .dsk, or FAIL (error) or PARTIAL
 *                         (partial file + error) or SLOW [image]: waits 5
 *                         seconds, then copies (default ident_a)
 *   FAKE_SAMDISK_NODRIVE  if defined, "rpm" fails
 *   FAKE_SAMDISK_VERSION  the version it prints with no arguments, for
 *                         example "4.0 alpha" (default "3.8.12 (fake for tests)")
 *   FAKE_SAMDISK_RPM_SLOW seconds "rpm" waits in silence before
 *                         answering: like a drive without a disk
 *
 * Like the real SAMdisk 3.8.12, "copy" writes progress ("Reading Cyl
 * 20  Head 0") only if the output is a console, never on a pipe.
 *
 * The position in the script is kept in SCRIPT.pos: each launch takes the
 * next line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define sleep_s(n) Sleep((n) * 1000)
#define sleep_ms(n) Sleep(n)
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#define sleep_s(n) sleep(n)
#define sleep_ms(n) usleep((n) * 1000)
#endif
#define sleep5() sleep_s(5)

static int read_line(const char *script, char *out, size_t n, int advance);

static int next_line(const char *script, char *out, size_t n)
{
    return read_line(script, out, n, 1);
}

/* the scan peeks at the next line without consuming it */
static int peek_line(const char *script, char *out, size_t n)
{
    return read_line(script, out, n, 0);
}

static int read_line(const char *script, char *out, size_t n, int advance)
{
    char pos[1024], line[256]; FILE *f; int k = 0, i;
    snprintf(pos, sizeof(pos), "%s.pos", script);
    f = fopen(pos, "r");
    if (f) { if (fscanf(f, "%d", &k) != 1) k = 0; fclose(f); }
    f = fopen(script, "r");
    if (!f) return -1;
    for (i = 0; i <= k; i++)
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    snprintf(out, n, "%s", line);
    if (!advance) return 0;
    f = fopen(pos, "w");
    if (f) { fprintf(f, "%d\n", k + 1); fclose(f); }
    return 0;
}

static int copy(const char *name, const char *dst)
{
    static char buf[65536];
    char src[1024]; const char *dir = getenv("FAKE_SAMDISK_DIR");
    FILE *in, *out; size_t n; int c;

    snprintf(src, sizeof(src), "%s/%s.dsk", dir ? dir : "fixtures", name);
    in = fopen(src, "rb");
    if (!in) { printf("fake_samdisk: missing %s\n", src); return 1; }
    out = fopen(dst, "wb");
    if (!out) { fclose(in); printf("fake_samdisk: cannot write %s\n", dst); return 1; }
    /* like SAMdisk 3.8.12: progress rewrites the same line with \r, and
     * only on a console; on a pipe there is none. A pause between one line
     * and the next, so whoever reads the screen can see them */
    if (isatty(fileno(stdout))) {
        for (c = 0; c < 80; c += 20) { printf("Reading Cyl %d  Head 0\r", c); fflush(stdout); sleep_ms(300); }
        printf("\n");
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    printf("fake_samdisk: copied %s\n", name);
    return 0;
}

int main(int argc, char **argv)
{
    char line[256]; const char *script = getenv("FAKE_SAMDISK_SCRIPT");

    /* with no arguments, like the real one: the GUI reads the version from it */
    if (argc < 2) {
        const char *v = getenv("FAKE_SAMDISK_VERSION");
        printf("SAMdisk %s\n\n SAMDISK [copy|scan|rpm] <args>\n", v ? v : "3.8.12 (fake for tests)");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "rpm")) {
        if (getenv("FAKE_SAMDISK_NODRIVE")) { printf("Error: drive not ready\n"); return 1; }
        if (getenv("FAKE_SAMDISK_RPM_SLOW")) sleep_s(atoi(getenv("FAKE_SAMDISK_RPM_SLOW")));
        printf("300.0 rpm\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "scan")) {
        printf("fake scan of %s\n80 cyls, 2 heads, 18 sectors\n", argv[2]);
        /* "BAD name": the next disk has bad sectors, as SAMdisk writes them */
        if (script && !peek_line(script, line, sizeof(line)) && !strncmp(line, "BAD", 3))
            printf(" 10.0  1 2 3 4 5[dc] 6 7 8 9 10 11 12 13 14 15 16 17 18\n");
        else
            printf(" 10.0  1 2 3 4 5 6 7d 8 9 10 11 12 13 14 15 16 17 18\n");
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "copy")) {
        /* source other than a:: like the real SAMdisk, copies the image */
        if (strcmp(argv[2], "a:")) {
            FILE *in = fopen(argv[2], "rb"), *o;
            int c;
            if (!in) { printf("%s: No such file or directory\n", argv[2]); return 1; }
            o = fopen(argv[3], "wb");
            if (!o) { fclose(in); return 1; }
            while ((c = fgetc(in)) != EOF) fputc(c, o);
            fclose(in); fclose(o);
            return 0;
        }
        if (!script || next_line(script, line, sizeof(line))) {
            printf("fake_samdisk: script finished\n");
            return 1;
        }
        if (!strcmp(line, "PARTIAL")) {   /* writes half and fails */
            FILE *o = fopen(argv[3], "wb"); int i;
            if (o) { for (i = 0; i < 1000; i++) fputc(0xA5, o); fclose(o); }
            printf("Error: read interrupted (simulated)\n");
            return 1;
        }
        if (!strcmp(line, "FAIL")) { printf("Error: read failed (simulated)\n"); return 1; }
        if (!strncmp(line, "SLOW", 4)) {   /* to test Ctrl+C mid-read */
            printf("fake_samdisk: slow\n");
            fflush(stdout);
            sleep5();
            return copy(line[4] == ' ' ? line + 5 : "ident_a", argv[3]);
        }
        if (argc >= 6 && !strcmp(argv[4], "--retries=30") && !strcmp(argv[5], "--rescans=5"))
            printf("fake_samdisk: careful read --retries=30 --rescans=5\n");
        if (!strncmp(line, "BAD ", 4)) return copy(line + 4, argv[3]);
        return copy(line, argv[3]);
    }
    printf("fake_samdisk: unexpected command\n");
    return 2;
}
