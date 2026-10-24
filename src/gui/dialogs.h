/* The window's dialogs: Settings (settings_dlg.c) and About (about.c).
 * Both are modal, with their layout in retroarchiver.rc and their texts
 * from tr() when they open. Windows only. */
#ifndef RA_DIALOGS_H
#define RA_DIALOGS_H

#include <windows.h>
#include "gui/settings.h"

/* ---- Settings ---- */

enum { PAGE_GENERAL, PAGE_FLOPPY, PAGE_OPTICAL, PAGE_AUTO, PAGE_COUNT };

/* a language of the menu: its code ("" = Windows' one) and its own name */
#define UI_LANG_MAX 17
typedef struct { wchar_t code[16], name[64]; } ui_lang;

/* Edits p, starting from the tab page. 1 if the user pressed OK (p then
 * holds the new values, already checked), 0 if Cancel. Saving them is the
 * caller's: the dialog only edits */
int settings_dialog(HWND owner, prefs *p, const ui_lang *langs, int nlangs, int page);

/* ---- About ---- */

#define RA_REPO_URL     L"https://github.com/ElCresh/RetroArchiver"
#define RA_LICENSE_FILE L"LICENSE.txt"     /* RetroArchiver's own license, next to the exe */

/* the programs to ask for their version: the paths of the settings */
typedef struct { wchar_t samdisk[MAX_PATH], dic[MAX_PATH]; } about_tools;

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

/* two reads as good as each other: 1 keep the new one, 0 the old one */
int choose_dialog(HWND owner, const edit_choice *c);

/* n boxes of 261017: CONVERT_GO, CONVERT_LATER or CONVERT_NEVER */
enum { CONVERT_LATER, CONVERT_GO, CONVERT_NEVER };
int convert_dialog(HWND owner, int n);

#endif
