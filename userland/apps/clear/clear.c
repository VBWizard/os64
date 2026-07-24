#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *positional = NULL;
    int32_t returnCode = 0;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Clear the console.";
    int32_t nPositionals = os64_args_parse(&args, "clear", &positional, 1);
    if (nPositionals == 0)
    {
        os64_printf("\f");
    }
    else if (nPositionals > 0)
    {
        os64_args_help(&args, "clear");
        returnCode = 1;
    }
    else
        returnCode = (nPositionals == OS64_ARG_HELP) ? 0 : 1;
    return returnCode;
}
