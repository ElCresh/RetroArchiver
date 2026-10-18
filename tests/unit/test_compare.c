#include "unit.h"
#include "core/compare.h"
#include "core/edsk.h"
#include "core/fsu8.h"

static int run(const char *a, const char *b, const char *c, compare_result *r)
{
    const char *p[3]; int n = 0;
    p[n++] = a; p[n++] = b;
    if (c) p[n++] = c;
    compare_init(r);
    return compare_files(p, n, 0, r);
}

/* the EDSK track table lives in the 256 header bytes: from 0x34 there is
 * room for 204 tracks, even if the header declares more */
static void test_edsk_table(void)
{
    unsigned char h[256]; edsk d; strbuf msg; FILE *f;
    memset(h, 0, sizeof(h));
    memcpy(h, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
    h[0x30] = 255; h[0x31] = 2;
    u8_mkdir("tmp_unit");
    f = u8_fopen("tmp_unit/troppe.dsk", "wb");
    if (f) { fwrite(h, 1, sizeof(h), f); fclose(f); }
    sb_init(&msg);
    CHECK(edsk_load("tmp_unit/troppe.dsk", &d, &msg) == 1);
    CHECK(d.ok && d.ntrk == 256 - 0x34);
    edsk_free(&d);
    sb_free(&msg);
}

void test_compare(void)
{
    compare_result r;

    CHECK(run("fixtures/ident_a.dsk", "fixtures/ident_b.dsk", NULL, &r) == 0);
    CHECK(r.v == V_IDENTICAL);
    CHECK(r.stdcyl == 80 && r.heads == 2 && r.nsec0 == 18);
    CHECK(strstr(r.detail.s, "IDENTICAL") != NULL);
    compare_free(&r);

    CHECK(run("fixtures/extra_a.dsk", "fixtures/extra_b.dsk", NULL, &r) == 0);
    CHECK(r.v == V_NOISE_ONLY);
    CHECK(r.cylinders == 83 && r.nomcyl == 80);
    compare_free(&r);

    CHECK(run("fixtures/bad_a.dsk", "fixtures/bad_b.dsk", NULL, &r) == 1);
    CHECK(r.v == V_DATA_DIFFER);
    CHECK(r.unstable_tracks == 1);
    compare_free(&r);

    CHECK(run("fixtures/dd40_a.dsk", "fixtures/dd40_b.dsk", NULL, &r) == 0);
    CHECK(r.nomcyl == 40 && r.nsec0 == 9 && r.stdcyl == 40);
    compare_free(&r);

    CHECK(run("fixtures/weak_1.dsk", "fixtures/weak_2.dsk", "fixtures/weak_3.dsk", &r) == 1);
    CHECK(r.v == V_UNSTABLE);
    CHECK(r.unstable_tracks == 2);
    CHECK(r.agree == 1);
    compare_free(&r);

    CHECK(run("fixtures/good_1.dsk", "fixtures/good_2.dsk", "fixtures/good_3.dsk", &r) == 0);
    CHECK(r.v == V_IDENTICAL && r.agree == 3);
    compare_free(&r);

    CHECK(run("fixtures/missing.dsk", "fixtures/ident_a.dsk", NULL, &r) == 2);
    CHECK(r.v == V_ERROR);
    CHECK(strstr(r.detail.s, "cannot open") != NULL);
    compare_free(&r);

    /* with names the text refers to reads by name, without the path */
    {
        const char *p[3] = { "fixtures/weak_1.dsk", "fixtures/weak_2.dsk", "fixtures/missing.dsk" };
        const char *nm[3] = { "FLOPPY01.dsk", "FLOPPY01_b.dsk", "FLOPPY01_c.dsk" };
        compare_init(&r); r.names = nm;
        CHECK(compare_files(p, 2, 0, &r) == 1);
        CHECK(strstr(r.detail.s, "  A: FLOPPY01.dsk ") && strstr(r.detail.s, "  B: FLOPPY01_b.dsk "));
        CHECK(!strstr(r.detail.s, "fixtures/"));
        compare_free(&r);
        compare_init(&r); r.names = nm;
        CHECK(compare_files(p, 3, 0, &r) == 2);
        CHECK(strstr(r.detail.s, "cannot open FLOPPY01_c.dsk") && !strstr(r.detail.s, "fixtures/"));
        compare_free(&r);
        p[2] = "fixtures/weak_3.dsk";
        compare_init(&r); r.names = nm;
        CHECK(compare_files(p, 3, 0, &r) == 1);
        CHECK(strstr(r.detail.s, "  3) FLOPPY01_c.dsk ") && !strstr(r.detail.s, "fixtures/"));
        compare_free(&r);
    }

    CHECK_STR(verdict_name(V_UNSTABLE), "unstable");
    CHECK_STR(verdict_name(V_NOISE_ONLY), "noise_only");
    test_edsk_table();
}
