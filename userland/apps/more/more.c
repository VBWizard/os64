#include "os64/os64.h"
#include "../../common/file_pager.h"

#define DEFAULT_PAGE_LINES 24

int main(int argc, char **argv)
{
    const char **paths = os64_malloc((size_t)argc * sizeof(*paths));
    if (paths == NULL)
    {
        os64_hprintf(OS64_STDERR, "more: out of memory\n");
        return 2;
    }
    const char *interval = NULL;
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'i', "interval", true, "lines to display between prompts",
         .value_out = &interval}
    };
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Page through text files or standard input.";
    args.details = "Space or Page Down advances a page, Enter advances one line, h shows help, and q quits.";

    int32_t parsed = os64_args_parse(&args, "more [-i LINES] [FILE ...]",
                                     paths, argc);
    if (parsed == OS64_ARG_HELP)
    {
        os64_free(paths);
        return 0;
    }
    if (parsed < 0)
    {
        os64_free(paths);
        return 2;
    }

    uint32_t lines = file_pager_default_lines(DEFAULT_PAGE_LINES);
    if (interval != NULL && !file_pager_parse_lines(interval, &lines))
    {
        os64_hprintf(OS64_STDERR, "more: invalid interval: %s\n", interval);
        os64_free(paths);
        return 2;
    }
    int32_t returnCode = 0;
    int32_t inputs = parsed == 0 ? 1 : parsed;
    for (int32_t i = 0; i < inputs; i++)
    {
        file_pager_options_t options = {
            "more", parsed == 0 ? NULL : paths[i], lines, FILE_PAGER_MORE
        };
        file_pager_result_t result = file_pager_run(&options);
        if (result == FILE_PAGER_RESULT_ERROR)
            returnCode = 1;
        else if (result == FILE_PAGER_RESULT_QUIT)
            break;
    }
    os64_free(paths);
    return returnCode;
}
