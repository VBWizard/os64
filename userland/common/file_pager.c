#include "file_pager.h"
#include "os64/os64.h"

#define PAGER_READ_SIZE 32768
#define PAGER_PROMPT_WIDTH 48
#define PAGER_SEARCH_MAX 256

static char read_buffer[PAGER_READ_SIZE];

uint32_t file_pager_default_lines(uint32_t fallback)
{
    os64_tty_info_t tty;
    if (os64_tty_read(&tty) == 0 && tty.rows > 1)
        return tty.rows - 1;       // the last row belongs to the prompt
    return fallback;
}

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

static void show_help(const file_pager_options_t *options)
{
    write_all("\f", 1);
    os64_printf("%s key commands\n\n", options->program);
    os64_puts("  Space, f, Page Down   forward one page\n"
              "  Enter, j, Down        forward one line\n");
    if (options->mode == FILE_PAGER_LESS)
        os64_puts("  b, Page Up            backward one page\n"
                  "  k, Up                 backward one line\n"
                  "  g, Home               jump to the beginning\n"
                  "  G, End                jump to the end\n"
                  "  /pattern              search forward\n"
                  "  ?pattern              search backward\n"
                  "  n / N                 repeat / reverse search\n");
    os64_puts("  h                     show this help\n"
              "  q                     quit\n\n"
              "Press any key to return.");
    char ignored;
    os64_read(OS64_STDIN, &ignored, 1);
}

// Decode the terminal sequences emitted by both keyboard drivers. Return the
// pager's existing character vocabulary so navigation policy stays below.
static char read_key(void)
{
    char c;
    if (os64_read(OS64_STDIN, &c, 1) <= 0)
        return 'q';
    if (c != 27)
        return c;

    char bracket;
    if (os64_read(OS64_STDIN, &bracket, 1) != 1 || bracket != '[')
        return 0;
    char final;
    if (os64_read(OS64_STDIN, &final, 1) != 1)
        return 0;
    if (final == 'A') return 'k';
    if (final == 'B') return 'j';
    if (final == 'H') return 'g';
    if (final == 'F') return 'G';
    if (final == '5' || final == '6')
    {
        char tilde;
        if (os64_read(OS64_STDIN, &tilde, 1) == 1 && tilde == '~')
            return final == '5' ? 'b' : ' ';
    }
    return 0;
}

