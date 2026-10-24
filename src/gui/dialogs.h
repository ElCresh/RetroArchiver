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

#endif
