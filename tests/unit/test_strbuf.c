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
    /* bytes with a zero among them (a body from the network) */
    sb_add(&b, "a\0b", 3);
    for (i = 0; i < 300; i++) sb_add(&b, "cd", 2);
    CHECK(b.len == 603 && b.s[1] == 0 && b.s[2] == 'b' && b.s[602] == 'd' && b.s[603] == 0);
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
