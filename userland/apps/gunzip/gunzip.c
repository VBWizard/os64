// gunzip — stream a gzip file to standard output.
//
// This first consumer deliberately has no replace-the-input mode. Publishing
// a decoded file safely needs the temporary-file-and-rename contract used by
// tar and os64get; stdout already has honest failure semantics and composes
// immediately with tar, the browser's fetch path, and ordinary pipelines.

#include "os64/os64.h"
#include "gzip/gzip.h"

#define GUNZIP_IO_SIZE (64u * 1024u)

static uint8_t gInput[GUNZIP_IO_SIZE];
static uint8_t gOutput[GUNZIP_IO_SIZE];

static int write_all(const uint8_t *bytes, size_t length)
{
    size_t written = 0;
    while (written < length) {
        int64_t result = os64_write(OS64_STDOUT, bytes + written,
                                    length - written);
        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

static int decode_handle(int32_t handle, const char *name)
{
    os64_gzip_t *stream = os64_gzip_create(UINT64_MAX);
    if (stream == NULL) {
        os64_hprintf(OS64_STDERR, "gunzip: out of memory\n");
        return -1;
    }

    for (;;) {
        int64_t read_result = os64_read(handle, gInput, sizeof(gInput));
        if (read_result < 0) {
            os64_hprintf(OS64_STDERR, "gunzip: error reading %s\n", name);
            os64_gzip_destroy(stream);
            return -1;
        }

        bool end_of_input = read_result == 0;
        const uint8_t *input = gInput;
        size_t input_left = (size_t)read_result;

        for (;;) {
            uint8_t *output = gOutput;
            size_t output_left = sizeof(gOutput);
            os64_gzip_status_t status = os64_gzip_process(
                stream, &input, &input_left, &output, &output_left,
                end_of_input);
            size_t produced = sizeof(gOutput) - output_left;
            if (produced != 0 && write_all(gOutput, produced) < 0) {
                os64_hprintf(OS64_STDERR,
                             "gunzip: error writing standard output\n");
                os64_gzip_destroy(stream);
                return -1;
            }

            if (status == OS64_GZIP_NEED_OUTPUT)
                continue;
            if (status == OS64_GZIP_NEED_INPUT) {
                // NEED_INPUT consumes the offered chunk into the decoder's
                // bit reservoir. Bytes left here would make the caller spin
                // forever on the same input, so treat that as a library
                // contract failure rather than hiding it.
                if (input_left != 0) {
                    os64_hprintf(OS64_STDERR,
                                 "gunzip: decoder left input behind\n");
                    os64_gzip_destroy(stream);
                    return -1;
                }
                break;
            }
            if (status == OS64_GZIP_DONE) {
                os64_gzip_destroy(stream);
                return 0;
            }

            os64_hprintf(OS64_STDERR, "gunzip: %s: %s\n", name,
                         os64_gzip_status_name(status));
            os64_gzip_destroy(stream);
            return -1;
        }
    }
}

static int decode_path(const char *path)
{
    if (os64_streq(path, "-"))
        return decode_handle(OS64_STDIN, "standard input");

    os64_dirent_t entry = {0};
    if (os64_stat(path, &entry) < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot stat '%s'\n", path);
        return -1;
    }
    if ((entry.flags & OS64_DE_DIR) != 0) {
        os64_hprintf(OS64_STDERR, "gunzip: '%s' is a directory\n", path);
        return -1;
    }

    int32_t handle = (int32_t)os64_open(path, "r");
    if (handle < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot open '%s'\n", path);
        return -1;
    }
    int result = decode_handle(handle, path);
    if (os64_close(handle) < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot close '%s'\n", path);
        result = -1;
    }
    return result;
}

int main(int argc, char **argv)
{
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Decompress gzip data to standard output.";
    args.details = "With no FILE, or when FILE is -, read standard input. "
                   "Each FILE is decoded in order; input files are never "
                   "replaced.";

    int32_t input_count = 0;
    int32_t result;
    while ((result = os64_args_next(&args)) != OS64_ARG_END) {
        if (result == OS64_ARG_POSITIONAL) {
            input_count++;
            continue;
        }
        if (result == OS64_ARG_HELP) {
            os64_args_help(&args, "gunzip [FILE ...]");
            return 0;
        }
        os64_args_help(&args, "gunzip [FILE ...]");
        return 2;
    }

    if (input_count == 0)
        return decode_handle(OS64_STDIN, "standard input") == 0 ? 0 : 1;

    int return_code = 0;
    os64_args_init(&args, argc, argv, NULL, 0);
    while ((result = os64_args_next(&args)) != OS64_ARG_END)
        if (result == OS64_ARG_POSITIONAL && decode_path(args.value) < 0)
            return_code = 1;
    return return_code;
}
