// value.c — family H: what a control HOLDS, sanitized for its type.
//
// ONE DOOR, and nothing outside it reads the attribute behind a value. Every
// other family asks here, so "what does this box contain" has one answer
// whether it is being drawn, validated, or put on a wire.
//
// The sanitization is the standard's, per type (§4.10.5.1). It is not
// cosmetic: a newline inside a single-line field would become `%0A` in a
// query nobody meant to send, and a `number` holding letters would be sent
// as letters to a server that asked for a number.

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

// The standard's VALID FLOATING-POINT NUMBER: digits, then an optional
// fraction, then an optional exponent. The grammar says nothing about how
// big the number may be, so neither does this.
static bool valid_float(const char *s)
{
    if (*s == '-' || *s == '+') {
        // A leading '+' is NOT part of a valid floating-point number, only
        // of a valid number for an exponent.
        if (*s == '+')
            return false;
        s++;
    }
    if (!digits(&s, 1, 20, NULL))
        return false;
    if (*s == '.') {
        s++;
        if (!digits(&s, 1, 20, NULL))
            return false;
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '+' || *s == '-')
            s++;
        if (!digits(&s, 1, 10, NULL))
            return false;
    }
    return *s == '\0';
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

// The standard's VALID DATE STRING: four or more digits for the year, and a
// year of zero is not a year.
static bool date_part(const char **at, int32_t *year, int32_t *month, int32_t *day)
{
    const char *s = *at;
    int32_t y = 0, m = 0, d = 0, got = 0;
    while (digit(*s)) {
        y = y * 10 + (*s - '0');
        s++;
        got++;
        if (got > 6)
            return false;
    }
    if (got < 4 || y == 0)
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
    // Zeller's congruence for 1 January, 0 = Saturday.
    int32_t y = year - 1, doomsday = (1 + 5 * (y % 4) + 4 * (y % 100) + 6 * (y % 400)) % 7;
    if (doomsday == 5)
        return 53;   // a Thursday
    if (doomsday == 4 && leap(year))
        return 53;   // a Wednesday, in a leap year
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
        int32_t got = 0;
        while (digit(*s)) {
            year = year * 10 + (*s - '0');
            s++;
            got++;
            if (got > 6)
                return false;
        }
        if (got < 4 || year == 0 || *s++ != '-' || *s++ != 'W')
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

// ── A decimal, exactly, for the one value that has to be computed ───────
//
// A range with no value holds the MIDDLE of its span, so that number has to
// be worked out and spelled — and there is no floating point to be had here
// (the kernel is built without it and userland's formatter has no `%f`), so
// a decimal is carried as an integer and a power of ten and the arithmetic
// is exact. A span too wide to hold that way is a refusal rather than a
// rounded answer nobody asked for.

typedef struct {
    int64_t mantissa;
    int32_t exp10;
} PDecimal;

#define P_DECIMAL_MAX 999999999999999ll   // room to add two and multiply by five

static bool decimal_parse(const char *s, PDecimal *out)
{
    if (s == NULL || !valid_float(s))
        return false;
    bool negative = *s == '-';
    if (negative)
        s++;
    int64_t mantissa = 0;
    int32_t exp10 = 0;
    for (; digit(*s); s++) {
        if (mantissa > P_DECIMAL_MAX / 10)
            return false;
        mantissa = mantissa * 10 + (*s - '0');
    }
    if (*s == '.') {
        for (s++; digit(*s); s++) {
            if (mantissa > P_DECIMAL_MAX / 10)
                return false;
            mantissa = mantissa * 10 + (*s - '0');
            exp10--;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        bool down = *s == '-';
        if (*s == '+' || *s == '-')
            s++;
        int32_t power = 0;
        for (; digit(*s); s++) {
            power = power * 10 + (*s - '0');
            if (power > 40)
                return false;   // wider than an exact decimal can be carried
        }
        exp10 += down ? -power : power;
    }
    out->mantissa = negative ? -mantissa : mantissa;
    out->exp10 = exp10;
    return true;
}

// Scale a decimal down to a smaller power of ten, exactly or not at all.
static bool decimal_at(PDecimal *d, int32_t exp10)
{
    while (d->exp10 > exp10) {
        if (d->mantissa > P_DECIMAL_MAX / 10 || d->mantissa < -(P_DECIMAL_MAX / 10))
            return false;
        d->mantissa *= 10;
        d->exp10--;
    }
    return d->exp10 == exp10;
}

static bool decimal_spell(PDecimal d, char *out, size_t cap)
{
    // Trailing zeros in the mantissa are a power of ten, not digits: they
    // are what turns 5.0 into 5.
    while (d.mantissa != 0 && d.mantissa % 10 == 0 && d.exp10 < 0) {
        d.mantissa /= 10;
        d.exp10++;
    }
    char digits_out[24];
    int32_t n = 0;
    int64_t value = d.mantissa < 0 ? -d.mantissa : d.mantissa;
    do {
        digits_out[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && n < (int32_t)sizeof(digits_out));
    // A power of ten this far from the digits would be spelled as a field of
    // zeros; refuse rather than write one.
    if (d.exp10 > 24 || d.exp10 < -24)
        return false;
    size_t at = 0;
    if (d.mantissa < 0 && at + 1 < cap)
        out[at++] = '-';
    if (d.exp10 >= 0) {
        for (int32_t i = n - 1; i >= 0; i--)
            if (at + 1 < cap)
                out[at++] = digits_out[i];
        for (int32_t i = 0; i < d.exp10; i++)
            if (at + 1 < cap)
                out[at++] = '0';
    } else {
        int32_t whole = n + d.exp10;
        if (whole <= 0) {
            if (at + 1 < cap)
                out[at++] = '0';
            if (at + 1 < cap)
                out[at++] = '.';
            for (int32_t i = 0; i < -whole; i++)
                if (at + 1 < cap)
                    out[at++] = '0';
            for (int32_t i = n - 1; i >= 0; i--)
                if (at + 1 < cap)
                    out[at++] = digits_out[i];
        } else {
            for (int32_t i = n - 1; i >= 0; i--) {
                if (i == n - whole - 1 && at + 1 < cap)
                    out[at++] = '.';
                if (at + 1 < cap)
                    out[at++] = digits_out[i];
            }
        }
    }
    out[at] = '\0';
    return at + 1 < cap;
}

// The Range state's value sanitization: it holds the middle of its span when
// it holds nothing usable — the minimum plus half the difference, or the
// minimum when the maximum is below it. An absent or invalid bound is 0
// and 100.
static const char *range_default(os64_page_t *page, const os64_html_node_t *n, size_t *len)
{
    PDecimal low = {0, 0}, high = {100, 0}, bound;
    if (decimal_parse(p_attr(n, "min"), &bound))
        low = bound;
    if (decimal_parse(p_attr(n, "max"), &bound))
        high = bound;
    int32_t exp10 = low.exp10 < high.exp10 ? low.exp10 : high.exp10;
    if (!decimal_at(&low, exp10) || !decimal_at(&high, exp10))
        return NULL;
    if (high.mantissa < low.mantissa)
        high = low;
    // (low + high) / 2, kept exact by taking it as (low + high) * 5 one
    // power of ten further down, so an odd sum does not round.
    if (low.mantissa > P_DECIMAL_MAX - high.mantissa ||
        low.mantissa < -P_DECIMAL_MAX - high.mantissa)
        return NULL;
    PDecimal middle = {(low.mantissa + high.mantissa) * 5, exp10 - 1};
    char spelled[48];
    if (!decimal_spell(middle, spelled, sizeof(spelled)))
        return NULL;
    size_t n2 = os64_strlen(spelled);
    char *copy = p_copy(page, spelled, n2);
    if (copy != NULL)
        *len = n2;
    return copy;
}

// ── The sanitizers ──────────────────────────────────────────────────────

static char *strip_breaks(os64_page_t *page, const char *s, size_t *len)
{
    size_t n = os64_strlen(s);
    char *out = p_alloc(page, n + 1);
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

static char *color_value(os64_page_t *page, const char *s, size_t *len)
{
    bool simple = s[0] == '#';
    for (int32_t i = 1; simple && i <= 6; i++)
        simple = hex(s[i]);
    if (simple && s[7] != '\0')
        simple = false;
    if (!simple) {
        *len = 7;
        return p_copy(page, "#000000", 7);
    }
    char *out = p_copy(page, s, 7);
    if (out == NULL)
        return NULL;
    for (int32_t i = 1; i <= 6; i++)
        if (out[i] >= 'A' && out[i] <= 'F')
            out[i] = (char)(out[i] + ('a' - 'A'));
    *len = 7;
    return out;
}

const char *p_page_value(os64_page_t *page, const os64_html_node_t *n,
                         os64_page_element_t element, os64_page_input_t input, size_t *len)
{
    *len = 0;
    if (element == OS64_PAGE_EL_TEXTAREA) {
        // Its child text, verbatim: the whitespace inside a textarea IS the
        // value, which is why libhtml kept it. Line endings become LF here
        // and CRLF again at submission, per the standard's two steps.
        char *text = p_subtree_text(page, n, false, len);
        if (text == NULL)
            return NULL;
        size_t write = 0;
        for (size_t read = 0; read < *len; read++) {
            if (text[read] == '\r') {
                text[write++] = '\n';
                if (read + 1 < *len && text[read + 1] == '\n')
                    read++;
                continue;
            }
            text[write++] = text[read];
        }
        text[write] = '\0';
        *len = write;
        return text;
    }
    // A SELECT holds whichever of its options is picked, and those are read
    // after this; the walk fills it in once it knows them.
    if (element == OS64_PAGE_EL_SELECT)
        return "";

    const char *raw = p_attr(n, "value");
    if (raw == NULL)
        raw = "";
    if (element == OS64_PAGE_EL_BUTTON) {
        *len = os64_strlen(raw);
        return raw;
    }
    switch (input) {
    case OS64_PAGE_INPUT_TEXT:
    case OS64_PAGE_INPUT_SEARCH:
    case OS64_PAGE_INPUT_TEL:
    case OS64_PAGE_INPUT_PASSWORD:
        return strip_breaks(page, raw, len);
    case OS64_PAGE_INPUT_URL: {
        char *out = strip_breaks(page, raw, len);
        if (out != NULL)
            strip_ends(out, len);
        return out;
    }
    case OS64_PAGE_INPUT_EMAIL: {
        char *out = strip_breaks(page, raw, len);
        if (out == NULL)
            return NULL;
        if (p_has_attr(n, "multiple"))
            strip_each(out, len);
        else
            strip_ends(out, len);
        return out;
    }
    case OS64_PAGE_INPUT_NUMBER:
        // Letters in a number field are not a number, and the standard says
        // a browser holds nothing rather than sending them.
        if (!valid_float(raw))
            return "";
        *len = os64_strlen(raw);
        return raw;
    case OS64_PAGE_INPUT_DATE:
    case OS64_PAGE_INPUT_MONTH:
    case OS64_PAGE_INPUT_WEEK:
    case OS64_PAGE_INPUT_TIME:
    case OS64_PAGE_INPUT_DATETIME_LOCAL:
        if (!valid_for(input, raw))
            return "";
        *len = os64_strlen(raw);
        return raw;
    case OS64_PAGE_INPUT_RANGE: {
        if (valid_float(raw)) {
            *len = os64_strlen(raw);
            return raw;
        }
        const char *middle = range_default(page, n, len);
        return middle != NULL ? middle : "";
    }
    case OS64_PAGE_INPUT_COLOR:
        return color_value(page, raw, len);
    default:
        // HIDDEN, the ticks, the buttons and a file's name go through
        // verbatim; what each of them SENDS is family D's answer, not this
        // one's.
        *len = os64_strlen(raw);
        return raw;
    }
}
