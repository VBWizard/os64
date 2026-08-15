// date.c — the calendar half of the time() split (header has the doctrine).
//
// The epoch→civil conversion is the "days from civil era" algorithm: group
// the calendar into 400-year eras of exactly 146097 days (the Gregorian
// cycle — 400 years of leap rules come out to a fixed count, which is the
// whole trick), and shift the year to start in March so leap day lands at
// the END of the counting year and the month-length pattern becomes the
// single linear ramp (153*m+2)/5. No tables, no year loop, no special
// cases — correct across the full range including pre-1970. This is the
// clean modern derivation (Howard Hinnant's), not a port of the kernel's
// os32-inherited gmtime; the library owed itself the good one.

#include "os64/date.h"
#include "os64/proc.h"     // os64_getenv — where the TZ policy arrives from
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"
#include <stddef.h>

static void date_set_zone(os64_date_t *date, int32_t offset, const char *name)
{
    date->utc_offset_minutes = offset;
    size_t i = 0;
    if (name != NULL) {
        while (name[i] != '\0' && i + 1 < sizeof(date->zone)) {
            date->zone[i] = name[i];
            i++;
        }
    }
    date->zone[i] = '\0';
}

int64_t os64_time(os64_time_t *t)
{
    return (int64_t)os64_syscall1(SYSCALL_TIME, (uint64_t)t);
}

int64_t os64_set_time(int64_t epoch)
{
    return (int64_t)os64_syscall1(SYSCALL_SET_TIME, (uint64_t)epoch);
}

void os64_date_from_epoch(int64_t epoch, os64_date_t *out)
{
    // Split into whole days and seconds-into-day, flooring (C's / and %
    // truncate toward zero, which is wrong for pre-1970: -1 second must be
    // 23:59:59 of December 31, 1969, not a negative clock).
    int64_t days = epoch / 86400;
    int64_t secs = epoch % 86400;
    if (secs < 0) {
        secs += 86400;
        days -= 1;
    }

    out->hour   = (int32_t)(secs / 3600);
    out->minute = (int32_t)((secs / 60) % 60);
    out->second = (int32_t)(secs % 60);

    // January 1, 1970 was a Thursday (4). Same floor care as above.
    int64_t wd = (days + 4) % 7;
    if (wd < 0)
        wd += 7;
    out->weekday = (int32_t)wd;

    // civil_from_days: 719468 shifts day 0 from 1970-01-01 to 0000-03-01,
    // the start of an era. Within an era, the nested corrections peel off
    // the 4/100/400-year leap rules; the (153*mp+2)/5 ramp is the lengths
    // of March..February encoded as one line.
    days += 719468;
    int64_t  era = (days >= 0 ? days : days - 146096) / 146097;
    uint64_t doe = (uint64_t)(days - era * 146097);                        // [0, 146096]
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    int64_t  y   = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
    uint64_t mp  = (5 * doy + 2) / 153;                                    // [0, 11]
    uint64_t d   = doy - (153 * mp + 2) / 5 + 1;                           // [1, 31]
    uint64_t m   = mp < 10 ? mp + 3 : mp - 9;                              // [1, 12]

    out->year  = (int32_t)(y + (m <= 2));   // Jan/Feb belong to the NEXT civil year
    out->month = (int32_t)m;
    out->day   = (int32_t)d;
    date_set_zone(out, 0, "UTC");
}

// ---- The TZ engine ---------------------------------------------------------
// (Format doctrine in the header. Everything below is pure math and string
// work — the only syscall in this file stays up in os64_time.)

// days_from_civil — the exact inverse of the eras trick in
// os64_date_from_epoch: calendar date -> days since 1970-01-01.
static int64_t days_from_civil(int32_t y, int32_t m, int32_t d)
{
    y -= (m <= 2);
    int64_t  era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);                            // [0, 399]
    uint32_t doy = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5
                              + d - 1);                                  // [0, 365]
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

static int32_t days_in_month(int32_t year, int32_t month)
{
    static const int32_t days[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && year % 4 == 0 &&
        (year % 100 != 0 || year % 400 == 0))
        return 29;
    return days[month - 1];
}

