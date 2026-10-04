#include "os64/os64.h"

#define CUT_BUFFER_SIZE 32768
#define CUT_MAX_FILES 512

typedef struct {
    uint64_t first;
    uint64_t last;
} cut_range_t;

typedef struct {
    char mode;
    unsigned char delimiter;
    bool suppress;
    bool complement;
    bool no_partial;
    const char *output_delimiter;
    size_t output_delimiter_length;
    cut_range_t *ranges;
    size_t range_count;
} cut_options_t;

static char input_buffer[CUT_BUFFER_SIZE];
static char output_buffer[CUT_BUFFER_SIZE];
static size_t output_length;

// The shared parser expects short-option values in the next argv entry.
// Translate cut's conventional -f2 / -d: spellings without changing argv.
static char **normalize_arguments(int *argc, char **argv,
                                  const os64_optspec_t *specs, size_t spec_count)
{
    size_t count = (size_t)*argc;
    if (count > INT32_MAX / 2 || count > (SIZE_MAX / sizeof(char *) - 1) / 2)
        return NULL;
    size_t pointer_bytes = (count * 2 + 1) * sizeof(char *);
    size_t bytes = pointer_bytes;
    for (size_t i = 0; i < count; i++)
    {
        size_t length = os64_strlen(argv[i]) + 1;
        if (length > SIZE_MAX - bytes)
            return NULL;
        bytes += length;
    }
    char **normalized = os64_malloc(bytes);
    if (!normalized)
        return NULL;
    char *storage = (char *)normalized + pointer_bytes;
    int produced = 0;
    bool operand = false, ended = false;
    for (size_t i = 0; i < count; i++)
    {
        char *token = argv[i];
        normalized[produced++] = token;
        if (!i || operand || ended)
        {
            operand = false;
            continue;
        }
        if (os64_streq(token, "--"))
        {
            ended = true;
            continue;
        }
        if (token[0] != '-' || !token[1])
            continue;
        if (token[1] == '-')
        {
            for (size_t s = 0; s < spec_count; s++)
                if (specs[s].takes_value && specs[s].name &&
                    os64_streq(token + 2, specs[s].name))
                    operand = true;
            continue;
        }
        for (size_t at = 1; token[at]; at++)
        {
            bool takes_value = false;
            for (size_t s = 0; s < spec_count; s++)
                if (specs[s].letter == token[at])
                    takes_value = specs[s].takes_value;
            if (!takes_value)
                continue;
            if (token[at + 1])
            {
                os64_memcpy(storage, token, at + 1);
                storage[at + 1] = '\0';
                normalized[produced - 1] = storage;
                storage += at + 2;
                normalized[produced++] = token + at + 1;
            }
            else
                operand = true;
            break;
        }
    }
    normalized[produced] = NULL;
    *argc = produced;
    return normalized;
}

static int32_t parse_arguments(os64_args_t *args, const char *usage,
                               const char **files)
{
    int32_t count = 0, option;
    while ((option = os64_args_next(args)) != OS64_ARG_END)
    {
        if (option == OS64_ARG_HELP)
        {
            os64_args_help(args, usage);
            return OS64_ARG_HELP;
        }
        if (option == OS64_ARG_ERROR)
        {
            os64_hprintf(OS64_STDERR, "cut: invalid or incomplete option: %s\n",
                         args->value ? args->value : "");
            return OS64_ARG_ERROR;
        }
        if (option == OS64_ARG_POSITIONAL)
        {
            if (count == CUT_MAX_FILES)
            {
                os64_hprintf(OS64_STDERR, "cut: too many input files (maximum %u)\n",
                             CUT_MAX_FILES);
                return OS64_ARG_ERROR;
            }
            files[count++] = args->value;
            continue;
        }
        for (int32_t i = 0; i < args->nspecs; i++)
        {
            const os64_optspec_t *spec = &args->specs[i];
            if (spec->letter != option)
                continue;
            if ((option == 'b' || option == 'c' || option == 'f') && *spec->value_out)
            {
                os64_hprintf(OS64_STDERR, "cut: specify the selection LIST once\n");
                return OS64_ARG_ERROR;
            }
            if (spec->takes_value)
                *spec->value_out = args->value;
            else
                *spec->flag = true;
            break;
        }
    }
    return count;
}

