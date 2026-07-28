#include "os64/os64.h"
bool printHuman = false;

int main(int argc, char **argv)
{
    int32_t returnCode = 0;
    os64_args_t args = {0};
    const char *positional = NULL;
    static const os64_optspec_t specs[] = {
        {'h', "human", false, "Print in human readable MB", .flag = &printHuman}};
    os64_memory_t freeInfo = {0};
    uint64_t checkValue;

    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Shows the system's free memory in bytes or megabytes";
    int32_t nPositionals = os64_args_parse(&args, "free", &positional, 1);
    if (nPositionals == 0)
    {
        if (os64_memory(&freeInfo))
        {
            returnCode = 2;
            os64_printf("Fable dropped the ball with SYSCALL_MEMORY so 'NO FREE FOR YOU!'\n");
        }
        else if (printHuman)
            os64_printf("Free: %-15s%-15s%-15s%-15s%-15s\n      %-15lu%-15lu%-15lu%-15lu%-15lu\n",
                        "Total MB", "Usable MB", "Used MB", "Free MB", "Avail MB",
                        freeInfo.total / 1024 / 1024, freeInfo.usable / 1024 / 1024, freeInfo.used / 1024 / 1024, freeInfo.free / 1024 / 1024, freeInfo.available / 1024 / 1024);
        else
            os64_printf("Free: %-15s%-15s%-15s%-15s%-15s\n      %-15lu%-15lu%-15lu%-15lu%-15lu\n",
                        "Total Bytes", "Usable Bytes", "Used Bytes", "Free Bytes", "Avail Bytes",
                        freeInfo.total, freeInfo.usable, freeInfo.used, freeInfo.free, freeInfo.available);
        if (returnCode == 0)
        {
            checkValue = freeInfo.free + freeInfo.used;
            if (checkValue != freeInfo.usable)
                os64_printf("The ledger does *not* balance! Free + used (%lu) does not equal usable (%lu). Difference is %ld\n", 
                    checkValue, freeInfo.usable, checkValue - freeInfo.usable);
        }
                
    }
    else if (nPositionals > 0)
    {
        os64_args_help(&args, "free"); // parse stayed silent for this case
        returnCode = 1;
    }
    else if (nPositionals == OS64_ARG_ERROR)
        returnCode = 1;
        
    return returnCode;
}
