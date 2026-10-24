/* Small helpers shared by the window and its dialogs (Settings, About):
 * UTF-8 <-> UTF-16 conversions, running a program to read what it prints,
 * the paths next to the executable. Windows only. */
#ifndef RA_WINUTIL_H
#define RA_WINUTIL_H

#include <windows.h>

/* into a buffer of n characters/bytes, truncated on a character boundary */
void to_w(const char *s, wchar_t *out, int n);
void to_u8(const wchar_t *s, char *out, int n);

/* the program's output, at most n-1 bytes, with the input from NUL (so
 * DiscImageCreator does not stop on "Press any key"); stopped after 3
 * seconds. 0 if it does not start. code may be NULL: DiscImageCreator
 * without the Visual C++ runtime starts but exits at once with
 * STATUS_DLL_NOT_FOUND */
int run_capture(wchar_t *cmd, char *out, int n, DWORD *code);

/* DiscImageCreator /v: the line after "AppVersion", its last word
 * ("20260101T233129"); 0 if it is not there */
int parse_dic_version(const char *s, char *ver, int n);
/* the exit codes of a program that misses a DLL or an entry point */
int exit_missing_dll(DWORD code);

/* <exe folder>\rel; "" if too long */
void exe_path(const wchar_t *rel, wchar_t *out, int n);
/* 1 if the file exists (and is not a folder) */
int file_exists(const wchar_t *path);

#endif
