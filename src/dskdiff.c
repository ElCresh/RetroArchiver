/* dskdiff - compares two or more EDSK/DSK reads of the same floppy and
 * says whether the differences are in the real data or only in the extra
 * cylinders. The comparison lives in core/compare.c, shared with dskrun
 * and the GUI: only arguments and printing remain here.
 * On-screen text has no accents: the Windows XP console does not render
 * them well.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/compare.h"
#ifdef _WIN32
#include <windows.h>
#endif

#define PATHBUF 1024

/* On Windows argv arrives in the ANSI code page, while the core opens files
 * as UTF-8: convert here. If the conversion fails, use the argument as is. */
static const char *utf8_arg(const char *arg, char *buf, size_t n)
{
#ifdef _WIN32
    wchar_t w[PATHBUF];
    if (MultiByteToWideChar(CP_ACP, 0, arg, -1, w, PATHBUF) &&
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, NULL, NULL))
        return buf;
#else
    (void)buf; (void)n;
#endif
    return arg;
}

int main(int argc, char **argv)
{
    const char *fl[COMPARE_MAXF];
    char conv[COMPARE_MAXF][PATHBUF];
    int i, n = 0, forced = 0, rc;
    compare_result r;

    printf("\ndskdiff 1.2 - compares several reads of the same floppy\n\n");

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "/c")) {
            if (i + 1 < argc) { forced = atoi(argv[i + 1]); i++; }
            if (forced < 1 || forced > 255) forced = 0;
            continue;
        }
        if (n < COMPARE_MAXF) {
            fl[n] = utf8_arg(argv[i], conv[n], sizeof(conv[n]));
            n++;
        }
    }
    if (n < 2) {
        printf("  usage: dskdiff FIRST.DSK SECOND.DSK [THIRD.DSK ...] [-c CYLINDERS]\n\n");
        printf("  With two reads it shows the differences byte by byte.\n");
        printf("  With three or more it tells how many variants each track has\n");
        printf("  and whether the reads have a majority.\n\n");
        printf("  The disk format is detected automatically (40, 80, other).\n");
        printf("  Use -c to force it if the detection is wrong.\n\n");
        return 2;
    }
    compare_init(&r);
    rc = compare_files(fl, n, forced, &r);
    fputs(r.detail.s, stdout);
    compare_free(&r);
    return rc;
}
