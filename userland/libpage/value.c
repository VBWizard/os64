// value.c — family H: what a control HOLDS, sanitized for its type.
//
// Initial values and edits share p_sanitize_value. The caller supplies the
// arena so a failed edit can discard temporary storage without changing
// the published value. Range arithmetic and numeric conversion are private
// helpers with their contracts recorded in LIBPAGE_REVIEW.md.

#include "internal.h"
#include "os64/fmt.h"

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

// ── A value as a number (HTML's "convert a string to a number") ─────────

// The year itself, not its residue: four digits or more, and none past
// 275760, the year a Date's range ends in, safely below the one (past
// 2^53 ms, in the 287,000s) where a double stops holding every
// millisecond, which a local date and time's step needs. A later year the value sanitizer keeps does not
// convert, and min, max and step say nothing of it (DEBTS.md § libpage).
static bool whole_year(const char **at, int32_t *year)
{
    const char *s = *at;
    int64_t y = 0;
    size_t seen = 0;
    while (digit(*s)) {
        y = y * 10 + (*s - '0');
        if (y > 275760)
            return false;
        s++;
        seen++;
    }
    if (seen < 4 || y == 0)
        return false;
    *year = (int32_t)y;
    *at = s;
    return true;
}

// Days from 1970-01-01 to a proleptic Gregorian date (Hinnant's civil
// algorithm).
static int64_t days_from_civil(int64_t y, int32_t m, int32_t d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool whole_date(const char **at, int32_t *year, int32_t *month, int32_t *day)
{
    const char *s = *at;
    int32_t y = 0, m = 0, d = 0;
    if (!whole_year(&s, &y) || *s++ != '-' || !digits(&s, 2, 2, &m) || m < 1 || m > 12)
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

// Milliseconds since midnight: hh:mm, then :ss and its fraction, of any
// length as HTML's time parser reads one (the first three digits kept): a
// value is sanitized to three or fewer, but a min, a max or a value
// attribute may spell more.
static bool whole_time(const char **at, double *ms)
{
    const char *s = *at;
    int32_t hour = 0, minute = 0, second = 0, fraction = 0, places = 0;
    if (!digits(&s, 2, 2, &hour) || hour > 23 || *s++ != ':' || !digits(&s, 2, 2, &minute) || minute > 59)
        return false;
    if (*s == ':') {
        s++;
        if (!digits(&s, 2, 2, &second) || second > 59)
            return false;
        if (*s == '.') {
            s++;
            const char *start = s;
            if (!digits(&s, 1, 3, &fraction))
                return false;
            places = (int32_t)(s - start);
            while (digit(*s))
                s++;
        }
    }
    while (places++ < 3)
        fraction *= 10;
    *ms = (double)(((hour * 60 + minute) * 60 + second) * 1000 + fraction);
    *at = s;
    return true;
}

bool p_input_number(os64_page_input_t input, const char *s, double *out)
{
    if (s == NULL)
        return false;
    int32_t year = 0, month = 0, day = 0, week = 0;
    double ms = 0;
    switch (input) {
    case OS64_PAGE_INPUT_NUMBER:
    case OS64_PAGE_INPUT_RANGE:
        // HTML's rules for parsing floating-point number values, lenient,
        // as range.c's sanitizer reads min, max and step: a judge stricter
        // than the sanitizer would put a range's own value off its grid.
        return p_number_parse(s, false, out);
    case OS64_PAGE_INPUT_DATE:
        if (!whole_date(&s, &year, &month, &day) || *s != '\0')
            return false;
        *out = (double)days_from_civil(year, month, day) * 86400000.0;
        return true;
    case OS64_PAGE_INPUT_MONTH:
        // Months since January 1970, as HTML counts a month.
        if (!whole_date(&s, &year, &month, NULL) || *s != '\0')
            return false;
        *out = (double)(((int64_t)year - 1970) * 12 + month - 1);
        return true;
    case OS64_PAGE_INPUT_WEEK: {
        // The Monday that begins the ISO week: week 1 holds January 4.
        if (!whole_year(&s, &year) || *s++ != '-' || *s++ != 'W' || !digits(&s, 2, 2, &week) || week < 1 ||
            week > weeks_in(year) || *s != '\0')
            return false;
        int64_t jan4 = days_from_civil(year, 1, 4);
        int64_t monday = jan4 - (((jan4 % 7) + 7 + 3) % 7);
        *out = (double)(monday + (int64_t)(week - 1) * 7) * 86400000.0;
        return true;
    }
    case OS64_PAGE_INPUT_TIME:
        if (!whole_time(&s, &ms) || *s != '\0')
            return false;
        *out = ms;
        return true;
    case OS64_PAGE_INPUT_DATETIME_LOCAL:
        if (!whole_date(&s, &year, &month, &day) || (*s != 'T' && *s != ' '))
            return false;
        s++;
        if (!whole_time(&s, &ms) || *s != '\0')
            return false;
        *out = (double)days_from_civil(year, month, day) * 86400000.0 + ms;
        return true;
    default:
        return false;
    }
}

void p_input_step(os64_page_input_t input, double *step, uint32_t *scale, double *base)
{
    *step = 1;
    *scale = 1;
    *base = 0;
    switch (input) {
    case OS64_PAGE_INPUT_DATE: *scale = 86400000; break;
    case OS64_PAGE_INPUT_WEEK: *scale = 604800000; *base = -259200000; break;
    case OS64_PAGE_INPUT_TIME: case OS64_PAGE_INPUT_DATETIME_LOCAL: *step = 60; *scale = 1000; break;
    default: break;
    }
}

void p_input_limits(const os64_html_node_t *n, os64_page_input_t input, PLimits *out)
{
    *out = (PLimits){0};
    double fallback = 1;
    p_input_step(input, &fallback, &out->scale, &out->base);
    bool min_spelled = p_input_number(input, p_attr(n, "min"), &out->min);
    out->has_min = min_spelled;
    out->has_max = p_input_number(input, p_attr(n, "max"), &out->max);
    if (input == OS64_PAGE_INPUT_RANGE) {
        if (!out->has_min) { out->min = 0; out->has_min = true; }
        if (!out->has_max) { out->max = 100; out->has_max = true; }
        if (out->max < out->min) { out->max = out->min; out->pinned = true; }
    }
    const char *step_text = p_attr(n, "step");
    out->any = step_text != NULL && os64_streq_nocase(step_text, "any");
    if (!p_number_parse(step_text, false, &out->step) || out->step <= 0)
        out->step = fallback;
    double from = 0;
    if (min_spelled)
        out->base = out->min;
    else if (p_input_number(input, p_attr(n, "value"), &from))
        out->base = from;
}

// ── A number spelled for a type ─────────────────────────────────────────

// The proleptic Gregorian date `days` after 1970-01-01 (Hinnant's
// inverse of days_from_civil).
static void civil_from_days(int64_t days, int64_t *year, int32_t *month, int32_t *day)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *day = (int32_t)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int32_t)(mp < 10 ? mp + 3 : mp - 9);
    *year = yoe + era * 400 + (*month <= 2);
}

