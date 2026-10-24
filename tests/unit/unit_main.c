#include "unit.h"

int unit_fail = 0, unit_n = 0;

int main(void)
{
    printf("\ncore\n");
    test_strbuf();
    test_fsu8();
    test_compare();
    test_sha256();
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
    test_optical();
    test_lang();                    /* src/gui/lang.c: portable, tested here */
    printf("  %d checks, %s\n", unit_n,
           unit_fail ? "at least one FAIL." : "all green.");
    return unit_fail ? 1 : 0;
}
