/* RetroArchiver settings, in two ini files.
 *
 * - the defaults: RetroArchiver.ini next to the executable, written by
 *   the installer. Read only: a normal user cannot write in Program Files
 *   (and from Vista on it would end up in the VirtualStore).
 * - the user's: %APPDATA%\RetroArchiver\RetroArchiver.ini, the only file
 *   the program writes.
 *
 * For each key the user's value wins, else the default, else the value
 * hard-coded here. Whoever upgrades from C:\RetroArchiver keeps their
 * choices: the old ini next to the exe becomes their defaults.
 * Windows only: goes through GetPrivateProfileString, which reads
 * UTF-16 LE files with a BOM.
 */
#ifndef RA_SETTINGS_H
#define RA_SETTINGS_H

#include <windows.h>

/* Sections and keys, in English and grouped as the settings are shown:
 * General, Floppy, Optical (CDs and DVDs), Automatic; Box keeps the last
 * choices of the box preparation, Window the window's position.
 * Version 261017 wrote them in Italian ([Archiviazione] Cartella...,
 * [Finestra] X...): set_get still reads those when the English key is
 * missing, set_convert turns a whole file into the English keys. */
#define SEC_GENERAL   L"General"
#define SEC_FLOPPY    L"Floppy"
#define SEC_OPTICAL   L"Optical"
#define SEC_AUTO      L"Automatic"
#define SEC_BOX       L"Box"
#define SEC_WINDOW    L"Window"
#define SEC_IA        L"ArchiveOrg"

#define KEY_FOLDER     L"ArchiveFolder"    /* General: where the boxes go */
#define KEY_MAXREADS   L"MaxReads"         /* General */
#define KEY_SOUNDS     L"Sounds"           /* General: 1 = Windows sounds */
#define KEY_UILANG     L"UILanguage"       /* General: it, en...; empty = Windows' one */
#define KEY_SAMDISK    L"SAMdisk"          /* Floppy */
#define KEY_READFROM   L"ReadFrom"         /* Floppy: a:, or a .dsk file */
#define KEY_FLOPPYNAME L"FileName"         /* Floppy: FLOPPY{nn} */
#define KEY_DIC        L"DiscImageCreator" /* Optical */
#define KEY_DRIVE      L"Drive"            /* Optical: D: */
#define KEY_CDNAME     L"CDFileName"       /* Optical: CD{nn} */
#define KEY_DVDNAME    L"DVDFileName"      /* Optical: DVD{nn} */
#define KEY_DRIVEOFFSET L"DriveOffset"     /* Optical: the CD drive's read offset in samples, "" = not set */
/* Optical: the drives that cannot read the lead-in or the lead-out, "ASUS
 * DRW-08D6MT A101; ...": learned by the reads, their data CDs are read
 * with DiscImageCreator data straight away */
#define KEY_NOLEADIN   L"NoLeadInDrives"
#define KEY_CDSPEED    L"CDSpeed"          /* Optical: CD read speed, 16 */
#define KEY_DATDIR     L"DatFolder"        /* Optical: where the redump DATs are; "" = the program's folder */
#define KEY_ENABLED    L"Enabled"          /* Automatic: the last choice of the box preparation */
#define KEY_COUNTDOWN  L"CountdownSeconds" /* Automatic: seconds between the label and the read */
#define KEY_FLOPPYPOLL L"FloppyCheckSeconds" /* Automatic: how often the empty floppy drive is asked */
#define KEY_PLATFORM   L"Platform"         /* Box */
#define KEY_LANGUAGE   L"Language"         /* Box: the box's language, not the program's */
#define KEY_COPYPROT   L"CopyProtection"   /* Box: the CD copy protection scan, auto on off */
#define KEY_CONVERTASK L"ConvertAsk"       /* General: 0 = never ask again to convert the boxes of 261017 */
/* ArchiveOrg: the IA-S3 keys of the user, protected with DPAPI and base64
 * (set_get_secret below); only in the user's ini, never in the defaults */
#define KEY_IAKEYS     L"Keys"
#define KEY_IACOLL     L"Collection"       /* ArchiveOrg: the collection of the new items */
/* ArchiveOrg: before every identifier; "" = none, which is what the user
 * chose (the identifiers say nothing about who made them): only in the ini,
 * the window has no field for it */
#define KEY_IAPREFIX   L"IdentifierPrefix"

#define SET_FILE        L"RetroArchiver.ini"
#define SET_USER_DIR    L"RetroArchiver"     /* inside Application Data */
#define SET_ARCHIVE_DIR L"RetroArchive"      /* the default archive, at the root of the system drive */

typedef struct {
    wchar_t defaults[MAX_PATH];   /* "" if its location is unknown */
    wchar_t user[MAX_PATH];       /* "" if its location is unknown: then nothing is saved */
} settings;

/* the two real paths: next to the executable and in Application Data */
void set_init(settings *s);
/* two chosen paths (the tests) */
void set_init_paths(settings *s, const wchar_t *defaults, const wchar_t *user);

void set_get(const settings *s, const wchar_t *sec, const wchar_t *key,
             const wchar_t *def, wchar_t *out, int n);
