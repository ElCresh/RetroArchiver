/* strbuf - self-growing string, to compose text without fixed buffers;
 * str_copy for fixed-length fields. */
#ifndef STRBUF_H
#define STRBUF_H
#include <stdarg.h>
#include <stddef.h>

typedef struct { char *s; size_t len, cap; } strbuf;

/* lets gcc check a printf format against its arguments (-Wformat); MinGW
 * with __USE_MINGW_ANSI_STDIO formats like glibc */
#if defined(__GNUC__) && defined(_WIN32)
#define RA_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#elif defined(__GNUC__)
#define RA_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define RA_PRINTF(f, a)
#endif

void sb_init(strbuf *b);
void sb_free(strbuf *b);
void sb_clear(strbuf *b);
void sb_vprintf(strbuf *b, const char *fmt, va_list ap);
void sb_printf(strbuf *b, const char *fmt, ...) RA_PRINTF(2, 3);
void sb_puts(strbuf *b, const char *s);

/* copy into dst (n bytes), truncating without splitting a UTF-8 character */
void str_copy(char *dst, size_t n, const char *src);

#endif
