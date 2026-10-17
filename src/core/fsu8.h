/* fsu8 - files and folders with UTF-8 names.
 * On Windows it converts to UTF-16 and uses the "W" APIs: a title with an
 * accented letter must be able to become a folder name. */
#ifndef FSU8_H
#define FSU8_H
#include <stdio.h>
#include <stddef.h>

#ifdef _WIN32
#define U8_SEP "\\"
#else
#define U8_SEP "/"
#endif

FILE *u8_fopen(const char *path, const char *mode);
int   u8_mkdir(const char *path);              /* 0 created or already there */
int   u8_mkdirs(const char *path);             /* intermediate folders too; 0 if it exists at the end */
int   u8_remove(const char *path);             /* 0 ok */
int   u8_rename_replace(const char *from, const char *to);
int   u8_exists(const char *path);             /* 1 if file or folder */
int   u8_isdir(const char *path);
long long u8_filesize(const char *path);       /* -1 if it does not exist */
/* calls fn for each entry, except . and .. ; 0 ok, -1 if the folder does not open */
int   u8_listdir(const char *dir,
                 void (*fn)(void *ctx, const char *name, int isdir), void *ctx);
/* subfolders too: fn receives, for files only, the path relative to dir
 * with '/' as separator ("CD01/CD01.cue"); 0 ok, -1 if dir does not open */
int   u8_walk(const char *dir, void (*fn)(void *ctx, const char *rel), void *ctx);
/* removes a file, or a folder with everything in it; 0 ok */
int   u8_rmtree(const char *path);
/* bytes of a file, or of all the files in a folder; -1 if it does not exist */
long long u8_treesize(const char *path);
/* the UTF-8 text s in code page cp (the console of an Italian XP: 850);
 * characters missing there become '?'. Windows only: elsewhere a copy
 * of s. The caller frees it with free(); NULL on failure */
char *u8_to_cp(const char *s, unsigned cp);
/* out = a + separator + b; an empty a gives just b */
void  u8_join(char *out, size_t n, const char *a, const char *b);

#endif
