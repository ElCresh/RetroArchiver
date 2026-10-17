/* session - archiving a box, medium by medium: first the floppies, then
 * the CDs, then the DVDs.
 * Knows nothing about consoles or windows: it talks to its host through
 * session_host. dskrun uses it in a console, the GUI in a thread. */
#ifndef SESSION_H
#define SESSION_H
#include <stddef.h>
#include "media.h"
#include "meta.h"

typedef enum {
    EV_PHASE,       /* text: "Scansione", "Lettura 2", "Confronto" */
    EV_LINE,        /* text: a SAMdisk line or a warning */
    EV_DISK_DONE,   /* text: outcome of the disk */
    EV_BOX_DONE,    /* text: "completo" or "incompleto" */
    EV_ERROR        /* text: what went wrong and what to do */
} event_kind;

enum { CMD_READ, CMD_SKIP, CMD_FINISH, CMD_QUIT };

/* the medium the session is about to ask for */
typedef struct {
    int n, total;           /* position in the box and number of media */
    int kind, num, count;   /* MEDIA_..., number within the kind and how many of that kind */
    /* 0: reading is not available yet for this kind. Read and Skip
     * leave it to be read, Finish box makes it missing */
    int readable;
} disk_turn;

typedef struct {
    void (*event)(void *ctx, event_kind k, int disk, const char *text);
    /* waits for the medium; may rewrite label and note */
    int  (*wait_disk)(void *ctx, const disk_turn *t,
                      char *label, size_t llen, char *note, size_t nlen);
    void *ctx;
    /* nonzero: a "read failed" or "interrupted" disk is asked for again
     * right away with wait_disk (Retry) instead of moving on or exiting */
    int ask_again;
} session_host;

typedef struct {
    meta_box *box;              /* filled-in descriptive data; the session adds the disks */
    const char *outroot;        /* archive folder */
    /* naming scheme per kind, "FLOPPY{nn}" "CD{nn}" "DVD{nn}"; a resumed
     * box keeps the ones it was opened with */
    const char *names[MEDIA_KINDS];
    int maxread;                /* 2..META_MAXREAD */
    int forced_cyl;             /* 0 = automatic */
    int keepall;                /* keep all reads, even of clean disks */
    int start;                  /* first disk to ask for, usually 1 */
    /* who reads each kind; NULL = reading not available yet */
    const media_ops *media[MEDIA_KINDS];
} session_cfg;

#define SESSION_CLOSED 0        /* bag closed, complete or incomplete */
#define SESSION_OPEN   1        /* interrupted or with missing disks: can be resumed */
#define SESSION_ERROR  2

int  session_run(const session_cfg *cfg, const session_host *host);

/* the reads whose fingerprint did not match at closing: outside data/,
 * in this tag folder of the bag (not in the manifest), as
 * NAME.danneggiato */
#define SESSION_ASIDE_DIR "messi-da-parte"
/* the names of the files set aside in the bag, one per line ('\n'); how many */
int  session_aside_list(const char *bagdir, strbuf *out);
/* {n} {nn} {nnn} {set} {data}; the result is truncated to n bytes */
void session_expand(char *out, size_t n, const char *tpl, int disk, const char *title);
/* bag folder: outroot + cleaned-up title */
void session_bagdir(char *out, size_t n, const char *outroot, const char *title);

/* the naming scheme of a kind: the one fixed in the box, otherwise the
 * one in names (may be NULL), otherwise FLOPPY{nn} CD{nn} DVD{nn} */
const char *session_name_tpl(const meta_box *b, const char *const names[MEDIA_KINDS], int kind);
/* the medium's file name, without extension: "FLOPPY03", "CD01" */
void session_disk_base(const meta_box *b, const char *const names[MEDIA_KINDS], int kind, int num,
                       char *base, size_t n);
/* fixes in the box the naming schemes of the kinds it has. One opened
 * before mixed media did not have them: the floppies take the scheme of
 * the files already read (DISK{nnn}...), so the names stay of the same
 * kind */
void session_settle_names(meta_box *b, const char *const names[MEDIA_KINDS]);

#endif
