#include "unit.h"
#include "core/sha256.h"
#include "core/fsu8.h"

static void digest(const void *p, size_t n, char hex[65])
{
    sha256_ctx c; unsigned char d[32];
    sha256_init(&c); sha256_update(&c, p, n); sha256_final(&c, d);
    sha256_hex(d, hex);
}

void test_sha256(void)
{
    char hex[65]; sha256_ctx c; unsigned char d[32]; char a[1000]; int i;
    long long bytes = 0; FILE *f;

    digest("", 0, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    digest("abc", 3, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    memset(a, 'a', sizeof(a));
    sha256_init(&c);
    for (i = 0; i < 1000; i++) sha256_update(&c, a, sizeof(a));
    sha256_final(&c, d); sha256_hex(d, hex);
    CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    u8_mkdir("tmp_unit");
    f = u8_fopen("tmp_unit/abc.txt", "wb");
    CHECK(f != NULL);
    if (f) { fputs("abc", f); fclose(f); }
    CHECK(sha256_file("tmp_unit/abc.txt", hex, &bytes) == 0);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(bytes == 3);
    CHECK(sha256_file("tmp_unit/non-esiste.txt", hex, NULL) == -1);

    /* a file larger than the read buffer: one million 'a' */
    f = u8_fopen("tmp_unit/milione.txt", "wb");
    CHECK(f != NULL);
    if (f) { for (i = 0; i < 1000; i++) fwrite(a, 1, sizeof(a), f); fclose(f); }
    CHECK(sha256_file("tmp_unit/milione.txt", hex, &bytes) == 0);
    CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(bytes == 1000000);
    u8_remove("tmp_unit/milione.txt");
}
