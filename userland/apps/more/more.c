#include "os64/os64.h"
#include "../../common/file_pager.h"

#define DEFAULT_PAGE_LINES 24
#define MORE_MAX_FILES 512

int main(int argc, char **argv)
{
    const char *paths[MORE_MAX_FILES] = {0};
    const char *interval = NULL;
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'i', "interval", true, "lines to display between prompts",
         .value_out = &interval}
    };
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Page through named text files.";
    args.details = "Space or Page Down advances a page, Enter advances one line, h shows help, and q quits. Standard input paging awaits controlling TTY support.";

    int32_t parsed = os64_args_parse(&args, "more [-i LINES] FILE ...",
                                     paths, MORE_MAX_FILES);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 1)
    {
        if (parsed == 0)
            os64_hprintf(OS64_STDERR, "more: standard input paging requires TTY support; pass a file\n");
        return 2;
    }

    uint32_t lines = file_pager_default_lines(DEFAULT_PAGE_LINES);
    if (interval != NULL && !file_pager_parse_lines(interval, &lines))
    {
        os64_hprintf(OS64_STDERR, "more: invalid interval: %s\n", interval);
        return 2;
    }
    int32_t returnCode = 0;
    for (int32_t i = 0; i < parsed; i++)
    {
        file_pager_options_t options = {
            "more", paths[i], lines, FILE_PAGER_MORE
        };
        file_pager_result_t result = file_pager_run(&options);
        if (result == FILE_PAGER_RESULT_ERROR)
            returnCode = 1;
        else if (result == FILE_PAGER_RESULT_QUIT)
            break;
    }
    return returnCode;
}
