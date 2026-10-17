/* Mini framework for the core tests: CHECK counts and reports, without
 * stopping at the first error. Tests run from the tests/ folder. */
#ifndef UNIT_H
#define UNIT_H
#include <stdio.h>
#include <string.h>

extern int unit_fail, unit_n;

#define CHECK(cond) do { unit_n++; if (!(cond)) { unit_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); unit_n++; \
    if (!a_ || !b_ || strcmp(a_, b_)) { unit_fail++; \
    printf("  FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
           a_ ? a_ : "(null)", b_ ? b_ : "(null)"); } } while (0)

void test_strbuf(void);
void test_fsu8(void);
void test_compare(void);
void test_sha256(void);
void test_json(void);
void test_fat12(void);
void test_meta(void);
void test_bagit(void);
void test_runner(void);
void test_floppy(void);
void test_session(void);
void test_archive(void);
void test_optical(void);

#endif
