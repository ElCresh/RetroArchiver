#include <stdlib.h>
#include "unit.h"
#include "core/bagit.h"
#include "core/fsu8.h"
#include "core/sha256.h"

static void put(const char *path, const char *text)
{
    FILE *f = u8_fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static int contains(const char *path, const char *needle)
{
    static char buf[8192]; FILE *f = u8_fopen(path, "rb"); size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, needle) != NULL;
}

/* digests already verified are reused; the others are computed */
static void bag_known(meta_box *b)
{
    static const char zero[] = "0000000000000000000000000000000000000000000000000000000000000000";
    bag_digests k, m; strbuf err; int bad; char hex[65];
    memset(&k, 0, sizeof(k)); memset(&m, 0, sizeof(m));
    sb_init(&err);
    CHECK(bag_create("tmp_unit/bag5", &err) == 0);
    put("tmp_unit/bag5/data/disco01.dsk", "abc");
    put("tmp_unit/bag5/data/b.bin", "other");
    bag_digests_add(&k, "data/b.bin", zero);
    CHECK(bag_digests_find(&k, "data/b.bin") == 0);
    CHECK(bag_digests_find(&k, "data/x") == -1);
    CHECK(bag_close("tmp_unit/bag5", b, &k, &err, &bad) == 0);
    CHECK(contains("tmp_unit/bag5/manifest-sha256.txt", "0000000000000000000000000000000000000000000000000000000000000000  data/b.bin\n"));
    CHECK(contains("tmp_unit/bag5/manifest-sha256.txt",
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  data/disco01.dsk\n"));
    CHECK(bag_manifest_load("tmp_unit/bag5", "manifest-sha256.txt", &m) == 0);
    CHECK(m.n == 2);
    CHECK(bag_digests_find(&m, "data/disco01.dsk") >= 0);
    sha256_file("tmp_unit/bag5/bag-info.txt", hex, NULL);
    CHECK(bag_manifest_load("tmp_unit/bag5", "nothing.txt", &m) == -1);
    bag_digests_free(&k);
    bag_digests_free(&m);
    sb_free(&err);
}

void test_bagit(void)
{
    char name[96], longt[301]; meta_box *b = meta_new(); meta_disk *d; strbuf err; int bad = -1;

    bag_dirname("The Secret of Monkey Island", name, 81);
    CHECK_STR(name, "The Secret of Monkey Island");
    bag_dirname("Citt\xc3\xa0: parte 1/2", name, 81);
    CHECK_STR(name, "Citt\xc3\xa0_ parte 1_2");
    bag_dirname("\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 Disk.", name, 81);   /* Japanese */
    CHECK_STR(name, "___ Disk");
    bag_dirname("  ... ", name, 81);
    CHECK_STR(name, "untitled");
    bag_dirname("Caf\xc3\xa9 \xe2\x82\xac", name, 81);                     /* euro: in cp1252 */
    CHECK_STR(name, "Caf\xc3\xa9 \xe2\x82\xac");
    memset(longt, 'A', 300); longt[300] = 0;
    bag_dirname(longt, name, 81);
    CHECK(strlen(name) <= 80);
    bag_dirname("con", name, 81);
    CHECK_STR(name, "_con");
    bag_dirname("Nul.disk", name, 81);
    CHECK_STR(name, "_Nul.disk");
    bag_dirname("Console", name, 81);
    CHECK_STR(name, "Console");
    /* the manifest is not percent-encoded: no '%' in data/ */
    bag_dirname("100% Puro", name, 81);
    CHECK_STR(name, "100_ Puro");

    sb_init(&err);
    u8_mkdir("tmp_unit");
    CHECK(bag_create("tmp_unit/bag1", &err) == 0);
    CHECK(bag_is_open("tmp_unit/bag1"));
    CHECK(u8_isdir("tmp_unit/bag1/data"));
    put("tmp_unit/bag1/data/disco01.dsk", "abc");
    put("tmp_unit/bag1/data/metadata.json", "{}\n");

    str_copy(b->title, sizeof(b->title), "Prova");
    str_copy(b->platform, sizeof(b->platform), "MS-DOS");
    b->media_expected = 2;
    str_copy(b->closed, sizeof(b->closed), "2026-10-15T22:00:00");
    d = meta_disk_get(b, 1);
    str_copy(d->status, sizeof(d->status), "clean");
    str_copy(d->master, sizeof(d->master), "disco01.dsk");
    d->nreads = 1;
    str_copy(d->reads[0].file, sizeof(d->reads[0].file), "disco01.dsk");
    sha256_file("tmp_unit/bag1/data/disco01.dsk", d->reads[0].sha256, &d->reads[0].bytes);

    CHECK(bag_close("tmp_unit/bag1", b, NULL, &err, &bad) == 0);
    CHECK(bad == 0);
    CHECK(!bag_is_open("tmp_unit/bag1"));
    CHECK(contains("tmp_unit/bag1/bag-info.txt", "Box-Status: incomplete\n"));
    CHECK(contains("tmp_unit/bag1/bag-info.txt", "Missing-Media: floppy 2\n"));
    CHECK(contains("tmp_unit/bag1/bag-info.txt", "Media-Type: 2 floppy\n"));
    CHECK(contains("tmp_unit/bag1/bag-info.txt", "BagIt-Profile-Identifier: urn:retroarchiver:bagit-profile:3\n"));
    CHECK(contains("tmp_unit/bag1/bag-info.txt", "Payload-Oxum: 6.2\n"));
    CHECK(contains("tmp_unit/bag1/bag-info.txt", "Bagging-Date: 2026-10-15\n"));
    CHECK(contains("tmp_unit/bag1/manifest-sha256.txt",
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  data/disco01.dsk\n"));
#ifndef _WIN32
    CHECK(system("python3 -I check_bag.py tmp_unit/bag1 > /dev/null") == 0);
#endif

    /* a file changed after the read: the bag does not close */
    CHECK(bag_create("tmp_unit/bag2", &err) == 0);
    put("tmp_unit/bag2/data/disco01.dsk", "abX");
    sb_clear(&err);
    CHECK(bag_close("tmp_unit/bag2", b, NULL, &err, &bad) == -1);
    CHECK(strstr(err.s, "changed") != NULL);
    CHECK(bad == 1);
    CHECK(bag_is_open("tmp_unit/bag2"));
    /* a missing file: there too we know which disk to re-read */
    u8_remove("tmp_unit/bag2/data/disco01.dsk");
    sb_clear(&err);
    CHECK(bag_close("tmp_unit/bag2", b, NULL, &err, &bad) == -1);
    CHECK(strstr(err.s, "missing") != NULL);
    CHECK(bad == 1);

    /* empty payload: the bag does not close, the verifier would call it invalid */
    {
        meta_box *e = meta_new();
        str_copy(e->title, sizeof(e->title), "Senza piattaforma");
        sb_clear(&err);
        CHECK(bag_create("tmp_unit/bag3", &err) == 0);
        CHECK(bag_close("tmp_unit/bag3", e, NULL, &err, &bad) == -1);
        CHECK(bag_is_open("tmp_unit/bag3"));
        CHECK(strstr(err.s, "platform") != NULL);
        meta_delete(e);
    }

    /* a bag left open by 261017: the .in-corso marker, Box-Status in Italian */
    CHECK(bag_create("tmp_unit/bag4", &err) == 0);
    CHECK(u8_rename_replace("tmp_unit/bag4/" BAG_OPEN_MARK, "tmp_unit/bag4/" BAG_OPEN_MARK_OLD) == 0);
    CHECK(bag_is_open("tmp_unit/bag4"));
    put("tmp_unit/bag4/data/disco01.dsk", "abc");
    sb_clear(&err);
    CHECK(bag_close("tmp_unit/bag4", b, NULL, &err, &bad) == 0);
    CHECK(!bag_is_open("tmp_unit/bag4") && !u8_exists("tmp_unit/bag4/" BAG_OPEN_MARK_OLD));
    CHECK(bag_status_complete("completo") && bag_status_complete(BAG_COMPLETE));
    CHECK(!bag_status_complete("incompleto") && !bag_status_complete(BAG_INCOMPLETE));

#ifndef _WIN32
    /* a file changed after closing: the verifier must reject it */
    put("tmp_unit/bag1/data/disco01.dsk", "abX");
    CHECK(system("python3 -I check_bag.py tmp_unit/bag1 > /dev/null") != 0);
#endif

    bag_known(b);
    meta_delete(b);
    sb_free(&err);
}
