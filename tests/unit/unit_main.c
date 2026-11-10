#include "unit.h"

int unit_fail = 0, unit_n = 0;

int main(void)
{
    printf("\ncore\n");
    test_strbuf();
    test_fsu8();
    test_compare();
    test_sha256();
    test_b64();
    test_romhash();
    test_json();
    test_fat12();
    test_meta();
    test_bagit();
    test_runner();
    test_floppy();
    test_session();
    test_archive();
    test_boxtxn();
    test_boxedit();
    test_session_edit();
    test_boxclean();
    test_boximg();
    test_optical();
    test_arc_text();
    test_https();                   /* against tests/fake_https.c on 127.0.0.1 */
    test_iaupload();                /* the same fake server, as Archive.org */
    test_lang();                    /* src/gui/lang.c: portable, tested here */
    printf("  %d checks, %s\n", unit_n,
           unit_fail ? "at least one FAIL." : "all green.");
    return unit_fail ? 1 : 0;
}