// The nearest whole number, a half away from zero (C's round, which
// libpage has no libm for); every double past 2^52 is already whole.
static double nearest(double x)
{
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0)
        return x;
    double whole = (double)(int64_t)x, part = x - whole;
    return part >= 0.5 ? whole + 1 : part <= -0.5 ? whole - 1 : whole;
}

// A rounded time value as its day and the milliseconds into it: false
// past 2^53, which no year a date holds reaches.
static bool split_day(double ms, int64_t *days, int64_t *in_day)
{
    if (!(ms > -9007199254740992.0 && ms < 9007199254740992.0))
        return false;
    int64_t whole = (int64_t)nearest(ms);
    *days = whole / 86400000 - (whole % 86400000 < 0);
    *in_day = whole - *days * 86400000;
    return true;
}

// The floor of a time value's place in its day, exact for any finite
// double, however large: x is m * 2^e, so its remainder is m's times
// 2^e's, by doubling, and a fraction lowers a negative x's by one.
static int64_t time_of_day(double x)
{
    const uint64_t day = 86400000;
    union { double f; uint64_t bits; } u = {x};
    bool negative = (u.bits >> 63) != 0;
    int32_t exponent = (int32_t)(u.bits >> 52 & 0x7ff);
    uint64_t mantissa = u.bits & ((1ull << 52) - 1);
    if (exponent == 0)
        exponent = 1;
    else
        mantissa |= 1ull << 52;
    exponent -= 1075;
    uint64_t whole = 0;
    bool fraction = false;
    if (exponent >= 0) {
        whole = mantissa % day;
        for (int32_t i = 0; i < exponent; i++)
            whole = whole * 2 % day;
    } else if (exponent > -64) {
        whole = (mantissa >> -exponent) % day;
        fraction = (mantissa & ((1ull << -exponent) - 1)) != 0;
    } else {
        fraction = mantissa != 0;
    }
    if (!negative || (whole == 0 && !fraction))
        return (int64_t)whole;
    return (int64_t)(day - whole - fraction);
}

