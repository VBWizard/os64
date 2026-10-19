#include "os64/os64.h"

#define IO_SIZE 32768

typedef struct {
    size_t offset, length;
} line_t;

typedef struct {
    bool numeric, reverse, unique, fold, blanks;
} options_t;

typedef struct {
    unsigned char *data;
    size_t length, capacity;
} buffer_t;

static int reserve(buffer_t *buffer, size_t extra)
{
    if (extra > SIZE_MAX - buffer->length)
        return -1;
    size_t needed = buffer->length + extra;
    if (needed <= buffer->capacity)
        return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : IO_SIZE;
    while (capacity < needed)
    {
        if (capacity > SIZE_MAX / 2)
        {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    void *data = os64_realloc(buffer->data, capacity);
    if (!data)
        return -1;
    buffer->data = data;
    buffer->capacity = capacity;
    return 0;
}

static int read_input(buffer_t *buffer, const char *name)
{
    int32_t handle = OS64_STDIN;
    bool from_stdin = os64_streq(name, "-");
    if (!from_stdin)
    {
        os64_dirent_t entry = {0};
        if (os64_stat(name, &entry) < 0 || (entry.flags & OS64_DE_DIR))
        {
            os64_hprintf(OS64_STDERR, "sort: cannot read '%s'\n", name);
            return -1;
        }
        handle = (int32_t)os64_open(name, "r");
        if (handle < 0)
        {
            os64_hprintf(OS64_STDERR, "sort: cannot open '%s'\n", name);
            return -1;
        }
    }

    size_t start = buffer->length;
    int result = 0;
    for (;;)
    {
        if (reserve(buffer, IO_SIZE) < 0)
        {
            os64_hprintf(OS64_STDERR, "sort: out of memory\n");
            result = -1;
            break;
        }
        int64_t n = os64_read(handle, buffer->data + buffer->length, IO_SIZE);
        if (n < 0)
        {
            os64_hprintf(OS64_STDERR, "sort: error reading '%s'\n", name);
            result = -1;
            break;
        }
        if (n == 0)
            break;
        buffer->length += (size_t)n;
    }
    if (!from_stdin && os64_close(handle) < 0)
    {
        os64_hprintf(OS64_STDERR, "sort: cannot close '%s'\n", name);
        result = -1;
    }
    // Each input's unterminated last line is a separate record. The EOF
    // read reserved space for its newline, including at a block boundary.
    if (result == 0 && buffer->length > start &&
        buffer->data[buffer->length - 1] != '\n')
        buffer->data[buffer->length++] = '\n';
    return result;
}

static bool blank(unsigned char c)
{
    return c == ' ' || c == '\t';
}

static bool digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

typedef struct {
    const unsigned char *integer, *fraction;
    size_t integers, fractions;
    bool negative;
} number_t;

static number_t number(const unsigned char *s, size_t length)
{
    size_t i = 0;
    number_t n = {0};
    while (i < length && blank(s[i]))
        i++;
    if (i < length && s[i] == '-')
    {
        n.negative = true;
        i++;
    }
    while (i < length && s[i] == '0')
        i++;
    n.integer = s + i;
    while (i < length && digit(s[i]))
    {
        n.integers++;
        i++;
    }
    if (i < length && s[i] == '.')
        i++;
    else
        length = i;
    n.fraction = s + i;
    while (i < length && digit(s[i]))
    {
        n.fractions++;
        i++;
    }
    while (n.fractions && n.fraction[n.fractions - 1] == '0')
        n.fractions--;
    if (!n.integers && !n.fractions)
        n.negative = false;
    return n;
}

static int numeric_compare(const unsigned char *a, size_t alen,
                           const unsigned char *b, size_t blen)
{
    // Compare decimal digits directly: long integers and tiny fractions
    // retain their ordering without integer overflow or floating rounding.
    number_t x = number(a, alen), y = number(b, blen);
    if (x.negative != y.negative)
        return x.negative ? -1 : 1;
    int result = 0;
    if (x.integers != y.integers)
        result = x.integers < y.integers ? -1 : 1;
    else
    {
        for (size_t i = 0; i < x.integers && !result; i++)
            result = (int)x.integer[i] - (int)y.integer[i];
        size_t count = x.fractions > y.fractions ? x.fractions : y.fractions;
        for (size_t i = 0; i < count && !result; i++)
            result = (i < x.fractions ? x.fraction[i] : '0') -
                     (i < y.fractions ? y.fraction[i] : '0');
    }
    return x.negative ? -result : result;
}

static int text_compare(const unsigned char *a, size_t alen,
                        const unsigned char *b, size_t blen, bool fold)
{
    size_t count = alen < blen ? alen : blen;
    for (size_t i = 0; i < count; i++)
    {
        unsigned char x = a[i], y = b[i];
        if (fold)
        {
            if (x >= 'a' && x <= 'z') x -= 'a' - 'A';
            if (y >= 'a' && y <= 'z') y -= 'a' - 'A';
        }
        if (x != y)
            return (int)x - (int)y;
    }
    return (alen > blen) - (alen < blen);
}

static int compare(const unsigned char *data, line_t a, line_t b,
                   const options_t *options)
{
    line_t x = a, y = b;
    if (options->blanks)
    {
        while (x.length && blank(data[x.offset])) { x.offset++; x.length--; }
        while (y.length && blank(data[y.offset])) { y.offset++; y.length--; }
    }
    int result = options->numeric
        ? numeric_compare(data + x.offset, x.length, data + y.offset, y.length)
        : text_compare(data + x.offset, x.length, data + y.offset, y.length,
                       options->fold);
    // Without -u, equal keys use the original whole line as a tie-breaker.
    // With -u, the stable merge keeps the first representative of each key.
    if (!result && !options->unique)
        result = text_compare(data + a.offset, a.length,
                              data + b.offset, b.length, false);
    return options->reverse ? -result : result;
}

static void merge_sort(line_t *lines, line_t *scratch, size_t count,
                       const unsigned char *data, const options_t *options)
{
    if (count < 2)
        return;
    size_t middle = count / 2;
    merge_sort(lines, scratch, middle, data, options);
    merge_sort(lines + middle, scratch + middle, count - middle, data, options);
    size_t left = 0, right = middle;
    for (size_t i = 0; i < count; i++)
        if (left < middle && (right == count ||
            compare(data, lines[left], lines[right], options) <= 0))
            scratch[i] = lines[left++];
        else
            scratch[i] = lines[right++];
    os64_memcpy(lines, scratch, count * sizeof(*lines));
}

static int write_all(const unsigned char *data, size_t length)
{
    while (length)
    {
        int64_t n = os64_write(OS64_STDOUT, data, length);
        if (n <= 0)
            return -1;
        data += n;
        length -= (size_t)n;
    }
    return 0;
}

static int output_lines(const buffer_t *buffer, const line_t *lines,
                         size_t count, const options_t *options)
{
    unsigned char output[IO_SIZE];
    size_t used = 0;
    for (size_t i = 0; i < count; i++)
    {
        if (options->unique && i &&
            compare(buffer->data, lines[i - 1], lines[i], options) == 0)
            continue;
        const unsigned char *data = buffer->data + lines[i].offset;
        size_t remaining = lines[i].length + 1; // Include the stored newline.
        while (remaining)
        {
            size_t chunk = sizeof(output) - used;
            if (chunk > remaining) chunk = remaining;
            os64_memcpy(output + used, data, chunk);
            used += chunk;
            data += chunk;
            remaining -= chunk;
            if (used == sizeof(output))
            {
                if (write_all(output, used) < 0) return -1;
                used = 0;
            }
        }
    }
    return write_all(output, used);
}

int main(int argc, char **argv)
{
    options_t options = {0};
    const os64_optspec_t specs[] = {
        {.letter = 'n', .name = "numeric-sort", .help = "compare leading decimal numbers"},
        {.letter = 'r', .name = "reverse", .help = "reverse the comparison order"},
        {.letter = 'u', .name = "unique", .help = "output the first line for each equal key"},
        {.letter = 'f', .name = "ignore-case", .help = "fold ASCII lowercase to uppercase"},
        {.letter = 'b', .name = "ignore-leading-blanks", .help = "ignore leading spaces and tabs"}
    };
    os64_args_t args;
    os64_args_init(&args, argc, argv, specs, sizeof(specs) / sizeof(specs[0]));
    args.about = "Sort lines from files or standard input to standard output.";
    args.details = "No FILE, or FILE -, reads stdin. Sorting is in memory, in byte order.\n"
                   "Numeric keys accept blanks, optional minus, digits and a decimal point;\n"
                   "missing numbers compare as zero. No exponent or locale processing.";
    int32_t token;
    size_t inputs = 0;
    while ((token = os64_args_next(&args)) != OS64_ARG_END)
    {
        switch (token)
        {
            case 'n': options.numeric = true; break;
            case 'r': options.reverse = true; break;
            case 'u': options.unique = true; break;
            case 'f': options.fold = true; break;
            case 'b': options.blanks = true; break;
            case OS64_ARG_POSITIONAL: inputs++; break;
            default:
                if (token != OS64_ARG_HELP)
                    os64_hprintf(OS64_STDERR, "sort: invalid option '%s'\n", args.value);
                os64_args_help(&args, "sort [-nrufb] [FILE ...]");
                return token == OS64_ARG_HELP ? 0 : 2;
        }
    }

    buffer_t buffer = {0};
    line_t *lines = NULL, *scratch = NULL;
    int result = 1;
    if (!inputs && read_input(&buffer, "-") < 0)
        goto done;
    os64_args_init(&args, argc, argv, specs, sizeof(specs) / sizeof(specs[0]));
    while ((token = os64_args_next(&args)) != OS64_ARG_END)
        if (token == OS64_ARG_POSITIONAL && read_input(&buffer, args.value) < 0)
            goto done;

    size_t count = 0;
    for (size_t i = 0; i < buffer.length; i++)
        if (buffer.data[i] == '\n') count++;
    if (count)
    {
        if (count > SIZE_MAX / sizeof(*lines)) goto memory_error;
        lines = os64_malloc(count * sizeof(*lines));
        scratch = os64_malloc(count * sizeof(*scratch));
        if (!lines || !scratch) goto memory_error;
        size_t start = 0, index = 0;
        for (size_t i = 0; i < buffer.length; i++)
            if (buffer.data[i] == '\n')
            {
                lines[index++] = (line_t){start, i - start};
                start = i + 1;
            }
        merge_sort(lines, scratch, count, buffer.data, &options);
    }
    if (output_lines(&buffer, lines, count, &options) < 0)
    {
        os64_hprintf(OS64_STDERR, "sort: error writing standard output\n");
        goto done;
    }
    result = 0;
    goto done;

memory_error:
    os64_hprintf(OS64_STDERR, "sort: out of memory\n");
done:
    os64_free(scratch);
    os64_free(lines);
    os64_free(buffer.data);
    return result;
}
