/* fat12 - volume label and file list from an EDSK image with a FAT12
 * file system (DOS and Windows disks). Read only, no repair: if
 * something does not add up it says so in note. */
#ifndef FAT12_H
#define FAT12_H
#include "edsk.h"

typedef struct {
    char path[256];      /* "SAVE/SAVE1.SAV" */
    long size;
    int  isdir;
    char date[20];       /* "1990-10-01T12:00:00", empty if not valid */
} fat_entry;

typedef struct {
    char volume_label[12];
    int  nfiles;
    fat_entry *files;
    char note[160];      /* why it cannot be read, or what is missing */
} fat_info;

/* 1 if the FAT is readable (even only partly, with notes), 0 if not */
int  fat12_read(const edsk *d, fat_info *out);
void fat12_free(fat_info *fi);

#endif