static char prompt(const file_pager_options_t *options, uint64_t line,
                   uint64_t total, const char *pattern, uint64_t match_line)
{
    if (total != 0)
    {
        uint64_t percent = line >= total ? 100 : line * 100 / total;
        if (pattern != NULL && *pattern != '\0')
            os64_printf("\r--%s-- %lu%%  match \"%s\" line %lu",
                        options->program, percent, pattern, match_line + 1);
        else
            os64_printf("\r--%s-- %lu%%", options->program, percent);
    }
    else
        os64_printf("\r--%s--", options->program);

    for (;;)
    {
        char c = read_key();
        bool search_key = options->mode == FILE_PAGER_LESS &&
            (c == '/' || c == '?' || c == 'n' || c == 'N');
        if (c == ' ' || c == 'f' || c == 'F' || c == 'b' || c == 'B' ||
            c == 'j' || c == 'J' || c == 'k' || c == 'K' || c == 'g' ||
            c == 'G' || c == 'h' || c == 'H' || c == 'q' || c == 'Q' ||
            search_key || c == '\n' || c == '\r')
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

// 1 = a new pattern, 0 = empty (reuse the stored pattern), -1 = cancelled.
static int read_search_pattern(char direction, char *pattern, size_t capacity)
{
    size_t length = 0;
    os64_printf("\r%c", direction);
    for (;;)
    {
        char c;
        if (os64_read(OS64_STDIN, &c, 1) <= 0)
            return -1;
        if (c == '\n' || c == '\r')
        {
            pattern[length] = '\0';
            erase_prompt();
            return length != 0 ? 1 : 0;
        }
        if (c == 27)
        {
            erase_prompt();
            return -1;
        }
        if (c == '\b' || c == 127)
        {
            if (length != 0)
            {
                length--;
                write_all("\b \b", 3);
            }
            continue;
        }
        if (c >= ' ' && c <= '~' && length + 1 < capacity)
        {
            pattern[length++] = c;
            write_all(&c, 1);
        }
    }
}

static bool bytes_match(const char *data, uint64_t size, uint64_t offset,
                        const char *pattern, size_t length)
{
    if (length == 0 || offset > size || length > size - offset)
        return false;
    for (size_t i = 0; i < length; i++)
        if (data[offset + i] != pattern[i])
            return false;
    return true;
}

static uint64_t line_for_offset(const char *data, uint64_t offset)
{
    uint64_t line = 0;
    for (uint64_t i = 0; i < offset; i++)
        if (data[i] == '\n')
            line++;
    return line;
}

static uint64_t newlines_between(const char *data, uint64_t start, uint64_t end)
{
    uint64_t count = 0;
    for (uint64_t i = start; i < end; i++)
        if (data[i] == '\n')
            count++;
    return count;
}

static bool search_data(const char *data, uint64_t size, const char *pattern,
                        bool forward, uint64_t start, uint64_t *found_offset)
{
    size_t length = os64_strlen(pattern);
    if (forward)
    {
        for (uint64_t i = start; i < size; i++)
            if (bytes_match(data, size, i, pattern, length))
            {
                *found_offset = i;
                return true;
            }
    }
    else
    {
        if (start == 0)
            return false;
        for (uint64_t i = start; i-- > 0; )
            if (bytes_match(data, size, i, pattern, length))
            {
                *found_offset = i;
                return true;
            }
    }
    return false;
}

static void search_failed(const char *pattern)
{
    os64_printf("\rPattern not found: %s", pattern);
    char ignored;
    os64_read(OS64_STDIN, &ignored, 1);
    erase_prompt();
}

static uint32_t displayed_rows(const char *data, uint64_t start, uint64_t end,
                               uint32_t cols, bool marked)
{
    if (cols == 0)
        return 1;
    uint32_t column = marked ? 2 : 0;
    uint32_t rows = 1;
    for (uint64_t i = start; i < end && data[i] != '\n'; i++)
    {
        if (data[i] == '\t')
            column = (column + 8) & ~7U;
        else if (data[i] != '\r')
            column++;
        if (column >= cols)
        {
            rows++;
            column = 0;
        }
    }
    return rows;
}

static uint64_t context_first_line(const char *data, uint64_t size,
                                   uint64_t match_offset, uint64_t match_line,
                                   uint32_t cols, uint32_t page_rows,
                                   uint64_t *first_offset)
{
    uint64_t match_start = match_offset;
    while (match_start > 0 && data[match_start - 1] != '\n')
        match_start--;

    // Near EOF there may not be two-thirds of a screen below the match. In
    // that case spend the unused rows on extra context above it so the page
    // remains full instead of putting the prompt halfway up the glass.
    uint32_t below_rows = 0;
    uint64_t cursor = match_start;
    while (cursor < size && below_rows < page_rows)
    {
        uint64_t end = cursor;
        while (end < size && data[end] != '\n') end++;
        if (end < size) end++;
        below_rows += displayed_rows(data, cursor, end, cols,
                                     cursor == match_start);
        cursor = end;
    }
    uint32_t target_rows = cursor == size && below_rows < page_rows
        ? page_rows - below_rows : page_rows / 3;

    uint64_t first = match_line;
    uint64_t start = match_start;
    uint32_t rows = 0;
    while (first > 0 && start > 0)
    {
        uint64_t previous_end = start;
        uint64_t previous_start = previous_end - 1;
        while (previous_start > 0 && data[previous_start - 1] != '\n')
            previous_start--;
        uint32_t needed = displayed_rows(data, previous_start, previous_end,
                                         cols, false);
        if (rows + needed > target_rows)
            break;
        rows += needed;
        first--;
        start = previous_start;
    }
    *first_offset = start;
    return first;
}

static int paint(const char *data, uint64_t size, uint64_t first,
                 uint32_t row_count, uint32_t cols, bool clear_screen,
                 uint64_t match_line, uint64_t start_override)
{
    uint64_t start = start_override != UINT64_MAX
        ? start_override : offset_for_line(data, size, first);
    uint64_t cursor = start;
    uint64_t line = first;
    uint32_t rows = 0;
    uint32_t lines = 0;

    if (clear_screen && write_all("\f", 1) < 0)
        return -1;
    while (cursor < size)
    {
        uint64_t end = cursor;
        while (end < size && data[end] != '\n')
            end++;
        if (end < size)
            end++;
        bool marked = line == match_line;
        uint32_t needed = displayed_rows(data, cursor, end, cols, marked);
        if (lines != 0 && rows + needed > row_count)
            break;
        if (marked && write_all("> ", 2) < 0)
            return -1;
        if (end > cursor && write_all(data + cursor, (size_t)(end - cursor)) < 0)
            return -1;
        if (end == size && data[end - 1] != '\n' && write_all("\n", 1) < 0)
            return -1;
        rows += needed;
        lines++;
        line++;
        cursor = end;
        if (rows >= row_count)
            break;
    }
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
    os64_tty_info_t tty = {0};
    uint32_t cols = 80;
    if (os64_tty_read(&tty) == 0 && tty.cols != 0)
        cols = tty.cols;
    uint64_t first = 0;
    bool first_paint = true;
    bool force_clear = false;
    char pattern[PAGER_SEARCH_MAX] = {0};
    bool search_forward = true;
    uint64_t match_offset = UINT64_MAX;
    uint64_t match_line = UINT64_MAX;
    uint64_t viewport_offset = UINT64_MAX;

    while (first < total)
    {
        int shown = paint(data, entry.size, first, options->page_lines, cols,
                          options->mode == FILE_PAGER_LESS || first_paint ||
                          force_clear, match_line, viewport_offset);
        first_paint = false;
        force_clear = false;
        if (shown < 0)
        {
            os64_unmap(data);
            return 1;
        }
        bool at_end = first + (uint64_t)shown >= total;
        if (at_end && options->mode == FILE_PAGER_MORE)
            break;

        char key = prompt(options, first + (uint64_t)shown, total,
                          match_offset != UINT64_MAX ? pattern : NULL,
                          match_line);
        if (key == 'q' || key == 'Q')
            break;
        if (key == 'h' || key == 'H')
        {
            show_help(options);
            force_clear = true;
            continue;
        }
        if (options->mode == FILE_PAGER_LESS &&
            (key == '/' || key == '?' || key == 'n' || key == 'N'))
        {
            if (key == '/' || key == '?')
            {
                char entered[PAGER_SEARCH_MAX] = {0};
                int pattern_result = read_search_pattern(key, entered,
                                                         sizeof(entered));
                if (pattern_result < 0 ||
                    (pattern_result == 0 && pattern[0] == '\0'))
                    continue;
                if (pattern_result > 0)
                    os64_strcopy(pattern, sizeof(pattern), entered);
                search_forward = key == '/';
            }
            else if (pattern[0] == '\0' || match_offset == UINT64_MAX)
                continue;

            bool direction = key == 'N' ? !search_forward : search_forward;
            uint64_t start;
            if (key == '/' || key == '?')
                start = direction
                    ? offset_for_line(data, entry.size, first)
                    : offset_for_line(data, entry.size,
                                      first + (uint64_t)shown);
            else if (direction)
                start = match_offset < entry.size ? match_offset + 1 : entry.size;
            else
                start = match_offset;

            uint64_t found;
            if (search_data(data, entry.size, pattern, direction, start, &found))
            {
                uint64_t found_line;
                if (match_offset == UINT64_MAX)
                    found_line = line_for_offset(data, found);
                else if (found >= match_offset)
                    found_line = match_line +
                        newlines_between(data, match_offset, found);
                else
                    found_line = match_line -
                        newlines_between(data, found, match_offset);
                match_offset = found;
                match_line = found_line;
                first = context_first_line(data, entry.size, match_offset,
                                           match_line, cols,
                                           options->page_lines,
                                           &viewport_offset);
            }
            else
                search_failed(pattern);
            continue;
        }

        uint64_t step = (key == '\n' || key == '\r' || key == 'j' || key == 'J' ||
                         key == 'k' || key == 'K')
            ? 1 : (uint64_t)shown;
        viewport_offset = UINT64_MAX;
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
