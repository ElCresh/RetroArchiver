/* media - what the session needs to know about a kind of medium. Today
 * the floppy (floppy.c); CD and DVD will be optical.c, with the same
 * functions. The session takes one media_ops per kind
 * (session_cfg.media[MEDIA_CD]...) and picks the one for each medium in
 * the box: a kind without media_ops is asked for but not read, and stays
 * "to be read". */
#ifndef MEDIA_H
#define MEDIA_H
#include "compare.h"
#include "json.h"
#include "runner.h"
#include "strbuf.h"

/* scan and read: the reading program does not start (the reason goes to on_line) */
#define MEDIA_NOTOOL -2

typedef struct {
    const char *name;         /* "floppy", like meta_kind_key() */
    const char *ext;          /* ".dsk" */
    /* does the drive respond? 0 yes, -1 no, with the instruction in msg */
    int  (*check)(strbuf *msg);
    /* scan into out_txt; 0 ok, -1 failed, 1 interrupted, MEDIA_NOTOOL */
    int  (*scan)(const char *out_txt, runner_line_fn on_line, void *ctx);
    /* read the medium into out_img; 0 ok, -1 failed, 1 interrupted, MEDIA_NOTOOL */
    int  (*read)(const char *out_img, runner_line_fn on_line, void *ctx);
    int  (*compare)(const char *const *imgs, int n, int forced_cyl, compare_result *r);
    /* adds the image's technical data to the open JSON object */
    void (*tech)(void *ctx, const char *img, json_writer *w);
    /* survey: how many tracks of the scan have bad sectors (0 none);
     * NULL if the medium cannot tell */
    int  (*problems)(const char *scan_txt);
    /* careful reading for fragile disks (1) or normal (0); may be NULL */
    void (*careful)(int on);
    const char *tool;           /* "SAMdisk", in the messages */
    const char *scan_phase;     /* name of the scan phase: "Scansione", "TOC" */
    /* 1: compare is called after the first read too, and if it returns 0
     * that read is enough (CD found in the redump DAT); ext "" means each
     * read is a folder (data/CD01/) */
    int one_read;
    /* a line of the program that opens a phase of the read ("Controllo
     * C2"): the session announces it; NULL or NULL returned = none */
    const char *(*phase_of)(const char *line);
    /* the image's volume label (FAT12, ISO 9660) in out, UTF-8; 1 if
     * there is one, 0 if not. Used when the user did not type a label */
    int (*volume_label)(const char *img, char *out, size_t n);
    /* CD: the copy protection scan of the next read, META_PROT_AUTO (the
     * files on the disc decide), _ON or _OFF; NULL if the medium has none */
    void (*protection)(int mode);
    /* after a read, also a failed one: 1 made with the protection scan, 0
     * without; -1 it failed for a reason the scan cannot change (the drive's
     * offset is not set, a disc of another type, no access, a drive that
     * resets itself); 2 made with it, and the drive reset itself: the scan
     * is the cause, to read again without */
    int (*protection_used)(void);
} media_ops;

extern const media_ops floppy_media;
extern const media_ops cd_media;
extern const media_ops dvd_media;

/* ---- CD and DVD with DiscImageCreator (optical.c) ---- */
/* program to launch instead of "DiscImageCreator" */
void optical_set_dic(const char *path);
/* the drive: "d:" or "D" (default "d:") */
void optical_set_drive(const char *drive);
/* folder where to look for the redump DATs (default: the program's) */
void optical_set_datdir(const char *dir);
/* the drive's read offset in samples ("+6", "-472"), given to DiscImageCreator
 * when it asks for it; "" not set. -1 if it is not a number */
int  optical_set_drive_offset(const char *samples);
/* the same check without setting it: 0 with the offset as DIC wants it in
 * out ("+6", "0"; "" for an empty one), -1 if it is not a number */
int  optical_parse_offset(const char *samples, char *out, size_t n);
/* the list of the copy protection signature files (default: the program's
 * protection-signatures.txt, or the built-in list if it is missing) */
