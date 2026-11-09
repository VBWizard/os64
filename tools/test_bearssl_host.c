#include <stdio.h>
#include <stdlib.h>
#include "../userland/libtls/test/foundation.h"

static void report(const char *line) { puts(line); }

int main(void)
{
    bearssl_test_workspace *w = malloc(sizeof *w);
    if (!w) return 1;
    int result = bearssl_foundation_test(w, BEARSSL_PORT_TEST, report);
    free(w);
    return result;
}
