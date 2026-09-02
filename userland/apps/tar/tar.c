// tar.c — stream regular files and directories in the POSIX ustar format.

#include "os64/os64.h"
#include "tar_format.h"

#define TAR_OPERAND_MAX 512
#define TAR_IO_SIZE     (64 * 1024)

typedef struct {
    bool create;
    bool list;
    bool extract;
    bool verbose;
    bool one_file_system;
    const char *archive;
} tar_options_t;

typedef struct {
    char source[TAR_PATH_CAP];
    char member[TAR_PATH_CAP];
    uint64_t size;
    uint64_t mtime;
    uint32_t flags;
    bool command_operand;
} tar_work_t;

typedef struct {
    tar_work_t *items;
    size_t count;
    size_t capacity;
} tar_work_stack_t;

typedef struct {
    int32_t handle;
    bool standard_stream;
    char final_path[TAR_PATH_CAP];
    char temporary_path[TAR_PATH_CAP];
    char final_canonical[TAR_PATH_CAP];
    char temporary_canonical[TAR_PATH_CAP];
} tar_output_t;

static uint8_t gIOBuffer[TAR_IO_SIZE];
static uint64_t gTemporarySequence;

static int write_all(int32_t handle, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    size_t written = 0;
    while (written < length) {
        int64_t result = os64_write(handle, bytes + written, length - written);
        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

static int print_escaped_line(int32_t handle, const char *prefix,
                              const char *path)
{
    if (write_all(handle, prefix, os64_strlen(prefix)) < 0 ||
        os64_write_escaped(handle, path) < 0 ||
        write_all(handle, "\n", 1) < 0)
        return -1;
    return 0;
}

// 1 means the requested bytes arrived, 0 is a clean EOF before any byte, and
// -1 is an error or an EOF that cut a record short.
static int read_exact(int32_t handle, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    size_t received = 0;
    while (received < length) {
        int64_t result = os64_read(handle, bytes + received, length - received);
        if (result < 0)
            return -1;
        if (result == 0)
            return received == 0 ? 0 : -1;
        received += (size_t)result;
    }
    return 1;
}

static int discard_exact(int32_t handle, uint64_t length)
{
    while (length != 0) {
        size_t chunk = length < sizeof(gIOBuffer) ? (size_t)length
                                                  : sizeof(gIOBuffer);
        if (read_exact(handle, gIOBuffer, chunk) != 1)
            return -1;
        length -= chunk;
    }
    return 0;
}

static uint64_t padded_size(uint64_t size)
{
    uint64_t remainder = size % TAR_BLOCK_SIZE;
    return remainder == 0 ? size : size + TAR_BLOCK_SIZE - remainder;
}

static int join_path(char out[TAR_PATH_CAP], const char *parent,
                     const char *child)
{
    size_t length = os64_strlen(parent);
    const char *separator = length != 0 && parent[length - 1] == '/' ? "" : "/";
    int32_t wanted = os64_snprintf(out, TAR_PATH_CAP, "%s%s%s",
                                   parent, separator, child);
    return wanted >= 0 && wanted < TAR_PATH_CAP ? 0 : -1;
}

// os64 has no links, so two canonical path strings identify the same file.
// This is used to keep the archive being written out of its own traversal.
static int canonical_path(const char *path, char out[TAR_PATH_CAP])
{
    char cwd[TAR_PATH_CAP];
    const char *sources[2];
    int32_t source_count = 0;
    size_t length = 0;

    if (path == NULL || path[0] == '\0')
        return -1;
    if (path[0] != '/') {
        if (os64_getcwd(cwd, sizeof(cwd)) < 0)
            return -1;
        sources[source_count++] = cwd;
    }
    sources[source_count++] = path;

    for (int32_t source = 0; source < source_count; source++) {
        const char *cursor = sources[source];
        while (*cursor != '\0') {
            while (*cursor == '/')
                cursor++;
            if (*cursor == '\0')
                break;

            const char *component = cursor;
            while (*cursor != '\0' && *cursor != '/')
                cursor++;
            size_t component_length = (size_t)(cursor - component);

            if (component_length == 1 && component[0] == '.')
                continue;
            if (component_length == 2 && component[0] == '.' &&
                component[1] == '.') {
                while (length > 0 && out[length - 1] != '/')
                    length--;
                if (length > 0)
                    length--;
                continue;
            }

            if (length + component_length + 2 > TAR_PATH_CAP)
                return -1;
            out[length++] = '/';
            for (size_t i = 0; i < component_length; i++)
                out[length++] = component[i];
        }
    }

    if (length == 0)
        out[length++] = '/';
    out[length] = '\0';
    return 0;
}

static bool same_canonical_path(const char *path, const char *canonical)
{
    char candidate[TAR_PATH_CAP];
    return canonical[0] != '\0' &&
           canonical_path(path, candidate) == 0 &&
           os64_streq(candidate, canonical);
}

static int work_push(tar_work_stack_t *stack, const tar_work_t *work)
{
    if (stack->count == stack->capacity) {
        size_t new_capacity = stack->capacity == 0 ? 32 : stack->capacity * 2;
        if (new_capacity < stack->capacity ||
            new_capacity > SIZE_MAX / sizeof(*stack->items))
            return -1;
        tar_work_t *grown = os64_realloc(stack->items,
                                         new_capacity * sizeof(*stack->items));
        if (grown == NULL)
            return -1;
        stack->items = grown;
        stack->capacity = new_capacity;
    }
    stack->items[stack->count++] = *work;
    return 0;
}

static int make_temporary_path(const char *target, char out[TAR_PATH_CAP])
{
    size_t slash = os64_strlen(target);
    while (slash > 0 && target[slash - 1] != '/')
        slash--;

    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        uint64_t sequence = gTemporarySequence++;
        int32_t wanted;
        if (slash == 0) {
            wanted = os64_snprintf(out, TAR_PATH_CAP, ".tar-%lu-%lu.part",
                                   os64_taskid(), sequence);
        } else if (slash == 1) {
            wanted = os64_snprintf(out, TAR_PATH_CAP, "/.tar-%lu-%lu.part",
                                   os64_taskid(), sequence);
        } else {
            wanted = os64_snprintf(out, TAR_PATH_CAP,
                                   "%.*s/.tar-%lu-%lu.part",
                                   (int32_t)(slash - 1), target,
                                   os64_taskid(), sequence);
        }
        if (wanted < 0 || wanted >= TAR_PATH_CAP)
            return -1;

        os64_dirent_t existing = {0};
        if (os64_stat(out, &existing) < 0)
            return 0;
    }
    return -1;
}

static int output_open(tar_output_t *output, const char *archive)
{
    os64_memset(output, 0, sizeof(*output));
    if (os64_streq(archive, "-")) {
        output->handle = OS64_STDOUT;
        output->standard_stream = true;
        return 0;
    }

    os64_dirent_t existing = {0};
    if (os64_stat(archive, &existing) == 0 &&
        (existing.flags & OS64_DE_DIR) != 0) {
        os64_hprintf(OS64_STDERR,
                     "tar: archive path is a directory: %s\n", archive);
        return -1;
    }

    if (os64_strcopy(output->final_path, sizeof(output->final_path), archive) >=
        sizeof(output->final_path) ||
        canonical_path(archive, output->final_canonical) < 0 ||
        make_temporary_path(archive, output->temporary_path) < 0 ||
        canonical_path(output->temporary_path, output->temporary_canonical) < 0) {
        os64_hprintf(OS64_STDERR,
                     "tar: archive path is too long to publish safely: %s\n",
                     archive);
        return -1;
    }

    output->handle = (int32_t)os64_open(output->temporary_path, "w");
    if (output->handle < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot create archive beside '%s'\n",
                     archive);
        return -1;
    }
    return 0;
}