int64_t os64_date_to_epoch(const os64_date_t *date, int64_t *epoch)
{
    if (date == NULL || epoch == NULL ||
        date->month < 1 || date->month > 12 ||
        date->day < 1 || date->day > days_in_month(date->year, date->month) ||
        date->hour < 0 || date->hour > 23 ||
        date->minute < 0 || date->minute > 59 ||
        date->second < 0 || date->second > 59)
        return -1;

    *epoch = days_from_civil(date->year, date->month, date->day) * 86400
             + (int64_t)date->hour * 3600
             + (int64_t)date->minute * 60
             + date->second;
    return 0;
}

static int32_t is_letter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int32_t is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// A zone name: 3+ letters (the classic minimum). Copies up to 7 into dst,
// returns the cursor past the name, or NULL if it isn't one.
static const char *tz_scan_name(const char *p, char *dst)
{
    int32_t n = 0;
    while (is_letter(*p)) {
        if (n < 7)
            dst[n++] = *p;
        p++;
    }
    dst[n] = '\0';
    return (n >= 3) ? p : NULL;
}

// An offset: [+|-]h[h][:mm], hours WEST of UTC (the V7 prank) — converted
// here, once, to east-positive minutes so no other line of os64 ever has to
// think about it again.
static const char *tz_scan_offset(const char *p, int32_t *east_min)
{
    int32_t sign = 1;
    if (*p == '+')
        p++;
    else if (*p == '-') {
        sign = -1;
        p++;
    }
    if (!is_digit(*p))
        return NULL;
    int32_t h = 0;
    while (is_digit(*p))
        h = h * 10 + (*p++ - '0');
    int32_t m = 0;
    if (*p == ':') {
        p++;
        if (!is_digit(*p))
            return NULL;
        while (is_digit(*p))
            m = m * 10 + (*p++ - '0');
    }
    if (h > 24 || m > 59)
        return NULL;
    *east_min = -sign * (h * 60 + m);
    return p;
}

// A transition rule: ,M<month>.<week>.<weekday>[/h[:mm]]. Only the M form —
// it is the one humans write and the only one with named semantics.
static const char *tz_scan_rule(const char *p, os64_tz_rule_t *r)
{
    if (*p != ',' || p[1] != 'M')
        return NULL;
    p += 2;

    int32_t vals[3];
    for (int32_t i = 0; i < 3; i++) {
        if (!is_digit(*p))
            return NULL;
        int32_t v = 0;
        while (is_digit(*p))
            v = v * 10 + (*p++ - '0');
        vals[i] = v;
        if (i < 2) {
            if (*p != '.')
                return NULL;
            p++;
        }
    }
    if (vals[0] < 1 || vals[0] > 12 || vals[1] < 1 || vals[1] > 5 || vals[2] > 6)
        return NULL;
    r->month   = vals[0];
    r->week    = vals[1];
    r->weekday = vals[2];

    r->minute = 120;   // 2:00 local — POSIX's default switch time
    if (*p == '/') {
        p++;
        if (!is_digit(*p))
            return NULL;
        int32_t h = 0;
        while (is_digit(*p))
            h = h * 10 + (*p++ - '0');
        int32_t m = 0;
        if (*p == ':') {
            p++;
            if (!is_digit(*p))
                return NULL;
            while (is_digit(*p))
                m = m * 10 + (*p++ - '0');
        }
        if (h > 24 || m > 59)
            return NULL;
        r->minute = h * 60 + m;
    }
    return p;
}

int64_t os64_tz_parse(const char *s, os64_tz_t *tz)
{
    if (s == NULL || tz == NULL)
        return -1;
    *tz = (os64_tz_t){0};

    const char *p = tz_scan_name(s, tz->std_name);
    if (p == NULL)
        return -1;
    p = tz_scan_offset(p, &tz->std_offset_min);
    if (p == NULL)
        return -1;

    if (is_letter(*p)) {
        p = tz_scan_name(p, tz->dst_name);
        if (p == NULL)
            return -1;
        tz->has_dst = 1;
        // Daylight offset defaults to one hour AHEAD of standard; an
        // explicit one (rare, but real: Lord Howe Island springs 30 min)
        // may follow the name.
        tz->dst_offset_min = tz->std_offset_min + 60;
        if (*p == '+' || *p == '-' || is_digit(*p)) {
            p = tz_scan_offset(p, &tz->dst_offset_min);
            if (p == NULL)
                return -1;
        }
        // Rules omitted = the US rule, matching POSIX's default: second
        // Sunday of March to first Sunday of November, 2:00 local.
        tz->dst_start = (os64_tz_rule_t){ .month = 3,  .week = 2, .weekday = 0, .minute = 120 };
        tz->dst_end   = (os64_tz_rule_t){ .month = 11, .week = 1, .weekday = 0, .minute = 120 };
        if (*p == ',') {
            p = tz_scan_rule(p, &tz->dst_start);
            if (p == NULL)
                return -1;
            p = tz_scan_rule(p, &tz->dst_end);
            if (p == NULL)
                return -1;
        }
    }
    return (*p == '\0') ? 0 : -1;
}

