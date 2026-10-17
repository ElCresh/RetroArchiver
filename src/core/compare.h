/* compare - comparison of 2-8 reads of the same floppy.
 * Computes the verdict and composes the text that dskdiff prints: the
 * caller decides whether to show it in a console, in a window or save it
 * to a file. */
#ifndef COMPARE_H
#define COMPARE_H
#include "strbuf.h"

#define COMPARE_MAXF 8

typedef enum {
    V_IDENTICAL,    /* everything equal */
    V_NOISE_ONLY,   /* differences only in the extra cylinders or in the header */
    V_DATA_DIFFER,  /* two reads: differences in the data area */
    V_UNSTABLE,     /* three or more reads: data area tracks that change */
    V_UNMAPPED,     /* file without header: tracks cannot be mapped */
    V_ERROR         /* unreadable file or wrong number of files */
} verdict;

typedef struct {
    verdict v;
    int exit_code;                  /* 0 stable, 1 read again, 2 error */
    int cylinders, heads;           /* of the first read */
    int nomcyl, nsec0, stdcyl;      /* detected geometry and threshold used */
    int agree;                      /* agreeing reads on the worst track */
    int unstable_tracks;            /* data area tracks that change */
    strbuf detail;                  /* the full text for the user */
    /* media that are not floppies (optical.c) state their own outcome and
     * verdict: "verificato redump" and "redump"; "" = the floppy ones */
    char outcome[24], verdict_key[16];
    /* nonzero: the read (from 1) that must become the master, for example
     * the only one redump knows */
    int master;
    /* the names the text uses for the reads, one per path: in the bag
     * "FLOPPY01.dsk", not the PC path. NULL (dskdiff): the paths as
     * given. Set by the caller, after compare_init */
    const char *const *names;
} compare_result;

void compare_init(compare_result *r);
void compare_free(compare_result *r);
/* returns r->exit_code */
int  compare_files(const char *const *paths, int n, int forced_cyl, compare_result *r);
const char *verdict_name(verdict v);

#endif