static void output_abandon(tar_output_t *output)
{
    if (output->standard_stream)
        return;
    if (output->handle >= 0)
        os64_close(output->handle);
    os64_unlink(output->temporary_path);
    output->handle = -1;
}

static int output_publish(tar_output_t *output)
{
    if (output->standard_stream)
        return 0;
    if (os64_close(output->handle) < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot close temporary archive '%s'\n",
                     output->temporary_path);
        os64_unlink(output->temporary_path);
        output->handle = -1;
        return -1;
    }
    output->handle = -1;
    if (os64_rename(output->temporary_path, output->final_path) < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot publish archive '%s'\n",
                     output->final_path);
        os64_unlink(output->temporary_path);
        return -1;
    }
    return 0;
}

static bool is_output_path(const tar_output_t *output, const char *path)
{
    return !output->standard_stream &&
           (same_canonical_path(path, output->final_canonical) ||
            same_canonical_path(path, output->temporary_canonical));
}

static int write_padding(int32_t handle, uint64_t size)
{
    size_t padding = (size_t)((TAR_BLOCK_SIZE - size % TAR_BLOCK_SIZE) %
                              TAR_BLOCK_SIZE);
    if (padding == 0)
        return 0;
    os64_memset(gIOBuffer, 0, padding);
    return write_all(handle, gIOBuffer, padding);
}

