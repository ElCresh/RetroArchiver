#include "unit.h"
#include "core/strbuf.h"

void test_strbuf(void)
{
    strbuf b; int i; char small[4], big[64];

    sb_init(&b);
    CHECK_STR(b.s, "");
    sb_printf(&b, "disco %02d", 7);
    sb_puts(&b, " ok");
    CHECK_STR(b.s, "disco 07 ok");
    for (i = 0; i < 1000; i++) sb_puts(&b, "x");
    CHECK(b.len == 11 + 1000);
    sb_clear(&b);
    CHECK_STR(b.s, "");
    sb_free(&b);

    str_copy(big, sizeof(big), "Monkey");
    CHECK_STR(big, "Monkey");
    str_copy(big, sizeof(big), NULL);
    CHECK_STR(big, "");
    /* "aa" with accented a's is 4 bytes: only one fits in 4 bytes */
    str_copy(small, sizeof(small), "\xc3\xa0\xc3\xa0");
    CHECK_STR(small, "\xc3\xa0");
    str_copy(small, sizeof(small), "abcdef");
    CHECK_STR(small, "abc");
}
