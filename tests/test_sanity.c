// Sanity test for the unit-test harness.
// Convention: each test is a standalone program with its own main(),
// uses assert(), and prints "OK: <name>" on success.
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(1 + 1 == 2);
    printf("OK: sanity\n");
    return 0;
}
