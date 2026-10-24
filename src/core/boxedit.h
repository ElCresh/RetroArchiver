/* boxedit - changing a closed box: its data and its composition in one
 * transaction, each medium read again in its own (boxtxn). Nothing is
 * deleted: what is replaced, rejected or removed goes to set-aside/, and
 * metadata.json's history says what changed, when and why. */
#ifndef BOXEDIT_H
#define BOXEDIT_H
#include <stddef.h>
#include "bagit.h"
#include "boxtxn.h"
#include "media.h"
#include "meta.h"
#include "strbuf.h"

/* set aside by an edit */
#define EDIT_REPLACED ".replaced"
#define EDIT_REJECTED ".rejected"
#define EDIT_REMOVED  ".removed"
#define EDIT_IDENTICAL ".identical"     /* the same tracks as the read kept */

/* how good a read is: 6 verified by redump, 5 clean (a floppy, or a CD
 * read with cd), 4 clean read with DiscImageCreator's data, 3 marginal or
 * unstable, 2 fragile or read once, 1 with errors, 0 no good read */
int  boxedit_grade(const char *status, const char *method);

enum { KEEP_NEW, KEEP_OLD, KEEP_OLD_IDENTICAL, KEEP_ASK };
/* which read stays; the keys are media_identity.key */
int  boxedit_decide(int old_grade, const char *old_key, int new_grade, const char *new_key);

/* 1 if the box can be edited (closed, written by this version, no edit
 * left halfway); 0 with the reason in why */
int  boxedit_editable(const char *bagdir, strbuf *why);

enum { CH_FIELD, CH_ADDED, CH_REMOVED, CH_FOLDER };
typedef struct {
    int what;
    char field[16];             /* CH_FIELD: "title", "platform", "publisher"... */
    char from[512], to[512];    /* CH_FIELD: the values; CH_FOLDER: the folders */
    int kind, num;              /* CH_ADDED, CH_REMOVED: the medium */
} edit_change;
/* what saving edited would change in the box of bagdir: how many entries */
int  boxedit_diff(const char *bagdir, const meta_box *old, const meta_box *edited, edit_change *out, int max);

/* the data and the composition of edited into the box of bagdir: one
 * transaction. newbagdir: where the box is afterwards. 0 done (also when
 * there was nothing to change), -1 the box is as it was, with the reason */
int  boxedit_save_data(const char *bagdir, const meta_box *edited, const char *reason,
                       char *newbagdir, size_t n, strbuf *err);

/* one medium read again: old is its record in the box, now the record of
 * the new read, whose files are in editing/data */
typedef struct {
    const meta_disk *old, *now;
    media_identity old_id, new_id;
    int keep;                   /* KEEP_... */
    const char *reason;
} edit_disk;
/* KEEP_NEW: the old files in set-aside (.replaced, or .damaged for those in
 * bad), the new ones in data/, the history, one transaction; known becomes
 * the new manifest. KEEP_OLD: the new reads in set-aside as .rejected, the
 * bag is not touched. KEEP_OLD_IDENTICAL: the new reads are removed (the
 * same bytes as the old). 0 ok, -1 the medium is as it was */
int  boxedit_commit_disk(const char *bagdir, meta_box *box, const edit_disk *e, bag_digests *known,
                         const bag_digests *bad, meta_tech_fn tech, void *ctx, strbuf *err);

/* 1 if the box is closed and was written by 261017 (schema 2 or older,
 * messi-da-parte/, Italian outcomes): it can be converted */
int  boxedit_is_old(const char *bagdir);
/* the box of 261017 in the current format, in one transaction: metadata.json
 * schema 3 with a "convert" entry in its history, bag-info and manifests
 * again, messi-da-parte/ into set-aside/ with the English suffixes; data/
 * does not change. 0 converted, 1 already in the current format, -1 not
 * touched, with the reason (open, damaged, unreadable) */
enum {
    CONV_DONE, CONV_CURRENT,    /* converted; already in the current format */
    CONV_NOT_A_BOX, CONV_OPEN, CONV_BROKEN,
    CONV_DAMAGED,               /* a file does not match the manifest: file says which */
    CONV_UNFINISHED,            /* an interrupted change could not be put in order */
    CONV_FAILED                 /* the transaction did not go through: the box is as it was */
};
/* why, for the window, which writes it in its own language; err keeps the
 * English text for the console */
typedef struct { int code; char file[300]; } conv_why;
int  boxedit_convert(const char *bagdir, conv_why *why, strbuf *err);

/* the technical data of a medium, by its kind */
void boxedit_tech(void *ctx, const meta_disk *d, const char *img, json_writer *w);

#endif
