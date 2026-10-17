/* sha256 - SHA-256 digests (FIPS 180-4) for the BagIt manifests. */
#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    unsigned char buf[64];
    size_t n;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, unsigned char out[32]);
void sha256_hex(const unsigned char d[32], char out[65]);
/* 0 ok, -1 if the file cannot be read; bytes may be NULL */
int  sha256_file(const char *path, char hex[65], long long *bytes);
/* like sha256_file, but path can also be a folder (a read CD):
 * digest of the lines "digest  path\n" of its files, in order */
int  sha256_path(const char *path, char hex[65], long long *bytes);
/* one line of that text: for callers that already have the file digests */
void sha256_tree_line(sha256_ctx *c, const char *hex, const char *rel);

#endif
