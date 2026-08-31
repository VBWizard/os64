#include "os64/os64.h"

#define DEFAULT_TAIL_LINES 20
#define STREAM_CHUNK_SIZE 32768
#define TAIL_MAX_FILES 512

static char buf[32768];

typedef struct stream_chunk {
    struct stream_chunk *previous;
    struct stream_chunk *next;
    size_t start;
    size_t used;
    char data[STREAM_CHUNK_SIZE];
} stream_chunk_t;

static int write_all(int32_t handle, const char *data, size_t len)
{
    size_t written = 0;

    while (written < len)
    {
        int64_t n = os64_write(handle, data + written, len - written);
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }

    return 0;
}

static int64_t find_tail_start(int32_t handle, uint64_t fileSize,
                               uint64_t lineCount)
{
    if (lineCount == 0)
        return (int64_t)fileSize;

    uint64_t blockEnd = fileSize;
    uint64_t newlines = 0;

    while (blockEnd > 0)
    {
        size_t blockSize = blockEnd > sizeof(buf) ? sizeof(buf) : (size_t)blockEnd;
        uint64_t blockStart = blockEnd - blockSize;

        if (os64_seek(handle, (int64_t)blockStart, OS64_SEEK_SET) < 0)
            return -1;

        size_t filled = 0;
        while (filled < blockSize)
        {
            int64_t n = os64_read(handle, buf + filled, blockSize - filled);
            if (n < 0)
                return -1;
            if (n == 0)
                break;
            filled += (size_t)n;
        }

        for (size_t i = filled; i > 0; i--)
        {
            uint64_t absolute = blockStart + i - 1;

            if (buf[i - 1] != '\n' || absolute == fileSize - 1)
                continue;

            newlines++;
            if (newlines == lineCount)
                return (int64_t)(absolute + 1);
        }

        blockEnd = blockStart;
    }

    return 0;
}

static int print_tail(int32_t handle, uint64_t fileSize, uint64_t lineCount)
{
    int64_t start = find_tail_start(handle, fileSize, lineCount);
    if (start < 0 || os64_seek(handle, start, OS64_SEEK_SET) < 0)
        return -1;

    int64_t n;
    while ((n = os64_read(handle, buf, sizeof(buf))) > 0)
        if (write_all(OS64_STDOUT, buf, (size_t)n) < 0)
            return -1;

    return n < 0 ? -1 : 0;
}

static void free_stream_chunks(stream_chunk_t *first)
{
    while (first != NULL)
    {
        stream_chunk_t *next = first->next;
        os64_unmap(first);
        first = next;
    }
}

static int discard_stream_line(stream_chunk_t **first, stream_chunk_t *last)
{
    stream_chunk_t *chunk = *first;

    while (chunk != NULL)
    {
        for (size_t i = chunk->start; i < chunk->used; i++)
        {
            if (chunk->data[i] != '\n')
                continue;

            chunk->start = i + 1;
            while (chunk != last && chunk->start == chunk->used)
            {
                stream_chunk_t *next = chunk->next;
                next->previous = NULL;
                os64_unmap(chunk);
                chunk = next;
            }
            *first = chunk;
            return 0;
        }

        if (chunk == last)
            break;

        stream_chunk_t *next = chunk->next;
        next->previous = NULL;
        os64_unmap(chunk);
        chunk = next;
        *first = chunk;
    }

    return -1;
}

