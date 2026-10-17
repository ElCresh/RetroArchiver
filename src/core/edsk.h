/* edsk - reading of EDSK ("EXTENDED") and DSK ("MV - CPC") images. */
#ifndef EDSK_H
#define EDSK_H
#include "strbuf.h"

#define EDSK_MAXTRK 256

typedef struct {
    unsigned char *buf;
    long           len;
    int            cyls, heads, ntrk;
    long           off[EDSK_MAXTRK];     /* start of the track in the file */
    long           siz[EDSK_MAXTRK];     /* length, TIB included         */
    int            cyl[EDSK_MAXTRK];
    int            head[EDSK_MAXTRK];
    int            fmt[EDSK_MAXTRK];     /* 1 = formatted track          */
    int            nomcyl;               /* cylinders of the real format */
    int            nsec0;                /* sectors on track 0           */
    int            ok;                   /* header recognised            */
} edsk;

/* 1 if the file was read, even without a recognised header
 * (then d->ok = 0); 0 if it cannot be read. Errors and warnings go to msg. */
int  edsk_load(const char *path, edsk *d, strbuf *msg);
/* like edsk_load, but in the messages the file is called shown */
int  edsk_load_as(const char *path, const char *shown, edsk *d, strbuf *msg);
void edsk_free(edsk *d);
/* track containing the offset; -1 if header or outside the map */
int  edsk_track_of(const edsk *d, long off);
/* a disk has 80, 40 or 35 cylinders: above that is uncertain territory */
int  edsk_snap(int ncyl);
/* 32-bit FNV-1a: only used to tell whether two tracks are equal */
unsigned long edsk_fnv(const unsigned char *p, long n);
void fmt_thousands(long v, char *out);   /* 1515776 -> "1.515.776" */
/* data of sector r of track (c, h): the first copy if SAMdisk saved
 * more than one. NULL if missing; *len = bytes of the sector */
const unsigned char *edsk_sector(const edsk *d, int c, int h, int r, int *len);

#endif
