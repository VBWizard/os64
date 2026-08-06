#include "os64/os64.h"
#include "../../common/file_pager.h"

#define DEFAULT_PAGE_LINES 24

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *interval = NULL;
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'i', "interval", true, "lines to display between prompts",
         .value_out = &interval}
    };
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Page through a named text file.";
    args.details = "Space advances a page, Enter advances one line, and q quits. Standard input paging awaits controlling TTY support.";

    int32_t parsed = os64_args_parse(&args, "more [-i LINES] FILE", &path, 1);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed != 1)
    {
        if (parsed == 0)
            os64_hprintf(OS64_STDERR, "more: standard input paging requires TTY support; pass a file\n");
        return 2;
    }

    uint32_t lines = DEFAULT_PAGE_LINES;
    if (interval != NULL && !file_pager_parse_lines(interval, &lines))
    {
        os64_hprintf(OS64_STDERR, "more: invalid interval: %s\n", interval);
        return 2;
    }
    file_pager_options_t options = {"more", path, lines, FILE_PAGER_MORE};
    return file_pager_run(&options);
}
