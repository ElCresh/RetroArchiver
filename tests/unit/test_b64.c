/* src/core/b64.c: base64 both ways, with the vectors of RFC 4648 */
#include "unit.h"
#include "core/b64.h"

static void enc(const char *in, const char *want)
{
    char out[128];
    size_t k = b64_encode(in, strlen(in), out, sizeof(out));
    CHECK(k == strlen(want));
    CHECK_STR(out, want);
}

static void test_encode(void)
{
    char out[8];
    enc("", "");
    enc("f", "Zg==");
    enc("fo", "Zm8=");
    enc("foo", "Zm9v");
    enc("foob", "Zm9vYg==");
    enc("fooba", "Zm9vYmE=");
    enc("foobar", "Zm9vYmFy");
    /* all the bytes of a group, so + and / come out too */
    {
        static const unsigned char raw[3] = { 0xfb, 0xef, 0xbe };
        char b[8];
        CHECK(b64_encode(raw, 3, b, sizeof(b)) == 4);
        CHECK_STR(b, "++++");
    }
    {
        static const unsigned char raw[3] = { 0xff, 0xff, 0xff };
        char b[8];
        b64_encode(raw, 3, b, sizeof(b));
        CHECK_STR(b, "////");
    }
    /* a buffer one byte short: nothing written */
    CHECK(b64_encode("foo", 3, out, 4) == 0);
    CHECK(b64_encode("foo", 3, out, 5) == 4);
}

static void dec(const char *in, const char *want)
{
    unsigned char out[128];
    long k = b64_decode(in, out, sizeof(out));
    CHECK(k == (long)strlen(want));
    if (k >= 0) { out[k] = 0; CHECK_STR((const char *)out, want); }
}

static void test_decode(void)
{
    unsigned char out[8];
    dec("", "");
    dec("Zg==", "f");
    dec("Zm8=", "fo");
    dec("Zm9v", "foo");
    dec("Zm9vYmFy", "foobar");
    /* blanks are allowed (a PEM line), the padding is not required */
    dec("Zm9v YmFy", "foobar");
    dec("Zm9vYmFy\r\n", "foobar");
    dec("Zg", "f");
    dec("Zm8", "fo");

    CHECK(b64_decode("Zm9v!", out, sizeof(out)) == -1);     /* not in the alphabet */
    CHECK(b64_decode("Z", out, sizeof(out)) == -1);         /* a lone character */
    CHECK(b64_decode("Zg===", out, sizeof(out)) == -1);     /* too much padding */
    CHECK(b64_decode("Zg=A", out, sizeof(out)) == -1);      /* after the padding */
    CHECK(b64_decode("Zm9vYmFy", out, 5) == -1);            /* does not fit */
    /* a value written by hand in the ini: refused, not read as something */
    CHECK(b64_decode("my-secret-key", out, sizeof(out)) == -1);
}

/* every length from 0 to 300 comes back the same */
static void test_roundtrip(void)
{
    unsigned char raw[300], back[300];
    char text[B64_ENCODED_SIZE(300)];
    size_t n, i;
    int bad = 0;
    for (i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char)(i * 37 + i / 5);
    for (n = 0; n <= sizeof(raw); n++) {
        size_t k = b64_encode(raw, n, text, sizeof(text));
        if (k != 4 * ((n + 2) / 3)) bad = 1;
        if (b64_decode(text, back, sizeof(back)) != (long)n) bad = 1;
        else if (n && memcmp(raw, back, n)) bad = 1;
    }
    CHECK(!bad);
    /* zero bytes in the middle survive */
    {
        static const unsigned char z[5] = { 1, 0, 0, 0, 2 };
        char t[16];
        b64_encode(z, 5, t, sizeof(t));
        CHECK(b64_decode(t, back, sizeof(back)) == 5 && !memcmp(z, back, 5));
    }
}

void test_b64(void)
{
    test_encode();
    test_decode();
    test_roundtrip();
}