static int write_header(int32_t handle, const tar_work_t *work)
{
    tar_entry_t entry = {0};
    if (os64_strcopy(entry.path, sizeof(entry.path), work->member) >=
        sizeof(entry.path))
        return -1;
    entry.size = (work->flags & OS64_DE_DIR) ? 0 : work->size;
    entry.mtime = work->mtime;
    entry.mode = (work->flags & OS64_DE_DIR) ? 0755u : 0644u;
    entry.type = (work->flags & OS64_DE_DIR)
        ? TAR_TYPE_DIRECTORY : TAR_TYPE_REGULAR;

    uint8_t block[TAR_BLOCK_SIZE];
    tar_format_result_t format = tar_header_encode(block, &entry);
    if (format != TAR_FORMAT_OK) {
        os64_hprintf(OS64_STDERR, "tar: cannot encode '%s': %s\n",
                     work->member, tar_format_error(format));
        return -1;
    }
    return write_all(handle, block, sizeof(block));
}

static int write_regular_file(int32_t archive_handle, const tar_work_t *work)
{
    int32_t source = (int32_t)os64_open(work->source, "r");
    if (source < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot open '%s'\n", work->source);
        return -1;
    }

    uint64_t remaining = work->size;
    int result = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(gIOBuffer) ? (size_t)remaining
                                                     : sizeof(gIOBuffer);
        int64_t received = os64_read(source, gIOBuffer, chunk);
        if (received <= 0) {
            os64_hprintf(OS64_STDERR,
                         "tar: '%s' changed or became unreadable while archiving\n",
                         work->source);
            result = -1;
            break;
        }
        if (write_all(archive_handle, gIOBuffer, (size_t)received) < 0) {
            os64_hprintf(OS64_STDERR, "tar: cannot write archive\n");
            result = -1;
            break;
        }
        remaining -= (uint64_t)received;
    }

    if (os64_close(source) < 0)
        result = -1;
    if (result == 0 && write_padding(archive_handle, work->size) < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot write archive padding\n");
        result = -1;
    }
    return result;
}

static int queue_directory(tar_work_stack_t *stack, const tar_work_t *parent,
                           const tar_output_t *output,
                           const tar_options_t *options)
{
    int32_t directory = (int32_t)os64_opendir(parent->source);
    if (directory < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot open directory '%s'\n",
                     parent->source);
        return -1;
    }

    int result = 0;
    int64_t read_result;
    os64_dirent_t child = {0};
    while ((read_result = os64_readdir(directory, &child)) == 1) {
        if (os64_streq(child.name, ".") || os64_streq(child.name, ".."))
            continue;

        tar_work_t work = {0};
        if (join_path(work.source, parent->source, child.name) < 0 ||
            join_path(work.member, parent->member, child.name) < 0) {
            os64_hprintf(OS64_STDERR,
                         "tar: path too long beneath '%s'\n", parent->source);
            result = -1;
            continue;
        }
        if (is_output_path(output, work.source)) {
            if (options->verbose)
                os64_hprintf(OS64_STDERR,
                             "tar: skipping archive '%s'\n", work.source);
            continue;
        }

        work.size = child.size;
        work.mtime = child.mtime;
        work.flags = child.flags;
        if (work_push(stack, &work) < 0) {
            os64_hprintf(OS64_STDERR,
                         "tar: out of memory while walking '%s'\n",
                         parent->source);
            result = -1;
            break;
        }
    }

    if (read_result < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot read directory '%s'\n",
                     parent->source);
        result = -1;
    }
    if (os64_close(directory) < 0)
        result = -1;
    return result;
}