static size_t spell_date(char *out, size_t cap, int64_t days, bool day_too)
{
    int64_t year = 0;
    int32_t month = 0, day = 0;
    civil_from_days(days, &year, &month, &day);
    if (year < 1 || year > 275760)
        return 0;
    return day_too ? (size_t)os64_snprintf(out, cap, "%04lld-%02d-%02d", (long long)year, (int)month, (int)day)
                   : (size_t)os64_snprintf(out, cap, "%04lld-%02d", (long long)year, (int)month);
}

// hh:mm, then :ss when there are seconds, then .sss when there are
// milliseconds, as Blink spells a time; a local date and time's sanitizer
// shortens its fraction.
static void spell_time(char *out, size_t cap, int64_t ms)
{
    int32_t milli = (int32_t)(ms % 1000), second = (int32_t)(ms / 1000 % 60), minute = (int32_t)(ms / 60000 % 60),
            hour = (int32_t)(ms / 3600000);
    if (milli != 0)
        os64_snprintf(out, cap, "%02d:%02d:%02d.%03d", hour, minute, second, milli);
    else if (second != 0)
        os64_snprintf(out, cap, "%02d:%02d:%02d", hour, minute, second);
    else
        os64_snprintf(out, cap, "%02d:%02d", hour, minute);
}

bool p_input_spell(os64_page_input_t input, double number, bool as_date, char out[32])
{
    int64_t days = 0, in_day = 0;
    out[0] = '\0';
    switch (input) {
    case OS64_PAGE_INPUT_NUMBER:
    case OS64_PAGE_INPUT_RANGE:
        p_number_spell(number, out);
        return true;
    case OS64_PAGE_INPUT_DATE:
        return split_day(number, &days, &in_day) && spell_date(out, 32, days, true) != 0;
    case OS64_PAGE_INPUT_MONTH: {
        if (as_date)
            return split_day(number, &days, &in_day) && spell_date(out, 32, days, false) != 0;
        // A count of months, rounded. The bound keeps the cast exact and
        // loses nothing: a spelled year allows about 3.3 million.
        if (!(number > -1e8 && number < 1e8))
            return false;
        int64_t months = (int64_t)nearest(number), whole = months / 12 - (months % 12 < 0);
        int64_t year = 1970 + whole, month = months - whole * 12 + 1;
        if (year < 1 || year > 275760)
            return false;
        os64_snprintf(out, 32, "%04lld-%02lld", (long long)year, (long long)month);
        return true;
    }
    case OS64_PAGE_INPUT_WEEK: {
        // The ISO week holding the day: its Thursday's year, and that
        // Thursday's place among the year's.
        if (!split_day(number, &days, &in_day))
            return false;
        int64_t weekday = ((days + 3) % 7 + 7) % 7, thursday = days - weekday + 3, year = 0;
        int32_t month = 0, day = 0;
        civil_from_days(thursday, &year, &month, &day);
        if (year < 1 || year > 275760)
            return false;
        int64_t week = (thursday - days_from_civil(year, 1, 1)) / 7 + 1;
        os64_snprintf(out, 32, "%04lld-W%02lld", (long long)year, (long long)week);
        return true;
    }
    case OS64_PAGE_INPUT_TIME:
        spell_time(out, 32, time_of_day(number));
        return true;
    case OS64_PAGE_INPUT_DATETIME_LOCAL: {
        if (!split_day(number, &days, &in_day))
            return false;
        size_t at = spell_date(out, 32, days, true);
        if (at == 0)
            return false;
        out[at++] = 'T';
        spell_time(out + at, 32 - at, in_day);
        return true;
    }
    default:
        return false;
    }
}

// ── valueAsNumber, valueAsDate, stepUp and stepDown ─────────────────────

static bool input_node(const os64_html_node_t *n)
{
    return n != NULL && n->kind == OS64_HTML_ELEMENT && n->ns == OS64_HTML_NS_HTML && n->tag == OS64_HTML_TAG_INPUT;
}

static bool as_applies(os64_page_input_t input, os64_page_as_t as)
{
    switch (input) {
    case OS64_PAGE_INPUT_DATE: case OS64_PAGE_INPUT_MONTH: case OS64_PAGE_INPUT_WEEK: case OS64_PAGE_INPUT_TIME:
        return true;
    case OS64_PAGE_INPUT_DATETIME_LOCAL: case OS64_PAGE_INPUT_NUMBER: case OS64_PAGE_INPUT_RANGE:
        return as == OS64_PAGE_AS_NUMBER;
    default:
        return false;
    }
}

