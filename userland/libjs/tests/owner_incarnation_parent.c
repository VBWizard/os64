#include <os64/os64.h>
int main(void)
{
    char *argv[]={"/tests/ownerincarnationtest",NULL};
    int64_t child=os64_spawn(argv[0],argv); int32_t status=-1;
    int64_t waited=child<0?child:os64_wait(child,&status);
    bool pass=waited>=0 && (uint32_t)status==UINT32_C(0x4f490200);
    char line[160];
    os64_snprintf(line,sizeof line,"OWNER INCARNATION child: wait=%ld exit=0x%08x expected=0x4f490200 %s\n",
        (long)waited,(unsigned)status,pass?"PASS":"FAIL");
    os64_serial_log(line);
    os64_serial_log(pass?"OWNER INCARNATION QEMU COMPLETE PASS\n":"OWNER INCARNATION QEMU COMPLETE FAIL\n");
    return pass?0:1;
}
