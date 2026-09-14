// value.c — family H: what a control HOLDS, sanitized for its type.
//
// Initial values and edits share p_sanitize_value. The caller supplies the
// arena so a failed edit can discard temporary storage without changing
// the published value. Range arithmetic and numeric conversion are private
// helpers with their contracts recorded in LIBPAGE_REVIEW.md.

#include "internal.h"

os64_page_input_t p_input_type(const os64_html_node_t *n)
{
    static const struct {
        const char *name;
        os64_page_input_t type;
    } table[] = {{"hidden", OS64_PAGE_INPUT_HIDDEN},
                 {"text", OS64_PAGE_INPUT_TEXT},
                 {"search", OS64_PAGE_INPUT_SEARCH},
                 {"tel", OS64_PAGE_INPUT_TEL},
                 {"url", OS64_PAGE_INPUT_URL},
                 {"email", OS64_PAGE_INPUT_EMAIL},
                 {"password", OS64_PAGE_INPUT_PASSWORD},
                 {"date", OS64_PAGE_INPUT_DATE},
                 {"month", OS64_PAGE_INPUT_MONTH},
                 {"week", OS64_PAGE_INPUT_WEEK},
                 {"time", OS64_PAGE_INPUT_TIME},
                 {"datetime-local", OS64_PAGE_INPUT_DATETIME_LOCAL},
                 {"number", OS64_PAGE_INPUT_NUMBER},
                 {"range", OS64_PAGE_INPUT_RANGE},
                 {"color", OS64_PAGE_INPUT_COLOR},
                 {"checkbox", OS64_PAGE_INPUT_CHECKBOX},
                 {"radio", OS64_PAGE_INPUT_RADIO},
                 {"file", OS64_PAGE_INPUT_FILE},
                 {"submit", OS64_PAGE_INPUT_SUBMIT},
                 {"image", OS64_PAGE_INPUT_IMAGE},
                 {"reset", OS64_PAGE_INPUT_RESET},
                 {"button", OS64_PAGE_INPUT_BUTTON}};
    const char *type = p_attr(n, "type");
    if (type == NULL)
        return OS64_PAGE_INPUT_TEXT;
    for (int32_t i = 0; i < P_ARRAY(table); i++)
        if (os64_streq_nocase(type, table[i].name))
            return table[i].type;
    // An unknown type is Text, which is the standard's own fallback for the
    // attribute and therefore not a guess.
    return OS64_PAGE_INPUT_TEXT;
}

// ── The grammars a type demands ─────────────────────────────────────────

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static bool digits(const char **at, int32_t least, int32_t most, int32_t *value)
{
    int32_t n = 0, got = 0;
    while (digit(**at) && got < most) {
        if (value != NULL)
            n = n * 10 + (**at - '0');
        (*at)++;
        got++;
    }
    if (got < least)
        return false;
    if (value != NULL)
        *value = n;
    return true;
}

static bool leap(int32_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int32_t days_in(int32_t year, int32_t month)
{
    static const int32_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && leap(year))
        return 29;
    return days[month - 1];
}

// Calendar rules repeat every 400 years. Keep that residue while scanning
// an arbitrarily long positive year, so syntax checks cannot overflow.
static bool year_part(const char **at, int32_t *year)
{
    const char *s = *at;
    int32_t residue = 0;
    size_t digits_seen = 0;
    bool nonzero = false;
    while (digit(*s)) {
        nonzero |= *s != '0';
        residue = (residue * 10 + *s - '0') % 400;
        s++;
        digits_seen++;
    }
    if (digits_seen < 4 || !nonzero)
        return false;
    *year = residue != 0 ? residue : 400;
    *at = s;
    return true;
}

static bool date_part(const char **at, int32_t *year, int32_t *month, int32_t *day)
{
    const char *s = *at;
    int32_t y = 0, m = 0, d = 0;
    if (!year_part(&s, &y))
        return false;
    if (*s++ != '-' || !digits(&s, 2, 2, &m) || m < 1 || m > 12)
        return false;
    if (day != NULL) {
        if (*s++ != '-' || !digits(&s, 2, 2, &d) || d < 1 || d > days_in(y, m))
            return false;
        *day = d;
    }
    *year = y;
    *month = m;
    *at = s;
    return true;
}