static int print_stream_tail(int32_t handle, uint64_t lineCount)
{
    stream_chunk_t *first = NULL;
    stream_chunk_t *last = NULL;
    uint64_t retainedNewlines = 0;
    bool lastWasNewline = false;
    int result = -1;

    if (lineCount == 0)
    {
        int64_t n;
        while ((n = os64_read(handle, buf, sizeof(buf))) > 0)
            ;
        return n < 0 ? -1 : 0;
    }

    for (;;)
    {
        if (last == NULL || last->used == sizeof(last->data))
        {
            stream_chunk_t *chunk = os64_map(sizeof(*chunk));
            if (chunk == NULL)
                goto done;

            chunk->previous = last;
            if (last != NULL)
                last->next = chunk;
            else
                first = chunk;
            last = chunk;
        }

        int64_t n = os64_read(handle, last->data + last->used,
                              sizeof(last->data) - last->used);
        if (n < 0)
            goto done;
        if (n == 0)
            break;

        size_t oldUsed = last->used;
        last->used += (size_t)n;

        for (size_t i = oldUsed; i < last->used; i++)
        {
            lastWasNewline = last->data[i] == '\n';
            if (!lastWasNewline)
                continue;

            retainedNewlines++;
            if (retainedNewlines > lineCount)
            {
                if (discard_stream_line(&first, last) < 0)
                    goto done;
                retainedNewlines--;
            }
        }
    }

    // A final newline terminates the last real line; without one, the partial
    // line after the newest delimiter is itself one of the requested lines.
    if (last != NULL && !lastWasNewline && retainedNewlines == lineCount)
    {
        if (discard_stream_line(&first, last) < 0)
            goto done;
    }

    for (stream_chunk_t *chunk = first; chunk != NULL; chunk = chunk->next)
    {
        if (write_all(OS64_STDOUT, chunk->data + chunk->start,
                      chunk->used - chunk->start) < 0)
            goto done;
    }

    result = 0;

done:
    free_stream_chunks(first);
    return result;
}

#define ERROR_FOLLOW_READ_HANDLE_LESS_THAN_ZERO -2
#define ERROR_FOLLOW_READ_SEEK_ERROR -3
#define ERROR_FOLLOW_READ_CLOSE_HANDLE_LESS_THAN_ZERO -4

static int follow_read(const char *path, uint64_t *position)
{
    int32_t handle = (int32_t)os64_open(path, "r");
    if (handle < 0)
        return ERROR_FOLLOW_READ_HANDLE_LESS_THAN_ZERO;

    if (os64_seek(handle, (int64_t)*position, OS64_SEEK_SET) < 0)
    {
        os64_close(handle);
        return ERROR_FOLLOW_READ_SEEK_ERROR;
    }

    int64_t n;
    bool writeFailed = false;
    while ((n = os64_read(handle, buf, sizeof(buf))) > 0)
    {
        if (write_all(OS64_STDOUT, buf, (size_t)n) < 0)
        {
            writeFailed = true;
            break;
        }
        *position += (uint64_t)n;
    }
    int result = n < 0 || writeFailed ? -1 : 0;
    if (os64_close(handle) < 0)
        result = ERROR_FOLLOW_READ_CLOSE_HANDLE_LESS_THAN_ZERO;
    return result;
}

static int follow_files(const char *files[], uint64_t positions[],
                        bool active[], int32_t fileCount, int32_t lastOutput)
{
    for (;;)
    {
        for (int32_t i = 0; i < fileCount; i++)
        {
            if (!active[i])
                continue;

            os64_dirent_t entry = {0};
            if (os64_stat(files[i], &entry) < 0 ||
                (entry.flags & OS64_DE_DIR) != 0)
            {
                os64_hprintf(OS64_STDERR,
                             "tail: cannot continue following %s\n", files[i]);
                return -1;
            }
            if (entry.size < positions[i])
            {
                if (fileCount > 1 && lastOutput != i)
                    os64_printf("\n==> %s <==\n", files[i]);
                os64_printf("*** file truncated ***\n");
                positions[i] = 0;
                lastOutput = i;
            }
            if (entry.size == positions[i])
                continue;

            if (fileCount > 1 && lastOutput != i)
                os64_printf("\n==> %s <==\n", files[i]);
            int follow_read_return = 0;
            if ((follow_read_return = follow_read(files[i], &positions[i])) < 0)
                return follow_read_return;
            lastOutput = i;
        }
        os64_sleep(100);
    }
}

