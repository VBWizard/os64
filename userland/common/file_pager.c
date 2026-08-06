#include "file_pager.h"
#include "os64/os64.h"

#define PAGER_READ_SIZE 32768
#define PAGER_PROMPT_WIDTH 48

static char read_buffer[PAGER_READ_SIZE];

bool file_pager_parse_lines(const char *text, uint32_t *lines)
{
    if (text == NULL || *text == '\0')
        return false;

    uint64_t value = 0;
    for (const char *p = text; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10 + (uint64_t)(*p - '0');
        if (value > UINT32_MAX)
            return false;
    }
    if (value == 0)
        return false;
    *lines = (uint32_t)value;
    return true;
}

static int write_all(const char *bytes, size_t length)
{
    size_t done = 0;
    while (done < length)
    {
        int64_t n = os64_write(OS64_STDOUT, bytes + done, length - done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static void erase_prompt(void)
{
    static const char blank[PAGER_PROMPT_WIDTH + 1] =
        "                                                ";
    os64_write(OS64_STDOUT, "\r", 1);
    os64_write(OS64_STDOUT, blank, PAGER_PROMPT_WIDTH);
    os64_write(OS64_STDOUT, "\r", 1);
}

static char prompt(const file_pager_options_t *options, uint64_t line,
                   uint64_t total)
{
    if (total != 0)
    {
        uint64_t percent = line >= total ? 100 : line * 100 / total;
        os64_printf("\r--%s-- %lu%%", options->program, percent);
    }
    else
        os64_printf("\r--%s--", options->program);

    for (;;)
    {
        char c;
        int64_t n = os64_read(OS64_STDIN, &c, 1);
        if (n <= 0)
            return 'q';
        if (c == 27)
        {
            // Arrow keys arrive as ESC '[' final. Consume a complete sequence
            // synchronously; a lone Escape is simply ignored in this slice.
            char sequence[2];
            if (os64_read(OS64_STDIN, sequence, 2) == 2 && sequence[0] == '[')
            {
                if (sequence[1] == 'A') c = 'k';
                else if (sequence[1] == 'B') c = 'j';
                else continue;
            }
            else
                continue;
        }
        if (c == ' ' || c == 'f' || c == 'F' || c == 'b' || c == 'B' ||
            c == 'j' || c == 'J' || c == 'k' || c == 'K' || c == 'g' ||
            c == 'G' || c == 'q' || c == 'Q' || c == '\n' || c == '\r')
        {
            erase_prompt();
            return c;
        }
    }
}

static int load_file(int32_t handle, uint64_t size, char **data_out)
{
    if (size >= SIZE_MAX)
        return -1;
    char *data = os64_map((size_t)size + 1);
    if (data == NULL)
        return -1;

    uint64_t used = 0;
    while (used < size)
    {
        size_t request = size - used > sizeof(read_buffer)
            ? sizeof(read_buffer) : (size_t)(size - used);
        int64_t n = os64_read(handle, read_buffer, request);
        if (n <= 0)
        {
            os64_unmap(data);
            return -1;
        }
        for (int64_t i = 0; i < n; i++)
            data[used + (uint64_t)i] = read_buffer[i];
        used += (uint64_t)n;
    }
    data[size] = '\0';
    *data_out = data;
    return 0;
}

static uint64_t count_lines(const char *data, uint64_t size)
{
    if (size == 0)
        return 0;
    uint64_t lines = 1;
    for (uint64_t i = 0; i < size; i++)
        if (data[i] == '\n' && i + 1 < size)
            lines++;
    return lines;
}

static uint64_t offset_for_line(const char *data, uint64_t size,
                                uint64_t wanted)
{
    if (wanted == 0)
        return 0;
    uint64_t line = 0;
    for (uint64_t i = 0; i < size; i++)
        if (data[i] == '\n' && ++line == wanted)
            return i + 1;
    return size;
}

static int paint(const char *data, uint64_t size, uint64_t first,
                 uint32_t count, bool clear_screen)
{
    uint64_t start = offset_for_line(data, size, first);
    uint64_t end = start;
    uint32_t lines = 0;
    while (end < size && lines < count)
    {
        if (data[end++] == '\n')
            lines++;
    }
    if (end == size && start < size && (end == 0 || data[end - 1] != '\n'))
        lines++;

    if (clear_screen && write_all("\f", 1) < 0)
        return -1;
    if (end > start && write_all(data + start, (size_t)(end - start)) < 0)
        return -1;
    if (end > start && data[end - 1] != '\n' && write_all("\n", 1) < 0)
        return -1;
    return (int)lines;
}

int file_pager_run(const file_pager_options_t *options)
{
    os64_dirent_t entry = {0};
    if (os64_stat(options->path, &entry) < 0)
    {
        os64_hprintf(OS64_STDERR, "%s: cannot access %s\n",
                     options->program, options->path);
        return 1;
    }
    if (entry.flags & OS64_DE_DIR)
    {
        os64_hprintf(OS64_STDERR, "%s: %s is a directory\n",
                     options->program, options->path);
        return 1;
    }

    int32_t handle = (int32_t)os64_open(options->path, "r");
    if (handle < 0)
    {
        os64_hprintf(OS64_STDERR, "%s: cannot open %s\n",
                     options->program, options->path);
        return 1;
    }

    char *data = NULL;
    int loaded = load_file(handle, entry.size, &data);
    os64_close(handle);
    if (loaded < 0)
    {
        os64_hprintf(OS64_STDERR, "%s: error reading %s\n",
                     options->program, options->path);
        return 1;
    }

    uint64_t total = count_lines(data, entry.size);
    uint64_t first = 0;
    bool first_paint = true;

    while (first < total)
    {
        int shown = paint(data, entry.size, first, options->page_lines,
                          options->mode == FILE_PAGER_LESS || first_paint);
        first_paint = false;
        if (shown < 0)
        {
            os64_unmap(data);
            return 1;
        }
        bool at_end = first + (uint64_t)shown >= total;
        if (at_end && options->mode == FILE_PAGER_MORE)
            break;

        char key = prompt(options, first + (uint64_t)shown, total);
        if (key == 'q' || key == 'Q')
            break;

        uint64_t step = (key == '\n' || key == '\r' || key == 'j' || key == 'J' ||
                         key == 'k' || key == 'K')
            ? 1 : options->page_lines;
        if (options->mode == FILE_PAGER_MORE)
        {
            if (key == 'b' || key == 'B' || key == 'k' || key == 'K' ||
                key == 'g' || key == 'G')
                continue;
            first += step;
            continue;
        }

        if (key == 'b' || key == 'B' || key == 'k' || key == 'K')
            first = first > step ? first - step : 0;
        else if (key == 'g')
            first = 0;
        else if (key == 'G')
            first = total > options->page_lines ? total - options->page_lines : 0;
        else
        {
            uint64_t last = total > options->page_lines
                ? total - options->page_lines : 0;
            first = first + step > last ? last : first + step;
        }
    }

    os64_unmap(data);
    return 0;
}
