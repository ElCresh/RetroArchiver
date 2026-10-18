#include "unit.h"
#include "core/romhash.h"
#include "core/fsu8.h"

static void hash(const void *p, size_t n, romhash *h)
{
    romhash_ctx c;
    romhash_init(&c); romhash_update(&c, p, n); romhash_final(&c, h);
}

void test_romhash(void)
{
    romhash h; romhash_ctx c; char a[1001]; int i; FILE *f;

    hash("", 0, &h);
    CHECK_STR(h.crc, "00000000");
    CHECK_STR(h.md5, "d41d8cd98f00b204e9800998ecf8427e");
    CHECK_STR(h.sha1, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    hash("abc", 3, &h);
    CHECK_STR(h.crc, "352441c2");
    CHECK_STR(h.md5, "900150983cd24fb0d6963f7d28e17f72");
    CHECK_STR(h.sha1, "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(h.size == 3);
    hash("The quick brown fox jumps over the lazy dog", 43, &h);
    CHECK_STR(h.crc, "414fa339");
    CHECK_STR(h.md5, "9e107d9d372bb6826bd81d3542a419d6");
    CHECK_STR(h.sha1, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    /* 56 bytes: the padding needs a second block */
    hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, &h);
    CHECK_STR(h.sha1, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK_STR(h.md5, "8215ef0796a20bcaaae116d3876c664a");

    /* one million 'a' in pieces that do not fall on the blocks */
    memset(a, 'a', sizeof(a));
    romhash_init(&c);
    for (i = 0; i < 1000; i++) romhash_update(&c, a, i % 2 ? 999 : 1001);
    romhash_final(&c, &h);
    CHECK_STR(h.sha1, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    CHECK_STR(h.md5, "7707d6ae4e027c70eea2a935c2296f21");
    CHECK(h.size == 1000000);

    u8_mkdir("tmp_unit");
    f = u8_fopen("tmp_unit/abc.bin", "wb");
    if (f) { fputs("abc", f); fclose(f); }
    CHECK(romhash_file("tmp_unit/abc.bin", &h) == 0 && h.size == 3);
    CHECK_STR(h.sha1, "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(romhash_file("tmp_unit/non-esiste.bin", &h) == -1);
    u8_remove("tmp_unit/abc.bin");
}
