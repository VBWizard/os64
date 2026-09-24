#include "os64/os64.h"

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    const char *positional = NULL;
    int32_t returnCode = 0;

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Clear the console.";
    int32_t nPositionals = os64_args_parse(&args, "clear", &positional, 1);
    if (nPositionals == 0)
    {
        // Home the cursor, then erase the screen — the ANSI clear every
        // terminal honors (ncurses' `clear`/`tput clear` emit exactly this).
        // A form feed cleared os64's OWN renderer and nothing else: over
        // telnet the bytes reach a real VT100/xterm, which treats 0x0C as
        // nothing. ESC[2J homes on os64 too, so the ESC[H is redundant here
        // and load-bearing there (a standard terminal does not move the
        // cursor on 2J).
        os64_printf("\033[H\033[2J");
    }
    else if (nPositionals > 0)
    {
        os64_args_help(&args, "clear");
        returnCode = 1;
    }
    else
        returnCode = (nPositionals == OS64_ARG_HELP) ? 0 : 1;
    return returnCode;
}