static int flush_output(void)
{
    size_t done = 0;
    while (done < output_length)
    {
        int64_t n = os64_write(OS64_STDOUT, output_buffer + done,
                               output_length - done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    output_length = 0;
    return 0;
}

static int emit(const char *data, size_t length)
{
    while (length)
    {
        size_t available = sizeof(output_buffer) - output_length;
        size_t n = length < available ? length : available;
        os64_memcpy(output_buffer + output_length, data, n);
        output_length += n;
        data += n;
        length -= n;
        if (output_length == sizeof(output_buffer) && flush_output() < 0)
            return -1;
    }
    return 0;
}

static bool parse_position(const char **cursor, uint64_t *position)
{
    const char *p = *cursor;
    uint64_t n = 0;
    if (*p < '0' || *p > '9')
        return false;
    do
    {
        unsigned digit = (unsigned)(*p - '0');
        if (n > (UINT64_MAX - digit) / 10)
            return false;
        n = n * 10 + digit;
        p++;
    } while (*p >= '0' && *p <= '9');
    if (!n)
        return false;
    *cursor = p;
    *position = n;
    return true;
}

static bool list_separator(char c)
{
    return c == ',' || c == ' ' || c == '\t';
}

static int parse_list(const char *list, cut_options_t *options)
{
    size_t capacity = 1;
    for (const char *p = list; *p; p++)
        if (list_separator(*p))
            capacity++;
    if (capacity > SIZE_MAX / sizeof(cut_range_t))
        return -1;
    options->ranges = os64_malloc(capacity * sizeof(cut_range_t));
    if (!options->ranges)
        return -1;

    const char *p = list;
    do
    {
        cut_range_t range = {1, UINT64_MAX};
        if (*p == '-')
        {
            p++;
            if (!parse_position(&p, &range.last))
                return 1;
        }
        else
        {
            if (!parse_position(&p, &range.first))
                return 1;
            range.last = range.first;
            if (*p == '-')
            {
                p++;
                range.last = UINT64_MAX;
                if (*p >= '0' && *p <= '9' &&
                    !parse_position(&p, &range.last))
                    return 1;
            }
        }
        if (range.last < range.first || (*p && !list_separator(*p)))
            return 1;

        // Sorted insertion keeps later selection a forward walk. Arguments
        // bound this work; input length does not multiply the list scan.
        size_t at = options->range_count++;
        while (at && options->ranges[at - 1].first > range.first)
        {
            options->ranges[at] = options->ranges[at - 1];
            at--;
        }
        options->ranges[at] = range;
        if (*p)
        {
            p++;
            if (!*p)
                return 1;
        }
    } while (*p);

    size_t merged = 0;
    for (size_t i = 0; i < options->range_count; i++)
    {
        cut_range_t range = options->ranges[i];
        if (merged && range.first <= options->ranges[merged - 1].last)
        {
            if (range.last > options->ranges[merged - 1].last)
                options->ranges[merged - 1].last = range.last;
        }
        else
            options->ranges[merged++] = range;
    }
    options->range_count = merged;
    return 0;
}

static bool selected(const cut_options_t *options, uint64_t position,
                     size_t *range_index)
{
    while (*range_index < options->range_count &&
           options->ranges[*range_index].last < position)
        (*range_index)++;
    bool inside = *range_index < options->range_count &&
                  options->ranges[*range_index].first <= position;
    return inside != options->complement;
}

static int cut_line(const char *line, size_t length,
                    const cut_options_t *options)
{
    size_t range_index = 0;
    bool emitted = false;
    if (options->mode == 'f')
    {
        bool delimited = os64_memchr(line, options->delimiter, length) != NULL;
        // For newline-separated fields, the final newline terminates the
        // last field; it does not create an additional empty field.
        if (options->delimiter == '\n' && length && line[length - 1] == '\n')
            length--;
        if (!delimited)
        {
            if (options->suppress)
                return 0;
            if (emit(line, length) < 0)
                return -1;
        }
        else
        {
            size_t start = 0;
            uint64_t field = 1;
            for (size_t end = 0; ; end++)
            {
                if (end != length &&
                    (unsigned char)line[end] != options->delimiter)
                    continue;
                if (selected(options, field, &range_index))
                {
                    if (emitted && emit(options->output_delimiter,
                                        options->output_delimiter_length) < 0)
                        return -1;
                    if (emit(line + start, end - start) < 0)
                        return -1;
                    emitted = true;
                }
                if (end == length)
                    break;
                start = end + 1;
                field++;
            }
            if (options->delimiter == '\n' && options->suppress && !emitted)
                return 0;
        }
    }
    else
    {
        bool previous_selected = false;
        size_t previous_range = 0;
        uint64_t character = 0;
        for (size_t at = 0; at < length; )
        {
            size_t width = 1;
            if (options->mode == 'c' || options->no_partial)
            {
                uint32_t cp;
                width = os64_utf8_decode(line + at, length - at, &cp);
            }
            // With -b -n, a selected final byte brings along the complete
            // UTF-8 character. Invalid sequences advance one original byte.
            uint64_t position = options->mode == 'c' ? ++character : at + width;
            bool keep = selected(options, position, &range_index);
            if (keep)
            {
                if (emitted && (!previous_selected || previous_range != range_index) &&
                    options->output_delimiter &&
                    emit(options->output_delimiter,
                         options->output_delimiter_length) < 0)
                    return -1;
                if (emit(line + at, width) < 0)
                    return -1;
                emitted = true;
            }
            previous_selected = keep;
            previous_range = range_index;
            at += width;
        }
    }
    return emit("\n", 1);
}

static int cut_handle(int32_t handle, const char *name,
                      const cut_options_t *options)
{
    char *line = NULL;
    size_t length = 0, capacity = 0;
    int result = 0;
    for (;;)
    {
        int64_t n = os64_read(handle, input_buffer, sizeof(input_buffer));
        if (n < 0)
        {
            os64_hprintf(OS64_STDERR, "cut: error reading %s\n", name);
            result = 1;
            break;
        }
        if (!n)
        {
            if (length && cut_line(line, length, options) < 0)
                result = -1;
            break;
        }
        size_t at = 0;
        while (at < (size_t)n)
        {
            // A newline field delimiter makes the input one field record:
            // its embedded newlines separate fields rather than records.
            const char *newline = options->mode == 'f' && options->delimiter == '\n' ?
                                  NULL : os64_memchr(input_buffer + at, '\n', (size_t)n - at);
            size_t part = newline ? (size_t)(newline - input_buffer) - at : (size_t)n - at;
            if (part > SIZE_MAX - length)
                goto no_memory;
            size_t required = length + part;
            if (required > capacity)
            {
                size_t grown = capacity ? capacity : CUT_BUFFER_SIZE;
                while (grown < required)
                {
                    if (grown > SIZE_MAX / 2)
                    {
                        grown = required;
                        break;
                    }
                    grown *= 2;
                }
                char *replacement = os64_realloc(line, grown);
                if (!replacement)
                    goto no_memory;
                line = replacement;
                capacity = grown;
            }
            if (part)
                os64_memcpy(line + length, input_buffer + at, part);
            length = required;
            at += part;
            if (newline)
            {
                if (cut_line(line, length, options) < 0)
                {
                    result = -1;
                    goto done;
                }
                length = 0;
                at++;
            }
        }
    }
    goto done;
no_memory:
    os64_hprintf(OS64_STDERR, "cut: out of memory reading %s\n", name);
    result = 1;
done:
    os64_free(line);
    return result;
}

int main(int argc, char **argv)
{
    const char *bytes = NULL, *characters = NULL, *fields = NULL;
    const char *delimiter = NULL, *output_delimiter = NULL;
    const char *files[CUT_MAX_FILES];
    cut_options_t options = {0};
    os64_args_t args = {0};
    const os64_optspec_t specs[] = {
        {'b', "bytes", true, "select byte positions in LIST", .value_out = &bytes},
        {'c', "characters", true, "select UTF-8 character positions in LIST", .value_out = &characters},
        {'f', "fields", true, "select delimiter-separated fields in LIST", .value_out = &fields},
        {'d', "delimiter", true, "field delimiter (default: TAB; empty: NUL)", .value_out = &delimiter},
        {'s', "only-delimited", false, "skip lines without a field delimiter", .flag = &options.suppress},
        {'n', NULL, false, "with -b, keep complete UTF-8 characters by their final byte", .flag = &options.no_partial},
        {'C', "complement", false, "select positions outside LIST", .flag = &options.complement},
        {'O', "output-delimiter", true, "join selected fields or ranges with STRING (empty: NUL)", .value_out = &output_delimiter}
    };
    const char *usage = "cut (-b LIST | -c LIST | -f LIST) [OPTIONS] [FILE ...]";
    size_t spec_count = sizeof(specs) / sizeof(specs[0]);
    char **normalized = normalize_arguments(&argc, argv, specs, spec_count);
    if (!normalized)
    {
        os64_hprintf(OS64_STDERR, "cut: out of memory parsing arguments\n");
        return 1;
    }
    os64_args_init(&args, argc, normalized, specs, spec_count);
    args.about = "Print selected parts of each input line.";
    args.details = "LIST: N, N-M, -M, N-; separate ranges with commas or blanks.\n"
                   "Positions start at 1; overlapping ranges print once in input order.\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "Example: cut -d ' ' -f 2 file (repeated spaces make empty fields).";
    int32_t count = parse_arguments(&args, usage, files);
    os64_free(normalized);
    if (count < 0)
        return count == OS64_ARG_HELP ? 0 : 2;
    if ((bytes != NULL) + (characters != NULL) + (fields != NULL) != 1)
    {
        os64_hprintf(OS64_STDERR, "cut: choose one of -b, -c, or -f\n");
        return 2;
    }
    if ((!fields && (delimiter || options.suppress)) || (options.no_partial && !bytes))
    {
        os64_hprintf(OS64_STDERR, "cut: -d and -s require -f; -n requires -b\n");
        return 2;
    }
    if (delimiter && delimiter[0] && delimiter[1])
    {
        os64_hprintf(OS64_STDERR, "cut: delimiter must be a single byte\n");
        return 2;
    }
    options.mode = fields ? 'f' : characters ? 'c' : 'b';
    options.delimiter = delimiter ? (unsigned char)delimiter[0] : '\t';
    char default_delimiter = (char)options.delimiter;
    options.output_delimiter = output_delimiter ? output_delimiter : fields ? &default_delimiter : NULL;
    options.output_delimiter_length = output_delimiter ? os64_strlen(output_delimiter) : 1;
    if (output_delimiter && !options.output_delimiter_length)
        options.output_delimiter_length = 1;
    const char *list = fields ? fields : characters ? characters : bytes;
    int parsed = parse_list(list, &options);
    if (parsed)
    {
        os64_hprintf(OS64_STDERR, parsed < 0 ? "cut: out of memory parsing LIST\n" :
                     "cut: invalid LIST: %s\n", list);
        os64_free(options.ranges);
        return parsed < 0 ? 1 : 2;
    }

    int status = 0;
    bool write_failed = false;
    output_length = 0;
    for (int32_t i = 0; i < (count ? count : 1); i++)
    {
        const char *path = count ? files[i] : "-";
        bool use_stdin = os64_streq(path, "-");
        int32_t handle = OS64_STDIN;
        if (!use_stdin)
        {
            os64_dirent_t entry = {0};
            if (os64_stat(path, &entry) < 0)
            {
                os64_hprintf(OS64_STDERR, "cut: cannot stat '%s'\n", path);
                status = 1;
                continue;
            }
            if (entry.flags & OS64_DE_DIR)
            {
                os64_hprintf(OS64_STDERR, "cut: '%s' is a directory\n", path);
                status = 1;
                continue;
            }
            handle = (int32_t)os64_open(path, "r");
            if (handle < 0)
            {
                os64_hprintf(OS64_STDERR, "cut: cannot open '%s'\n", path);
                status = 1;
                continue;
            }
        }
        int result = cut_handle(handle, use_stdin ? "standard input" : path, &options);
        if (!use_stdin && os64_close(handle) < 0)
        {
            os64_hprintf(OS64_STDERR, "cut: cannot close '%s'\n", path);
            status = 1;
        }
        if (result)
            status = 1;
        if (result < 0)
        {
            write_failed = true;
            break;
        }
    }
    if (write_failed || flush_output() < 0)
    {
        os64_hprintf(OS64_STDERR, "cut: error writing standard output\n");
        status = 1;
    }
    os64_free(options.ranges);
    return status;
}
