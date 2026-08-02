// hog.c — burn a core, on purpose, as a measuring instrument.
//
// A hog's value is that its true CPU appetite is KNOWN BY CONSTRUCTION:
// ~100% of one core, minus scheduling slices. That makes it the calibration
// standard for the accounting: whatever top says a hog costs, the
// difference from ~100% is the measurement's error, not the hog's. (Born
// 2026-07-29 to referee the VBox missing-CPU mystery: if a hog reads ~55%,
// the TSC→µs ledger and the tick-fed interval clock are disagreeing by
// exactly the visible shortfall — two clocks, one liar, now identifiable.)
//
// hog [seconds] [-n hogcount]  — spin for that many wall(ish) seconds, then exit.
// hog            — spin forever; Ctrl+C kills it (the forced-syscall
//                  redirect exists precisely so a zero-syscall spinner
//                  can't shrug off SIGINT), or from another prompt:
//                  echo kill > /proc/<id>/ctl
//
// The spin checks the clock only every ~million iterations — a syscall
// cadence measured in hertz, so the burn stays >99.99% pure userland
// cycles and the accounting sees an honest, uncontaminated flame. 🔥

#include "os64/os64.h"

#define MAX_THREADS 100
#define HOG_ERROR_TOO_MANY_THREADS_REQUESTED 1;
#define HOG_THREAD_NO_START 2

static volatile int64_t shared_slots[MAX_THREADS];

/// @brief The worker for hog, burns CPU cycles
/// @param arg - int64_t - seconds to burn
/// @return int furnace count
int64_t burn(void *arg)
{

    int64_t seconds = (int64_t)arg;
    // The flame. volatile so the whole point doesn't optimize away.
    volatile uint64_t furnace = 0;
    os64_ticks_t start = {0}, now = {0};
    os64_ticks(&start);
    uint64_t endTick = 0;
    if (seconds > 0 && start.per_second > 0)
        endTick = start.ticks + (uint64_t)seconds * start.per_second;

    while (1 == 1)
    {
        for (uint64_t i = 0; i < (1u << 20); i++)
            furnace += i;

        if (endTick != 0)
        {
            os64_ticks(&now);
            if (now.ticks >= endTick)
                break;
        }
    }
    // uint64_t burnedTicks = now.ticks - start.ticks;
    return endTick - start.ticks;
}

int main(int argc, char **argv)
{
    int32_t returnCode = 0;
    os64_args_t args = {0};
    const char *positional = NULL;
    const char *paramNumOfThreads = NULL;
    int64_t threadCount = 0;
    int64_t handles[MAX_THREADS];
    const os64_optspec_t specs[] = {
        {'n', "number", true, "the number of hog threads to run", .value_out = &paramNumOfThreads}};

    os64_args_init(&args, argc, argv, specs, 1);
    args.about = "Burn one core on purpose (accounting's calibration flame)";
    int32_t nPositionals = os64_args_parse(&args, "hog [seconds] [-n thread count]", &positional, 1);
    int64_t totalTicks = 0;

    if (paramNumOfThreads)
    {
        threadCount = os64_atoi(paramNumOfThreads);
        if (threadCount > MAX_THREADS)
        {
            char errorMsg[256] = {0};
            os64_snprintf(errorMsg, 256, "Cannot run %d threads, exiting\n");
            os64_write(OS64_STDERR, errorMsg, 256);
            return HOG_ERROR_TOO_MANY_THREADS_REQUESTED;
        }
    }
    else
        threadCount = 1;
        
    if (nPositionals < 0)
        return (nPositionals == OS64_ARG_HELP) ? 0 : 1;

    int64_t seconds = 0;   // 0 = forever
    if (nPositionals == 1)
    {
        seconds = os64_atoi(positional);
        if (seconds <= 0)
        {
            os64_hprintf(OS64_STDERR, "hog: seconds must be positive\n");
            return 1;
        }
    }

    for (int64_t i = 0; i < threadCount; i++)
    {
        handles[i] = os64_thread(burn, (void *)seconds);
        if (handles[i] < 0)
        {
            os64_printf("threadtest: could not start thread %ld\n", i);
            return HOG_THREAD_NO_START;
        }
    }

    for (int64_t i = 0; i < threadCount; i++)
    {
        int64_t threadSeconds = 0;
        os64_thread_join((int32_t)handles[i], &threadSeconds);
        totalTicks += threadSeconds;
    }
        /*    os64_printf("hog: burned %lu.%lus by the tick clock (furnace reads %lu)\n",
                        burnedTicks / (start.per_second ? start.per_second : 1),
                        (burnedTicks % (start.per_second ? start.per_second : 1)) / 10,
                        (uint64_t)furnace);
        */
        os64_printf("Summary: %d threads ran for a total of %d ticks\n", threadCount, totalTicks);
        return returnCode;
    }
