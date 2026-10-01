#include "platform.h"
#include "os64/io.h"
#include "os64/str.h"
#include "dtoa.h"
#include <limits.h>
#include <math.h>

/* This formatter covers the pinned engine's diagnostics, not general stdio.
 * Decimal floats use QuickJS's conversion with ties-even rounding, matching
 * the supported round-to-nearest floating-point environment. */
typedef struct {
    char *buffer;
    size_t cap, count, pending;
    JSPortFile *stream;
    char chunk[128];
    int failed;
} Output;

static void flush(Output *out)
{
    size_t at = 0;
    while (at < out->pending && !out->failed) {
        int64_t n = os64_write(out->stream->handle, out->chunk + at, out->pending - at);
        if (n <= 0 || (uint64_t)n > out->pending - at) out->failed = 1;
        else at += (size_t)n;
    }
    out->pending = 0;
}

static void emit(Output *out, char c)
{
    if (out->count == INT_MAX) {
        out->failed = 1;
        return;
    }
    if (out->stream != NULL) {
        if (!out->failed) {
            out->chunk[out->pending++] = c;
            if (out->pending == sizeof(out->chunk)) flush(out);
        }
    } else if (out->count + 1 < out->cap) out->buffer[out->count] = c;
    out->count++;
}

static void repeat(Output *out, char c, size_t n)
{
    while (n--) emit(out, c);
}

static int decimal(const char **at)
{
    int n = 0;
    while (**at >= '0' && **at <= '9') {
        int digit = *(*at)++ - '0';
        if (n > (INT_MAX - digit) / 10)
            jsport_fatal("format width overflow", __FILE__, __LINE__);
        n = n * 10 + digit;
    }
    return n;
}

