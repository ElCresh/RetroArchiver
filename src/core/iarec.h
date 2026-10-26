/* iarec - what an upload left behind: BAG/uploads/archive.org.json.
 *
 * The folder sits beside set-aside/, outside the data package and outside
 * the tagmanifest: writing it changes no manifest, so the bag stays closed
 * and valid and a digest recheck does not see it. Recording the upload in
 * metadata.json instead would change the package, hence the manifests,
 * hence make the item "to update" at once: a loop with no end.
 *
 * No network here: the archive (and one day dskrun) reads the record
 * without the TLS code. iaupload.c fills it in and writes it. */
#ifndef IAREC_H
#define IAREC_H
#include <stddef.h>
#include "strbuf.h"

#define IA_RECORD_SCHEMA "retroarchiver-upload-1"
#define IA_SERVICE       "archive.org"
#define IA_UPLOAD_DIR    "uploads"
#define IA_RECORD_NAME   "archive.org.json"
#define IA_JOURNAL_NAME  "archive.org.journal"
#define IA_DETAILS_URL   "https://archive.org/details/"

#define IA_ID_MAX       128      /* an identifier is at most 100 characters */
#define IA_MAX_SUBJECT  16
#define IA_MAX_LANG     8
#define IA_DESC_MAX     8192

/* what the archive shows for a box */
enum { IA_ST_NONE, IA_ST_INTERRUPTED, IA_ST_PENDING, IA_ST_VERIFIED,
       IA_ST_FAILED, IA_ST_OUTDATED };
/* the value of "state" in the record; IA_ST_NONE gives "" */
const char *ia_state_key(int state);
int  ia_state_parse(const char *key);          /* IA_ST_NONE if unknown */

/* the fields sent to Archive.org: what the user saw, and maybe changed,
 * in the upload window. Kept in the record so that an update knows what
 * was sent last time */
typedef struct {
    char mediatype[24], collection[64];
    char title[384], creator[160], publisher[160], date[16];
    char platform[64], edition[128], media[80];
    char rights[300], scanner[64], profile[160];
    int  nlang;  char language[IA_MAX_LANG][24];
    int  nsubj;  char subject[IA_MAX_SUBJECT][80];
    char description[IA_DESC_MAX];
} ia_meta;

/* what each file of the package is, in an update */
enum { IA_F_SAME, IA_F_NEW, IA_F_CHANGED, IA_F_REMOVED };
const char *ia_what_key(int what);

typedef struct {
    char name[320];              /* its path in the item, "data/CD01/CD01.bin" */
    long long size;
    char sha256[65], md5[33], sha1[41];
    char at[20];                 /* when it was uploaded, "" if not yet */
    int  what;                   /* IA_F_* : the plan, not part of the record */
} ia_file;

typedef struct { int n, cap; ia_file *f; } ia_files;

void ia_files_free(ia_files *p);
/* appends a copy; NULL if there is no memory */
ia_file *ia_files_add(ia_files *p, const ia_file *f);
const ia_file *ia_files_find(const ia_files *p, const char *name);
long long ia_files_bytes(const ia_files *p, int what_mask);   /* -1: every file */

typedef struct {
    char identifier[IA_ID_MAX], url[320], collection[64];
    char created[20], updated[20], verified[20];
    int  state;                  /* IA_ST_* */
    char tagmanifest[65];        /* SHA-256 of tagmanifest-sha256.txt when it was sent */
    ia_meta meta;
    ia_files files;
    /* the events, as JSON objects already written, in order: kept as text
     * so that a record written by a newer version keeps its own */
    char events[8192];
} ia_record;

void ia_record_init(ia_record *r);
void ia_record_free(ia_record *r);
/* adds one JSON object to the events; -1 if it does not fit */
int  ia_record_event(ia_record *r, const char *entry);

/* uploads/ of a bag, and the two files in it */
void ia_upload_dir(const char *bagdir, char *out, size_t n);
void ia_record_path(const char *bagdir, char *out, size_t n);
void ia_journal_path(const char *bagdir, char *out, size_t n);

/* 1 if the record is there and readable, 0 if there is none */
int  ia_record_load(const char *bagdir, ia_record *r);
/* temporary file and rename, as metadata.json; 0 ok */
int  ia_record_save(const char *bagdir, const ia_record *r, strbuf *err);

/* the SHA-256 of tagmanifest-sha256.txt, which covers the manifest (so
 * every file of data/) and bag-info.txt: different from the one in the
 * record means the box changed after the upload. 0 ok */
int  ia_tagmanifest_digest(const char *bagdir, char hex[65]);

/* what the archive shows: the record's state, IA_ST_INTERRUPTED if a
 * journal is there, IA_ST_OUTDATED if the bag changed since. r may be
 * NULL; otherwise it gets the record (free it with ia_record_free) */
int  ia_box_state(const char *bagdir, ia_record *r);

#endif
