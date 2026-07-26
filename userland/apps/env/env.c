#include "os64/os64.h"

int main(int argc, char **argv)
{
    int32_t returnCode = 0;
    os64_args_t args = {0};
    const char *positional = NULL;
    os64_envent_t envEntry = {0};
    int32_t retVal;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Print the environment variable key=value pairs.\nTakes no parameters";
    int32_t nPositionals = os64_args_parse(&args, "env", &positional, 1);
    if (nPositionals == 0)
    {
        //Do the work here
        while (!(retVal=os64_env_next(&envEntry)))
        {
            os64_printf("%s=%s\n", envEntry.key, envEntry.value);
        }
        if (retVal != 1)
            returnCode = 2;
    }
    else if (nPositionals > 0)
    {
        os64_args_help(&args, "env");
        returnCode = 1;
    }
    else
        returnCode = (nPositionals == OS64_ARG_HELP) ? 0 : 1;

    return returnCode;
}
