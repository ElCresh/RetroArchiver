/* archive - the boxes already present in an archive folder.
 * Reads each bag (metadata.json, bag-info.txt, the open marker) and makes a
 * summary of it: the archive window shows it, without redoing the sums. */
#ifndef ARCHIVE_H
#define ARCHIVE_H
#include <stddef.h>
#include "meta.h"
#include "strbuf.h"

enum { ARC_OPEN, ARC_COMPLETE, ARC_INCOMPLETE, ARC_BROKEN };
/* the colour of the box: open, all good, to look at, with errors */
enum { TONE_OPEN, TONE_OK, TONE_WARN, TONE_BAD };

typedef struct {
    char dir[600];                  /* bag folder */
    char title[256], publisher[128], year[16], platform[64];
    char language[64], edition[128], notes[512];
    int  state;                     /* ARC_... */
    int  expected, archived;        /* expected media, media with an image */
    int  count[MEDIA_KINDS];        /* composition: 3 floppies, 1 CD */
    int  kind_archived[MEDIA_KINDS];
    int  clean, marginal, fragile, errors;
    int  redump;                    /* of the clean ones, how many verified with redump */
    /* one character per medium, from the first to the expected: p clean,
     * m marginal, f fragile, e error, - not yet archived or missing */
    char disks[META_MAXDISK + 1];
    /* the type of each medium: f floppy, c CD, d DVD */
    char kinds[META_MAXDISK + 1];
    int  first_todo;                /* first medium to do, 0 if none */
    long long bytes;                /* everything there is in data/ */
    char date[20];                  /* closing, or the last read if open */
    char error[300];                /* ARC_BROKEN: what is wrong */
} arc_box;

typedef struct { int n, cap; arc_box *box; } arc_list;

/* all the bags under root, most recent first; -1 if root does not open */
int  arc_scan(const char *root, arc_list *out);
void arc_free(arc_list *l);
/* a single bag; 0 if bagdir is not a bag */
int  arc_load(const char *bagdir, arc_box *b);

int  arc_tone(const arc_box *b);
/* "3 puliti, 1 marginale" */
void arc_outcome(const arc_box *b, char *out, size_t n);
/* the state and what to do: "Aperto: 5 floppy su 9 archiviati. Riprendi dal floppy 6." */
void arc_advice(const arc_box *b, char *out, size_t n);
/* "floppy 5, 7" or "floppy 5, CD 1": the media not archived */
void arc_missing(const arc_box *b, char *out, size_t n);
/* "3 / 3 floppy, 0 / 1 CD": archived out of the total, per type */
void arc_media(const arc_box *b, char *out, size_t n);
/* type and number within the type of the medium at position n, from 1 */
void arc_slot(const arc_box *b, int n, int *kind, int *num);

/* digest recheck: with the bag closed those of the manifests, with the
 * bag open those taken after each read (metadata.json) */
typedef struct {
    int files, changed, lost, extra;
    char first[160];                /* the first file that does not match, "" if all ok */
} arc_check;

int  arc_verify(const char *bagdir, arc_check *c, strbuf *err);   /* -1 if it cannot */

#endif
