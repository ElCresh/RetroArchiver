#include <stdlib.h>
#include "unit.h"
#include "core/media.h"
#include "core/fsu8.h"

static int lines;
static void count(void *ctx, const char *line) { (void)ctx; (void)line; lines++; }

void test_floppy(void)
{
    json_writer w; strbuf msg; FILE *f;

    jw_init(&w);
    jw_begin_obj(&w, NULL);
    floppy_media.tech(NULL, "fixtures/fat_144.dsk", &w);
    jw_end_obj(&w);
    CHECK(strstr(w.out.s, "\"format\": \"1.44M\"") != NULL);
    CHECK(strstr(w.out.s, "\"captured_cylinders\": 83") != NULL);
    CHECK(strstr(w.out.s, "\"volume_label\": \"MONKEY3\"") != NULL);
    CHECK(strstr(w.out.s, "\"name\": \"SAVE/SAVE1.SAV\"") != NULL);
    jw_free(&w);

    jw_init(&w);
    jw_begin_obj(&w, NULL);
    floppy_media.tech(NULL, "fixtures/ident_a.dsk", &w);
    jw_end_obj(&w);
    CHECK(strstr(w.out.s, "\"filesystem\": null") != NULL);
    CHECK(strstr(w.out.s, "no recognizable FAT12") != NULL);
    jw_free(&w);

    CHECK_STR(floppy_media.name, "floppy");
    CHECK_STR(floppy_media.ext, ".dsk");

    /* the drive check in the preparation step: only for a drive */
    CHECK(floppy_source_is_drive("a:") && floppy_source_is_drive("B:") && floppy_source_is_drive(" a:\\ "));
    CHECK(!floppy_source_is_drive("C:\\collaudo\\fixtures\\fat_144.dsk"));
    CHECK(!floppy_source_is_drive("a") && !floppy_source_is_drive("") && !floppy_source_is_drive("1:"));
    CHECK(floppy_parse_rpm("300.0 rpm\n") == 300);
    CHECK(floppy_parse_rpm("Drive speed: 359.84 rpm") == 360);
    CHECK(floppy_parse_rpm("rpm: 300") == 300);
    CHECK(floppy_parse_rpm("Error: drive not ready\n") == -1);
    CHECK(floppy_parse_rpm("SAMDISK [copy|scan|rpm] <args>") == -1);
    CHECK(floppy_parse_rpm("") == -1);
    {   /* the SAMdisk version: 4.0 alpha does not read real floppies */
        char ver[32];
        CHECK(floppy_samdisk_series("SAMdisk 3.8.12 (Jan 1 2020), (c) Simon Owen\n", ver, sizeof(ver)) == 3);
        CHECK_STR(ver, "3.8.12");
        CHECK(floppy_samdisk_series("\nSAMdisk 4.0 alpha\n", ver, sizeof(ver)) == 4);
        CHECK_STR(ver, "4.0");
        CHECK(floppy_samdisk_series("Usage: foo\n", ver, sizeof(ver)) == 0);
        CHECK(floppy_samdisk_series("SAMdisk \n", ver, sizeof(ver)) == 0);
    }

#ifndef _WIN32
    /* with the fake SAMdisk: check, scan and read */
    sb_init(&msg);
    floppy_set_samdisk("../build/fake_samdisk");
    u8_mkdir("tmp_unit");
    f = fopen("tmp_unit/script.txt", "w");
    if (f) { fputs("ident_a\nFAIL\n", f); fclose(f); }
    remove("tmp_unit/script.txt.pos");
    setenv("FAKE_SAMDISK_DIR", "fixtures", 1);
    setenv("FAKE_SAMDISK_SCRIPT", "tmp_unit/script.txt", 1);
    CHECK(floppy_media.check(&msg) == 0);
    lines = 0;
    CHECK(floppy_media.scan("tmp_unit/x.scan.txt", count, NULL) == 0);
    CHECK(lines == 3 && u8_filesize("tmp_unit/x.scan.txt") > 0);
    CHECK(floppy_media.read("tmp_unit/x.dsk", count, NULL) == 0);
    CHECK(u8_filesize("tmp_unit/x.dsk") == u8_filesize("fixtures/ident_a.dsk"));
    CHECK(floppy_media.read("tmp_unit/y.dsk", count, NULL) == -1);
    CHECK(!u8_exists("tmp_unit/y.dsk"));
    {   /* like the window: samdisk rpm a: with runner_capture and a timeout */
        const char *rpm[] = { "../build/fake_samdisk", "rpm", "a:", NULL };
        char out[256]; int code = -1;
        CHECK(runner_capture(rpm, out, sizeof(out), 5000, &code) == RUNNER_DONE);
        CHECK(code == 0 && floppy_parse_rpm(out) == 300);
        setenv("FAKE_SAMDISK_RPM_SLOW", "4", 1);         /* without a disk it waits */
        CHECK(runner_capture(rpm, out, sizeof(out), 800, &code) == RUNNER_TIMEOUT);
        CHECK(floppy_parse_rpm(out) == -1);
        setenv("FAKE_SAMDISK_RPM_SLOW", "1", 1);         /* slow, but within the time */
        CHECK(runner_capture(rpm, out, sizeof(out), 5000, &code) == RUNNER_DONE);
        CHECK(floppy_parse_rpm(out) == 300);
        unsetenv("FAKE_SAMDISK_RPM_SLOW");
        setenv("FAKE_SAMDISK_NODRIVE", "1", 1);
        CHECK(runner_capture(rpm, out, sizeof(out), 5000, &code) == RUNNER_DONE);
        CHECK(code == 1 && floppy_parse_rpm(out) == -1);
        unsetenv("FAKE_SAMDISK_NODRIVE");
    }
    {   /* the fake SAMdisk posing as 4.0 alpha, as the window sees it */
        const char *bare[] = { "../build/fake_samdisk", NULL };
        char out[256], ver[32]; int code = -1;
        CHECK(runner_capture(bare, out, sizeof(out), 5000, &code) == RUNNER_DONE);
        CHECK(floppy_samdisk_series(out, ver, sizeof(ver)) == 3);
        setenv("FAKE_SAMDISK_VERSION", "4.0 alpha", 1);
        CHECK(runner_capture(bare, out, sizeof(out), 5000, &code) == RUNNER_DONE);
        CHECK(floppy_samdisk_series(out, ver, sizeof(ver)) == 4 && !strcmp(ver, "4.0"));
        unsetenv("FAKE_SAMDISK_VERSION");
    }
    setenv("FAKE_SAMDISK_NODRIVE", "1", 1);
    CHECK(floppy_media.check(&msg) == -1);
    CHECK(strstr(msg.s, "does not respond") != NULL);
    unsetenv("FAKE_SAMDISK_NODRIVE");
    sb_clear(&msg);
    floppy_set_samdisk("samdisk-che-non-esiste");
    CHECK(floppy_media.check(&msg) == -1);
    CHECK(strstr(msg.s, "Cannot find") != NULL);
    /* a missing SAMdisk is not a damaged disk */
    lines = 0;
    CHECK(floppy_media.scan("tmp_unit/z.scan.txt", count, NULL) == MEDIA_NOTOOL);
    CHECK(!u8_exists("tmp_unit/z.scan.txt"));
    CHECK(floppy_media.read("tmp_unit/z.dsk", count, NULL) == MEDIA_NOTOOL);
    CHECK(lines == 2 && !u8_exists("tmp_unit/z.dsk"));
    floppy_set_samdisk("samdisk");
    sb_free(&msg);
#else
    (void)f; (void)msg; (void)count;
#endif
}
