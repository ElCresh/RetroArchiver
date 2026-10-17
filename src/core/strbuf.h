/* strbuf - self-growing string, to compose text without fixed buffers;
 * str_copy for fixed-length fields. */
#ifndef STRBUF_H
#define STRBUF_H
#include <stdarg.h>
#include <stddef.h>

typedef struct { char *s; size_t len, cap; } strbuf;

void sb_init(strbuf *b);
void sb_free(strbuf *b);
void sb_clear(strbuf *b);
void sb_vprintf(strbuf *b, const char *fmt, va_list ap);
void sb_printf(strbuf *b, const char *fmt, ...);
void sb_puts(strbuf *b, const char *s);

/* copy into dst (n bytes), truncating without splitting a UTF-8 character */
void str_copy(char *dst, size_t n, const char *src);

#endif
