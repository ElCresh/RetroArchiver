/* The window's dialogs: Settings (settings_dlg.c) and About (about.c).
 * Both are modal, with their layout in retroarchiver.rc and their texts
 * from tr() when they open. Windows only. */
#ifndef RA_DIALOGS_H
#define RA_DIALOGS_H

#include <windows.h>
#include "gui/settings.h"

/* ---- Settings ---- */

enum { PAGE_GENERAL, PAGE_FLOPPY, PAGE_OPTICAL, PAGE_AUTO, PAGE_IA, PAGE_COUNT };

/* a language of the menu: its code ("" = Windows' one) and its own name */
#define UI_LANG_MAX 17
typedef struct { wchar_t code[16], name[64]; } ui_lang;

/* Edits p, starting from the tab page. 1 if the user pressed OK (p then
 * holds the new values, already checked), 0 if Cancel. Saving them is the
 * caller's: the dialog only edits. s is there for the Archive.org keys,
 * which are not a setting like the others: they are a secret the dialog
 * reads and, on OK, writes itself */
int settings_dialog(HWND owner, const settings *s, prefs *p,
                    const ui_lang *langs, int nlangs, int page);

/* ---- About ---- */

#define RA_REPO_URL     L"https://github.com/ElCresh/RetroArchiver"
#define RA_LICENSE_FILE L"LICENSE.txt"     /* RetroArchiver's own license, next to the exe */

/* the programs to ask for their version: the paths of the settings; the
 * user's ini says where a cacert.pem of theirs would be */
typedef struct { wchar_t samdisk[MAX_PATH], dic[MAX_PATH], userini[MAX_PATH]; } about_tools;

void about_dialog(HWND owner, const about_tools *t);

/* ---- Edit box (box_edit_dlg.c) ---- */

#include "core/meta.h"
#include "core/session.h"

typedef struct {
    meta_box *box;                       /* the caller's: the box's data and composition as edited */
    char reread[META_MAXDISK + 1];       /* by position in the edited composition */
    char reason[256];
} box_edit_result;

/* Edits a copy of current (the box in bagdir). IDOK Save, IDC_BE_READ
 * Save and read (at least one medium ticked), IDCANCEL. platforms: the
 * list of the setup, NULL-terminated */
int box_edit_dialog(HWND owner, const char *bagdir, const meta_box *current,
                    const wchar_t *const *platforms, box_edit_result *out);

/* ---- Images of a box (images_dlg.c) ---- */

/* The images of the closed box in bagdir: add, remove, relabel, reorder,
 * and Save (confirmation, then the transaction in a thread). 1 if the box
 * was changed */
int images_dialog(HWND owner, const char *bagdir);
/* "Box, front", "CD 2, back", "Insert": what an image shows, in the
 * window's language */
void image_caption(const meta_image *im, wchar_t *out, int n);
/* the file of an image of the box in bagdir */
void image_path(const char *bagdir, const char *file, wchar_t *out, int n);

/* two reads as good as each other: 1 keep the new one, 0 the old one */
int choose_dialog(HWND owner, const edit_choice *c);

/* n boxes of 261017: CONVERT_GO, CONVERT_LATER or CONVERT_NEVER */
enum { CONVERT_LATER, CONVERT_GO, CONVERT_NEVER };
int convert_dialog(HWND owner, int n);

/* ---- Upload to Archive.org (ia_dlg.c) ---- */

typedef struct {
    const char *bagdir;          /* the box, closed and whole */
    const settings *set;         /* the keys, and where the user's cacert.pem would be */
    const wchar_t *collection;   /* the collection of the new items, from the settings */
    const wchar_t *prefix;       /* before the proposed identifier; may be empty */
    HWND logwnd;                 /* the window whose black log takes the English lines */
    UINT logmsg;                 /* with that message; lParam: a malloc'd wchar_t* to free */
    UINT donemsg;                /* when the window closes; wParam 1: something went up */
} ia_dlg_args;

/* Opens the upload window: the fields, the files, the public warning,
 * then the progress. It is not modal, so another box can be read while an
 * upload goes on: the caller keeps the handle, lets IsDialogMessage see
 * its messages, and drops it when donemsg arrives. NULL if it does not
 * open. Everything in a is copied */
HWND ia_upload_open(HWND owner, const ia_dlg_args *a);

#endif
