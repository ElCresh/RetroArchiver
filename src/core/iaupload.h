/* iaupload - a closed box becomes an item on Archive.org.
 *
 * What it does, in order: refuses a box that must not go (open, damaged,
 * being edited, made by 261017 and not converted yet); proposes an
 * identifier and asks Archive.org whether it is free; turns the box's
 * data into the item's fields; makes the plan of the files; sends them one
 * by one, writing a journal so that an interrupted upload can be taken up
 * again; waits for Archive.org to archive them and checks what it reports
 * against what was sent; writes the record (iarec.h).
 *
 * Each file is read twice: once to hash it (SHA-256, MD5, SHA-1) and once
 * to send it. The first pass is worth its time: the SHA-256 must match the
 * manifest, so a file changed since the bag was closed stops the upload
 * before a single byte leaves, and the MD5 goes in the Content-MD5 header,
 * which makes the server refuse a file that arrived different.
 *
 * It knows nothing about windows or consoles: it talks to its caller
 * through an ia_host of callbacks, as the session does. The keys live only
 * in the ia_config the caller passes and in the Authorization header:
 * never in a result, a message, the journal or the record. */
#ifndef IAUPLOAD_H
#define IAUPLOAD_H
#include <stddef.h>
#include "iarec.h"
#include "meta.h"
#include "tls.h"

#define IA_MD_BASE "https://archive.org"
#define IA_S3_BASE "https://s3.us.archive.org"

/* an identifier is proposed no longer than this: room for a -2 suffix */
#define IA_ID_PROPOSE 90
/* a field whose header would be longer than this goes with a metadata
 * write right after the upload instead (a long description) */
#define IA_HEADER_MAX 2000
/* the tries of a PUT that the network broke, and the seconds between them */
#define IA_PUT_TRIES 3
/* how long Archive.org is given to archive the files, and how often it is asked */
#define IA_TASK_WAIT_S 60
#define IA_TASK_POLL_S 10

/* why an upload, or one of the questions, stopped */
enum {
    IA_OK = 0,
    IA_ERR_CANCELED,      /* Stop, from another thread */
    IA_ERR_BOX,           /* this box cannot go: ia_box_why says what to do */
    IA_ERR_NO_CERTS,      /* the CA bundle is missing or unusable */
    IA_ERR_TLS,           /* no secure connection: result->tls says why */
    IA_ERR_NET,           /* the network: result->net_err */
    IA_ERR_HTTP,          /* an answer we do not understand: result->status */
    IA_ERR_KEYS,          /* Archive.org does not accept the keys */
    IA_ERR_TAKEN,         /* the identifier is somebody else's item */
    IA_ERR_IDENT,         /* the identifier does not follow the rules */
    IA_ERR_FILE,          /* a file of the package cannot be read */
    IA_ERR_CHANGED,       /* a file no longer matches the manifest */
    IA_ERR_VERIFY,        /* everything went up, but the item does not match */
    IA_ERR_PENDING,       /* everything went up; Archive.org is still archiving */
    IA_ERR_SPAM,          /* Archive.org says the item looks like spam */
    IA_ERR_RECORD,        /* the record could not be written */
    IA_ERR_MEM
};

/* why a box cannot be uploaded */
enum { IA_BOX_OK, IA_BOX_NOTBOX, IA_BOX_OPEN, IA_BOX_BROKEN, IA_BOX_EDITING, IA_BOX_OLD };
/* IA_BOX_* for the bag in bagdir */
int ia_box_can_upload(const char *bagdir);

/* what the upload is doing */
enum { IA_PH_PREPARE, IA_PH_HASH, IA_PH_PUT, IA_PH_META, IA_PH_TASKS, IA_PH_VERIFY, IA_PH_RETRY,
       IA_PH_DELETE };

typedef struct {
    int phase;
    int file, files;            /* the file being handled, out of how many */
    const char *name;           /* its name in the item */
    long long done, size;       /* of that file */
    long long total_done, total;
    int seconds;                /* IA_PH_RETRY and IA_PH_TASKS: how long the wait is */
} ia_progress;

typedef struct {
    void *ctx;
    void (*progress)(void *ctx, const ia_progress *p);
    /* one line for the window's log, in English as the core writes it */
    void (*log)(void *ctx, const char *line);
    int  (*canceled)(void *ctx);         /* nonzero: stop now */
} ia_host;