// Turn "week w, weekday d of month m in year y, at minute t LOCAL, offset
// o east" into the UTC epoch of that instant. week 5 = last: overshoot and
// pull back.
static int64_t rule_to_epoch(int32_t year, const os64_tz_rule_t *r,
                             int32_t offset_east_min)
{
    static const int32_t mdays[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    int64_t first = days_from_civil(year, r->month, 1);
    int32_t wd_first = (int32_t)((first + 4) % 7);   // 1970-01-01 was Thursday
    if (wd_first < 0)
        wd_first += 7;

    int32_t day = 1 + ((r->weekday - wd_first + 7) % 7) + (r->week - 1) * 7;
    int32_t dim = mdays[r->month - 1];
    if (r->month == 2 &&
        (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        dim = 29;
    while (day > dim)
        day -= 7;

    return days_from_civil(year, r->month, day) * 86400
           + (int64_t)r->minute * 60
           - (int64_t)offset_east_min * 60;
}

int32_t os64_tz_dst_at(const os64_tz_t *tz, int64_t epoch)
{
    if (!tz->has_dst)
        return 0;

    // Which year's rules apply is judged in local STANDARD time — stable on
    // both sides of every transition.
    os64_date_t d;
    os64_date_from_epoch(epoch + (int64_t)tz->std_offset_min * 60, &d);

    // The start instant reads its clock in standard time (that's what's in
    // force when it fires); the end instant reads its in daylight time.
    int64_t start = rule_to_epoch(d.year, &tz->dst_start, tz->std_offset_min);
    int64_t end   = rule_to_epoch(d.year, &tz->dst_end,   tz->dst_offset_min);

    if (start <= end)                       // northern hemisphere
        return epoch >= start && epoch < end;
    return epoch >= start || epoch < end;   // southern: DST wraps New Year
}

int32_t os64_tz_offset_at(const os64_tz_t *tz, int64_t epoch)
{
    return os64_tz_dst_at(tz, epoch) ? tz->dst_offset_min
                                     : tz->std_offset_min;
}

void os64_date_from_epoch_tz(int64_t epoch, const os64_tz_t *tz,
                             os64_date_t *out)
{
    int32_t daylight = os64_tz_dst_at(tz, epoch);
    int32_t offset = daylight ? tz->dst_offset_min : tz->std_offset_min;
    os64_date_from_epoch(epoch + (int64_t)offset * 60, out);
    date_set_zone(out, offset, daylight ? tz->dst_name : tz->std_name);
}

int64_t os64_date_now(os64_date_t *out, os64_time_t *raw)
{
    os64_time_t t;
    int64_t r = os64_time(&t);
    if (r != 0)
        return r;

    // Policy resolution (header has the doctrine): a parseable TZ in the
    // environment wins — offset and DST both. Anything else (unset, or
    // malformed — a broken TZ should give you standard time, not garbage)
    // falls back to the kernel's configured standard offset.
    // os64_localtime below applies THE SAME policy to arbitrary moments —
    // change one, change both, or "now" and "then" start disagreeing
    // about what local means.
    const char *tzs = os64_getenv("TZ");
    os64_tz_t tz;
    if (tzs != NULL && os64_tz_parse(tzs, &tz) == 0)
        os64_date_from_epoch_tz(t.epoch, &tz, out);
    else {
        os64_date_from_epoch(t.epoch + (int64_t)t.tz_offset_minutes * 60, out);
        date_set_zone(out, t.tz_offset_minutes,
                      t.tz_offset_minutes == 0 ? "UTC" : "");
    }

    if (raw != NULL)
        *raw = t;
    return 0;
}

// os64_date_now for a moment that ISN'T now — the mtime renderer (born
// 2026-08-06, hours after dirents learned what time it is; ls -l is the
// first customer). SAME policy as os64_date_now, and the DST evaluation
// happens AT THE MOMENT BEING CONVERTED: a January mtime renders in EST
// while your July clock reads EDT — which is DST done right, and the whole
// reason the library owns this instead of every app hand-rolling offsets.
// The TZ-env path costs no syscall; the fallback costs one time() call to
// learn the machine offset. Callers with thousands of entries can afford
// either — a TZ parse is microseconds — and a cached-zone variant can join
// the day a profile says so, not before.
int64_t os64_localtime(int64_t epoch, os64_date_t *out)
{
    const char *tzs = os64_getenv("TZ");
    os64_tz_t tz;
    if (tzs != NULL && os64_tz_parse(tzs, &tz) == 0)
    {
        os64_date_from_epoch_tz(epoch, &tz, out);
        return 0;
    }

    os64_time_t t;
    if (os64_time(&t) == 0) {
        os64_date_from_epoch(epoch + (int64_t)t.tz_offset_minutes * 60, out);
        date_set_zone(out, t.tz_offset_minutes,
                      t.tz_offset_minutes == 0 ? "UTC" : "");
    } else
        os64_date_from_epoch(epoch, out);   // no zone anywhere: UTC, honestly
    return 0;
}

static int32_t same_civil_time(const os64_date_t *a, const os64_date_t *b)
{
    return a->year == b->year && a->month == b->month && a->day == b->day &&
           a->hour == b->hour && a->minute == b->minute &&
           a->second == b->second;
}

int64_t os64_mktime(const os64_date_t *date, int64_t *epoch)
{
    int64_t naive;
    if (os64_date_to_epoch(date, &naive) != 0 || epoch == NULL)
        return -1;

    const char *tzs = os64_getenv("TZ");
    os64_tz_t tz;
    if (tzs != NULL && os64_tz_parse(tzs, &tz) == 0) {
        // Try standard first so the repeated fall-back hour has one stable
        // interpretation. Then try daylight; a spring-forward gap matches
        // neither candidate and is correctly rejected.
        int32_t offsets[2] = { tz.std_offset_min, tz.dst_offset_min };
        int32_t count = tz.has_dst ? 2 : 1;
        for (int32_t i = 0; i < count; i++) {
            int64_t candidate = naive - (int64_t)offsets[i] * 60;
            if (os64_tz_offset_at(&tz, candidate) != offsets[i])
                continue;
            os64_date_t check;
            os64_date_from_epoch_tz(candidate, &tz, &check);
            if (same_civil_time(date, &check)) {
                *epoch = candidate;
                return 0;
            }
        }
        return -1;
    }

    os64_time_t now;
    int32_t offset = os64_time(&now) == 0 ? now.tz_offset_minutes : 0;
    *epoch = naive - (int64_t)offset * 60;
    return 0;
}

// ---- strftime -------------------------------------------------------------

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} date_writer_t;

static void writer_char(date_writer_t *writer, char c)
{
    if (writer->capacity > 0 && writer->length + 1 < writer->capacity)
        writer->buffer[writer->length] = c;
    writer->length++;
}

static void writer_text(date_writer_t *writer, const char *text)
{
    while (*text != '\0')
        writer_char(writer, *text++);
}

static void writer_uint(date_writer_t *writer, uint64_t value,
                        int32_t width, char pad)
{
    char digits[24];
    int32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count < width) {
        writer_char(writer, pad);
        width--;
    }
    while (count > 0)
        writer_char(writer, digits[--count]);
}

static void writer_year(date_writer_t *writer, int32_t year)
{
    int64_t wide = year;
    if (wide < 0) {
        writer_char(writer, '-');
        wide = -wide;
    }
    writer_uint(writer, (uint64_t)wide, 4, '0');
}

static void date_format(date_writer_t *writer, const char *format,
                        const os64_date_t *date)
{
    static const char *const short_weekdays[7] =
        { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const long_weekdays[7] =
        { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const short_months[12] =
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *const long_months[12] =
        { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

    while (*format != '\0') {
        if (*format != '%') {
            writer_char(writer, *format++);
            continue;
        }
        format++;
        char conversion = *format;
        if (conversion == '\0') {
            writer_char(writer, '%');
            break;
        }
        format++;

        switch (conversion) {
        case 'a':
            writer_text(writer, date->weekday >= 0 && date->weekday < 7
                                  ? short_weekdays[date->weekday] : "???");
            break;
        case 'A':
            writer_text(writer, date->weekday >= 0 && date->weekday < 7
                                  ? long_weekdays[date->weekday] : "???");
            break;
        case 'b':
            writer_text(writer, date->month >= 1 && date->month <= 12
                                  ? short_months[date->month - 1] : "???");
            break;
        case 'B':
            writer_text(writer, date->month >= 1 && date->month <= 12
                                  ? long_months[date->month - 1] : "???");
            break;
        case 'c': date_format(writer, "%a %b %e %H:%M:%S %Y", date); break;
        case 'd': writer_uint(writer, (uint32_t)date->day, 2, '0'); break;
        case 'e': writer_uint(writer, (uint32_t)date->day, 2, ' '); break;
        case 'F': date_format(writer, "%Y-%m-%d", date); break;
        case 'H': writer_uint(writer, (uint32_t)date->hour, 2, '0'); break;
        case 'I': {
            int32_t hour = date->hour % 12;
            writer_uint(writer, (uint32_t)(hour == 0 ? 12 : hour), 2, '0');
            break;
        }
        case 'j': {
            int64_t day = days_from_civil(date->year, date->month, date->day)
                          - days_from_civil(date->year, 1, 1) + 1;
            writer_uint(writer, (uint64_t)day, 3, '0');
            break;
        }
        case 'm': writer_uint(writer, (uint32_t)date->month, 2, '0'); break;
        case 'M': writer_uint(writer, (uint32_t)date->minute, 2, '0'); break;
        case 'n': writer_char(writer, '\n'); break;
        case 'p': writer_text(writer, date->hour < 12 ? "AM" : "PM"); break;
        case 'S': writer_uint(writer, (uint32_t)date->second, 2, '0'); break;
        case 't': writer_char(writer, '\t'); break;
        case 'T': date_format(writer, "%H:%M:%S", date); break;
        case 'u': writer_uint(writer, date->weekday == 0 ? 7u : (uint32_t)date->weekday, 1, '0'); break;
        case 'w': writer_uint(writer, (uint32_t)date->weekday, 1, '0'); break;
        case 'x': date_format(writer, "%m/%d/%y", date); break;
        case 'X': date_format(writer, "%H:%M:%S", date); break;
        case 'y': {
            int32_t year = date->year % 100;
            if (year < 0)
                year = -year;
            writer_uint(writer, (uint32_t)year, 2, '0');
            break;
        }
        case 'Y': writer_year(writer, date->year); break;
        case 'z': {
            int32_t offset = date->utc_offset_minutes;
            writer_char(writer, offset < 0 ? '-' : '+');
            int64_t absolute = offset;
            if (absolute < 0)
                absolute = -absolute;
            writer_uint(writer, (uint64_t)(absolute / 60), 2, '0');
            writer_uint(writer, (uint64_t)(absolute % 60), 2, '0');
            break;
        }
        case 'Z': writer_text(writer, date->zone); break;
        case '%': writer_char(writer, '%'); break;
        default:
            // Preserve an extension we do not know instead of silently
            // deleting it; callers can spot the unsupported conversion.
            writer_char(writer, '%');
            writer_char(writer, conversion);
            break;
        }
    }
}

size_t os64_strftime(char *buffer, size_t capacity, const char *format,
                     const os64_date_t *date)
{
    if (buffer == NULL || capacity == 0 || format == NULL || date == NULL)
        return 0;

    date_writer_t writer = { .buffer = buffer, .capacity = capacity, .length = 0 };
    date_format(&writer, format, date);
    size_t end = writer.length < capacity ? writer.length : capacity - 1;
    buffer[end] = '\0';
    return writer.length < capacity ? writer.length : 0;
}
