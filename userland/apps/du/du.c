// du.c — summarize apparent file sizes beneath directory trees.

#include "os64/os64.h"

#define DU_PATH_MAX       256
#define DU_MAX_OPERANDS   512
// A task owns sixteen handles. Standard input/output/error plus twelve open
// directory ancestors leaves one honest spare instead of failing mysteriously.
#define DU_MAX_TREE_DEPTH 12

typedef struct {
    bool humanReadable;
    bool summarize;
    bool allFiles;
    bool grandTotal;
    uint64_t maxDepth;
} du_options_t;

typedef struct {
    uint64_t bytes;
    bool complete;
} du_result_t;

static bool parse_depth(const char *text, uint64_t *depth)
{
    if (text == NULL || *text == '\0')
        return false;

    uint64_t value = 0;
    for (const char *p = text; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }

    *depth = value;
    return true;
}

static int join_path(char *out, size_t capacity, const char *directory,
                     const char *name)
{
    size_t length = os64_strlen(directory);
    const char *separator = length > 0 && directory[length - 1] == '/' ? "" : "/";
    int32_t wanted = os64_snprintf(out, capacity, "%s%s%s",
                                   directory, separator, name);
    return wanted >= 0 && (size_t)wanted < capacity ? 0 : -1;
}

static void format_human_size(uint64_t bytes, char *out, size_t capacity)
{
    if (bytes < 1024)
    {
        os64_snprintf(out, capacity, "%luB", bytes);
        return;
    }

    static const char *units[] = {"K", "M", "G", "T", "P", "E"};
    uint64_t divisor = 1024;
    int32_t unit = 0;
    while (unit < 5 && bytes >= divisor * 1024)
    {
        divisor *= 1024;
        unit++;
    }

    uint64_t whole = bytes / divisor;
    uint64_t tenth = (bytes % divisor) * 10 / divisor;
    os64_snprintf(out, capacity, "%lu.%lu%s", whole, tenth, units[unit]);
}

static void print_usage(uint64_t bytes, const char *path,
                        const du_options_t *options)
{
    if (options->humanReadable)
    {
        char amount[32];
        format_human_size(bytes, amount, sizeof(amount));
        os64_printf("%s\t%s\n", amount, path);
        return;
    }

    // K is part of the answer, not a fact the reader should have to remember.
    // Below 10K retain one decimal (the useful distinction at that scale);
    // from 10K upward a whole rounded-up KiB is the cleaner column.
    if (bytes < 10 * 1024)
    {
        uint64_t tenths = (bytes * 10 + 512) / 1024;
        os64_printf("%lu.%luK\t%s\n", tenths / 10, tenths % 10, path);
    }
    else
    {
        uint64_t kibibytes = bytes / 1024 + (bytes % 1024 != 0);
        os64_printf("%luK\t%s\n", kibibytes, path);
    }
}

static bool add_bytes(uint64_t *total, uint64_t amount, const char *path)
{
    if (*total > UINT64_MAX - amount)
    {
        os64_hprintf(OS64_STDERR,
                     "du: size overflow while totaling '%s'\n", path);
        return false;
    }
    *total += amount;
    return true;
}

