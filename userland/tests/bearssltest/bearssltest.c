#include "os64/io.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "../../libtls/test/foundation.h"

static void report(const char *line)
{
    os64_printf("bearssltest: %s\n", line);
    os64_serial_log(line);
}

int main(int argc, char **argv)
{
    if (argc == 2 && os64_streq(argv[1], "--license")) {
        os64_printf("%s", bearssl_test_license());
        return 0;
    }
    bearssl_test_workspace *w = os64_malloc(sizeof *w);
    if (!w) { report("FAIL allocating fixture workspace"); return 1; }
    int result = bearssl_foundation_test(w, 1, report);
    os64_free(w);
    if (result) return result;
    report("PASS vectors, differential transcript, and explicit entropy boundary");
    return 0xBEA20000;
}
