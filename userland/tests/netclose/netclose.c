// Queue a patterned stream and close immediately. The host must verify bytes
// through EOF; a successful exit here proves only that the writes queued.
#include "os64/os64.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 1;
    char target[128];
    os64_snprintf(target, sizeof(target), "tcp!%s!%s", argv[1], argv[2]);
    int64_t conn = os64_dial(target);
    if (conn < 0) return 2;
    static uint8_t bytes[32768];
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i * 37u + 11u);
    size_t off = 0;
    while (off < sizeof(bytes))
    {
        int64_t n = os64_write((int32_t)conn, bytes + off, sizeof(bytes) - off);
        if (n <= 0) { os64_close((int32_t)conn); return 3; }
        off += (size_t)n;
    }
    os64_close((int32_t)conn);
    os64_printf("netclose: 32768 bytes queued; handle closed\n");
    return 0;
}