int main(int argc, char **argv)
{
    int returnCode = 0;
    bool optFollow = false;
    uint64_t lineCount = DEFAULT_TAIL_LINES;
    const char *lineCountValue = NULL;
    const char *files[TAIL_MAX_FILES] = {0};
    uint64_t followPositions[TAIL_MAX_FILES] = {0};
    bool followActive[TAIL_MAX_FILES] = {0};
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'f', "follow", false, "stay attached and print data appended to FILE", .flag = &optFollow},
        {'n', "lines", true, "how many lines to show",
         .value_out = &lineCountValue, .numeric_alias = true}
    };

    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Print the last lines of each FILE to standard output.";
    args.details = "With no FILE, or when FILE is -, read standard input. "
                   "With --follow, wait for and print data appended to FILE.";

    int32_t parsed = os64_args_parse(&args, "tail [-f] [-n NUM] [FILE ...]",
                                     files, TAIL_MAX_FILES);
    if (parsed == OS64_ARG_HELP)
        os64_exit(0);
    if (parsed < 0)
        os64_exit(2);

    if (lineCountValue != NULL &&
        !os64_parse_u64(lineCountValue, &lineCount))
    {
        os64_hprintf(OS64_STDERR, "tail: invalid line count: %s\n", lineCountValue);
        os64_exit(2);
    }

    int32_t inputCount = parsed == 0 ? 1 : parsed;
    int32_t activeFollowers = 0;
    int32_t lastOutput = -1;
    for (int32_t i = 0; i < inputCount; i++)
    {
        const char *path = parsed == 0 ? NULL : files[i];
        bool useStdin = path == NULL || os64_streq(path, "-");
        const char *label = useStdin ? "standard input" : path;

        if (inputCount > 1)
        {
            if (i != 0)
                os64_printf("\n");
            os64_printf("==> %s <==\n", label);
        }

        if (useStdin)
        {
            if (optFollow)
            {
                os64_hprintf(OS64_STDERR,
                             "tail: cannot follow standard input\n");
                returnCode = 1;
                continue;
            }
            if (print_stream_tail(OS64_STDIN, lineCount) < 0)
            {
                os64_hprintf(OS64_STDERR,
                             "tail: error reading standard input\n");
                returnCode = 1;
            }
            lastOutput = i;
            continue;
        }

        os64_dirent_t statEntry = {0};
        if (os64_stat(path, &statEntry) < 0)
        {
            os64_hprintf(OS64_STDERR, "tail: could not stat %s\n", path);
            returnCode = 1;
            continue;
        }
        if (statEntry.flags & OS64_DE_DIR)
        {
            os64_hprintf(OS64_STDERR,
                         "tail: cannot read a directory: %s\n", path);
            returnCode = 1;
            continue;
        }

        int32_t fileHandle = (int32_t)os64_open(path, "r");
        if (fileHandle < 0)
        {
            os64_hprintf(OS64_STDERR, "tail: unable to open %s\n", path);
            returnCode = 1;
            continue;
        }
        if (print_tail(fileHandle, statEntry.size, lineCount) < 0)
        {
            os64_hprintf(OS64_STDERR, "tail: error reading %s\n", path);
            returnCode = 1;
        }
        int64_t position = os64_seek(fileHandle, 0, OS64_SEEK_CUR);
        if (os64_close(fileHandle) < 0 || position < 0)
        {
            os64_hprintf(OS64_STDERR, "tail: error closing %s\n", path);
            returnCode = 1;
            continue;
        }
        lastOutput = i;
        if (optFollow)
        {
            followPositions[i] = (uint64_t)position;
            followActive[i] = true;
            activeFollowers++;
        }
    }

    int follow_files_result = 0;
    if (optFollow && activeFollowers > 0)
        if ((follow_files_result=follow_files(files, followPositions, followActive,
                     inputCount, lastOutput)) < 0)
    {
        os64_hprintf(OS64_STDERR, "tail: follow failed (%d)\n", follow_files_result);
        returnCode = 1;
    }

    os64_exit(returnCode);
}
