#include <os64/os64.h>
int main(void)
{
    char *argv[] = {"/tests/recoverytest", NULL};
    int64_t child = os64_spawn(argv[0], argv);
    int32_t status = -1;
    int64_t waited = child < 0 ? child : os64_wait(child, &status);
    bool pass = waited >= 0 && (uint32_t)status == UINT32_C(0x446f0200);
    char line[160];
    os64_snprintf(line, sizeof(line), "F16 QEMU child: wait=%ld exit=0x%08x expected=0x446f0200 %s\n",
                  (long)waited, (unsigned)status, pass ? "PASS" : "FAIL");
    os64_serial_log(line);
    os64_serial_log(pass ? "F16 QEMU COMPLETE PASS\n" : "F16 QEMU COMPLETE FAIL\n");
    return pass ? 0 : 1;
}
