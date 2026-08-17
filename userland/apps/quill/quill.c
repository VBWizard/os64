// quill.c — os64's first text editor: one line at a time, no glass tricks.

#include "os64/os64.h"

#define QUILL_LINE_MAX 4096
#define QUILL_COMMAND_MAX 64
#define QUILL_PATH_MAX 512
#define QUILL_ESCAPE_WAIT_MS 20

typedef struct {
    char **lines;
    size_t count;
    size_t capacity;
    size_t current;
    bool trailingNewline;
    bool modified;
} quill_buffer_t;

typedef enum {
    INPUT_ACCEPT,
    INPUT_ACCEPT_END,
    INPUT_CANCEL,
    INPUT_END,
    INPUT_PREVIOUS,
    INPUT_NEXT,
    INPUT_EDIT
} input_result_t;

static int write_all(int32_t handle, const char *bytes, size_t length)
{
    size_t written = 0;
    while (written < length)
    {
        int64_t n = os64_write(handle, bytes + written, length - written);
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

static void caret_back(size_t count)
{
    static const char backspaces[] = "\b\b\b\b\b\b\b\b";
    while (count != 0)
    {
        size_t chunk = count > sizeof(backspaces) - 1
            ? sizeof(backspaces) - 1 : count;
        write_all(OS64_STDOUT, backspaces, chunk);
        count -= chunk;
    }
}

static void blank_forward(size_t count)
{
    static const char spaces[] = "        ";
    while (count != 0)
    {
        size_t chunk = count > sizeof(spaces) - 1
            ? sizeof(spaces) - 1 : count;
        write_all(OS64_STDOUT, spaces, chunk);
        count -= chunk;
    }
}

// Remove bytes from an input line and repaint only the tail. The console's
// backspace moves without erasing, so tail + blanks + walk-back is quill's
// complete inline-redraw vocabulary (the same proven gait husk uses).
static void input_delete(char *line, size_t *length, size_t *position,
                         size_t start, size_t count)
{
    caret_back(*position - start);
    for (size_t i = start; i + count < *length; i++)
        line[i] = line[i + count];
    *length -= count;
    *position = start;
    if (*position < *length)
        write_all(OS64_STDOUT, line + *position, *length - *position);
    blank_forward(count);
    caret_back(*length - *position + count);
}

// Editable one-line input. Enter accepts; a bare Escape cancels, or accepts
// the last line and ends when its caller requests that add-mode behavior.
// Escape is distinguished from an arrow by a short timed read because both
// begin with the same 1979 byte. The keyboard enqueues an arrow's whole
// sequence as one burst, so this waits only for a genuinely bare Escape.
static input_result_t input_line(const char *prompt, const char *initial,
                                 char *line, size_t capacity,
                                 bool commandNavigation,
                                 bool startAtFirstNonblank,
                                 bool acceptTextOnEscape)
{
    size_t length = os64_strcopy(line, capacity, initial);
    if (length >= capacity)
        return INPUT_CANCEL;
    size_t position = length;

    write_all(OS64_STDOUT, prompt, os64_strlen(prompt));
    if (length != 0)
        write_all(OS64_STDOUT, line, length);
    if (startAtFirstNonblank && length != 0)
    {
        position = 0;
        while (position < length &&
               (line[position] == ' ' || line[position] == '\t'))
            position++;
        if (position == length)       // an all-whitespace line starts at zero
            position = 0;
        caret_back(length - position);
    }

    for (;;)
    {
        char c;
        int64_t readResult = os64_read(OS64_STDIN, &c, 1);
        if (readResult == 0)
        {
            write_all(OS64_STDOUT, "\n", 1);
            return INPUT_END;
        }
        if (readResult != 1)
            continue;

        if (c == '\r' || c == '\n')
        {
            line[length] = '\0';
            write_all(OS64_STDOUT, "\n", 1);
            return INPUT_ACCEPT;
        }
        if (c == '\b' || c == 127)
        {
            if (position != 0)
                input_delete(line, &length, &position, position - 1, 1);
            continue;
        }
        if (c == 27)
        {
            char first;
            if (os64_read_for(OS64_STDIN, &first, 1,
                              QUILL_ESCAPE_WAIT_MS) != 1)
            {
                line[length] = '\0';
                write_all(OS64_STDOUT, "\n", 1);
                if (acceptTextOnEscape && length != 0)
                    return INPUT_ACCEPT_END;
                return INPUT_CANCEL;
            }
            if (first != '[')
                continue;

            char final;
            if (os64_read_for(OS64_STDIN, &final, 1,
                              QUILL_ESCAPE_WAIT_MS) != 1)
                continue;

            char parameter = 0;
            if (final >= '0' && final <= '9')
            {
                parameter = final;
                if (os64_read_for(OS64_STDIN, &final, 1,
                                  QUILL_ESCAPE_WAIT_MS) != 1 || final != '~')
                    continue;
            }

            // At an empty command prompt the vertical arrows ARE commands,
            // and Right opens the selected line. Once any command text is
            // present, arrows retain their ordinary inline-editing meaning.
            if (commandNavigation && length == 0 &&
                (final == 'A' || final == 'B' || final == 'C'))
            {
                write_all(OS64_STDOUT, "\n", 1);
                if (final == 'A')
                    return INPUT_PREVIOUS;
                if (final == 'B')
                    return INPUT_NEXT;
                return INPUT_EDIT;
            }

            if (final == 'D' && position != 0)             // Left
            {
                position--;
                write_all(OS64_STDOUT, "\b", 1);
            }
            else if (final == 'C' && position < length)    // Right
            {
                write_all(OS64_STDOUT, line + position, 1);
                position++;
            }
            else if (final == 'H')                         // Home
            {
                caret_back(position);
                position = 0;
            }
            else if (final == 'F')                         // End
            {
                if (position < length)
                {
                    write_all(OS64_STDOUT, line + position,
                              length - position);
                    position = length;
                }
            }
            else if (final == '~' && parameter == '3' &&  // Delete
                     position < length)
                input_delete(line, &length, &position, position, 1);
            continue;
        }
        if (c == 0x01)                                    // Ctrl+A
        {
            caret_back(position);
            position = 0;
            continue;
        }
        if (c == 0x05)                                    // Ctrl+E
        {
            if (position < length)
            {
                write_all(OS64_STDOUT, line + position, length - position);
                position = length;
            }
            continue;
        }
        if (c == 0x15)                                    // Ctrl+U
        {
            if (position != 0)
                input_delete(line, &length, &position, 0, position);
            continue;
        }
        if (c == 0x0b)                                    // Ctrl+K
        {
            if (position < length)
                input_delete(line, &length, &position, position,
                             length - position);
            continue;
        }
        if (c == 0x17)                                    // Ctrl+W
        {
            size_t start = position;
            while (start != 0 && line[start - 1] == ' ')
                start--;
            while (start != 0 && line[start - 1] != ' ')
                start--;
            if (start < position)
                input_delete(line, &length, &position, start,
                             position - start);
            continue;
        }
        if ((unsigned char)c < 0x20 && c != '\t')
            continue;
        if (length + 1 >= capacity)
            continue;

        for (size_t i = length; i > position; i--)
            line[i] = line[i - 1];
        line[position++] = c;
        length++;
        write_all(OS64_STDOUT, line + position - 1, length - position + 1);
        caret_back(length - position);
    }
}

static char *copy_line(const char *bytes, size_t length)
{
    char *copy = os64_malloc(length + 1);
    if (copy == NULL)
        return NULL;
    os64_memcpy(copy, bytes, length);
    copy[length] = '\0';
    return copy;
}

static bool reserve_lines(quill_buffer_t *buffer, size_t wanted)
{
    if (wanted <= buffer->capacity)
        return true;

    size_t capacity = buffer->capacity == 0 ? 16 : buffer->capacity;
    while (capacity < wanted)
    {
        if (capacity > SIZE_MAX / 2)
            return false;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*buffer->lines))
        return false;

    char **lines = os64_realloc(buffer->lines,
                                capacity * sizeof(*buffer->lines));
    if (lines == NULL)
        return false;
    buffer->lines = lines;
    buffer->capacity = capacity;
    return true;
}

static bool insert_line(quill_buffer_t *buffer, size_t index,
                        const char *line, size_t length)
{
    if (index > buffer->count || !reserve_lines(buffer, buffer->count + 1))
        return false;

    char *copy = copy_line(line, length);
    if (copy == NULL)
        return false;
    for (size_t i = buffer->count; i > index; i--)
        buffer->lines[i] = buffer->lines[i - 1];
    buffer->lines[index] = copy;
    buffer->count++;
    buffer->current = index;
    // Line edits preserve the file's existing final-newline convention.
    buffer->modified = true;
    return true;
}

static void destroy_buffer(quill_buffer_t *buffer)
{
    for (size_t i = 0; i < buffer->count; i++)
        os64_free(buffer->lines[i]);
    os64_free(buffer->lines);
}

static int load_file(const char *path, quill_buffer_t *buffer, bool *isNew)
{
    os64_dirent_t entry = {0};
    if (os64_stat(path, &entry) < 0)
    {
        *isNew = true;
        return 0;
    }
    if ((entry.flags & OS64_DE_DIR) != 0 || entry.size >= SIZE_MAX)
        return -1;

    int32_t handle = (int32_t)os64_open(path, "r");
    if (handle < 0)
        return -1;

    size_t size = (size_t)entry.size;
    char *bytes = os64_malloc(size + 1);
    if (bytes == NULL)
    {
        os64_close(handle);
        return -1;
    }

    size_t used = 0;
    while (used < size)
    {
        int64_t n = os64_read(handle, bytes + used, size - used);
        if (n <= 0)
        {
            os64_free(bytes);
            os64_close(handle);
            return -1;
        }
        used += (size_t)n;
    }
    if (os64_close(handle) < 0)
    {
        os64_free(bytes);
        return -1;
    }
    bytes[size] = '\0';

    for (size_t i = 0; i < size; i++)
        if (bytes[i] == '\0')
        {
            os64_free(bytes);
            return -1;              // quill edits text, never binary files
        }

    size_t start = 0;
    for (size_t i = 0; i < size; i++)
    {
        if (bytes[i] != '\n')
            continue;
        size_t length = i - start;
        if (length != 0 && bytes[start + length - 1] == '\r')
            length--;               // normalize a CRLF file when next written
        if (!insert_line(buffer, buffer->count, bytes + start, length))
        {
            os64_free(bytes);
            return -1;
        }
        start = i + 1;
    }
    if (start < size && !insert_line(buffer, buffer->count, bytes + start,
                                     size - start))
    {
        os64_free(bytes);
        return -1;
    }

    buffer->trailingNewline = size != 0 && bytes[size - 1] == '\n';
    buffer->modified = false;
    if (buffer->count != 0)
        buffer->current = 0;
    os64_free(bytes);
    *isNew = false;
    return 0;
}

static void print_line(const quill_buffer_t *buffer, size_t index,
                       bool markCurrent)
{
    os64_printf("%c%lu  ", markCurrent ? '>' : ' ', index + 1);
    write_all(OS64_STDOUT, buffer->lines[index],
              os64_strlen(buffer->lines[index]));
    write_all(OS64_STDOUT, "\n", 1);
}

static void print_current(const quill_buffer_t *buffer)
{
    if (buffer->count == 0)
        os64_puts("(empty)\n");
    else
        print_line(buffer, buffer->current, true);
}

static void list_lines(const quill_buffer_t *buffer)
{
    if (buffer->count == 0)
    {
        os64_puts("(empty)\n");
        return;
    }
    for (size_t i = 0; i < buffer->count; i++)
        print_line(buffer, i, i == buffer->current);
}

static void show_commands(void)
{
    os64_puts(
        "quill commands\n"
        "  p    print the current line\n"
        "  n / Down     move to and print the next line\n"
        "  - / Up       move to and print the previous line\n"
        "  a    add lines after the current line (Escape keeps text and stops)\n"
        "  i    insert lines before the current line (Escape keeps text and stops)\n"
        "  c / Right    change the current line (Escape cancels)\n"
        "  d    delete the current line\n"
        "  l    list the file with line numbers\n"
        "  w    write the file\n"
        "  q    quit (refuses unsaved changes)\n"
        "  q!   quit and discard unsaved changes\n"
        "  wq   write and quit\n"
        "  h    show this help\n");
}

static bool add_lines(quill_buffer_t *buffer, size_t index)
{
    char line[QUILL_LINE_MAX];
    bool added = false;

    os64_puts("Enter lines; press Escape to keep typed text and stop.\n");
    for (;;)
    {
        char prompt[32];
        int32_t wanted = os64_snprintf(prompt, sizeof(prompt), "%lu> ",
                                       index + 1);
        if (wanted < 0 || (size_t)wanted >= sizeof(prompt))
            return false;

        input_result_t result = input_line(prompt, "", line, sizeof(line),
                                           false, false, true);
        if (result != INPUT_ACCEPT && result != INPUT_ACCEPT_END)
            return result == INPUT_CANCEL ? true : added;
        if (!insert_line(buffer, index, line, os64_strlen(line)))
        {
            os64_hprintf(OS64_STDERR, "quill: out of memory adding line\n");
            return false;
        }
        index++;
        added = true;
        if (result == INPUT_ACCEPT_END)
            return true;
    }
}

static void change_line(quill_buffer_t *buffer)
{
    if (buffer->count == 0)
    {
        os64_puts("quill: no line to change; use a to add one\n");
        return;
    }
    if (os64_strlen(buffer->lines[buffer->current]) >= QUILL_LINE_MAX)
    {
        os64_puts("quill: current line is too long for interactive editing\n");
        return;
    }

    char prompt[32];
    char line[QUILL_LINE_MAX];
    int32_t wanted = os64_snprintf(prompt, sizeof(prompt), "%lu> ",
                                   buffer->current + 1);
    if (wanted < 0 || (size_t)wanted >= sizeof(prompt))
        return;
    if (input_line(prompt, buffer->lines[buffer->current], line,
                   sizeof(line), false, true, false) != INPUT_ACCEPT)
        return;

    char *replacement = copy_line(line, os64_strlen(line));
    if (replacement == NULL)
    {
        os64_hprintf(OS64_STDERR, "quill: out of memory changing line\n");
        return;
    }
    os64_free(buffer->lines[buffer->current]);
    buffer->lines[buffer->current] = replacement;
    buffer->modified = true;
}

static void delete_line(quill_buffer_t *buffer)
{
    if (buffer->count == 0)
    {
        os64_puts("quill: no line to delete\n");
        return;
    }

    os64_free(buffer->lines[buffer->current]);
    for (size_t i = buffer->current; i + 1 < buffer->count; i++)
        buffer->lines[i] = buffer->lines[i + 1];
    buffer->count--;
    if (buffer->current == buffer->count && buffer->current != 0)
        buffer->current--;
    buffer->modified = true;
    print_current(buffer);
}

static int save_file(const char *path, quill_buffer_t *buffer)
{
    char temporary[QUILL_PATH_MAX];
    int32_t wanted = os64_snprintf(temporary, sizeof(temporary),
                                   "%s.quill-save", path);
    if (wanted < 0 || (size_t)wanted >= sizeof(temporary))
    {
        os64_hprintf(OS64_STDERR, "quill: path is too long to save safely\n");
        return -1;
    }

    int32_t handle = (int32_t)os64_open(temporary, "w");
    if (handle < 0)
    {
        os64_hprintf(OS64_STDERR, "quill: cannot create save file '%s'\n",
                     temporary);
        return -1;
    }

    bool failed = false;
    for (size_t i = 0; i < buffer->count; i++)
    {
        size_t length = os64_strlen(buffer->lines[i]);
        if (write_all(handle, buffer->lines[i], length) < 0 ||
            ((i + 1 < buffer->count || buffer->trailingNewline) &&
             write_all(handle, "\n", 1) < 0))
        {
            failed = true;
            break;
        }
    }
    if (os64_close(handle) < 0)
        failed = true;

    if (failed || os64_rename(temporary, path) < 0)
    {
        os64_unlink(temporary);
        os64_hprintf(OS64_STDERR, "quill: could not save '%s'\n", path);
        return -1;
    }

    buffer->modified = false;
    os64_printf("%lu line%s written to '%s'\n", buffer->count,
                buffer->count == 1 ? "" : "s", path);
    return 0;
}

static int edit_file(const char *path)
{
    quill_buffer_t buffer = {0};
    bool isNew = false;
    if (load_file(path, &buffer, &isNew) < 0)
    {
        os64_hprintf(OS64_STDERR, "quill: cannot read text file '%s'\n", path);
        destroy_buffer(&buffer);
        return 1;
    }

    if (isNew)
    {
        // New text files use the conventional final newline on their lines.
        buffer.trailingNewline = true;
        os64_printf("New file: %s\n", path);
        add_lines(&buffer, 0);
    }
    else
    {
        os64_printf("%lu line%s read from '%s'\n", buffer.count,
                    buffer.count == 1 ? "" : "s", path);
        print_current(&buffer);
    }
    os64_puts("Type h for help.\n");

    char command[QUILL_COMMAND_MAX];
    bool endRefused = false;
    for (;;)
    {
        input_result_t input = input_line("quill> ", "", command,
                                          sizeof(command), true, false, false);
        if (input == INPUT_END)
        {
            if (buffer.modified && !endRefused)
            {
                os64_puts("quill: unsaved changes; press Ctrl+D again to discard them\n");
                endRefused = true;
                continue;
            }
            break;
        }
        endRefused = false;

        const char *verb;
        if (input == INPUT_PREVIOUS)
            verb = "-";
        else if (input == INPUT_NEXT)
            verb = "n";
        else if (input == INPUT_EDIT)
            verb = "c";
        else
        {
            if (input != INPUT_ACCEPT || command[0] == '\0')
                continue;
            // A leading colon is harmless hospitality to hands trained by vi.
            verb = command[0] == ':' ? command + 1 : command;
        }
        if (os64_streq(verb, "p"))
            print_current(&buffer);
        else if (os64_streq(verb, "n"))
        {
            if (buffer.count == 0)
                os64_puts("quill: no lines\n");
            else if (buffer.current + 1 == buffer.count)
                os64_puts("quill: already at the last line\n");
            else
            {
                buffer.current++;
                print_current(&buffer);
            }
        }
        else if (os64_streq(verb, "-"))
        {
            if (buffer.count == 0)
                os64_puts("quill: no lines\n");
            else if (buffer.current == 0)
                os64_puts("quill: already at the first line\n");
            else
            {
                buffer.current--;
                print_current(&buffer);
            }
        }
        else if (os64_streq(verb, "a"))
        {
            size_t index = buffer.count == 0 ? 0 : buffer.current + 1;
            add_lines(&buffer, index);
        }
        else if (os64_streq(verb, "i"))
        {
            size_t index = buffer.count == 0 ? 0 : buffer.current;
            add_lines(&buffer, index);
        }
        else if (os64_streq(verb, "c"))
            change_line(&buffer);
        else if (os64_streq(verb, "d"))
            delete_line(&buffer);
        else if (os64_streq(verb, "l"))
            list_lines(&buffer);
        else if (os64_streq(verb, "w"))
            save_file(path, &buffer);
        else if (os64_streq(verb, "wq"))
        {
            if (save_file(path, &buffer) == 0)
                break;
        }
        else if (os64_streq(verb, "q"))
        {
            if (buffer.modified)
                os64_puts("quill: unsaved changes; use w, wq, or q!\n");
            else
                break;
        }
        else if (os64_streq(verb, "q!"))
            break;
        else if (os64_streq(verb, "h") || os64_streq(verb, "?"))
            show_commands();
        else
            os64_printf("quill: unknown command '%s' (type h for help)\n",
                        command);
    }

    destroy_buffer(&buffer);
    return 0;
}

int main(int argc, char **argv)
{
    const char *paths[1] = {0};
    os64_args_t args = {0};

    os64_args_init(&args, argc, argv, NULL, 0);
    args.about = "Edit a text file one line at a time.";
    args.details = "New files enter line-add mode immediately. Press Escape to return to command mode.";
    int32_t pathCount = os64_args_parse(&args, "quill FILE", paths, 1);
    if (pathCount == OS64_ARG_HELP)
        return 0;
    if (pathCount < 0)
        return 2;
    if (pathCount != 1)
    {
        os64_hprintf(OS64_STDERR, "quill: missing file operand\n");
        os64_args_help(&args, "quill FILE");
        return 2;
    }

    return edit_file(paths[0]);
}