static int create_archive(const tar_options_t *options,
                          const char **operands, int32_t operand_count)
{
    tar_output_t output = {.handle = -1};
    if (output_open(&output, options->archive) < 0)
        return 1;

    tar_work_stack_t stack = {0};
    int result = 0;
    for (int32_t i = operand_count; i-- > 0;) {
        tar_work_t work = {.command_operand = true};
        if (os64_strcopy(work.source, sizeof(work.source), operands[i]) >=
            sizeof(work.source)) {
            os64_hprintf(OS64_STDERR, "tar: source path is too long: %s\n",
                         operands[i]);
            result = 1;
            continue;
        }
        if (is_output_path(&output, work.source)) {
            os64_hprintf(OS64_STDERR,
                         "tar: refusing to archive the output file '%s'\n",
                         work.source);
            result = 1;
            continue;
        }

        tar_format_result_t path_result =
            tar_member_path(work.source, work.member);
        os64_dirent_t entry = {0};
        if (path_result != TAR_FORMAT_OK) {
            os64_hprintf(OS64_STDERR, "tar: cannot name '%s': %s\n",
                         work.source, tar_format_error(path_result));
            result = 1;
            continue;
        }
        if (os64_stat(work.source, &entry) < 0) {
            os64_hprintf(OS64_STDERR, "tar: cannot stat '%s'\n", work.source);
            result = 1;
            continue;
        }
        work.size = entry.size;
        work.mtime = entry.mtime;
        work.flags = entry.flags;
        if (work_push(&stack, &work) < 0) {
            os64_hprintf(OS64_STDERR, "tar: out of memory\n");
            result = 1;
            break;
        }
    }

    while (result == 0 && stack.count != 0) {
        tar_work_t work = stack.items[--stack.count];
        if (write_header(output.handle, &work) < 0) {
            result = 1;
            break;
        }
        if (options->verbose)
            print_escaped_line(OS64_STDERR, "", work.member);

        if ((work.flags & OS64_DE_DIR) == 0) {
            if (write_regular_file(output.handle, &work) < 0)
                result = 1;
            continue;
        }

        bool boundary = (work.flags & OS64_DE_MOUNT) != 0 &&
                        !work.command_operand;
        if (options->one_file_system && boundary) {
            if (options->verbose)
                os64_hprintf(OS64_STDERR,
                             "tar: not descending across mount at '%s'\n",
                             work.source);
            continue;
        }
        if (queue_directory(&stack, &work, &output, options) < 0)
            result = 1;
    }

    if (result == 0) {
        uint8_t ending[TAR_BLOCK_SIZE * 2];
        os64_memset(ending, 0, sizeof(ending));
        if (write_all(output.handle, ending, sizeof(ending)) < 0) {
            os64_hprintf(OS64_STDERR, "tar: cannot finish archive\n");
            result = 1;
        }
    }

    os64_free(stack.items);
    if (result != 0) {
        output_abandon(&output);
        return result;
    }
    return output_publish(&output) == 0 ? 0 : 1;
}

static int ensure_directory(const char *path)
{
    if (os64_streq(path, "."))
        return 0;

    os64_dirent_t entry = {0};
    if (os64_stat(path, &entry) == 0)
        return (entry.flags & OS64_DE_DIR) != 0 ? 0 : -1;
    return os64_mkdir(path) == 0 ? 0 : -1;
}

static int ensure_parents(const char *path)
{
    char parent[TAR_PATH_CAP];
    if (os64_strcopy(parent, sizeof(parent), path) >= sizeof(parent))
        return -1;

    for (size_t i = 0; parent[i] != '\0'; i++) {
        if (parent[i] != '/')
            continue;
        parent[i] = '\0';
        if (parent[0] != '\0' && ensure_directory(parent) < 0)
            return -1;
        parent[i] = '/';
    }
    return 0;
}

