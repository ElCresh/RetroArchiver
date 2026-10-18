/* romhash - CRC32, MD5 and SHA-1 in one pass: the fingerprints of a .dat
 * (the Logiqx format of redump and DiscImageCreator). Used when DIC does
 * not write the .dat itself, as with its data command. */
#ifndef ROMHASH_H
#define ROMHASH_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t crc;
    uint32_t md5[4], sha1[5];
    uint64_t bytes;
    unsigned char buf[64];
    size_t n;
} romhash_ctx;

typedef struct {
    long long size;
    char crc[9], md5[33], sha1[41];     /* lowercase hex, as in DIC's .dat */
} romhash;

void romhash_init(romhash_ctx *c);
void romhash_update(romhash_ctx *c, const void *data, size_t len);
void romhash_final(romhash_ctx *c, romhash *out);
/* 0 ok, -1 if the file cannot be read */
int  romhash_file(const char *path, romhash *out);

#endif
