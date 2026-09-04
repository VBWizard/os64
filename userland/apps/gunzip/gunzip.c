// gunzip — decompress streams and safely replace named .gz files.
//
// Decoder output is provisional until the gzip trailer has been verified.
// A named-file result therefore stays in an exclusively created sibling
// temporary file until it is complete, synced, and closed. Publication uses
// the same atomic destination policies as gzip: without -f an existing name
// is refused, while -f refuses replacement on filesystems that cannot do it
// atomically. The source is removed last, so damaged input and publication
// failures leave it intact. A final size/mtime check detects ordinary
// concurrent changes, but os64 has no file snapshot or lock spanning that
// check and removal.

#include "os64/os64.h"
#include "gzip/gzip.h"

#define GUNZIP_IO_SIZE      (64u * 1024u)
#define GUNZIP_PATH_MAX     512u
#define GUNZIP_MAX_OPERANDS 512

typedef struct {
    bool to_stdout;
    bool keep;
    bool force;
} gunzip_options_t;

static uint8_t gInput[GUNZIP_IO_SIZE];
static uint8_t gOutput[GUNZIP_IO_SIZE];
static uint32_t gTemporarySequence;

static int write_all(int32_t handle, const uint8_t *bytes, size_t length)
{
    size_t written = 0;
    while (written < length) {
        int64_t result = os64_write(handle, bytes + written,
                                    length - written);
        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

static int decode_handle(int32_t input_handle, int32_t output_handle,
                         const char *input_name, const char *output_name)
{
    os64_gzip_t *stream = os64_gzip_create(UINT64_MAX);
    if (stream == NULL) {
        os64_hprintf(OS64_STDERR, "gunzip: out of memory\n");
        return -1;
    }

    for (;;) {
        int64_t read_result = os64_read(input_handle, gInput, sizeof(gInput));
        if (read_result < 0) {
            os64_hprintf(OS64_STDERR, "gunzip: error reading %s\n",
                         input_name);
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
            if (produced != 0 &&
                write_all(output_handle, gOutput, produced) < 0) {
                os64_hprintf(OS64_STDERR, "gunzip: error writing %s\n",
                             output_name);
                os64_gzip_destroy(stream);
                return -1;
            }

            if (status == OS64_GZIP_NEED_OUTPUT)
                continue;
            if (status == OS64_GZIP_NEED_INPUT) {
                // NEED_INPUT consumes the offered chunk into the decoder's
                // bit reservoir. Bytes left here would make the caller spin
                // forever on the same input, so expose a contract failure.
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

            os64_hprintf(OS64_STDERR, "gunzip: %s: %s\n", input_name,
                         os64_gzip_status_name(status));
            os64_gzip_destroy(stream);
            return -1;
        }
    }
}

static bool has_gzip_suffix(const char *path)
{
    size_t length = os64_strlen(path);
    return length > 3 && path[length - 4] != '/' &&
           path[length - 3] == '.' && path[length - 2] == 'g' &&
           path[length - 1] == 'z';
}

static int destination_path(const char *source, char out[GUNZIP_PATH_MAX])
{
    size_t length = os64_strlen(source);
    if (!has_gzip_suffix(source) || length - 3 > INT32_MAX)
        return -1;
    int32_t wanted = os64_snprintf(out, GUNZIP_PATH_MAX, "%.*s",
                                   (int32_t)(length - 3), source);
    return wanted > 0 && wanted < (int32_t)GUNZIP_PATH_MAX ? 0 : -1;
}

static int32_t create_temporary(const char *target,
                                char out[GUNZIP_PATH_MAX])
{
    size_t slash = os64_strlen(target);
    while (slash > 0 && target[slash - 1] != '/')
        slash--;

    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        uint32_t sequence = gTemporarySequence++;
        int32_t wanted;
        if (slash == 0) {
            wanted = os64_snprintf(out, GUNZIP_PATH_MAX,
                                   ".gunzip-%lu-%u.part",
                                   os64_taskid(), sequence);
        } else if (slash == 1) {
            wanted = os64_snprintf(out, GUNZIP_PATH_MAX,
                                   "/.gunzip-%lu-%u.part",
                                   os64_taskid(), sequence);
        } else {
            wanted = os64_snprintf(out, GUNZIP_PATH_MAX,
                                   "%.*s/.gunzip-%lu-%u.part",
                                   (int32_t)(slash - 1), target,
                                   os64_taskid(), sequence);
        }
        if (wanted < 0 || wanted >= (int32_t)GUNZIP_PATH_MAX)
            return -1;

        int64_t handle = os64_open(out, "x");
        if (handle >= 0)
            return (int32_t)handle;

        os64_dirent_t existing = {0};
        if (os64_stat(out, &existing) < 0)
            return -1;
    }
    return -1;
}

static int decode_to_stdout(const char *path)
{
    if (os64_streq(path, "-"))
        return decode_handle(OS64_STDIN, OS64_STDOUT, "standard input",
                             "standard output");

    os64_dirent_t entry = {0};
    if (os64_stat(path, &entry) < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot stat '%s'\n", path);
        return -1;
    }
    if ((entry.flags & OS64_DE_DIR) != 0) {
        os64_hprintf(OS64_STDERR, "gunzip: '%s' is a directory\n", path);
        return -1;
    }

    int32_t input = (int32_t)os64_open(path, "r");
    if (input < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot open '%s'\n", path);
        return -1;
    }
    int result = decode_handle(input, OS64_STDOUT, path, "standard output");
    if (os64_close(input) < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot close '%s'\n", path);
        result = -1;
    }
    return result;
}

static int decode_path(const char *path, const gunzip_options_t *options)
{
    if (!has_gzip_suffix(path)) {
        os64_hprintf(OS64_STDERR,
                     "gunzip: '%s' does not have a .gz suffix -- unchanged\n",
                     path);
        return -1;
    }

    os64_dirent_t before = {0};
    if (os64_stat(path, &before) < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot stat '%s'\n", path);
        return -1;
    }
    if ((before.flags & OS64_DE_DIR) != 0) {
        os64_hprintf(OS64_STDERR, "gunzip: '%s' is a directory\n", path);
        return -1;
    }

    char destination[GUNZIP_PATH_MAX];
    char temporary[GUNZIP_PATH_MAX];
    if (destination_path(path, destination) < 0) {
        os64_hprintf(OS64_STDERR,
                     "gunzip: output path for '%s' is too long\n", path);
        return -1;
    }

    os64_dirent_t existing = {0};
    if (os64_stat(destination, &existing) == 0) {
        if ((existing.flags & OS64_DE_DIR) != 0) {
            os64_hprintf(OS64_STDERR,
                         "gunzip: output path '%s' is a directory\n",
                         destination);
            return -1;
        }
        if (!options->force) {
            os64_hprintf(OS64_STDERR,
                         "gunzip: '%s' already exists -- use -f to replace it\n",
                         destination);
            return -1;
        }
    }

    int32_t input = (int32_t)os64_open(path, "r");
    if (input < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot open '%s'\n", path);
        return -1;
    }
    int32_t output = create_temporary(destination, temporary);
    if (output < 0) {
        os64_hprintf(OS64_STDERR,
                     "gunzip: cannot claim staging space beside '%s'\n",
                     destination);
        os64_close(input);
        return -1;
    }

    int result = decode_handle(input, output, path, temporary);
    if (result == 0 && os64_sync(output) < 0) {
        os64_hprintf(OS64_STDERR,
                     "gunzip: cannot sync temporary output for '%s'\n", path);
        result = -1;
    }
    if (os64_close(output) < 0) {
        os64_hprintf(OS64_STDERR,
                     "gunzip: cannot close temporary output for '%s'\n", path);
        result = -1;
    }
    if (os64_close(input) < 0) {
        os64_hprintf(OS64_STDERR, "gunzip: cannot close '%s'\n", path);
        result = -1;
    }

    os64_dirent_t after = {0};
    if (result == 0 &&
        (os64_stat(path, &after) < 0 || after.size != before.size ||
         after.mtime != before.mtime)) {
        os64_hprintf(OS64_STDERR,
                     "gunzip: '%s' changed while it was being decompressed\n",
                     path);
        result = -1;
    }
    if (result == 0) {
        uint64_t publish_flags = options->force
            ? OS64_RENAME_REQUIRE_ATOMIC_REPLACE
            : OS64_RENAME_NOREPLACE;
        if (os64_rename_with_flags(temporary, destination,
                                   publish_flags) < 0) {
            if (options->force) {
                os64_hprintf(OS64_STDERR,
                             "gunzip: cannot atomically replace '%s'\n",
                             destination);
            } else {
                os64_hprintf(OS64_STDERR,
                             "gunzip: cannot publish '%s' without replacing it\n",
                             destination);
            }
            result = -1;
        }
    }
    if (result < 0) {
        os64_unlink(temporary);
        return -1;
    }

    if (!options->keep && os64_unlink(path) < 0) {
        os64_hprintf(OS64_STDERR,
                     "gunzip: decompressed '%s' but could not remove the original\n",
                     path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    gunzip_options_t options = {0};
    const char *operands[GUNZIP_MAX_OPERANDS] = {0};
    const os64_optspec_t specs[] = {
        {'c', "stdout", false, "write decompressed data to standard output",
         .flag = &options.to_stdout},
        {'k', "keep", false, "keep input files after decompression",
         .flag = &options.keep},
        {'f', "force", false, "atomically replace an existing output file",
         .flag = &options.force}
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 3);
    args.about = "Decompress gzip data.";
    args.details = "With no FILE, or when FILE is -, read standard input and "
                   "write standard output. Named FILE.gz inputs become FILE; "
                   "the compressed input is removed after safe publication.";

    int32_t operand_count = os64_args_parse(
        &args, "gunzip [-cfk] [FILE.gz ...]", operands,
        GUNZIP_MAX_OPERANDS);
    if (operand_count == OS64_ARG_HELP)
        return 0;
    if (operand_count < 0)
        return 2;

    if (operand_count == 0)
        return decode_handle(OS64_STDIN, OS64_STDOUT, "standard input",
                             "standard output") == 0 ? 0 : 1;

    int return_code = 0;
    for (int32_t i = 0; i < operand_count; i++) {
        int result = options.to_stdout || os64_streq(operands[i], "-")
            ? decode_to_stdout(operands[i])
            : decode_path(operands[i], &options);
        if (result < 0)
            return_code = 1;
    }
    return return_code;
}
