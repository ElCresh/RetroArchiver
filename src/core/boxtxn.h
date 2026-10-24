/* boxtxn - takes a closed bag from its old content to the new one without
 * ever leaving it half done. The new files wait in editing/ (a tag folder,
 * ignored by BagIt) while the bag stays closed and valid; then a journal
 * lists the moves, and the moves, metadata.json, the manifests, the check
 * and the folder's new name follow. Interrupted at any point (a power cut),
 * boxtxn_recover finishes it or puts the old bag back: never a mix.
 * Used to edit a box and, later, to convert the boxes of 261017. */
#ifndef BOXTXN_H
#define BOXTXN_H
#include <stddef.h>
#include "bagit.h"
#include "meta.h"
#include "strbuf.h"

#define BOXTXN_DIR   BAG_EDIT_DIR
#define BOXTXN_STAGE BAG_EDIT_DIR "/data"      /* where the new reads are made */

/* paths relative to the bag, with '/': "data/CD01" -> "set-aside/CD01.replaced" */
typedef struct { int n, cap; char (*from)[320]; char (*to)[320]; } txn_moves;
void txn_add(txn_moves *m, const char *from, const char *to);
void txn_free(txn_moves *m);
/* "<set-aside>/NAME<suffix>", with 2, 3... after it if that is already on
 * disk or already the destination of a move in m */
void txn_aside_name(const char *bagdir, const txn_moves *m, const char *name, const char *suffix,
                    char *out, size_t n);

/* 0 done (the bag is in newdir if that is not NULL); -1 put back as it was,
 * with the reason and what to do in err. known: the digests verified just
 * before, reused for the files the moves do not touch (may be NULL) */
int  boxtxn_commit(const char *bagdir, const txn_moves *m, const meta_box *box, meta_tech_fn tech,
                   void *ctx, const bag_digests *known, const char *newdir, strbuf *err);

enum { BOXTXN_NONE, BOXTXN_FORWARD, BOXTXN_BACK, BOXTXN_CLEANED };
/* an editing/ left behind: BOXTXN_FORWARD finished, BOXTXN_BACK put back,
 * BOXTXN_CLEANED nothing had started (the unused reads set aside), _NONE
 * nothing to do; -1 if it cannot. finaldir: where the bag is at the end */
int  boxtxn_recover(const char *bagdir, meta_tech_fn tech, void *ctx, char *finaldir, size_t n,
                    strbuf *log);

/* tests only: at step 1-9 boxtxn_commit stops at once and returns -2, as if
 * the power went (crash), or that step's operation fails (fail); 0 = off */
void boxtxn_test_crash(int step);
void boxtxn_test_fail(int step);
/* tests only: 1 makes every undo of a move fail */
void boxtxn_test_fail_undo(int on);

#endif
