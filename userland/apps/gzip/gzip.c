// gzip — compress streams and safely replace named files with .gz siblings.
//
// A temporary file beside the destination is created exclusively, then
// synced, closed, and published with an atomic destination policy. Without
// -f publication refuses an existing name; with -f it refuses filesystems
// that cannot replace an existing file atomically.
// The source is removed last, so staging and publication failures leave it
// intact. A final size/mtime check catches changes visible before publication;
// a live log must still be rotated before compression because os64 has no file
// snapshot or lock spanning that check and removal. Standard output remains
// the composable door for callers that manage publication themselves.

#include "os64/os64.h"
#include "gzip/gzip.h"

#define GZIP_IO_SIZE      (64u * 1024u)
#define GZIP_PATH_MAX     512u
#define GZIP_MAX_OPERANDS 512

typedef struct {
    bool to_stdout;
    bool keep;
    bool force;
} gzip_options_t;

static uint8_t gInput[GZIP_IO_SIZE];
static uint8_t gOutput[GZIP_IO_SIZE];
static uint32_t gTemporarySequence;

static int write_all(int32_t handle, const uint8_t *bytes, size_t length)
{
    size_t written = 0;
    while (written < length) {
        int64_t result = os64_write(handle, bytes + written, length - written);
        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

static int encode_handle(int32_t input_handle, int32_t output_handle,
                         const char *name, uint32_t modification_time,
                         uint64_t *input_size)
{
    os64_gzip_encoder_t *stream =
        os64_gzip_encoder_create(modification_time);
    if (stream == NULL) {
        os64_hprintf(OS64_STDERR, "gzip: out of memory\n");
        return -1;
    }

    for (;;) {
        int64_t read_result = os64_read(input_handle, gInput, sizeof(gInput));
        if (read_result < 0) {
            os64_hprintf(OS64_STDERR, "gzip: error reading %s\n", name);
            os64_gzip_encoder_destroy(stream);
            return -1;
        }

        bool end_of_input = read_result == 0;
        const uint8_t *input = gInput;
        size_t input_left = (size_t)read_result;
        for (;;) {
            uint8_t *output = gOutput;
            size_t output_left = sizeof(gOutput);
            os64_gzip_encode_status_t status = os64_gzip_encoder_process(
                stream, &input, &input_left, &output, &output_left,
                end_of_input);
            size_t produced = sizeof(gOutput) - output_left;
            if (produced != 0 &&
                write_all(output_handle, gOutput, produced) < 0) {
                os64_hprintf(OS64_STDERR, "gzip: error writing %s\n", name);
                os64_gzip_encoder_destroy(stream);
                return -1;
            }

            if (status == OS64_GZIP_ENCODE_NEED_OUTPUT)
                continue;
            if (status == OS64_GZIP_ENCODE_NEED_INPUT) {
                if (input_left != 0) {
                    os64_hprintf(OS64_STDERR,
                                 "gzip: encoder left input behind\n");
                    os64_gzip_encoder_destroy(stream);
                    return -1;
                }
                break;
            }
            if (status == OS64_GZIP_ENCODE_DONE) {
                *input_size = os64_gzip_encoder_input_size(stream);
                os64_gzip_encoder_destroy(stream);
                return 0;
            }

            os64_hprintf(OS64_STDERR, "gzip: %s: %s\n", name,
                         os64_gzip_encode_status_name(status));
            os64_gzip_encoder_destroy(stream);
            return -1;
        }
    }
}

static bool has_gzip_suffix(const char *path)
{
    size_t length = os64_strlen(path);
    return length >= 3 && path[length - 3] == '.' &&
           path[length - 2] == 'g' && path[length - 1] == 'z';
}

static int destination_path(const char *source, char out[GZIP_PATH_MAX])
{
    int32_t wanted = os64_snprintf(out, GZIP_PATH_MAX, "%s.gz", source);
    return wanted >= 0 && wanted < (int32_t)GZIP_PATH_MAX ? 0 : -1;
}

static int32_t create_temporary(const char *target,
                                char output[GZIP_PATH_MAX])
{
    size_t slash = os64_strlen(target);
    while (slash > 0 && target[slash - 1] != '/')
        slash--;

    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        uint32_t sequence = gTemporarySequence++;
        int32_t wanted;
        if (slash == 0) {
            wanted = os64_snprintf(output, GZIP_PATH_MAX,
                                   ".gzip-%lu-%u.part",
                                   os64_taskid(), sequence);
        } else if (slash == 1) {
            wanted = os64_snprintf(output, GZIP_PATH_MAX,
                                   "/.gzip-%lu-%u.part",
                                   os64_taskid(), sequence);
        } else {
            wanted = os64_snprintf(output, GZIP_PATH_MAX,
                                   "%.*s/.gzip-%lu-%u.part",
                                   (int32_t)(slash - 1), target,
                                   os64_taskid(), sequence);
        }
        if (wanted < 0 || wanted >= (int32_t)GZIP_PATH_MAX)
            return -1;

        int64_t handle = os64_open(output, "x");
        if (handle >= 0)
            return (int32_t)handle;

        os64_dirent_t existing = {0};
        if (os64_stat(output, &existing) < 0)
            return -1;
    }
    return -1;
}

static uint32_t gzip_time(uint64_t modification_time)
{
    return modification_time <= UINT32_MAX ? (uint32_t)modification_time : 0;
}

static int encode_to_stdout(const char *path)
{
    if (os64_streq(path, "-")) {
        uint64_t ignored = 0;
        return encode_handle(OS64_STDIN, OS64_STDOUT, "standard input", 0,
                             &ignored);
    }

    os64_dirent_t entry = {0};
    if (os64_stat(path, &entry) < 0) {
        os64_hprintf(OS64_STDERR, "gzip: cannot stat '%s'\n", path);
        return -1;
    }
    if ((entry.flags & OS64_DE_DIR) != 0) {
        os64_hprintf(OS64_STDERR, "gzip: '%s' is a directory\n", path);
        return -1;
    }
    int32_t input = (int32_t)os64_open(path, "r");
    if (input < 0) {
        os64_hprintf(OS64_STDERR, "gzip: cannot open '%s'\n", path);
        return -1;
    }
    uint64_t ignored = 0;
    int result = encode_handle(input, OS64_STDOUT, path,
                               gzip_time(entry.mtime), &ignored);
    if (os64_close(input) < 0) {
        os64_hprintf(OS64_STDERR, "gzip: cannot close '%s'\n", path);
        result = -1;
    }
    return result;
}

static int encode_path(const char *path, const gzip_options_t *options)
{
    if (has_gzip_suffix(path)) {
        os64_hprintf(OS64_STDERR,
                     "gzip: '%s' already has a .gz suffix -- unchanged\n",
                     path);
        return -1;
    }

    os64_dirent_t before = {0};
    if (os64_stat(path, &before) < 0) {
        os64_hprintf(OS64_STDERR, "gzip: cannot stat '%s'\n", path);
        return -1;
    }
    if ((before.flags & OS64_DE_DIR) != 0) {
        os64_hprintf(OS64_STDERR, "gzip: '%s' is a directory\n", path);
        return -1;
    }

    char destination[GZIP_PATH_MAX];
    if (destination_path(path, destination) < 0) {
        os64_hprintf(OS64_STDERR,
                     "gzip: output path for '%s' is too long\n", path);
        return -1;
    }

    os64_dirent_t existing = {0};
    if (os64_stat(destination, &existing) == 0) {
        if ((existing.flags & OS64_DE_DIR) != 0) {
            os64_hprintf(OS64_STDERR,
                         "gzip: output path '%s' is a directory\n",
                         destination);
            return -1;
        }
        if (!options->force) {
            os64_hprintf(OS64_STDERR,
                         "gzip: '%s' already exists -- use -f to replace it\n",
                         destination);
            return -1;
        }
    }

    int32_t input = (int32_t)os64_open(path, "r");
    if (input < 0) {
        os64_hprintf(OS64_STDERR, "gzip: cannot open '%s'\n", path);
        return -1;
    }

    char temporary[GZIP_PATH_MAX];
    int32_t output = create_temporary(destination, temporary);
    if (output < 0) {
        os64_hprintf(OS64_STDERR,
                     "gzip: cannot claim staging space beside '%s'\n",
                     destination);
        os64_close(input);
        return -1;
    }

    uint64_t encoded_size = 0;
    int result = encode_handle(input, output, path, gzip_time(before.mtime),
                               &encoded_size);
    if (result == 0 && os64_sync(output) < 0) {
        os64_hprintf(OS64_STDERR,
                     "gzip: cannot sync temporary output for '%s'\n", path);
        result = -1;
    }
    if (os64_close(output) < 0) {
        os64_hprintf(OS64_STDERR,
                     "gzip: cannot close temporary output for '%s'\n", path);
        result = -1;
    }
    if (os64_close(input) < 0) {
        os64_hprintf(OS64_STDERR, "gzip: cannot close '%s'\n", path);
        result = -1;
    }

    os64_dirent_t after = {0};
    if (result == 0 &&
        (encoded_size != before.size || os64_stat(path, &after) < 0 ||
         after.size != before.size || after.mtime != before.mtime)) {
        os64_hprintf(OS64_STDERR,
                     "gzip: '%s' changed while it was being compressed\n",
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
                             "gzip: cannot atomically replace '%s'\n",
                             destination);
            } else {
                os64_hprintf(OS64_STDERR,
                             "gzip: cannot publish '%s' without replacing it\n",
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
                     "gzip: compressed '%s' but could not remove the original\n",
                     path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    gzip_options_t options = {0};
    const char *operands[GZIP_MAX_OPERANDS] = {0};
    const os64_optspec_t specs[] = {
        {'c', "stdout", false, "write compressed data to standard output",
         .flag = &options.to_stdout},
        {'k', "keep", false, "keep input files after compression",
         .flag = &options.keep},
        {'f', "force", false, "atomically replace an existing .gz output file",
         .flag = &options.force}
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 3);
    args.about = "Compress files in gzip format.";
    args.details = "With no FILE, or when FILE is -, read standard input and "
                   "write standard output. Named files become FILE.gz; the "
                   "original is removed only after safe publication.";

    int32_t operand_count = os64_args_parse(
        &args, "gzip [-cfk] [FILE ...]", operands, GZIP_MAX_OPERANDS);
    if (operand_count == OS64_ARG_HELP)
        return 0;
    if (operand_count < 0)
        return 2;

    if (operand_count == 0) {
        uint64_t ignored = 0;
        return encode_handle(OS64_STDIN, OS64_STDOUT, "standard input", 0,
                             &ignored) == 0 ? 0 : 1;
    }

    int return_code = 0;
    for (int32_t i = 0; i < operand_count; i++) {
        int result = options.to_stdout || os64_streq(operands[i], "-")
            ? encode_to_stdout(operands[i])
            : encode_path(operands[i], &options);
        if (result < 0)
            return_code = 1;
    }
    return return_code;
}