// How many ISO weeks a year has: 52, or 53 when the year begins on a
// Thursday, or is a leap year beginning on a Wednesday.
static int32_t weeks_in(int32_t year)
{
    // Gregorian weekday for January 1, Sunday = 0.
    int32_t y = year - 1;
    int32_t weekday = (1 + y + y/4 - y/100 + y/400) % 7;
    if (weekday == 4 || (weekday == 3 && leap(year)))
        return 53;
    return 52;
}

static bool time_part(const char **at)
{
    const char *s = *at;
    int32_t hour = 0, minute = 0, second = 0;
    if (!digits(&s, 2, 2, &hour) || hour > 23)
        return false;
    if (*s++ != ':' || !digits(&s, 2, 2, &minute) || minute > 59)
        return false;
    if (*s == ':') {
        s++;
        if (!digits(&s, 2, 2, &second) || second > 59)
            return false;
        if (*s == '.') {
            s++;
            if (!digits(&s, 1, 3, NULL))
                return false;
        }
    }
    *at = s;
    return true;
}

static bool valid_for(os64_page_input_t input, const char *s)
{
    int32_t year = 0, month = 0, day = 0, week = 0;
    switch (input) {
    case OS64_PAGE_INPUT_DATE:
        return date_part(&s, &year, &month, &day) && *s == '\0';
    case OS64_PAGE_INPUT_MONTH:
        return date_part(&s, &year, &month, NULL) && *s == '\0';
    case OS64_PAGE_INPUT_WEEK: {
        if (!year_part(&s, &year) || *s++ != '-' || *s++ != 'W')
            return false;
        if (!digits(&s, 2, 2, &week) || week < 1 || week > weeks_in(year))
            return false;
        return *s == '\0';
    }
    case OS64_PAGE_INPUT_TIME:
        return time_part(&s) && *s == '\0';
    case OS64_PAGE_INPUT_DATETIME_LOCAL:
        if (!date_part(&s, &year, &month, &day))
            return false;
        if (*s != 'T' && *s != ' ')
            return false;
        s++;
        return time_part(&s) && *s == '\0';
    default:
        return false;
    }
}

// ── The sanitizers ──────────────────────────────────────────────────────

static char *strip_breaks(PArena *arena, const char *s, size_t *len)
{
    size_t n = os64_strlen(s);
    char *out = p_arena_alloc(arena, n + 1);
    if (out == NULL)
        return NULL;
    size_t at = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] != '\n' && s[i] != '\r')
            out[at++] = s[i];
    out[at] = '\0';
    *len = at;
    return out;
}

static void strip_ends(char *s, size_t *len)
{
    size_t first = 0, last = *len;
    while (first < last && ws(s[first]))
        first++;
    while (last > first && ws(s[last - 1]))
        last--;
    if (first != 0)
        for (size_t i = 0; i < last - first; i++)
            s[i] = s[first + i];
    *len = last - first;
    s[*len] = '\0';
}

// A `multiple` email field holds a comma-separated list, and each address in
// it is stripped on its own — so a list typed with spaces after the commas
// does not send them.
static void strip_each(char *s, size_t *len)
{
    size_t write = 0, read = 0;
    while (read <= *len) {
        size_t start = read;
        while (read < *len && s[read] != ',')
            read++;
        size_t first = start, last = read;
        while (first < last && ws(s[first]))
            first++;
        while (last > first && ws(s[last - 1]))
            last--;
        for (size_t i = first; i < last; i++)
            s[write++] = s[i];
        if (read < *len)
            s[write++] = ',';
        else
            break;
        read++;
    }
    *len = write;
    s[write] = '\0';
}

