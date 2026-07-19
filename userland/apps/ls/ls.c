#include "os64/os64.h"

/// @brief Print a directory or file entry
/// @param entryCount 
/// @param dirEntry The actual directory entry
/// @param longMode Indicates whether to print in long mode or not
void print_an_entry(os64_dirent_t *dirEntry, int longMode)
{
    switch (longMode)
    {
    case 0:
        os64_printf("%-20s", dirEntry->name);
        break;
    case 1:
    default:
        os64_printf("%-40s%-10lu%u\n", dirEntry->name, dirEntry->size, dirEntry->flags);
        break;
    }
}

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    int longMode = 0, retVal = 0, entryCount = 0, returnCode = 0;
    long dirHandle = 0;
    char pwdPathToList[512] = {0};
    const char *pathToListp = pwdPathToList;
    os64_dirent_t dirEntry = {0};
    os64_dirent_t statEntry;
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
            returnCode = 1;
            break;
        default:
            os64_args_help(&args, "ls [-l] [path]");
            returnCode = 2;
            break;
        }
    }

    if (!returnCode)
    {
        // Get a dirent_t for the passed positional argument to see if its a file or a directory.
        retVal = os64_stat(pathToListp, &statEntry);
        if (retVal != 0)
        {
            returnCode = 3;
            os64_hprintf(OS64_STDERR, "Error: Could not stat file '%s' (%u)\n", pathToListp, retVal);
        }
        if (!returnCode && statEntry.flags == 0)
        {
            //Argument passed is a file so just print it and we're done!
            print_an_entry(&statEntry, longMode);
            os64_printf("\n");
        }
        else if (!returnCode)
        {

            // Get the cwd. This will be used if no path was passed to ls, since pathToListp won't be set to that parameter's address
            // And yes I realize I don't "have to" do this but I am anyways. :-)
            if (!pathToListp[0]=='\0')
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
                    print_an_entry(&dirEntry, longMode);
                    if (!longMode && ++entryCount % 5 == 0)
                    {
                        os64_printf("\n");
                    }
                }
                if (!longMode && entryCount%5!=0)
                    os64_printf("\n");
            }
        }
    }
    if (dirHandle > 0)
        os64_close(dirHandle);
    os64_exit(returnCode);
}