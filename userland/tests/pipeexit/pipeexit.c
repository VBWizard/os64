// Repeated default-SIGPIPE deaths; inspect live pipes before/after in QEMU.
// Exit status checks run in the suite; the host syscall test checks unpin order.
#include "os64/os64.h"

#define PASS 0x91E00000u
static char payload[8192];

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1)
    {
        int32_t fds[2];
        if (os64_pipe(fds) != 0)
            return PASS | 1;
        os64_close(fds[0]);
        os64_write(fds[1], payload, sizeof(payload));
        return PASS | 2;   // default SIGPIPE must not return
    }
    for (unsigned i = 0; i < 32; i++)
    {
        int64_t child = os64_spawn("/tests/pipeexit",
                                  (char *[]){"/tests/pipeexit", "child", NULL});
        int32_t status = -1;
        if (child < 0 || os64_wait(child, &status) != child || status != 141)
        {
            os64_printf("pipeexit: FAIL child %u status %d\n", i, status);
            return PASS | 3;
        }
    }
    os64_printf("pipeexit: PASS 32 default-SIGPIPE exits with status 141\n");
    return PASS;
}