void optical_set_signatures(const char *path);
/* tests: the disc's files are those of this folder, not of the drive */
void optical_set_disc_root(const char *dir);
/* the signature files of copy protections among the files under root
 * (subfolders too, within limits): how many, with out = "CLCD32.DLL,
 * 00000001.TMP (SafeDisc)"; -1 if root cannot be listed */
int  optical_protection_files(const char *root, char *out, size_t n);
/* the redump DAT that would be used and its date ("2026-10-01"); 0 if
 * there is none */
int  optical_find_dat(char *path, size_t n, char *date, size_t dn);
/* a DiscImageCreator progress line ("Creating .scm (LBA)
 * 1234/300000"): 1 with the two numbers, 0 if it is a normal line */
int  optical_progress(const char *line, long *done, long *total);
/* before a read: are there need free bytes in dir, and can the file
 * system hold a file of biggest bytes (FAT32: 4 GB)? 0 yes, -1 with the
 * reason */
int  optical_room(const char *dir, long long need, long long biggest, strbuf *why);
int  optical_fs_ok(const char *fsname, long long biggest, strbuf *why);
long long optical_free_bytes(const char *dir);     /* -1 if unknown */
/* the dir + file path as DiscImageCreator must receive it, since it is an
 * ANSI program: with the folder's 8.3 short name if needed; -1 with the
 * reason if not even that can be represented in the code page */
int  optical_dic_path(const char *dir, const char *file, char *out, size_t n, strbuf *why);
/* a name read from DIC's .cue or .dat (ANSI code page) into UTF-8 */
void optical_name_utf8(char *s, size_t n);
/* a CD's TOC: the tracks (data or audio, first LBA) and the lead-out */
typedef struct {
    int n;                      /* tracks, 0 = unknown */
    int data[100];              /* 1 data track, 0 audio */
    long start[100];
    long end;                   /* the lead-out: the first LBA after the last track */
    int sessions;               /* 0 = not said */
    int pregap;                 /* track 1 does not start at LBA 0 */
} optical_toc;
/* the TOC from DiscImageCreator's _disc.txt; 0 ok, -1 none in it */
int  optical_toc_file(const char *path, optical_toc *t);
/* The CD drives that cannot read the lead-in or the lead-out, which
 * DiscImageCreator's cd command needs: "ASUS DRW-08D6MT A101; ..." (vendor,
 * product, firmware). Their single-track data CDs are read with the data
 * command straight away */
void optical_set_noleadin_drives(const char *list);
/* called (from the reading thread) when a read finds out that the drive
 * cannot read them, with its model: the caller remembers it */
void optical_on_drive_learned(void (*fn)(void *ctx, const char *model), void *ctx);
/* 1 if model is in list (spaces and case do not count) */
int  optical_drive_listed(const char *list, const char *model);
/* adds model to list, of n bytes; 1 if added, 0 if already there or no room */
int  optical_list_add(char *list, size_t n, const char *model);
/* the i-th model of list (from 0), with single spaces; 0 past the end */
int  optical_list_item(const char *list, int i, char *out, size_t n);
/* removes model from list, of n bytes: 1 if it was there */
int  optical_list_remove(char *list, size_t n, const char *model);
/* the model of the drive at that letter, as Windows knows it ("ASUS
 * DRW-08D6MT A101"); 0 if unknown */
int  optical_drive_model(char letter, char *out, size_t n);
/* program to launch instead of "samdisk" (tests, different folder) */
void floppy_set_samdisk(const char *path);
/* source of the read: "a:" (default) or an image file */
void floppy_set_source(const char *src);
/* 1 if the source is a drive ("a:", "B:", "a:\"), 0 if it is a file */
int  floppy_source_is_drive(const char *src);
/* the revolutions per minute in the output of "samdisk rpm a:"
 * ("300.0 rpm"), rounded; -1 if not there */
int  floppy_parse_rpm(const char *text);
/* the version in the output of samdisk without arguments ("3.8.12") in
 * ver, and the series (3, 4...); 0 if it does not look like SAMdisk. Real
 * floppies need 3: 4.0 alpha on Windows does not read them */
int  floppy_samdisk_series(const char *text, char *ver, size_t n);

#endif
