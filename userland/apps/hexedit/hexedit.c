// hexedit — a windowed, 64-bit terminal hex editor for os64.
//
// The file/cache/editing machinery lives in common/hex_buffer.c. This file is
// only the terminal policy: layout, keys, prompts and the visible cursor.

#include "os64/os64.h"
#include "../../common/hex_buffer.h"

#define ESCAPE_WAIT_MS 20u
#define PROMPT_MAX 160u
#define SEARCH_MAX 64u
#define HEX_PATH_MAX 256u

typedef enum {
    KEY_CHARACTER,
    KEY_ESCAPE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_DELETE
} key_kind_t;

typedef struct {
    key_kind_t kind;
    uint8_t ch;
} key_t;

typedef struct {
    hex_buffer_t buffer;
    const char *path;
    int32_t keys;
    uint64_t cursor;
    uint64_t view_start;
    uint32_t rows;
    uint32_t cols;
    uint32_t bytes_per_row;
    uint32_t data_rows;
    uint8_t nibble;
    bool ascii_mode;
    bool confirm_quit;
    char status[PROMPT_MAX];
    // WHOSE memory this is, for the header: /proc/53/mem names a number, and
    // a number is not an answer at 2am. Filled from the sibling cmdline
    // file's first line (argv[0] as typed, basename'd) when the path is a
    // task's mem; empty otherwise (Chris's "one thing", 2026-08-22).
    char task_name[48];
    uint8_t search[SEARCH_MAX];
    size_t search_length;
} editor_t;

