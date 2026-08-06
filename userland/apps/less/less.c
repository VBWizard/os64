#include "os64/os64.h"
#include "../../common/file_pager.h"

#define DEFAULT_PAGE_LINES 24

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *interval = NULL;
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'i', "interval", true, "lines to display per page",
         .value_out = &interval}
    };
    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Browse a named text file forward and backward.";
    args.details = "Space advances, b goes back, j/k move by a line, g/G jump, and q quits. Standard input paging awaits controlling TTY support.";

    int32_t parsed = os64_args_parse(&args, "less [-i LINES] FILE", &path, 1);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed != 1)
    {
        if (parsed == 0)
            os64_hprintf(OS64_STDERR, "less: standard input paging requires TTY support; pass a file\n");
        return 2;
    }

    uint32_t lines = DEFAULT_PAGE_LINES;
    if (interval != NULL && !file_pager_parse_lines(interval, &lines))
    {
        os64_hprintf(OS64_STDERR, "less: invalid interval: %s\n", interval);
        return 2;
    }
    file_pager_options_t options = {"less", path, lines, FILE_PAGER_LESS};
    return file_pager_run(&options);
}
