#include "unit.h"
#include <stdlib.h>
#include "core/fsu8.h"

static int seen;

static void count(void *ctx, const char *name, int isdir)
{
    (void)ctx;
    if (!strcmp(name, "citt\xc3\xa0.txt") && !isdir) seen++;
}

void test_fsu8(void)
{
    char p[256], q[256]; FILE *f;

    CHECK(u8_mkdir("tmp_unit") == 0);
    CHECK(u8_mkdir("tmp_unit") == 0);          /* already exists: not an error */
    u8_join(p, sizeof(p), "tmp_unit", "citt\xc3\xa0.txt");
    f = u8_fopen(p, "wb");
    CHECK(f != NULL);
    if (f) { fputs("ciao", f); fclose(f); }
    CHECK(u8_exists(p));
    CHECK(!u8_isdir(p));
    CHECK(u8_isdir("tmp_unit"));
    CHECK(u8_filesize(p) == 4);
    seen = 0;
    CHECK(u8_listdir("tmp_unit", count, NULL) == 0);
    CHECK(seen == 1);
    u8_join(q, sizeof(q), "tmp_unit", "altro.txt");
    CHECK(u8_rename_replace(p, q) == 0);
    CHECK(!u8_exists(p));
    CHECK(u8_remove(q) == 0);
    CHECK(u8_filesize(q) == -1);
    /* intermediate folders */
    CHECK(u8_mkdirs("tmp_unit/a/b/c") == 0);
    CHECK(u8_isdir("tmp_unit/a/b/c"));
    CHECK(u8_mkdirs("tmp_unit/a/b/c") == 0);
    CHECK(u8_mkdirs("tmp_unit" U8_SEP "d" U8_SEP "e" U8_SEP) == 0);
    CHECK(u8_isdir("tmp_unit/d/e"));
    f = u8_fopen("tmp_unit/file.txt", "wb");
    if (f) fclose(f);
    CHECK(u8_mkdirs("tmp_unit/file.txt") == -1);   /* a file in its place */
    u8_remove("tmp_unit/file.txt");
    /* text for the console: on Windows in its code page, elsewhere as is */
    {
        char *c = u8_to_cp("Citt\xc3\xa0 \xe2\x82\xac", 850);
#ifdef _WIN32
        CHECK(c && !strcmp(c, "Citt\x85 ?"));       /* accented a in 850; the euro does not exist there */
#else
        CHECK(c && !strcmp(c, "Citt\xc3\xa0 \xe2\x82\xac"));
#endif
        free(c);
    }
    u8_join(p, sizeof(p), "dir/", "x");
    CHECK_STR(p, "dir/x");
    u8_join(p, sizeof(p), "", "x");
    CHECK_STR(p, "x");
}
