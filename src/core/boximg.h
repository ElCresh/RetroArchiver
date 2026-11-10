/* boximg - the images of a closed box: scans of the box, of its media and
 * of what was inside. They live in data/images/ (in the manifest, like the
 * reads), metadata.json describes them, and they are attached, removed and
 * relabelled in one transaction, the same road as an edit (boxtxn): the box
 * is never left half done. The files are never converted: what is attached
 * is byte for byte what was chosen. Nothing is deleted: an image removed
 * goes to set-aside/ as NAME.removed.
 *
 *   box, front            box-front.jpg      (a second one: box-front-2.tif)
 *   CD 2, front           cd02-front.png
 *   floppy 3, back        floppy03-back.jpg
 *   insert                insert-01.tif, insert-02.pdf */
#ifndef BOXIMG_H
#define BOXIMG_H
#include <stddef.h>
#include "meta.h"
#include "strbuf.h"

#define BOXIMG_DIR "images"

/* 0 a file that cannot be attached, 1 a picture (.jpg .jpeg .png .tif .tiff
 * .bmp .gif), 2 a PDF (only as an insert); the case does not count */
int  boximg_file_type(const char *name);
/* what an image shows, as dskrun takes it: "box-front" ... "box-other",
 * "cd2-front", "floppy3-back", "insert". 0 ok, -1 not one of them */
int  boximg_parse_what(const char *what, meta_image *im);
/* the same from the name of a scan ("front.jpg", "retro scatola.png",
 * "cd2.tif"): 1 if recognised; 0 leaves im an insert */
int  boximg_guess(const char *filename, const meta_box *b, meta_image *im);
/* 0 if filename can be attached to b as im says; -1 with the reason and
 * what to do */
int  boximg_valid(const meta_box *b, const meta_image *im, const char *filename, strbuf *err);
/* "box-front", "cd02-front", "insert": the name without number and extension */
void boximg_base(const meta_image *im, char *out, size_t n);
/* the name for im with the extension of like in lowercase, not among taken
 * (whatever their extension and case) */
void boximg_name(const meta_image *im, const char *like, const char (*taken)[64], int ntaken,
                 char *out, size_t n);
/* 1 if im->file is still a name for what im shows */
int  boximg_same_base(const meta_image *im);
/* the order they are shown in: the front of the box, the rest of the box,
 * the media in the box's order, the inserts; within each, the list's order.
 * idx gets b->nimage indexes of b->image */
void boximg_order(const meta_box *b, int idx[META_MAXIMG]);
/* "box front", "CD 2 back", "insert": English, for the console */
void boximg_what_text(const meta_image *im, char *out, size_t n);

/* ---- changing the images of a box ---- */

typedef struct {
    meta_image im;              /* what it shows; im.file is given when saving a new one */
    char source[1024];          /* a new image: the file to copy; "" for one of the box */
    char was[64];               /* one of the box: its name there; "" for a new one */
} img_item;
/* the images the box is to have, in order */
typedef struct { img_item *item; int n, cap; } img_list;

void boximg_list_load(const meta_box *b, img_list *l);
/* a new image from the file source; -1 with the reason (boximg_valid, too many) */
int  boximg_list_add(img_list *l, const meta_box *b, const char *source, const meta_image *what, strbuf *err);
void boximg_list_remove(img_list *l, int i);
/* dir -1 up, +1 down; the index it ends at */
int  boximg_list_move(img_list *l, int i, int dir);
void boximg_list_free(img_list *l);

enum { IMGCH_ADDED, IMGCH_REMOVED, IMGCH_RELABELED, IMGCH_REORDERED };
typedef struct {
    int what;
    meta_image from, to;        /* ADDED: to; REMOVED: from; RELABELED: both */
    char source[1024];          /* ADDED */
} img_change;
/* what saving want would change in old: how many entries (one _REORDERED at
 * most, when the images that stay come in another order) */
int  boximg_diff(const meta_box *old, const img_list *want, img_change *out, int max);

typedef struct {
    void *ctx;
    /* file k of n (from 1) is being copied, done of total bytes of them all;
     * a nonzero answer stops the save */
    int (*progress)(void *ctx, int k, int n, const char *name, long long done, long long total);
} img_host;

typedef struct {
    int added, removed, relabeled, skipped;
    /* the new files already attached, not added again: "scan.jpg = box-front.jpg; ..." */
    char skipped_names[1024];
} img_result;

/* Makes the images of the box in bagdir those of want, in one transaction:
 * the new files copied (and their SHA-256 taken) into editing/, the removed
 * ones to set-aside/ as .removed, the relabelled ones renamed, metadata.json
 * with an "images" entry in its history, the manifests, the check. host and
 * res may be NULL. 0 saved (also when there was nothing to change), 1
 * stopped by the host, -1 error with the reason: in both the box is as it was */
int  boximg_save(const char *bagdir, const img_list *want, const char *reason, const img_host *host,
                 img_result *res, strbuf *err);

#endif
