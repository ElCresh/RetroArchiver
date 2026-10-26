/* b64 - base64 (RFC 4648), the standard alphabet with padding.
 *
 * Ten lines instead of CryptBinaryToString: one DLL less to check, and the
 * same code on the host for the tests. Used for the Content-MD5 of an
 * upload and for the encrypted keys in the ini. */
#ifndef B64_H
#define B64_H
#include <stddef.h>

/* how many bytes b64_encode writes for n bytes, terminator included */
#define B64_ENCODED_SIZE(n) (4 * (((size_t)(n) + 2) / 3) + 1)
/* at most this many bytes come out of a text k characters long */
#define B64_DECODED_SIZE(k) (3 * ((size_t)(k) / 4) + 3)

/* n bytes into out (cap bytes, terminator included): the characters
 * written, 0 if out is too small */
size_t b64_encode(const void *data, size_t n, char *out, size_t cap);

/* the text back into out (cap bytes): the bytes written, or -1 if it is
 * not valid base64 or does not fit. Blanks between the characters are
 * allowed (an ini value never has any, a PEM line does) */
long b64_decode(const char *s, void *out, size_t cap);

#endif
