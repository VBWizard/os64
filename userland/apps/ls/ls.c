#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    int longMode = 0, retVal = 0, entryCount = 0, returnCode = 0;
    long dirHandle = 0;
    char pwdPathToList[512] = {0};
    const char *pathToListp = pwdPathToList;
    os64_dirent_t dirEntry = {0};

    static const os64_optspec_t specs[] = {
        {'l', "long", 0, "one entry per line with sizes"}};

    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "List a directory or file";

    while (!returnCode && (retVal = os64_args_next(&args)) != OS64_ARG_END)
    {
        switch (retVal)
        {
        case 'l':
            longMode = 1;
            break;
        case OS64_ARG_POSITIONAL:
            pathToListp = args.value;
            break; // <-- "/bin" arrives here
        case OS64_ARG_HELP:
            os64_args_help(&args, "ls [-l] [path]");
            returnCode = 2;
            break;
        default:
            os64_args_help(&args, "ls [-l] [path]");
            returnCode = 3;
            break;
        }
    }

    if (!returnCode)
    {
        // Get the cwd. This will be used if no path was passed to ls, since pathToListp won't be set to that parameter's address
        if (!pathToListp)
            os64_getcwd(pwdPathToList, 512);
        dirHandle = os64_opendir(pathToListp);
        if (dirHandle < 0)
        {
            os64_hprintf(2, "ls: cannot open %s\n", pathToListp);
            returnCode = 4;
        }
        else
        {
            while (os64_readdir(dirHandle, &dirEntry) == 1)
            {
                switch (longMode)
                {
                    case 0:
                        os64_printf("%-20s", dirEntry.name);
                        if (++entryCount % 5 == 0)
                        {
                            os64_printf("\n");
                        }
                        break;
                    case 1:
                    default:
                        os64_printf("%-40s%-10lu%u\n",dirEntry.name, dirEntry.size, dirEntry.flags);
                        break;
                }
            }
            if (!longMode && entryCount%5!=0)
                os64_printf("\n");
        }
    }
    if (dirHandle > 0)
        os64_close(dirHandle);
    os64_exit(returnCode);
}