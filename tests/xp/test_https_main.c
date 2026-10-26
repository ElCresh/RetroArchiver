/* The network tests of the core (test_https.c) on their own, so they can run
 * on the XP VM without the fixtures the whole unit.exe wants: it needs only
 * fake_https.exe and the certificates of tests/tls. */
#include "unit.h"

int unit_fail = 0, unit_n = 0;

int main(void)
{
    printf("\nhttps\n");
    test_https();
    printf("  %d checks, %s\n", unit_n, unit_fail ? "at least one FAIL." : "all green.");
    return unit_fail ? 1 : 0;
}