static int write_all(int32_t handle, const char *data, size_t count)
{
    size_t written = 0;
    while (written < count) {
        int64_t n = os64_write(handle, data + written, count - written);
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

static key_t read_key(int32_t handle)
{
    uint8_t c;
    if (os64_read(handle, &c, 1) != 1)
        return (key_t){KEY_ESCAPE, 0};
    if (c != 27)
        return (key_t){KEY_CHARACTER, c};

    uint8_t next;
    if (os64_read_for(handle, &next, 1, ESCAPE_WAIT_MS) != 1)
        return (key_t){KEY_ESCAPE, 0};
    if (next != '[')
        return (key_t){KEY_ESCAPE, 0};
    if (os64_read_for(handle, &next, 1, ESCAPE_WAIT_MS) != 1)
        return (key_t){KEY_ESCAPE, 0};

    if (next == 'A') return (key_t){KEY_UP, 0};
    if (next == 'B') return (key_t){KEY_DOWN, 0};
    if (next == 'C') return (key_t){KEY_RIGHT, 0};
    if (next == 'D') return (key_t){KEY_LEFT, 0};
    if (next == 'H') return (key_t){KEY_HOME, 0};
    if (next == 'F') return (key_t){KEY_END, 0};

    if (next >= '0' && next <= '9') {
        uint8_t final;
        if (os64_read_for(handle, &final, 1, ESCAPE_WAIT_MS) == 1 &&
            final == '~') {
            if (next == '3') return (key_t){KEY_DELETE, 0};
            if (next == '5') return (key_t){KEY_PAGE_UP, 0};
            if (next == '6') return (key_t){KEY_PAGE_DOWN, 0};
        }
    }
    return (key_t){KEY_ESCAPE, 0};
}

static void set_status(editor_t *editor, const char *message)
{
    os64_strcopy(editor->status, sizeof(editor->status), message);
}

// Userland can recover the same canonical spelling open() uses from the
// kernel-owned canonical cwd. os64 has no links, so lexical normalization is
// the whole realpath problem: collapse '/', discard '.', and pop on '..'.
static int canonical_path(const char *path, char *out, size_t capacity)
{
    char cwd[HEX_PATH_MAX];
    const char *sources[2];
    size_t source_count = 0;
    size_t length = 0;

    if (path == NULL || out == NULL || capacity < 2)
        return -1;
    if (path[0] != '/') {
        if (os64_getcwd(cwd, sizeof(cwd)) < 0)
            return -1;
        sources[source_count++] = cwd;
    }
    sources[source_count++] = path;

    for (size_t source = 0; source < source_count; source++) {
        const char *p = sources[source];
        while (*p != '\0') {
            while (*p == '/')
                p++;
            if (*p == '\0')
                break;
            const char *start = p;
            while (*p != '\0' && *p != '/')
                p++;
            size_t component = (size_t)(p - start);

            if (component == 1 && start[0] == '.')
                continue;
            if (component == 2 && start[0] == '.' && start[1] == '.') {
                while (length > 0 && out[length - 1] != '/')
                    length--;
                if (length > 0)
                    length--;
                continue;
            }
            if (length + component + 2 > capacity)
                return -1;
            out[length++] = '/';
            for (size_t i = 0; i < component; i++)
                out[length++] = start[i];
        }
    }
    if (length == 0)
        out[length++] = '/';
    out[length] = '\0';
    return 0;
}

static void move_to(editor_t *editor, uint64_t offset)
{
    if (editor->buffer.size == 0) {
        editor->cursor = 0;
        return;
    }
    editor->cursor = offset < editor->buffer.size
        ? offset : editor->buffer.size - 1;
    editor->nibble = 0;
}

static void keep_cursor_visible(editor_t *editor)
{
    uint64_t row = editor->cursor - editor->cursor % editor->bytes_per_row;
    uint64_t span = (uint64_t)editor->bytes_per_row * editor->data_rows;
    if (row < editor->view_start)
        editor->view_start = row;
    else if (span != 0 && row >= editor->view_start + span)
        editor->view_start = row -
            (uint64_t)(editor->data_rows - 1) * editor->bytes_per_row;
}

static char printable(uint8_t value)
{
    return value >= 32 && value <= 126 ? (char)value : '.';
}

static bool address_space_path(const char *path)
{
    if (os64_streq(path, "/dev/zero") || os64_streq(path, "/dev/full"))
        return true;
    return hex_buffer_is_proc_mem_path(path);
}

static void render(editor_t *editor)
{
    keep_cursor_visible(editor);
    write_all(OS64_STDOUT, "\f", 1);
    const hex_map_t *map = hex_buffer_map_at(&editor->buffer, editor->cursor);
    // "/proc/53/mem (husk)" — the task's name rides beside its number.
    char whose[HEX_PATH_MAX + 56];
    if (editor->task_name[0] != '\0')
        os64_snprintf(whose, sizeof(whose), "%s (%s)", editor->path, editor->task_name);
    else
        os64_snprintf(whose, sizeof(whose), "%s", editor->path);
    if (editor->buffer.address_space && map != NULL)
        os64_printf("hexedit  %s  %016llx-%016llx %s %s%s%s  [%s]\n",
                    whose, (unsigned long long)map->start,
                    (unsigned long long)map->end, map->permissions,
                    map->description, editor->buffer.read_only ? "  RO" : "",
                    hex_buffer_modified(&editor->buffer) ? "  modified" : "",
                    editor->ascii_mode ? "ASCII" : "HEX");
    else if (editor->buffer.address_space)
        os64_printf("hexedit  %s  address space%s%s  [%s]\n",
                    whose, editor->buffer.read_only ? "  RO" : "",
                    hex_buffer_modified(&editor->buffer) ? "  modified" : "",
                    editor->ascii_mode ? "ASCII" : "HEX");
    else
        os64_printf("hexedit  %s  size 0x%016llx%s%s  [%s]\n",
                    editor->path, (unsigned long long)editor->buffer.size,
                    editor->buffer.read_only ? "  RO" : "",
                    hex_buffer_modified(&editor->buffer) ? "  modified" : "",
                    editor->ascii_mode ? "ASCII" : "HEX");

    for (uint32_t row = 0; row < editor->data_rows; row++) {
        uint64_t offset = editor->view_start +
                          (uint64_t)row * editor->bytes_per_row;
        if (offset >= editor->buffer.size) {
            os64_puts("\n");
            continue;
        }

        char line[192];
        size_t used = (size_t)os64_snprintf(line, sizeof(line),
                                            "%016llx ",
                                            (unsigned long long)offset);
        for (uint32_t col = 0; col < editor->bytes_per_row; col++) {
            uint64_t here = offset + col;
            uint8_t value = 0;
            int result = here < editor->buffer.size
                ? hex_buffer_get(&editor->buffer, here, &value)
                : HEX_BUFFER_ERROR;
            if (result == HEX_BUFFER_OK)
                used += (size_t)os64_snprintf(line + used, sizeof(line) - used,
                                              "%c%02x",
                                              here == editor->cursor ? '>' : ' ',
                                              value);
            else if (result == HEX_BUFFER_UNAVAILABLE)
                used += (size_t)os64_snprintf(line + used, sizeof(line) - used,
                                              "%c??",
                                              here == editor->cursor ? '>' : ' ');
            else
                used += (size_t)os64_snprintf(line + used, sizeof(line) - used,
                                              "   ");
        }
        used += (size_t)os64_snprintf(line + used, sizeof(line) - used, "  |");
        for (uint32_t col = 0; col < editor->bytes_per_row; col++) {
            uint64_t here = offset + col;
            uint8_t value;
            int result = here < editor->buffer.size
                ? hex_buffer_get(&editor->buffer, here, &value)
                : HEX_BUFFER_ERROR;
            line[used++] = result == HEX_BUFFER_OK ? printable(value) :
                           result == HEX_BUFFER_UNAVAILABLE ? '?' : ' ';
        }
        line[used++] = '|';
        line[used++] = '\n';
        line[used] = '\0';
        write_all(OS64_STDOUT, line, used);
    }

    if (editor->status[0] != '\0')
        os64_printf("%s\n", editor->status);
    else if (editor->buffer.address_space)
        os64_printf("address 0x%016llx\n",
                    (unsigned long long)editor->cursor);
    else
        os64_printf("offset 0x%016llx  byte %llu of %llu\n",
                    (unsigned long long)editor->cursor,
                    (unsigned long long)(editor->buffer.size == 0
                                             ? 0 : editor->cursor + 1),
                    (unsigned long long)editor->buffer.size);
    if (editor->buffer.map_count != 0)
        os64_puts("arrows/Pg move  [ ] maps  m reload  Tab mode  ^G goto  ^F find  ^Z undo  ^S save  ^Q quit");
    else
        os64_puts("arrows move  PgUp/PgDn  Tab hex/ascii  ^G goto  ^F find  ^Z undo  ^S save  ^Q quit");
}

static bool prompt_text(editor_t *editor, const char *prompt,
                        char *text, size_t capacity)
{
    write_all(OS64_STDOUT, "\f", 1);
    os64_printf("%s", prompt);
    size_t length = 0;
    for (;;) {
        key_t key = read_key(editor->keys);
        if (key.kind == KEY_ESCAPE)
            return false;
        if (key.kind != KEY_CHARACTER)
            continue;
        if (key.ch == '\r' || key.ch == '\n') {
            text[length] = '\0';
            return true;
        }
        if (key.ch == '\b' || key.ch == 127) {
            if (length != 0) {
                length--;
                write_all(OS64_STDOUT, "\b \b", 3);
            }
            continue;
        }
        if (key.ch >= 32 && key.ch <= 126 && length + 1 < capacity) {
            text[length++] = (char)key.ch;
            write_all(OS64_STDOUT, (const char *)&key.ch, 1);
        }
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_offset(const char *text, uint64_t *value)
{
    uint32_t base = 16;
    const char *p = text;
    if (p[0] == '#') {
        base = 10;
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (*p == '\0')
        return false;

    uint64_t result = 0;
    while (*p != '\0') {
        int digit = hex_value(*p++);
        if (digit < 0 || (uint32_t)digit >= base ||
            result > (UINT64_MAX - (uint32_t)digit) / base)
            return false;
        result = result * base + (uint32_t)digit;
    }
    *value = result;
    return true;
}

static void goto_prompt(editor_t *editor)
{
    char text[40];
    if (!prompt_text(editor,
                     "goto offset (hex, 0x optional; # introduces decimal): ",
                     text, sizeof(text))) {
        set_status(editor, "goto cancelled");
        return;
    }
    uint64_t offset;
    if (!parse_offset(text, &offset) || offset >= editor->buffer.size) {
        set_status(editor, editor->buffer.address_space
                              ? "invalid or unsupported address"
                              : "invalid offset or beyond end of file");
        return;
    }
    move_to(editor, offset);
    set_status(editor, "");
}

static bool parse_search(const char *text, uint8_t *pattern, size_t *length)
{
    if (text[0] == 't' && text[1] == ':') {
        size_t count = os64_strlen(text + 2);
        if (count == 0 || count > SEARCH_MAX)
            return false;
        for (size_t i = 0; i < count; i++)
            pattern[i] = (uint8_t)text[i + 2];
        *length = count;
        return true;
    }

    size_t count = 0;
    int high = -1;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == ' ' || *p == '\t')
            continue;
        int digit = hex_value(*p);
        if (digit < 0)
            return false;
        if (high < 0)
            high = digit;
        else {
            if (count == SEARCH_MAX)
                return false;
            pattern[count++] = (uint8_t)((high << 4) | digit);
            high = -1;
        }
    }
    if (high >= 0 || count == 0)
        return false;
    *length = count;
    return true;
}

static void find_next(editor_t *editor)
{
    if (editor->search_length == 0 ||
        editor->search_length > editor->buffer.size) {
        set_status(editor, "no search pattern");
        return;
    }

    uint64_t start = editor->cursor < UINT64_MAX ? editor->cursor + 1 : 0;
    uint64_t last;
    if (editor->buffer.address_space) {
        const hex_map_t *map = hex_buffer_map_at(&editor->buffer,
                                                  editor->cursor);
        if (map == NULL) {
            map = hex_buffer_next_map(&editor->buffer, editor->cursor);
            if (map != NULL)
                start = map->start;
        }
        if (map == NULL || editor->search_length > map->end - map->start) {
            set_status(editor, editor->buffer.map_count == 0
                                  ? "address-space search requires maps"
                                  : "no mapped search range at this address");
            return;
        }
        if (start < map->start)
            start = map->start;
        last = map->end - editor->search_length;
    } else {
        last = editor->buffer.size - editor->search_length;
    }

    for (uint64_t at = start; at <= last; at++) {
        bool match = true;
        for (size_t i = 0; i < editor->search_length; i++) {
            uint8_t value;
            if (hex_buffer_get(&editor->buffer, at + i, &value) !=
                    HEX_BUFFER_OK ||
                value != editor->search[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            move_to(editor, at);
            os64_snprintf(editor->status, sizeof(editor->status),
                          "match at 0x%016llx", (unsigned long long)at);
            return;
        }
    }
    set_status(editor, editor->buffer.address_space
                           ? "pattern not found in this mapping"
                           : "pattern not found after cursor");
}

static void jump_map(editor_t *editor, bool forward)
{
    const hex_map_t *map = forward
        ? hex_buffer_next_map(&editor->buffer, editor->cursor)
        : hex_buffer_previous_map(&editor->buffer, editor->cursor);
    if (map == NULL) {
        set_status(editor, forward ? "no later mapping" : "no earlier mapping");
        return;
    }
    move_to(editor, map->start);
    editor->view_start = map->start - map->start % editor->bytes_per_row;
    os64_snprintf(editor->status, sizeof(editor->status),
                  "%s map 0x%016llx-0x%016llx %s %s",
                  forward ? "next" : "previous",
                  (unsigned long long)map->start,
                  (unsigned long long)map->end,
                  map->permissions, map->description);
}

static void reload_maps(editor_t *editor)
{
    int count = hex_buffer_reload_maps(&editor->buffer);
    if (count < 0) {
        set_status(editor, "could not reload companion maps file");
        return;
    }
    hex_buffer_refresh(&editor->buffer);
    os64_snprintf(editor->status, sizeof(editor->status),
                  "reloaded %d mapping%s", count, count == 1 ? "" : "s");
}

static void search_prompt(editor_t *editor)
{
    char text[PROMPT_MAX];
    if (!prompt_text(editor,
                     "find bytes (example: 7f 45 4c 46) or text (t:hello): ",
                     text, sizeof(text))) {
        set_status(editor, "search cancelled");
        return;
    }
    if (!parse_search(text, editor->search, &editor->search_length)) {
        editor->search_length = 0;
        set_status(editor, "invalid or too-long search pattern");
        return;
    }
    find_next(editor);
}

static void move_key(editor_t *editor, key_kind_t key)
{
    uint64_t page = (uint64_t)editor->bytes_per_row * editor->data_rows;
    switch (key) {
        case KEY_LEFT:
            if (editor->cursor != 0) move_to(editor, editor->cursor - 1);
            break;
        case KEY_RIGHT:
            if (editor->cursor + 1 < editor->buffer.size)
                move_to(editor, editor->cursor + 1);
            break;
        case KEY_UP:
            move_to(editor, editor->cursor >= editor->bytes_per_row
                                ? editor->cursor - editor->bytes_per_row : 0);
            break;
        case KEY_DOWN:
            if (editor->cursor <= UINT64_MAX - editor->bytes_per_row)
                move_to(editor, editor->cursor + editor->bytes_per_row);
            break;
        case KEY_PAGE_UP:
            move_to(editor, editor->cursor >= page ? editor->cursor - page : 0);
            break;
        case KEY_PAGE_DOWN:
            if (editor->cursor <= UINT64_MAX - page)
                move_to(editor, editor->cursor + page);
            break;
        case KEY_HOME: {
            const hex_map_t *map = hex_buffer_map_at(&editor->buffer,
                                                      editor->cursor);
            move_to(editor, map != NULL ? map->start : 0);
            break;
        }
        case KEY_END:
            if (editor->buffer.size != 0) {
                const hex_map_t *map = hex_buffer_map_at(&editor->buffer,
                                                          editor->cursor);
                move_to(editor, map != NULL ? map->end - 1
                                             : editor->buffer.size - 1);
            }
            break;
        default: break;
    }
}

static void edit_hex(editor_t *editor, uint8_t ch)
{
    int digit = hex_value((char)ch);
    if (digit < 0 || editor->buffer.size == 0)
        return;
    uint8_t old;
    if (hex_buffer_get(&editor->buffer, editor->cursor, &old) != HEX_BUFFER_OK)
        return;   // an unavailable byte (??) has no nibbles to edit
    uint8_t value = editor->nibble == 0
        ? (uint8_t)((old & 0x0f) | (digit << 4))
        : (uint8_t)((old & 0xf0) | digit);
    if (hex_buffer_edit(&editor->buffer, editor->cursor, value) < 0) {
        set_status(editor, "edit failed (read-only or out of memory)");
        return;
    }
    if (editor->nibble == 0)
        editor->nibble = 1;
    else {
        editor->nibble = 0;
        if (editor->cursor + 1 < editor->buffer.size)
            editor->cursor++;
    }
}

static bool handle_character(editor_t *editor, uint8_t ch)
{
    if (ch == '\t') {
        editor->ascii_mode = !editor->ascii_mode;
        editor->nibble = 0;
        return true;
    }
    if (ch == 0x07) { goto_prompt(editor); return true; } // Ctrl+G
    if (ch == 0x06) { search_prompt(editor); return true; } // Ctrl+F
    if (ch == 0x1a) { // Ctrl+Z
        int undone = hex_buffer_undo(&editor->buffer);
        set_status(editor, undone > 0 ? "undid last edit" : "nothing to undo");
        return true;
    }
    if (ch == 0x13) { // Ctrl+S
        if (!hex_buffer_modified(&editor->buffer))
            set_status(editor, "nothing to save");
        else if (hex_buffer_save(&editor->buffer) < 0)
            set_status(editor, "save failed; edits remain in memory");
        else
            set_status(editor, "saved and synced");
        return true;
    }
    if (ch == 0x11) { // Ctrl+Q
        if (hex_buffer_modified(&editor->buffer) && !editor->confirm_quit) {
            set_status(editor, "unsaved edits: press Ctrl+Q again to discard, or Ctrl+S to save");
            editor->confirm_quit = true;
            return true;
        }
        return false;
    }

    editor->confirm_quit = false;
    if (editor->ascii_mode) {
        if (ch >= 32 && ch <= 126 && editor->buffer.size != 0) {
            if (hex_buffer_edit(&editor->buffer, editor->cursor, ch) < 0)
                set_status(editor, "edit failed (read-only or out of memory)");
            else if (editor->cursor + 1 < editor->buffer.size)
                editor->cursor++;
        }
        return true;
    }

    if (hex_value((char)ch) >= 0) edit_hex(editor, ch);
    else if (ch == 'g') goto_prompt(editor);
    else if (ch == '/') search_prompt(editor);
    else if (ch == 'n') find_next(editor);
    else if (ch == ']') jump_map(editor, true);
    else if (ch == '[') jump_map(editor, false);
    else if (ch == 'm' && editor->buffer.maps_path != NULL) reload_maps(editor);
    else if (ch == 'u') {
        int undone = hex_buffer_undo(&editor->buffer);
        set_status(editor, undone > 0 ? "undid last edit" : "nothing to undo");
    } else if (ch == 'w') {
        if (!hex_buffer_modified(&editor->buffer))
            set_status(editor, "nothing to save");
        else if (hex_buffer_save(&editor->buffer) < 0)
            set_status(editor, "save failed; edits remain in memory");
        else
            set_status(editor, "saved and synced");
    } else if (ch == 'q') {
        if (hex_buffer_modified(&editor->buffer) && !editor->confirm_quit) {
            set_status(editor, "unsaved edits: press q again to discard, or w to save");
            editor->confirm_quit = true;
        } else
            return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    bool read_only = false;
    bool address_space = false;
    const os64_optspec_t specs[] = {
        { 'R', "read-only", false, "open without permitting changes",
          .flag = &read_only },
        { 'A', "address-space", false,
          "treat offsets as a sparse address space with no finite EOF",
          .flag = &address_space },
    };
    os64_args_t args;
    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Interactively inspect and edit bytes at 64-bit file offsets.";
    args.details = "The editor keeps a 4 KiB window in memory. Proc mem views use the sibling maps file for navigation and bounded search.";
    const char *path = NULL;
    int32_t parsed = os64_args_parse(&args, "hexedit [-R] [-A] FILE", &path, 1);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed != 1)
        return 2;

    char resolved_path[HEX_PATH_MAX];
    if (canonical_path(path, resolved_path, sizeof(resolved_path)) < 0) {
        os64_hprintf(OS64_STDERR, "hexedit: path is too long or cwd is unavailable: %s\n",
                     path);
        return 1;
    }
    path = resolved_path;

    editor_t editor = {0};
    editor.path = path;
    editor.keys = (int32_t)os64_tty_handle();
    if (editor.keys < 0)
        editor.keys = OS64_STDIN;

    os64_tty_info_t tty;
    if (os64_tty_read(&tty) == 0) {
        editor.rows = tty.rows;
        editor.cols = tty.cols;
    } else {
        editor.rows = 25;
        editor.cols = 80;
    }
    editor.bytes_per_row = editor.cols >= 86 ? 16 : 8;
    editor.data_rows = editor.rows > 4 ? editor.rows - 3 : 1;

    bool proc_mem = hex_buffer_is_proc_mem_path(path);
    address_space = address_space || address_space_path(path);
    // /proc/<pid>/mem v1 is read-only by kernel contract. Do not make the
    // ordinary spelling fail merely because finite files prefer update mode.
    if (proc_mem)
        read_only = true;
    if (hex_buffer_open(&editor.buffer, path, read_only, address_space) < 0) {
        os64_hprintf(OS64_STDERR,
                     "hexedit: cannot open %s for %s\n", path,
                     read_only ? "reading" : "non-truncating update");
        if (editor.keys != OS64_STDIN)
            os64_close(editor.keys);
        return 1;
    }

    if (proc_mem) {
        // The name beside the number: read the sibling cmdline's first line
        // (argv[0], as the program was invoked) and keep its basename. Built
        // from the mem path by swapping the last component, the same way the
        // maps path is; a missing cmdline just leaves the header nameless.
        size_t plen = os64_strlen(path);
        if (plen >= 3 && plen + 5 < HEX_PATH_MAX) {
            char cmdline_path[HEX_PATH_MAX];
            for (size_t i = 0; i < plen - 3; i++)
                cmdline_path[i] = path[i];
            os64_strcopy(cmdline_path + (plen - 3), HEX_PATH_MAX - (plen - 3), "cmdline");
            int64_t ch = os64_open(cmdline_path, "r");
            if (ch >= 0) {
                char first[HEX_PATH_MAX];
                if (os64_readline((int32_t)ch, first, sizeof(first)) == 1) {
                    size_t n = os64_strlen(first);
                    while (n > 0 && (first[n - 1] == '\n' || first[n - 1] == '\r'))
                        first[--n] = '\0';
                    const char *base = first;
                    for (const char *p = first; *p != '\0'; p++)
                        if (*p == '/' && p[1] != '\0')
                            base = p + 1;
                    os64_strcopy(editor.task_name, sizeof(editor.task_name), base);
                }
                os64_close((int32_t)ch);
            }
        }

        int map_count = hex_buffer_attach_proc_maps(&editor.buffer, path);
        if (map_count > 0) {
            move_to(&editor, editor.buffer.maps[0].start);
            editor.view_start = editor.buffer.maps[0].start;
            os64_snprintf(editor.status, sizeof(editor.status),
                          "loaded %d mapping%s from %s", map_count,
                          map_count == 1 ? "" : "s", editor.buffer.maps_path);
        } else {
            set_status(&editor, "mem opened; companion maps file unavailable");
        }
    }

    bool running = true;
    while (running) {
        render(&editor);
        key_t key = read_key(editor.keys);
        if (key.kind == KEY_CHARACTER)
            running = handle_character(&editor, key.ch);
        else if (key.kind == KEY_ESCAPE) {
            editor.ascii_mode = false;
            editor.nibble = 0;
            set_status(&editor, "HEX mode");
        } else {
            editor.confirm_quit = false;
            move_key(&editor, key.kind);
        }
    }

    write_all(OS64_STDOUT, "\f", 1);
    hex_buffer_close(&editor.buffer);
    if (editor.keys != OS64_STDIN)
        os64_close(editor.keys);
    return 0;
}
