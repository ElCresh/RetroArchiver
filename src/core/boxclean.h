/* boxclean - Free space: what in set-aside/ of a closed box is superseded,
 * and its removal. A read identical to the one kept; a duplicate or a failed
 * read of a medium that in the end was read well. Nothing else, and nothing
 * outside set-aside/: data/, the manifests and metadata.json are never
 * touched, so the box stays valid and what Archive.org has stays the same.
 *
 *   entry                                   goes
 *   NAME.identical[N]                       all of it
 *   NAME.duplicate[N], medium ended well    all of it
 *   NAME.failed[N], medium ended well       a file: all of it; a folder (CD,
 *                                           DVD): all but its .txt logs
 *
 * "Ended well" is ST_CLEAN or ST_REDUMP and nothing else. The medium of an
 * entry comes from its name ("CD01_b.failed" -> CD 1); an entry whose medium
 * cannot be told, or with any other name, stays. */
#ifndef BOXCLEAN_H
#define BOXCLEAN_H
#include <stddef.h>
#include "meta.h"

typedef struct {
    char name[300];             /* the entry of set-aside/ */
    long long bytes;            /* what goes */
    int isdir;
    int partial;                /* a folder whose .txt logs stay */
} clean_item;

typedef struct {
    clean_item *item; int n, cap;
    long long bytes;            /* the total that goes */
    int kept; long long kept_bytes;     /* the entries that stay, and their size */
} clean_plan;

/* what would go. 0 with the plan (n may be 0: nothing to free); -1 if this
 * is not a box Free space works on: open, of 261017, with an edit under way
 * or left halfway, unreadable, not a box. The plan is empty then */
int  boxclean_plan(const char *bagdir, clean_plan *p);
/* the same for a closed box of the current format already loaded: the scan
 * of the archive has just read its metadata */
void boxclean_plan_box(const char *bagdir, const meta_box *box, clean_plan *p);
void boxclean_free(clean_plan *p);

/* removes one file, or one folder with all it holds; 0 done */
typedef int (*boxclean_rm)(void *ctx, const char *path, int isdir);
/* carries out a plan with rm (the window sends to the Recycle Bin, dskrun
 * deletes); how many calls failed. Then boxclean_tidy */
int  boxclean_run(const char *bagdir, const clean_plan *p, boxclean_rm rm, void *ctx);
/* rm that deletes for good */
int  boxclean_rm_delete(void *ctx, const char *path, int isdir);
/* set-aside/ left empty goes too */
void boxclean_tidy(const char *bagdir);

/* every expected medium ended well: closing such a box is when to offer it */
int  boxclean_all_well(const meta_box *box);

#endif