// The value as its type's number, read as the judge reads it, from the
// sanitized value (a range's is never empty).
static int64_t value_number(os64_page_state_t *state, const os64_html_node_t *node, os64_page_input_t input,
                            double *number, bool *converted)
{
    const char *value;
    size_t len;
    int64_t status = os64_page_node_value(state, node, &value, &len);
    if (status < 0)
        return status;
    *converted = len != 0 && p_input_number(input, value, number);
    return 0;
}

int64_t os64_page_node_value_as(os64_page_state_t *state, const os64_html_node_t *node, os64_page_as_t as,
                                double *number, bool *applies)
{
    if (number == NULL || applies == NULL || !input_node(node))
        return -OS64_PAGE_REASON_WRONG_KIND;
    *number = __builtin_nan("");
    *applies = false;
    os64_page_input_t input = p_input_type(node);
    double got = 0;
    bool converted = false;
    int64_t status = value_number(state, node, input, &got, &converted);
    if (status < 0)
        return status;
    *applies = as_applies(input, as);
    if (!*applies || !converted)
        return 0;
    if (as == OS64_PAGE_AS_DATE && input == OS64_PAGE_INPUT_MONTH) {
        int64_t months = (int64_t)got, whole = months / 12 - (months % 12 < 0);
        got = (double)days_from_civil(1970 + whole, (int32_t)(months - whole * 12 + 1), 1) * 86400000.0;
    }
    *number = got;
    return 0;
}

static int64_t set_spelled(os64_page_state_t *state, const os64_html_node_t *node, os64_page_input_t input,
                           double number, bool as_date)
{
    char text[32];
    if (number != number || !p_input_spell(input, number, as_date, text))
        text[0] = '\0';
    return os64_page_node_set_value(state, node, text, os64_strlen(text));
}

int64_t os64_page_node_set_value_as(os64_page_state_t *state, const os64_html_node_t *node, os64_page_as_t as,
                                    double number, bool *applies)
{
    // Infinite: equal to itself, as NaN is not, and not finite.
    if (applies == NULL || !input_node(node) || (number == number && number - number != 0))
        return -OS64_PAGE_REASON_WRONG_KIND;
    *applies = false;
    double ignored = 0;
    bool converted = false;
    os64_page_input_t input = p_input_type(node);
    // Read first, as the getter reads it: a node not of the state's is
    // refused before `applies` says anything of it.
    int64_t status = value_number(state, node, input, &ignored, &converted);
    if (status < 0)
        return status;
    *applies = as_applies(input, as);
    return *applies ? set_spelled(state, node, input, number, as == OS64_PAGE_AS_DATE) : 0;
}

int64_t os64_page_node_step(os64_page_state_t *state, const os64_html_node_t *node, int32_t n, bool down,
                            bool *applies)
{
    if (applies == NULL || !input_node(node))
        return -OS64_PAGE_REASON_WRONG_KIND;
    *applies = false;
    os64_page_input_t input = p_input_type(node);
    double value = 0;
    bool converted = false;
    int64_t status = value_number(state, node, input, &value, &converted);
    if (status < 0)
        return status;
    PLimits limits;
    p_input_limits(node, input, &limits);
    *applies = as_applies(input, OS64_PAGE_AS_NUMBER) && !limits.any;
    double stepped = 0;
    if (!*applies || !p_step_by(value, converted, &limits, down ? -(int64_t)n : n, &stepped))
        return 0;
    return set_spelled(state, node, input, stepped, false);
}

// ── The sanitizers ──────────────────────────────────────────────────────

