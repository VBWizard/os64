#include "os64/os64.h"

int main(int argc, char **argv)
{
    int32_t returnCode = 0;
    os64_args_t args = {0};
    const char *positional = NULL;
    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Display a clock in the upper right corner of the console (configurable later)\n";
    int32_t nPositionals = os64_args_parse(&args, "clock", &positional, 1);
    os64_date_t now;
    char displayedDate[40];

    if (nPositionals == 0)
    {
        while (1==1) //CTRL+C is a fine interface for communicating with the clock
        {
            os64_date_now(&now,NULL);
            os64_memset(displayedDate, 0, 40);
            // Do the work here
            os64_snprintf(displayedDate, 40, "%02d/%02d/%04d %02d:%02d:%02d",
                        now.month, now.day, now.year, now.hour, now.minute, now.second);
            // The SCREEN layer, on purpose (io.h's doctrine): one clock on
            // the machine's glass serves every text VT, and it deliberately
            // does not follow sessions into pty windows — this is the iron's
            // clock; the desktop's will be gclock.
            os64_screen_printat(95, 0, displayedDate);
            os64_sleep(100); //Might make this a parameter some  day.
        }
    }
    else if (nPositionals > 0)
    {
        os64_args_help(&args, "clock"); // parse stayed silent for this case
        returnCode = 1;
    }
    else if (nPositionals == OS64_ARG_ERROR)
        returnCode = 1;

    return returnCode;
}
