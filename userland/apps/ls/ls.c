#include "os64/os64.h"

#define MAX_DIR_ENTRIES 512
char pwdPathToList[512] = {0};
const char *pathToListp = pwdPathToList;
bool longMode = false;
int retVal = 0, returnCode = 0;
os64_dirent_t dirEntries[MAX_DIR_ENTRIES] = {0};

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
        os64_printf("%-40s%-10lu%s\n", dirEntry->name, dirEntry->size, dirEntry->flags==1?"<dir>":"<file>");
        break;
    }
}

int32_t get_directory_listing(const char *path, os64_dirent_t *entries, int32_t *entryCount)
{
    int lReturnCode = 0;
    int64_t dirHandle = os64_opendir(path);

    if (dirHandle < 0)
    {
        os64_hprintf(2, "ls: cannot open %s\n", path);
        lReturnCode = 4;
    }
    else
    {
        while (os64_readdir(dirHandle, &entries[*entryCount]) == 1 &&
               ++*entryCount < MAX_DIR_ENTRIES);
    }

    if (dirHandle > 0)
        os64_close(dirHandle);

    return lReturnCode;
}

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    os64_dirent_t statEntry;
    int32_t entryCount = 0;
    const char *positional = NULL;
    // The spec table now says where results LAND (.flag), so the whole
    // parse_params() this file used to carry — flag case, positional case,
    // help/default — is one os64_args_parse call. First adopter of the
    // convenience it inspired.
    static const os64_optspec_t specs[] = {
        {'l', "long", 0, "one entry per line with sizes", .flag = &longMode}};

    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "List a directory or file";

    int32_t nPositionals = os64_args_parse(&args, "ls [-l] [path]", &positional, 1);
    if (nPositionals < 0)
        returnCode = (nPositionals == OS64_ARG_HELP) ? 1 : 2;
    else if (nPositionals == 1)
        pathToListp = positional; // <-- "/bin" arrives here

    if (!returnCode)
    {
        // Get the cwd. This will be used if no path was passed to ls, since pathToListp won't be set to that parameter's address
        // And yes I realize I don't "have to" do this but I am anyways. :-)
        // Moved ABOVE the stat (it used to sit after it, so plain `ls` stat'd
        // the empty string) — and the test is pathToListp[0]=='\0' spelled
        // carefully: the old `!pathToListp[0]=='\0'` parsed as
        // `(!pathToListp[0]) == 0` (! binds tighter than ==), the exact
        // inverse of what was meant.
        if (pathToListp[0] == '\0')
            os64_getcwd(pwdPathToList, 512);

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
            if ((returnCode = get_directory_listing(pathToListp, dirEntries, &entryCount)) == 0)
            {
                for (int cnt = 0; cnt < entryCount;cnt++)
                {
                    print_an_entry(&dirEntries[cnt], longMode);
                    // (cnt + 1), NOT ++entryCount: bumping the loop bound while
                    // iterating toward it means cnt can never catch up — the
                    // loop walks straight off the end of dirEntries.
                    if (!longMode && (cnt + 1) % 5 == 0)
                    {
                        os64_printf("\n");
                    }
                }
            }
            if (!longMode && entryCount % 5 != 0)
                os64_printf("\n");
        }
    }
    os64_exit(returnCode);
}