int  set_int(const settings *s, const wchar_t *sec, const wchar_t *key, int def);
/* v NULL removes the key. 0 if the user's ini cannot be written */
int  set_put(const settings *s, const wchar_t *sec, const wchar_t *key, const wchar_t *v);
/* A user choice: written only if it differs from the default (the
 * installer's, else def); if equal, the key is removed. This way a new
 * default brought by an upgrade also reaches those who never changed it. */
int  set_save(const settings *s, const wchar_t *sec, const wchar_t *key,
              const wchar_t *def, const wchar_t *v);
int  set_put_int(const settings *s, const wchar_t *sec, const wchar_t *key, int v);

/* A file written with the Italian keys of 261017 becomes English: each old
 * key's value goes to the new key (unless that is already there), the old
 * key and the emptied old sections go. Running it again changes nothing.
 * Returns the keys converted, 0 if none, -1 if the file cannot be written */
int  set_convert(const wchar_t *file);

/* "C:\RetroArchive": the archive folder on the system drive */
void set_default_archive(wchar_t *out, int n);

/* ---- a secret value in the user's ini ----
 * The value never touches the file in clear text: CryptProtectData
 * (DPAPI, this Windows user) protects it and the blob goes in base64.
 * An ini copied to another user or another PC cannot be decrypted there,
 * and neither can one whose value was written by hand: both answer
 * SET_SECRET_BAD, which means "ask for the value again", not an error of
 * the program. */

#define SET_SECRET_MAX  512                /* the longest value, terminator apart */

enum { SET_SECRET_NONE, SET_SECRET_OK, SET_SECRET_BAD };

/* out (n bytes) gets the value, UTF-8; it is left empty unless the answer
 * is SET_SECRET_OK. Without DPAPI (not Windows) always SET_SECRET_NONE */
int set_get_secret(const settings *s, const wchar_t *sec, const wchar_t *key, char *out, int n);
/* v NULL or empty removes the key. 0 if it cannot be written or protected */
int set_put_secret(const settings *s, const wchar_t *sec, const wchar_t *key, const char *v);

/* ---- the Archive.org keys ----
 * The two IA-S3 keys of https://archive.org/account/s3.php, 16 characters
 * each today; they go in one secret as "access\nsecret". */

#define IA_KEY_MAX 128

typedef struct { char access[IA_KEY_MAX], secret[IA_KEY_MAX]; } ia_keys;

/* SET_SECRET_*: OK with the two keys in k, NONE never set, BAD unreadable */
int  set_ia_keys_get(const settings *s, ia_keys *k);
/* both keys empty forgets them. 0 if the user's ini cannot be written */
int  set_ia_keys_put(const settings *s, const ia_keys *k);
/* zeroes them where the compiler cannot drop the writing */
void ia_keys_wipe(ia_keys *k);

/* ---- what the Settings window edits ----
 * One struct with every setting as the program uses it: the window loads
 * it at startup, the Settings window edits a copy and saves it with
 * prefs_save, which writes only what differs from the defaults. */

#define PREFS_MAXREADS_MIN   2
#define PREFS_COUNTDOWN_MIN  1
#define PREFS_COUNTDOWN_MAX  60
#define PREFS_POLL_MIN       1
#define PREFS_POLL_MAX       30
#define PREFS_CDSPEED_MIN    1         /* as OPTICAL_SPEED_MIN/MAX in media.h */
#define PREFS_CDSPEED_MAX    48
#define PREFS_LIST           2048        /* NoLeadInDrives */

typedef struct {
    /* General */
    wchar_t folder[MAX_PATH];
    int     maxreads, sounds;
    wchar_t uilang[16];                  /* "" = Windows' language */
    /* Floppy */
    wchar_t samdisk[MAX_PATH], readfrom[MAX_PATH], floppy_name[64];
    /* Optical */
    wchar_t dic[MAX_PATH], drive[16], cd_name[64], dvd_name[64];
    wchar_t datdir[MAX_PATH], offset[16], noleadin[PREFS_LIST];
    int     cd_speed;                    /* x, given to DiscImageCreator */
    /* Automatic */
    int     countdown, floppy_poll;      /* seconds */
    /* Archive.org: the keys are not here, they are a secret of their own */
    wchar_t ia_collection[64], ia_prefix[64];
} prefs;

#define IA_COLLECTION_DEF L"open_source_software"   /* "Community Software" */
#define IA_KEYS_URL       L"https://archive.org/account/s3.php"

/* the defaults alone: the installer's ini, else the code's values */
void prefs_defaults(const settings *s, prefs *p);
/* the user's choices over the defaults; numbers out of range become the default */
void prefs_load(const settings *s, prefs *p);
/* into the user's ini, each key only if it differs from its default.
 * 0 if the user's ini cannot be written */
int  prefs_save(const settings *s, const prefs *p);

/* the third-party programs the installer brings, in the exe's folder */
#define SET_TOOL_SAMDISK L"tools\\samdisk\\samdisk.exe"
#define SET_TOOL_DIC     L"tools\\DIC\\DiscImageCreator.exe"

/* The configured program is not found (an older ini that said
 * C:\samdisk): if <exe folder>\rel exists, 1 and its path in out. */
int set_tool_fallback(const wchar_t *configured, const wchar_t *rel, wchar_t *out, int n);

#endif
