#include "os64/os64.h"

int main(int argc, char **argv)
{

    os64_args_t args = {0};
    static const os64_optspec_t specs[] = {
        {}};
    int returnCode = 0, retVal = 0;
    const char* fileToCat = NULL;
    int fileHandle = 0;
    char buf[512] = {0};
    int readCount = 0;
    os64_dirent_t statEntry;

    os64_args_init(&args, argc, argv, specs, 0);
    args.about = "Concatenate file(s) to standard output.";
    args.details = "When no file is specified, reads from standard input.";
    while ((retVal = os64_args_next(&args)) != OS64_ARG_END)
    {
        switch (retVal)
        {
            case OS64_ARG_POSITIONAL:
                fileToCat = args.value;
                break;
            case OS64_ARG_HELP:
                os64_args_help(&args, "cat [FILE]");
                returnCode = 2;
                break;
            default:
                os64_args_help(&args, "cat [FILE]");
                returnCode = 3;
                break;
            }
    }

    //As long as the user didn't ask for help, and they did pass a positional argument (presumably a path/filename) continue
    if (!returnCode && fileToCat)
    {
        //Get a dirent_t for the passed positional argument to see if its a file or a directory.
        retVal = os64_stat(fileToCat, &statEntry);

        if (retVal != 0)
        {
            os64_hprintf(OS64_STDERR, "Error: Could not stat %s\n", fileToCat);
            returnCode = 4;
        }
        else
        {
            //stat worked, so make sure the passed argument was a filename
            if (statEntry.flags == 1)
            {
                returnCode = 5;
                os64_hprintf(OS64_STDERR, "Error: Cannot cat a directory\n");
            }

            //If the passed argument was a file, good to continue opening the file
            if (!returnCode)
            {
                fileHandle = os64_open(fileToCat, "r");
                if (fileHandle < 1)
                {
                    os64_hprintf(OS64_STDERR, "Unable to open %s, error was %lu\n", fileToCat, fileHandle);
                    returnCode = 6;
                }
            }
        }
    }
    else
        fileHandle = OS64_STDIN;

    if (!returnCode)
    {
        // If we got here, everything is kosher. Read the file and output it!
        while ((readCount = os64_read(fileHandle, buf, sizeof(buf))) > 0)
        {
            os64_printf("%s", buf);
            //NOTE: Need to memset the buffer here!
        }
        os64_printf("\n");
    }

    if (fileHandle > 3)
        os64_close(fileHandle);
    os64_exit(returnCode);
}