static char *strip_breaks(PArena *arena, const char *s, size_t n, size_t *len)
{
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

static char *color_value(PArena *arena, const char *s, size_t n, size_t *len)
{
    bool simple = n == 7 && s[0] == '#';
    for (int32_t i = 1; simple && i <= 6; i++)
        simple = hex(s[i]);
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

static const char *datetime_value(PArena *arena, const char *raw, size_t raw_len, size_t *len)
{
    char *out=p_arena_copy(arena,raw,raw_len);
    if (out == NULL)
        return NULL;
    size_t time=0;
    while (out[time] != 'T' && out[time] != ' ')
        time++;
    out[time++]='T';
    size_t end=raw_len;
    if (end>time+8 && out[time+8]=='.') {
        while (out[end-1]=='0') end--;
        if (out[end-1]=='.') end--;
    }
    if (end==time+8 && out[time+5]==':' && out[time+6]=='0' && out[time+7]=='0')
        end-=3;
    out[end]='\0';*len=end;
    return out;
}

// A textarea's default/raw value is child text, not descendant text.
char *p_child_text_into(PArena *arena, const os64_html_node_t *node, size_t *len)
{
    size_t total = 0;
    for (const os64_html_node_t *at = node->first_child; at != NULL; at = at->next)
        if (at->kind == OS64_HTML_TEXT) {
            if (at->text_len > SIZE_MAX - total - 1) return NULL;
            total += at->text_len;
        }
    char *text = p_arena_alloc(arena, total + 1);
    if (text == NULL) return NULL;
    size_t offset = 0;
    for (const os64_html_node_t *at = node->first_child; at != NULL; at = at->next)
        if (at->kind == OS64_HTML_TEXT) {
            os64_memcpy(text + offset, at->text, at->text_len);
            offset += at->text_len;
        }
    text[total] = '\0';
    if (len != NULL) *len = total;
    return text;
}

const char *p_page_value(os64_page_t *page, const os64_html_node_t *n,
                         os64_page_element_t element, os64_page_input_t input, size_t *len)
{
    const char *raw = element == OS64_PAGE_EL_TEXTAREA ?
        p_child_text_into(&page->arena, n, len) : p_attr(n, "value");
    if (raw == NULL && element == OS64_PAGE_EL_TEXTAREA)
        return NULL;
    if (raw == NULL && (input == OS64_PAGE_INPUT_CHECKBOX || input == OS64_PAGE_INPUT_RADIO))
        raw = "on";
    size_t raw_len = element == OS64_PAGE_EL_TEXTAREA ? *len :
                     raw != NULL ? os64_strlen(raw) : 0;
    return p_sanitize_value(&page->arena, n, element, input, raw != NULL ? raw : "", raw_len, len);
}

// Scalar grammars consume terminated strings. A NUL inside the input span
// invalidates the scalar rather than making its valid prefix the value.
static bool contains_nul(const char *raw, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (raw[i] == '\0')
            return true;
    return false;
}

const char *p_sanitize_value(PArena *arena, const os64_html_node_t *n,
                            os64_page_element_t element, os64_page_input_t input,
                            const char *raw, size_t raw_len, size_t *len)
{
    *len = 0;
    if (element == OS64_PAGE_EL_TEXTAREA) {
        char *text = p_arena_copy(arena, raw, raw_len);
        if (text == NULL)
            return NULL;
        size_t write = 0;
        for (size_t read = 0; read < raw_len; read++) {
            if (text[read] == '\r') {
                text[write++] = '\n';
                if (read + 1 < raw_len && text[read + 1] == '\n')
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
        *len = raw_len;
        return raw;
    }
    switch (input) {
    case OS64_PAGE_INPUT_TEXT:
    case OS64_PAGE_INPUT_SEARCH:
    case OS64_PAGE_INPUT_TEL:
    case OS64_PAGE_INPUT_PASSWORD:
        return strip_breaks(arena, raw, raw_len, len);
    case OS64_PAGE_INPUT_URL: {
        char *out = strip_breaks(arena, raw, raw_len, len);
        if (out != NULL)
            strip_ends(out, len);
        return out;
    }
    case OS64_PAGE_INPUT_EMAIL: {
        char *out = strip_breaks(arena, raw, raw_len, len);
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
        if (contains_nul(raw, raw_len) || !p_number_parse(raw, true, &value))
            return "";
        *len = raw_len;
        return raw;
    }
    case OS64_PAGE_INPUT_DATE:
    case OS64_PAGE_INPUT_MONTH:
    case OS64_PAGE_INPUT_WEEK:
    case OS64_PAGE_INPUT_TIME:
    case OS64_PAGE_INPUT_DATETIME_LOCAL:
        if (contains_nul(raw, raw_len) || !valid_for(input, raw))
            return "";
        if (input == OS64_PAGE_INPUT_DATETIME_LOCAL)
            return datetime_value(arena,raw,raw_len,len);
        *len = raw_len;
        return raw;
    case OS64_PAGE_INPUT_RANGE:
        return p_range_value(arena, n, contains_nul(raw, raw_len) ? "" : raw, len);
    case OS64_PAGE_INPUT_COLOR:
        return color_value(arena, raw, raw_len, len);
    default:
        // Hidden values, ticks and buttons go through
        // verbatim; what each of them SENDS is family D's answer, not this
        // one's.
        *len = raw_len;
        return raw;
    }
}
