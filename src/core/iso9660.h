/* iso9660 - the file system of data CDs and DVDs, just enough for the
 * metadata: volume label, dates, file list. Like fat12.c for floppies.
 * Reads an .iso (2048-byte sectors) or the data track of a raw CD (.bin,
 * 2352 bytes, mode 1 or mode 2 form 1). If the disc has the Joliet
 * supplementary descriptor, long names and label come from there (UCS-2
 * big endian, converted to UTF-8); otherwise the ISO 9660 names (8.3). */
#ifndef ISO9660_H
#define ISO9660_H
#include <stddef.h>

#define ISO_MAXFILES 4000

typedef struct {
    char path[512];         /* "GIOCO/DATI/MONDO.DAT", in UTF-8 */
    long long size;
    char date[20];          /* "1996-05-01T10:20:00" or "" */
    int isdir;
} iso_file;

typedef struct {
    /* the Joliet label is up to 16 characters: up to 48 bytes in UTF-8 */
    char volume_label[56], system_id[40], publisher[130], application[130];
    int joliet;                         /* 1: names and label from Joliet */
    char created[20], modified[20];     /* "" if the disc does not say */
    long long volume_bytes;
    int nfiles, truncated;              /* truncated: beyond ISO_MAXFILES */
    iso_file *files;
    char note[160];                     /* why it cannot be read, or what is missing */
} iso_info;

/* sector_size 2048 (.iso), 2352 (raw CD); data_offset 0, 16 (mode 1) or
 * 24 (mode 2 form 1). 1 if there is an ISO 9660, 0 otherwise (reason in note) */
int  iso_read(const char *path, int sector_size, int data_offset, iso_info *out);
void iso_free(iso_info *i);

#endif