typedef struct {
    const char *bagdir;
    const char *md_base;        /* NULL: IA_MD_BASE (the tests point it elsewhere) */
    const char *s3_base;        /* NULL: IA_S3_BASE */
    const char *cacert;         /* the CA bundle, needed for an https base */
    const char *access, *secret;
    const char *identifier;
    const ia_meta *meta;        /* the fields as the user confirmed them */
    unsigned kept;              /* an update: the IA_FD_* fields left to Archive.org's value */
    int connect_ms, io_ms;      /* 0: the defaults of net.c */
    int task_wait_s, task_poll_s;   /* 0: IA_TASK_WAIT_S, IA_TASK_POLL_S */
    int put_tries;              /* 0: IA_PUT_TRIES */
    int verify_only;            /* nothing goes up: the item against the record of the last upload */
    long long now;              /* the tests: the time the certificates are checked against */
} ia_config;

/* what the identifier check found */
enum { IA_ID_FREE, IA_ID_TAKEN, IA_ID_OURS };

typedef struct {
    int why;                    /* IA_OK or IA_ERR_* */
    int status;                 /* the HTTP status, when one arrived */
    int net_err;                /* NET_ERR_* with IA_ERR_NET */
    tls_info tls;               /* with IA_ERR_TLS */
    int ident;                  /* IA_ID_* after ia_ident_check */
    int box_why;                /* IA_BOX_* with IA_ERR_BOX */
    char file[320];             /* the file the trouble is about, "" if none */
    char detail[400];           /* English: what the server said, or a core message */
    int  files_sent, mismatch;
    int  update;                /* the item was there: an update */
    int  files_removed, fields_changed;
    long long bytes_sent;
    char identifier[IA_ID_MAX], url[320];
} ia_result;

/* ---- the identifier ---- */

/* 1 if id follows Archive.org's rules: 5 to 100 characters, letters,
 * digits, '.', '_' and '-', the first one alphanumeric */
int  ia_ident_ok(const char *id);
/* "[prefix-]title-edition-platform[-language]", lowercase and without
 * accents; language only when it is not English */
void ia_ident_propose(const meta_box *b, const char *prefix, char *out, size_t n);
/* asks Archive.org: out->ident gets IA_ID_FREE, IA_ID_TAKEN, or
 * IA_ID_OURS when the item exists and mine (the identifier this box was
 * uploaded with, NULL if none) is that same one */
int  ia_ident_check(const ia_config *cfg, const char *id, const char *mine, ia_result *out);
/* the first free identifier from the proposal: base, base-2, base-3...
 * Returns IA_OK and writes it in out (n bytes) */
int  ia_ident_free(const ia_config *cfg, const char *base, char *out, size_t n,
                   ia_result *res);

/* ---- the fields ---- */

/* the fields proposed for the box in bagdir; collection may be NULL
 * (then the default of the schema). 0 ok, -1 and err if the box does not read */
int  ia_meta_from_box(const char *bagdir, const char *collection, ia_meta *out, strbuf *err);
/* the three-letter MARC code of a language name, the name itself if
 * unknown; several names separated by commas give several codes */
const char *ia_lang_marc(const char *name);

/* the fields of an item as GET /metadata/ID reports them (the whole
 * answer): a field written several times is a list, the description goes
 * back from HTML to plain text. 0 ok, -1 if it is not an item */
int  ia_meta_from_item(const char *json, ia_meta *out);
/* asks Archive.org for the item's fields now, for an update. IA_OK, and
 * out->ident IA_ID_FREE if the item is not there */
int  ia_item_read(const ia_config *cfg, const char *id, ia_meta *live, ia_result *out);

/* ---- the files ---- */

/* the files of the package in the order they go up: bagit.txt first (it
 * creates the item with the fields), then data/ from the small to the big
 * ones, then bag-info.txt, manifest-sha256.txt, tagmanifest-sha256.txt.
 * prev (may be NULL) is the record of the last upload: each file then says
 * whether it is the same, new, changed or (at the end) removed. */
int  ia_plan(const char *bagdir, const ia_record *prev, ia_files *out, strbuf *err);

/* ---- the upload ---- */

/* A box whose record says the item was made (ia_record_made) is an
 * update: only the new and changed files go up, with
 * x-archive-keep-old-version so the old ones stay in the item's history;
 * the files the box no longer has are deleted the same way; the fields
 * that differ from the item are written with one JSON Patch; then the same
 * wait and verification as the first time.
 * With cfg->verify_only nothing is sent and no field is written: the item
 * is read and compared with the files and the fields the record says went
 * up (the box may have changed since), and the record gets the outcome:
 * IA_OK and verified, IA_ERR_VERIFY and failed, IA_ERR_PENDING (the record
 * stays as it was) while Archive.org still has tasks and the item differs. */
int  ia_upload(const ia_config *cfg, const ia_host *host, ia_result *out);

/* the journal of an upload under way: one line per finished file. A
 * resumed upload reads it back (the lines whose digest still matches the
 * plan), an upload that ends removes it */
int  ia_journal_read(const char *bagdir, const char *tagmanifest, ia_files *out);

#endif
