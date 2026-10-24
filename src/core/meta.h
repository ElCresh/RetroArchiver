/* meta - the box model and its metadata.json. */
#ifndef META_H
#define META_H
#include <stddef.h>
#include "json.h"
#include "strbuf.h"

#define META_MAXDISK 64
#define META_MAXREAD 6
#include "version.h"
#define META_SCHEMA  "retroarchiver-3"
/* 261017 wrote retroarchiver-2 (and earlier retroarchiver-floppy-1), with
 * the outcomes in Italian: they are still read, as the English ones */
#define META_SCHEMA_2 "retroarchiver-2"

/* the outcome of a medium, "status" in metadata.json */
#define ST_CLEAN       "clean"
#define ST_MARGINAL    "marginal"
#define ST_ERROR       "error"
#define ST_ERRORS      "errors"           /* a CD or DVD with errors redump does not explain */
#define ST_FRAGILE     "fragile"
#define ST_UNSTABLE    "unstable"
#define ST_READ_ONCE   "read-once"
#define ST_REDUMP      "redump-verified"
#define ST_FAILED      "read-failed"
#define ST_INTERRUPTED "interrupted"
#define ST_MISSING     "missing"
#define ST_DUPLICATE   "duplicate"
#define ST_TO_READ     "to-read"          /* only in the file: in memory "" */

/* an outcome as 261017 wrote it ("pulito"...) in English; others unchanged */
const char *meta_status_english(const char *s);

/* the copy protection scan of the CDs (DiscImageCreator /sf), a choice of
 * the box: the files on the disc decide (auto), always, never */
enum { META_PROT_AUTO, META_PROT_ON, META_PROT_OFF };
/* "auto" "on" "off", as in metadata.json and on the command line */
const char *meta_prot_key(int mode);
int  meta_prot_parse(const char *key);          /* -1 if unknown */

/* the media types of a box, in the order they are asked for */
enum { MEDIA_FLOPPY, MEDIA_CD, MEDIA_DVD, MEDIA_KINDS };

typedef struct {
    char file[128];          /* name inside data/ */
    long long bytes;
    char sha256[65];
    char at[20];             /* "2026-10-15T21:20:11" */
} meta_read;

typedef struct {
    int  n;                  /* position in the box, from 1 */
    int  kind, num;          /* MEDIA_..., and number within the type (CD 1) */
    char label[128], note[256];
    char status[24];         /* "" until it has been handled */
    char master[128];        /* "" if no read succeeded */
    char scan[128], report[128];
    int  nreads;
    meta_read reads[META_MAXREAD];
    char verdict[16];        /* verdict_name() or "" */
    int  agree, unstable_tracks;
    int  duplicate_of;       /* the read was the same as this disk's: set aside, to read again */
    /* META_PROT_ON or _OFF: this disk is read with or without the protection
     * scan whatever the box says (the safety net: a read without it failed
     * or had errors; with it the drive reset itself); 0 follows the box */
    int  copy_protection;
} meta_disk;

typedef struct {
    char title[256], publisher[128], year[16], platform[64];
    char language[64], edition[128], notes[512];
    int  count[MEDIA_KINDS];        /* composition: 3 floppies, 1 CD */
    int  media_expected;            /* all the media: meta_set_counts */
    char names[MEDIA_KINDS][64];    /* naming scheme per type, "" = not decided */
    int  copy_protection;           /* META_PROT_..., auto in boxes made before it */
    char started[20], closed[20];   /* "" = not yet */
    char schema[32];                /* as read from metadata.json; "" for a new box */
    /* the changes made after closing: the JSON array as text, "" if none */
    char history[131072];
    int  ndisk;
    meta_disk disk[META_MAXDISK];   /* in order of n */
} meta_box;

/* "floppy", "cd", "dvd" (metadata.json); -1 if unknown */
const char *meta_kind_key(int kind);
int  meta_kind_parse(const char *key);
/* "floppy", "CD", "DVD": for texts, unchanged in the plural */
const char *meta_kind_label(int kind);

/* the composition; media_expected becomes the sum */
void meta_set_counts(meta_box *b, int floppy, int cd, int dvd);
/* how many media of that type (a box with only media_expected is all floppy) */
int  meta_count(const meta_box *b, int kind);
/* type and number within the type of position n: floppies first, then CD, then DVD */
void meta_slot(const meta_box *b, int n, int *kind, int *num);
void meta_slot_counts(const int count[MEDIA_KINDS], int n, int *kind, int *num);
/* "floppy 3", "CD 1" */
void meta_disk_name(int kind, int num, char *out, size_t n);
/* "3 floppy, 1 CD" */
void meta_composition_counts(const int count[MEDIA_KINDS], char *out, size_t n);
void meta_composition(const meta_box *b, char *out, size_t n);

int  meta_disk_done(const meta_disk *d);   /* final outcome, no longer to be asked */
int  meta_status_done(const char *status);
/* the class of the outcome: p good (pulito, verificato redump), m reads
 * differ (marginale, instabile), f read once (fragile, letto una volta),
 * e errors (errore, errori), - not an outcome yet */
char meta_status_class(const char *status);
meta_box *meta_new(void);
void meta_delete(meta_box *b);
meta_disk *meta_disk_get(meta_box *b, int n);
void meta_now(char out[20]);

/* the technical data of a medium, from its master image */
typedef void (*meta_tech_fn)(void *ctx, const meta_disk *d, const char *master_path, json_writer *w);
int  meta_save(const meta_box *b, const char *datadir, meta_tech_fn tech, void *ctx, strbuf *err);
/* adds one JSON object to the history; -1 if it does not fit or is not
 * valid, and then the history stays as it was */
int  meta_history_add(meta_box *b, const char *entry);
/* also reads the old schema "retroarchiver-floppy-1" (all floppies) */
int  meta_load(meta_box *b, const char *datadir, strbuf *err);

#endif
