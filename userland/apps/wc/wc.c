// wc.c — count lines, words, and bytes in files or standard input.

#include "os64/os64.h"

#define WC_BUF_SIZE (1024 * 1024)
#define WC_MAX_FILES 128

typedef struct {
    uint64_t lines;
    uint64_t words;
    uint64_t bytes;
} wc_counts_t;

typedef struct {
    uint64_t readCalls;
    uint64_t readTicks;
    uint64_t largestRead;
    os64_ticks_t started;
    os64_ticks_t finished;
} wc_diagnostics_t;

// Large enough to take advantage of the kernel's current large-read
// experiment, and static so it does not consume a thread's user stack.
static char buf[WC_BUF_SIZE];

static int count_handle(int32_t handle, wc_counts_t *counts, bool countLines,
                        bool countWords, bool countBytes,
                        wc_diagnostics_t *diagnostics)
{
    bool inWord = false;
    int64_t n;

    if (diagnostics != NULL)
        os64_ticks(&diagnostics->started);

    for (;;)
    {
        os64_ticks_t readStarted = {0}, readFinished = {0};
        if (diagnostics != NULL)
            os64_ticks(&readStarted);

        n = os64_read(handle, buf, sizeof(buf));

        if (diagnostics != NULL)
        {
            os64_ticks(&readFinished);
            diagnostics->readCalls++;
            diagnostics->readTicks += readFinished.ticks - readStarted.ticks;
            if (n > 0 && (uint64_t)n > diagnostics->largestRead)
                diagnostics->largestRead = (uint64_t)n;
        }

        if (n <= 0)
            break;

        if (countBytes)
            counts->bytes += (uint64_t)n;

        if (!countLines && !countWords)
            continue;

        for (int64_t i = 0; i < n; i++)
        {
            char c = buf[i];

            if (countLines && c == '\n')
                counts->lines++;

            if (countWords)
            {
                bool space = c == ' ' || c == '\t' || c == '\n' ||
                             c == '\r' || c == '\v' || c == '\f';
                if (space)
                    inWord = false;
                else if (!inWord)
                {
                    counts->words++;
                    inWord = true;
                }
            }
        }
    }

    if (diagnostics != NULL)
        os64_ticks(&diagnostics->finished);

    return n < 0 ? -1 : 0;
}

static void print_diagnostics(const wc_diagnostics_t *diagnostics,
                              const char *name)
{
    uint64_t perSecond = diagnostics->finished.per_second;
    uint64_t totalTicks = diagnostics->finished.ticks - diagnostics->started.ticks;
    uint64_t totalMS = perSecond ? totalTicks * 1000 / perSecond : 0;
    uint64_t readMS = perSecond ? diagnostics->readTicks * 1000 / perSecond : 0;

    os64_hprintf(OS64_STDERR,
                 "wc: diagnostic %s: %lu read calls, requested %lu bytes, "
                 "largest return %lu bytes, read %lums, total %lums\n",
                 name == NULL ? "standard input" : name,
                 diagnostics->readCalls, (uint64_t)WC_BUF_SIZE,
                 diagnostics->largestRead, readMS, totalMS);
}

static void print_counts(const wc_counts_t *counts, bool showLines,
                         bool showWords, bool showBytes, const char *name)
{
    if (showLines)
        os64_printf("%10lu", counts->lines);
    if (showWords)
        os64_printf("%10lu", counts->words);
    if (showBytes)
        os64_printf("%10lu", counts->bytes);
    if (name != NULL)
        os64_printf(" %s", name);
    os64_printf("\n");
}

int main(int argc, char **argv)
{
    bool showLines = false;
    bool showWords = false;
    bool showBytes = false;
    bool showDiagnostics = false;
    const char *files[WC_MAX_FILES];
    int32_t fileCount = 0;
    int32_t returnCode = 0;
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'l', "lines", false, "print the newline counts", .flag = &showLines},
        {'w', "words", false, "print the word counts", .flag = &showWords},
        {'c', "bytes", false, "print the byte counts", .flag = &showBytes},
        {'D', "diagnostic", false, "report read timing to standard error",
          .flag = &showDiagnostics}
    };

    os64_args_init(&args, argc, argv, specs, 4);
    args.about = "Print newline, word, and byte counts for each FILE.";
    args.details = "With no FILE, or when FILE is -, read standard input.";

    int32_t parsed = os64_args_parse(&args, "wc [-lwcD] [FILE ...]",
                                     files, WC_MAX_FILES);
    if (parsed == OS64_ARG_HELP)
        os64_exit(0);
    if (parsed < 0)
        os64_exit(2);
    fileCount = parsed;

    if (!showLines && !showWords && !showBytes)
        showLines = showWords = showBytes = true;

    wc_counts_t total = {0};
    int32_t inputs = fileCount == 0 ? 1 : fileCount;

    for (int32_t i = 0; i < inputs; i++)
    {
        const char *name = fileCount == 0 ? NULL : files[i];
        int32_t handle = OS64_STDIN;
        wc_counts_t counts = {0};
        wc_diagnostics_t diagnostics = {0};

        if (name != NULL && !os64_streq(name, "-"))
        {
            handle = (int32_t)os64_open(name, "r");
            if (handle < 0)
            {
                os64_hprintf(OS64_STDERR, "wc: cannot open %s\n", name);
                returnCode = 1;
                continue;
            }
        }

        if (count_handle(handle, &counts, showLines, showWords, showBytes,
                         showDiagnostics ? &diagnostics : NULL) < 0)
        {
            os64_hprintf(OS64_STDERR, "wc: error reading %s\n",
                         name == NULL ? "standard input" : name);
            returnCode = 1;
        }
        else
        {
            print_counts(&counts, showLines, showWords, showBytes, name);
            if (showDiagnostics)
                print_diagnostics(&diagnostics, name);
            total.lines += counts.lines;
            total.words += counts.words;
            total.bytes += counts.bytes;
        }

        if (handle != OS64_STDIN)
            os64_close(handle);
    }

    if (fileCount > 1)
        print_counts(&total, showLines, showWords, showBytes, "total");

    os64_exit(returnCode);
}