static int format(Output *out, const char *fmt, va_list args)
{
    for (const char *p = fmt; *p;) {
        if (*p != '%') { emit(out, *p++); continue; }
        p++;
        if (*p == '%') { emit(out, *p++); continue; }
        int left = 0, zero = 0, plus = 0, space = 0, alternate = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alternate = 1;
            else break;
        }
        int width;
        if (*p == '*') {
            p++;
            width = va_arg(args, int);
            if (width == INT_MIN) jsport_fatal("format width overflow", __FILE__, __LINE__);
            if (width < 0) { width = -width; left = 1; }
        } else width = decimal(&p);
        int precision = -1;
        if (*p == '.') {
            p++;
            if (*p == '*') { p++; precision = va_arg(args, int); }
            else precision = decimal(&p);
        }
        enum {NORMAL, LONG, LONGLONG, SIZE, SHORT, CHAR} length = NORMAL;
        if (*p == 'l') { p++; length = LONG; if (*p == 'l') { p++; length = LONGLONG; } }
        else if (*p == 'z') { p++; length = SIZE; }
        else if (*p == 'h') { p++; length = SHORT; if (*p == 'h') { p++; length = CHAR; } }
        char spec = *p;
        if (spec == 0) jsport_fatal("unfinished format", __FILE__, __LINE__);
        p++;
        char text[512], prefix[3];
        const char *body = text;
        size_t len = 0, prefix_len = 0, leading = 0;
        if (spec == 's') {
            body = va_arg(args, const char *);
            if (body == NULL) body = "(null)";
            while (body[len] && (precision < 0 || len < (size_t)precision)) len++;
            zero = 0;
        } else if (spec == 'c') {
            text[0] = (char)va_arg(args, int); len = 1; zero = 0;
        } else if (spec == 'f') {
            double value = va_arg(args, double);
            int digits = precision < 0 ? 6 : precision;
            if (digits > JS_DTOA_MAX_DIGITS)
                jsport_fatal("unsupported float precision", __FILE__, __LINE__);
            if (signbit(value)) { prefix[prefix_len++] = '-'; value = -value; }
            else if (plus || space) prefix[prefix_len++] = plus ? '+' : ' ';
            if (isnan(value) || isinf(value)) {
                body = isnan(value) ? "nan" : "inf"; len = 3; zero = 0;
            } else {
                JSDTOATempMem temp;
                int flags = JS_DTOA_FORMAT_FRAC | JSPORT_DTOA_TIES_EVEN;
                if (js_dtoa_max_len(value, 10, digits, flags) >= (int)sizeof(text))
                    jsport_fatal("float format overflow", __FILE__, __LINE__);
                len = (size_t)js_dtoa(text, value, 10, digits, flags, &temp);
                if (alternate && digits == 0) text[len++] = '.';
            }
        } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' ||
                   spec == 'X' || spec == 'o' || spec == 'p') {
            uint64_t value;
            if (spec == 'd' || spec == 'i') {
                int64_t signed_value;
                if (length == LONG) signed_value = va_arg(args, long);
                else if (length == LONGLONG) signed_value = va_arg(args, long long);
                else if (length == SIZE) signed_value = va_arg(args, ptrdiff_t);
                else {
                    signed_value = va_arg(args, int);
                    if (length == SHORT) signed_value = (short)signed_value;
                    if (length == CHAR) signed_value = (signed char)signed_value;
                }
                value = (uint64_t)signed_value;
                if (signed_value < 0) { prefix[prefix_len++] = '-'; value = 0 - value; }
                else if (plus || space) prefix[prefix_len++] = plus ? '+' : ' ';
            } else if (spec == 'p') {
                value = (uintptr_t)va_arg(args, void *);
                alternate = 1;
            } else if (length == LONG) value = va_arg(args, unsigned long);
            else if (length == LONGLONG) value = va_arg(args, unsigned long long);
            else if (length == SIZE) value = va_arg(args, size_t);
            else {
                value = va_arg(args, unsigned int);
                if (length == SHORT) value = (unsigned short)value;
                if (length == CHAR) value = (unsigned char)value;
            }
            unsigned base = spec == 'o' ? 8 : (spec == 'x' || spec == 'X' || spec == 'p') ? 16 : 10;
            const char *digits = spec == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            if (alternate && base == 16 && (value != 0 || spec == 'p')) {
                prefix[prefix_len++] = '0'; prefix[prefix_len++] = spec == 'X' ? 'X' : 'x';
            }
            uint64_t remaining = value;
            do { text[len++] = digits[remaining % base]; remaining /= base; } while (remaining);
            for (size_t i = 0; i < len / 2; i++) {
                char swap = text[i]; text[i] = text[len - i - 1]; text[len - i - 1] = swap;
            }
            if (precision == 0 && value == 0) len = 0;
            if (precision > 0 && (size_t)precision > len) leading = (size_t)precision - len;
            if (alternate && base == 8 && (len == 0 || text[0] != '0') && leading == 0) leading = 1;
            if (precision >= 0) zero = 0;
        } else jsport_fatal("unsupported diagnostic format", __FILE__, __LINE__);
        size_t total = prefix_len + leading + len;
        size_t padding = (size_t)width > total ? (size_t)width - total : 0;
        if (!left && !zero) repeat(out, ' ', padding);
        for (size_t i = 0; i < prefix_len; i++) emit(out, prefix[i]);
        if (!left && zero) repeat(out, '0', padding);
        repeat(out, '0', leading);
        for (size_t i = 0; i < len; i++) emit(out, body[i]);
        if (left) repeat(out, ' ', padding);
    }
    if (out->stream != NULL) flush(out);
    else if (out->cap != 0) out->buffer[out->count < out->cap ? out->count : out->cap - 1] = 0;
    return out->failed ? -1 : (int)out->count;
}

int jsport_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    Output out = {.buffer = buf, .cap = cap};
    va_list args; va_copy(args, ap);
    int result = format(&out, fmt, args);
    va_end(args);
    return result;
}
int jsport_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int result = jsport_vsnprintf(buf, cap, fmt, ap);
    va_end(ap); return result;
}
int jsport_vfprintf(JSPortFile *stream, const char *fmt, va_list ap)
{
    Output out = {.stream = stream};
    va_list args; va_copy(args, ap);
    int result = format(&out, fmt, args);
    va_end(args); return result;
}
int jsport_fprintf(JSPortFile *stream, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int result = jsport_vfprintf(stream, fmt, ap);
    va_end(ap); return result;
}
int jsport_printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int result = jsport_vfprintf(&jsport_stdout, fmt, ap);
    va_end(ap); return result;
}
int jsport_fputc(int c, JSPortFile *stream)
{
    unsigned char byte = (unsigned char)c;
    return os64_write(stream->handle, &byte, 1) == 1 ? byte : -1;
}
int jsport_putchar(int c) { return jsport_fputc(c, &jsport_stdout); }

size_t jsport_fwrite(const void *bytes, size_t size, size_t count, JSPortFile *stream)
{
    if (size == 0 || count == 0 || count > SIZE_MAX / size) return 0;
    size_t total = size * count, written = 0;
    while (written < total) {
        int64_t n = os64_write(stream->handle, (const char *)bytes + written, total - written);
        if (n <= 0 || (uint64_t)n > total - written) break;
        written += (size_t)n;
    }
    return written / size;
}
