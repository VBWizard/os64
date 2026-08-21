#include "os64/os64.h"
#include "../../common/file_pager.h"

#define DEFAULT_PAGE_LINES 24
#define LESS_MAX_FILES 512

int main(int argc, char **argv)
{
    const char *paths[LESS_MAX_FILES] = {0};
    const char *interval = NULL;
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'i', "interval", true, "lines to display per page",
         .value_out = &interval}
    };
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Browse text files or standard input forward and backward.";
    args.details = "Use :n and :p to change files. Press h for the complete key list.";

    int32_t parsed = os64_args_parse(&args, "less [-i LINES] [FILE ...]",
                                     paths, LESS_MAX_FILES);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 0)
        return 2;

    uint32_t lines = file_pager_default_lines(DEFAULT_PAGE_LINES);
    if (interval != NULL && !file_pager_parse_lines(interval, &lines))
    {
        os64_hprintf(OS64_STDERR, "less: invalid interval: %s\n", interval);
        return 2;
    }
    int32_t returnCode = 0;
    int32_t current = 0;
    int32_t inputs = parsed == 0 ? 1 : parsed;
    while (current >= 0 && current < inputs)
    {
        file_pager_options_t options = {
            "less", parsed == 0 ? NULL : paths[current], lines, FILE_PAGER_LESS
        };
        file_pager_result_t result = file_pager_run(&options);
        if (result == FILE_PAGER_RESULT_QUIT)
            break;
        if (result == FILE_PAGER_RESULT_PREVIOUS)
        {
            if (current > 0)
                current--;
            continue;
        }
        if (result == FILE_PAGER_RESULT_ERROR)
            returnCode = 1;
        current++;
    }
    return returnCode;
}