static bool hex(char c)
{
    return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static char *color_value(PArena *arena, const char *s, size_t *len)
{
    bool simple = s[0] == '#';
    for (int32_t i = 1; simple && i <= 6; i++)
        simple = hex(s[i]);
    if (simple && s[7] != '\0')
        simple = false;
    if (!simple) {
        *len = 7;
        return p_arena_copy(arena, "#000000", 7);
    }
    char *out = p_arena_copy(arena, s, 7);
    if (out == NULL)
        return NULL;
    for (int32_t i = 1; i <= 6; i++)
        if (out[i] >= 'A' && out[i] <= 'F')
            out[i] = (char)(out[i] + ('a' - 'A'));
    *len = 7;
    return out;
}

static const char *datetime_value(PArena *arena, const char *raw, size_t *len)
{
    char *out=p_arena_copy(arena,raw,os64_strlen(raw));
    if (out == NULL)
        return NULL;
    size_t time=0;
    while (out[time] != 'T' && out[time] != ' ')
        time++;
    out[time++]='T';
    size_t end=os64_strlen(out);
    if (end>time+8 && out[time+8]=='.') {
        while (out[end-1]=='0') end--;
        if (out[end-1]=='.') end--;
    }
    if (end==time+8 && out[time+5]==':' && out[time+6]=='0' && out[time+7]=='0')
        end-=3;
    out[end]='\0';*len=end;
    return out;
}

const char *p_page_value(os64_page_t *page, const os64_html_node_t *n,
                         os64_page_element_t element, os64_page_input_t input, size_t *len)
{
    const char *raw = element == OS64_PAGE_EL_TEXTAREA ?
        p_subtree_text(page, n, false, len) : p_attr(n, "value");
    if (raw == NULL && element == OS64_PAGE_EL_TEXTAREA)
        return NULL;
    if (raw == NULL && (input == OS64_PAGE_INPUT_CHECKBOX || input == OS64_PAGE_INPUT_RADIO))
        raw = "on";
    return p_sanitize_value(&page->arena, n, element, input, raw != NULL ? raw : "", len);
}

const char *p_sanitize_value(PArena *arena, const os64_html_node_t *n,
                            os64_page_element_t element, os64_page_input_t input,
                            const char *raw, size_t *len)
{
    *len = 0;
    if (element == OS64_PAGE_EL_TEXTAREA) {
        char *text = p_arena_copy(arena, raw, os64_strlen(raw));
        if (text == NULL)
            return NULL;
        size_t write = 0;
        for (size_t read = 0; text[read] != '\0'; read++) {
            if (text[read] == '\r') {
                text[write++] = '\n';
                if (text[read + 1] == '\n')
                    read++;
            } else {
                text[write++] = text[read];
            }
        }
        text[write] = '\0';
        *len = write;
        return text;
    }
    if (element == OS64_PAGE_EL_SELECT || input == OS64_PAGE_INPUT_FILE)
        return "";
    if (element == OS64_PAGE_EL_BUTTON) {
        *len = os64_strlen(raw);
        return raw;
    }
    switch (input) {
    case OS64_PAGE_INPUT_TEXT:
    case OS64_PAGE_INPUT_SEARCH:
    case OS64_PAGE_INPUT_TEL:
    case OS64_PAGE_INPUT_PASSWORD:
        return strip_breaks(arena, raw, len);
    case OS64_PAGE_INPUT_URL: {
        char *out = strip_breaks(arena, raw, len);
        if (out != NULL)
            strip_ends(out, len);
        return out;
    }
    case OS64_PAGE_INPUT_EMAIL: {
        char *out = strip_breaks(arena, raw, len);
        if (out == NULL)
            return NULL;
        if (p_has_attr(n, "multiple"))
            strip_each(out, len);
        else
            strip_ends(out, len);
        return out;
    }
    case OS64_PAGE_INPUT_NUMBER: {
        double value;
        if (!p_number_parse(raw, true, &value))
            return "";
        *len = os64_strlen(raw);
        return raw;
    }
    case OS64_PAGE_INPUT_DATE:
    case OS64_PAGE_INPUT_MONTH:
    case OS64_PAGE_INPUT_WEEK:
    case OS64_PAGE_INPUT_TIME:
    case OS64_PAGE_INPUT_DATETIME_LOCAL:
        if (!valid_for(input, raw))
            return "";
        if (input == OS64_PAGE_INPUT_DATETIME_LOCAL)
            return datetime_value(arena,raw,len);
        *len = os64_strlen(raw);
        return raw;
    case OS64_PAGE_INPUT_RANGE:
        return p_range_value(arena, n, raw, len);
    case OS64_PAGE_INPUT_COLOR:
        return color_value(arena, raw, len);
    default:
        // Hidden values, ticks and buttons go through
        // verbatim; what each of them SENDS is family D's answer, not this
        // one's.
        *len = os64_strlen(raw);
        return raw;
    }
}