static du_result_t measure_path(const char *path, const os64_dirent_t *known,
                                uint32_t depth, bool commandOperand,
                                const du_options_t *options)
{
    os64_dirent_t probed = {0};
    const os64_dirent_t *entry = known;
    if (entry == NULL)
    {
        if (os64_stat(path, &probed) < 0)
        {
            os64_hprintf(OS64_STDERR, "du: cannot stat '%s'\n", path);
            return (du_result_t){0, false};
        }
        entry = &probed;
    }

    if ((entry->flags & OS64_DE_DIR) == 0)
    {
        if (commandOperand || (options->allFiles && depth <= options->maxDepth))
            print_usage(entry->size, path, options);
        return (du_result_t){entry->size, true};
    }

    if (depth >= DU_MAX_TREE_DEPTH)
    {
        os64_hprintf(OS64_STDERR,
                     "du: maximum directory depth reached at '%s'\n", path);
        return (du_result_t){0, false};
    }

    int32_t directory = (int32_t)os64_opendir(path);
    if (directory < 0)
    {
        os64_hprintf(OS64_STDERR, "du: cannot open directory '%s'\n", path);
        return (du_result_t){0, false};
    }

    du_result_t measured = {0, true};
    int64_t readResult;
    os64_dirent_t child = {0};
    while ((readResult = os64_readdir(directory, &child)) == 1)
    {
        if (os64_streq(child.name, ".") || os64_streq(child.name, ".."))
            continue;

        char childPath[DU_PATH_MAX];
        if (join_path(childPath, sizeof(childPath), path, child.name) < 0)
        {
            os64_hprintf(OS64_STDERR,
                         "du: path too long beneath '%s'\n", path);
            measured.complete = false;
            continue;
        }

        du_result_t childResult = measure_path(childPath, &child, depth + 1,
                                               false, options);
        if (!childResult.complete)
            measured.complete = false;
        if (!add_bytes(&measured.bytes, childResult.bytes, path))
            measured.complete = false;
    }

    if (readResult < 0)
    {
        os64_hprintf(OS64_STDERR, "du: cannot read directory '%s'\n", path);
        measured.complete = false;
    }
    if (os64_close(directory) < 0)
    {
        os64_hprintf(OS64_STDERR, "du: cannot close directory '%s'\n", path);
        measured.complete = false;
    }

    if (measured.complete && depth <= options->maxDepth)
        print_usage(measured.bytes, path, options);
    return measured;
}

int main(int argc, char **argv)
{
    du_options_t options = {.maxDepth = UINT64_MAX};
    os64_args_t args = {0};
    const char *operands[DU_MAX_OPERANDS] = {0};
    const char *depthText = NULL;
    const os64_optspec_t specs[] = {
        {'h', "human-readable", false, "print sizes in powers of 1024",
         .flag = &options.humanReadable},
        {'s', "summarize", false, "display only a total for each operand",
         .flag = &options.summarize},
        {'d', "max-depth", true, "display directory totals through depth N",
         .value_out = &depthText},
        {'a', "all", false, "write counts for files as well as directories",
         .flag = &options.allFiles},
        {'c', "total", false, "produce a grand total",
         .flag = &options.grandTotal}
    };

    os64_args_init(&args, argc, argv, specs, 5);
    args.about = "Estimate apparent file usage beneath directory trees.";
    args.details = "Allocated-block counts are not exposed yet; default sizes carry a K suffix.";

    int32_t operandCount = os64_args_parse(
        &args, "du [-ahsc] [-d N] [PATH ...]", operands, DU_MAX_OPERANDS);
    if (operandCount == OS64_ARG_HELP)
        return 0;
    if (operandCount < 0)
        return 2;

    if (depthText != NULL && !parse_depth(depthText, &options.maxDepth))
    {
        os64_hprintf(OS64_STDERR, "du: invalid maximum depth: %s\n", depthText);
        return 2;
    }
    if (options.summarize)
        options.maxDepth = 0;
    if (options.summarize && options.allFiles)
    {
        os64_hprintf(OS64_STDERR, "du: -a and -s cannot be combined\n");
        return 2;
    }

    if (operandCount == 0)
    {
        operands[0] = ".";
        operandCount = 1;
    }

    uint64_t grandTotal = 0;
    bool complete = true;
    for (int32_t i = 0; i < operandCount; i++)
    {
        du_result_t result = measure_path(operands[i], NULL, 0, true, &options);
        if (!result.complete)
            complete = false;
        if (!add_bytes(&grandTotal, result.bytes, operands[i]))
            complete = false;
    }

    if (options.grandTotal && complete)
        print_usage(grandTotal, "total", &options);
    return complete ? 0 : 1;
}
