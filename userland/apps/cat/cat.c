#include "os64/os64.h"

#define CAT_BUFFER_SIZE 32768

static char buffer[CAT_BUFFER_SIZE];

static int write_all(const char *data, size_t length)
{
    size_t written = 0;
    while (written < length)
    {
        int64_t n = os64_write(OS64_STDOUT, data + written, length - written);
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

static int cat_handle(int32_t handle, const char *name)
{
    int64_t n;
    while ((n = os64_read(handle, buffer, sizeof(buffer))) > 0)
        if (write_all(buffer, (size_t)n) < 0)
        {
            os64_hprintf(OS64_STDERR, "cat: error writing standard output\n");
            return -1;
        }

    if (n < 0)
    {
        os64_hprintf(OS64_STDERR, "cat: error reading %s\n", name);
        return -1;
    }
    return 0;
}

static int cat_path(const char *path)
{
    if (os64_streq(path, "-"))
        return cat_handle(OS64_STDIN, "standard input");

    os64_dirent_t entry = {0};
    if (os64_stat(path, &entry) < 0)
    {
        os64_hprintf(OS64_STDERR, "cat: cannot stat '%s'\n", path);
        return -1;
    }
    if (entry.flags & OS64_DE_DIR)
    {
        os64_hprintf(OS64_STDERR, "cat: '%s' is a directory\n", path);
        return -1;
    }

    int32_t handle = (int32_t)os64_open(path, "r");
    if (handle < 0)
    {
        os64_hprintf(OS64_STDERR, "cat: cannot open '%s'\n", path);
        return -1;
    }

    int result = cat_handle(handle, path);
    if (os64_close(handle) < 0)
    {
        os64_hprintf(OS64_STDERR, "cat: cannot close '%s'\n", path);
        result = -1;
    }
    return result;
}

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Concatenate files to standard output.";
    args.details = "With no FILE, or when FILE is -, read standard input.";

    // Validate first so an unknown option after a valid file cannot produce
    // partial output before the command line is rejected.
    int32_t inputCount = 0;
    int32_t result;
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
    {
        if (result == OS64_ARG_POSITIONAL)
        {
            inputCount++;
            continue;
        }
        if (result == OS64_ARG_HELP)
        {
            os64_args_help(&args, "cat [FILE ...]");
            return 0;
        }
        os64_args_help(&args, "cat [FILE ...]");
        return 2;
    }

    if (inputCount == 0)
        return cat_handle(OS64_STDIN, "standard input") == 0 ? 0 : 1;

    int returnCode = 0;
    os64_args_init(&args, argc, argv, NULL, 0);
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
        if (result == OS64_ARG_POSITIONAL && cat_path(args.value) < 0)
            returnCode = 1;
    return returnCode;
}