static int extract_regular(int32_t archive_handle, const tar_entry_t *entry,
                           const char *target)
{
    if (ensure_parents(target) < 0) {
        os64_hprintf(OS64_STDERR,
                     "tar: cannot create parent directory for '%s'\n", target);
        return -1;
    }

    os64_dirent_t existing = {0};
    if (os64_stat(target, &existing) == 0 &&
        (existing.flags & OS64_DE_DIR) != 0) {
        os64_hprintf(OS64_STDERR,
                     "tar: cannot replace directory '%s' with a file\n", target);
        return -1;
    }

    char temporary[TAR_PATH_CAP];
    if (make_temporary_path(target, temporary) < 0) {
        os64_hprintf(OS64_STDERR,
                     "tar: path is too long to stage '%s' safely\n", target);
        return -1;
    }
    int32_t destination = (int32_t)os64_open(temporary, "w");
    if (destination < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot create '%s'\n", target);
        return -1;
    }

    uint64_t remaining = entry->size;
    int result = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(gIOBuffer) ? (size_t)remaining
                                                     : sizeof(gIOBuffer);
        if (read_exact(archive_handle, gIOBuffer, chunk) != 1) {
            os64_hprintf(OS64_STDERR,
                         "tar: archive ended inside '%s'\n", entry->path);
            result = -1;
            break;
        }
        if (write_all(destination, gIOBuffer, chunk) < 0) {
            os64_hprintf(OS64_STDERR, "tar: cannot write '%s'\n", target);
            result = -1;
            break;
        }
        remaining -= chunk;
    }

    uint64_t padding = padded_size(entry->size) - entry->size;
    if (result == 0 && discard_exact(archive_handle, padding) < 0) {
        os64_hprintf(OS64_STDERR,
                     "tar: archive ended after '%s'\n", entry->path);
        result = -1;
    }

    if (os64_close(destination) < 0)
        result = -1;
    if (result == 0 && os64_rename(temporary, target) < 0) {
        os64_hprintf(OS64_STDERR, "tar: cannot publish '%s'\n", target);
        result = -1;
    }
    if (result != 0)
        os64_unlink(temporary);
    return result;
}

static bool metadata_extension(char type)
{
    return type == 'x' || type == 'g' || type == 'L' || type == 'K';
}

static int read_archive(const tar_options_t *options)
{
    int32_t archive_handle;
    bool standard_stream = os64_streq(options->archive, "-");
    if (standard_stream)
        archive_handle = OS64_STDIN;
    else {
        archive_handle = (int32_t)os64_open(options->archive, "r");
        if (archive_handle < 0) {
            os64_hprintf(OS64_STDERR, "tar: cannot open archive '%s'\n",
                         options->archive);
            return 1;
        }
    }

    int result = 0;
    uint64_t header_number = 0;
    for (;;) {
        uint8_t block[TAR_BLOCK_SIZE];
        int read_result = read_exact(archive_handle, block, sizeof(block));
        if (read_result != 1) {
            os64_hprintf(OS64_STDERR,
                         read_result == 0
                             ? "tar: archive has no end marker\n"
                             : "tar: truncated archive header\n");
            result = 1;
            break;
        }
        header_number++;

        if (tar_block_is_zero(block)) {
            uint8_t second[TAR_BLOCK_SIZE];
            if (read_exact(archive_handle, second, sizeof(second)) != 1 ||
                !tar_block_is_zero(second)) {
                os64_hprintf(OS64_STDERR,
                             "tar: incomplete two-block end marker\n");
                result = 1;
            }
            break;
        }

        tar_entry_t entry = {0};
        tar_format_result_t format = tar_header_decode(block, &entry);
        if (format != TAR_FORMAT_OK) {
            os64_hprintf(OS64_STDERR, "tar: header %lu: %s\n",
                         header_number, tar_format_error(format));
            result = 1;
            break;
        }

        if (metadata_extension(entry.type)) {
            os64_hprintf(OS64_STDERR,
                         "tar: '%s' uses an unsupported metadata extension\n",
                         entry.path);
            result = 1;
            break;
        }

        if (options->list) {
            char prefix[32] = {0};
            if (options->verbose) {
                char kind = entry.type == TAR_TYPE_DIRECTORY ? 'd' :
                            entry.type == TAR_TYPE_REGULAR ? '-' : entry.type;
                os64_snprintf(prefix, sizeof(prefix), "%c %10lu ",
                              kind, entry.size);
            }
            if (print_escaped_line(OS64_STDOUT, prefix, entry.path) < 0) {
                os64_hprintf(OS64_STDERR, "tar: cannot write member list\n");
                result = 1;
                break;
            }
        }

        bool supported = entry.type == TAR_TYPE_REGULAR ||
                         entry.type == TAR_TYPE_DIRECTORY;
        if (!supported && options->extract) {
            os64_hprintf(OS64_STDERR,
                         "tar: skipping unsupported type '%c' for '%s'\n",
                         entry.type, entry.path);
            result = 1;
        }

        uint64_t padding = padded_size(entry.size) - entry.size;
        if (options->extract && supported) {
            char target[TAR_PATH_CAP];
            tar_format_result_t path_result =
                tar_extract_path(entry.path, target);
            if (path_result != TAR_FORMAT_OK) {
                os64_hprintf(OS64_STDERR,
                             "tar: refusing member '%s': %s\n",
                             entry.path, tar_format_error(path_result));
                result = 1;
                if (discard_exact(archive_handle, entry.size + padding) < 0) {
                    result = 1;
                    break;
                }
                continue;
            }

            if (entry.type == TAR_TYPE_DIRECTORY) {
                if (ensure_parents(target) < 0 ||
                    ensure_directory(target) < 0) {
                    os64_hprintf(OS64_STDERR,
                                 "tar: cannot create directory '%s'\n", target);
                    result = 1;
                } else if (options->verbose) {
                    print_escaped_line(OS64_STDERR, "", target);
                }
                if (discard_exact(archive_handle, entry.size + padding) < 0) {
                    os64_hprintf(OS64_STDERR,
                                 "tar: archive ended inside '%s'\n", entry.path);
                    result = 1;
                    break;
                }
                continue;
            }

            if (extract_regular(archive_handle, &entry, target) < 0) {
                result = 1;
                break;
            }
            if (options->verbose)
                print_escaped_line(OS64_STDERR, "", target);
            continue;
        }

        if (discard_exact(archive_handle, entry.size + padding) < 0) {
            os64_hprintf(OS64_STDERR,
                         "tar: archive ended inside '%s'\n", entry.path);
            result = 1;
            break;
        }
    }

    if (!standard_stream && os64_close(archive_handle) < 0)
        result = 1;
    return result;
}

