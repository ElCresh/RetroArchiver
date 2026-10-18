#include "unit.h"
#include "core/fat12.h"

void test_fat12(void)
{
    edsk d; strbuf msg; fat_info fi; int i, found = 0, len = 0;
    const unsigned char *s;

    sb_init(&msg);
    CHECK(edsk_load("fixtures/fat_144.dsk", &d, &msg) == 1);
    s = edsk_sector(&d, 0, 0, 1, &len);
    CHECK(s != NULL && len == 512);
    if (s) CHECK(s[510] == 0x55 && s[511] == 0xAA);
    CHECK(edsk_sector(&d, 0, 0, 19, &len) == NULL);     /* does not exist */
    CHECK(edsk_sector(&d, 81, 0, 1, &len) == NULL);     /* noise cylinder */

    CHECK(fat12_read(&d, &fi) == 1);
    CHECK_STR(fi.volume_label, "MONKEY3");
    CHECK(fi.nfiles == 4);
    for (i = 0; i < fi.nfiles; i++) {
        fat_entry *e = &fi.files[i];
        if (!strcmp(e->path, "MONKEY.003")) {
            found++;
            CHECK(e->size == 1300 && !e->isdir);
            CHECK_STR(e->date, "1990-10-01T12:00:00");
        }
        if (!strcmp(e->path, "README.TXT")) { found++; CHECK(e->size == 9); }
        if (!strcmp(e->path, "SAVE")) { found++; CHECK(e->isdir); }
        if (!strcmp(e->path, "SAVE/SAVE1.SAV")) {
            found++;
            CHECK(e->size == 700);
            CHECK_STR(e->date, "1990-11-02T20:05:00");
        }
    }
    CHECK(found == 4);
    fat12_free(&fi);
    edsk_free(&d);

    /* directory that refers back to itself: the listing stops, the note says so */
    CHECK(edsk_load("fixtures/fat_loop.dsk", &d, &msg) == 1);
    CHECK(fat12_read(&d, &fi) == 1);
    CHECK_STR(fi.note, "directory with circular references: partial list");
    found = 0;
    for (i = 0; i < fi.nfiles; i++)
        if (!strcmp(fi.files[i].path, "SAVE/SAVE1.SAV")) found++;
    CHECK(found == 1);
    fat12_free(&fi);
    edsk_free(&d);

    CHECK(edsk_load("fixtures/ident_a.dsk", &d, &msg) == 1);
    CHECK(fat12_read(&d, &fi) == 0);
    CHECK_STR(fi.note, "no recognizable FAT12 file system");
    fat12_free(&fi);
    edsk_free(&d);
    sb_free(&msg);
}
