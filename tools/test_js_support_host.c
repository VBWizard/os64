#include <stdio.h>
#include "../userland/tests/jssupporttest/cases.h"

int main(void)
{
    int failures = js_support_string_cases();
    printf("JavaScript support strings: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
