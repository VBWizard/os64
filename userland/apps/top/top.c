#include "topmain.h"

// The knobs (all four rulings are Chris's — see topmain.h):
static const char *delayValue = NULL;
static bool optZombies = false;
static bool optAdaptive = false;
static bool optNoSummary = false;

int main(int argc, char **argv)
{
    int32_t returnCode = 0;
    os64_args_t args = {0};
    const char *positional = NULL;
    static const os64_optspec_t specs[] = {
        {'d', "delay", true,  "Refresh delay in ms (default 1000, min 100)",
         .value_out = &delayValue},
        {'z', "zombies", false, "Show zombie tasks (hidden by default)",
         .flag = &optZombies},
        {'a', "adaptive", false, "Adaptive TIME units (us/ms/s) instead of X.Ys",
         .flag = &optAdaptive},
        {'s', "nosummary", false, "Hide the cores/idle/system summary lines",
         .flag = &optNoSummary},
    };

    os64_args_init(&args, argc, argv, specs, 4);
    args.about = "View the system's tasks and where the CPU time goes";
    int32_t nPositionals = os64_args_parse(&args, "top", &positional, 1);
    if (nPositionals == 0)
    {
        top_options_t opts = {0};
        opts.delayMS = delayValue ? os64_atoi(delayValue) : 1000;
        if (opts.delayMS < 100)
            opts.delayMS = 100;      // floor: the console deserves mercy
        if (opts.delayMS > 60000)
            opts.delayMS = 60000;
        opts.showZombies = optZombies;
        opts.adaptiveUnits = optAdaptive;
        opts.noSummary = optNoSummary;
        returnCode = topMain(&opts);
    }
    else if (nPositionals > 0)
    {
        os64_args_help(&args, "top"); // parse stayed silent for this case
        returnCode = 1;
    }
    else if (nPositionals == OS64_ARG_ERROR)
        returnCode = 1;

    return returnCode;
}