int main(int argc, char **argv)
{
    tar_options_t options = {.archive = "-"};
    const char *operands[TAR_OPERAND_MAX] = {0};
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'c', "create", false, "create an archive", .flag = &options.create},
        {'t', "list", false, "list archive members", .flag = &options.list},
        {'x', "extract", false, "extract archive members", .flag = &options.extract},
        {'f', "file", true, "read or write ARCHIVE instead of standard I/O",
         .value_out = &options.archive},
        {'v', "verbose", false, "print each member processed", .flag = &options.verbose},
        {'\0', "one-file-system", false, "do not descend into mounted filesystems",
         .flag = &options.one_file_system}
    };

    os64_args_init(&args, argc, argv, specs, 6);
    args.about = "Create, list, or extract POSIX ustar archives.";
    args.details = "Without -f, create writes stdout and list/extract read stdin. "
                   "Regular files and directories are supported; filesystem "
                   "ownership, modes, and restored timestamps are not.";
    int32_t operand_count = os64_args_parse(
        &args, "tar (-c|-t|-x) [-v] [-f ARCHIVE] [--one-file-system] [PATH ...]",
        operands, TAR_OPERAND_MAX);
    if (operand_count == OS64_ARG_HELP)
        return 0;
    if (operand_count < 0)
        return 2;

    int operations = (options.create ? 1 : 0) + (options.list ? 1 : 0) +
                     (options.extract ? 1 : 0);
    if (operations != 1) {
        os64_hprintf(OS64_STDERR,
                     "tar: choose exactly one of -c, -t, or -x\n");
        return 2;
    }
    if (options.create && operand_count == 0) {
        os64_hprintf(OS64_STDERR, "tar: refusing to create an empty archive\n");
        return 2;
    }
    if (!options.create && operand_count != 0) {
        os64_hprintf(OS64_STDERR,
                     "tar: member selection is not supported; remove trailing operands\n");
        return 2;
    }
    if (options.one_file_system && !options.create) {
        os64_hprintf(OS64_STDERR,
                     "tar: --one-file-system applies only while creating\n");
        return 2;
    }

    return options.create
        ? create_archive(&options, operands, operand_count)
        : read_archive(&options);
}
