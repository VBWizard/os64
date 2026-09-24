// Finite binary64 conversion for HTML values. The decimal scanner and
// shortest formatter have independent upstream algorithms; HTML's lexical
// rules and its choice of fixed/exponential spelling belong here.
#include "internal.h"
#include <float.h>
#include <limits.h>
#include "upstream/ryu/ryu/ryu.h"

#if DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024
#error libpage numeric adapter requires IEEE binary64 double
#endif

#if LDBL_MANT_DIG != 64 || LDBL_MAX_EXP != 16384 || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error libpage numeric adapter requires the x86-64 extended precision ABI
#endif

union ldshape {
    long double f;
    struct { uint64_t m; uint16_t se; uint16_t pad[3]; } i;
};

#include "number_scale.inc"
#include "number_mod.inc"

enum { PN_INVALID = 1, PN_RANGE = 2 };
typedef struct {
    const char *text;
    size_t len, at;
    int error;
} PNumberInput;

static int shgetc(PNumberInput *f)
{
    size_t at = f->at++;
    return at < f->len ? (unsigned char)f->text[at] : -1;
}

static void shunget(PNumberInput *f)
{
    if (f->at != 0)
        f->at--;
}

static void shlim(PNumberInput *f, int unused)
{
    (void)unused;
    f->error = PN_INVALID;
}

#include "number_decimal.inc"

static bool number_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool p_number_parse(const char *text, bool strict, double *out)
{
    if (text == NULL)
        return false;
    const char *s = text;
    if (!strict)
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f')
            s++;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        if (strict)
            return false;
        s++;
    }
    const char *start = s;
    bool whole = false, fraction = false;
    while (number_digit(*s)) {
        whole = true;
        s++;
    }
    if (*s == '.') {
        s++;
        while (number_digit(*s)) {
            fraction = true;
            s++;
        }
        if (strict && !fraction)
            return false;
    }
    if (!whole && !fraction)
        return false;
    if (*s == 'e' || *s == 'E') {
        const char *exponent = s++;
        if (*s == '+' || *s == '-')
            s++;
        if (!number_digit(*s)) {
            if (strict)
                return false;
            s = exponent;
        } else {
            while (number_digit(*s))
                s++;
        }
    }
    if (strict && *s != '\0')
        return false;
    PNumberInput input = {start, (size_t)(s-start), 0, 0};
    double value = (double)decfloat(&input, shgetc(&input), DBL_MANT_DIG,
                                   DBL_MIN_EXP-DBL_MANT_DIG, sign, 1);
    union { double f; uint64_t bits; } bits = {value};
    if (input.error == PN_INVALID || (bits.bits >> 52 & 0x7ff) == 0x7ff)
        return false;
    *out = value;
    return true;
}

size_t p_number_spell(double value, char out[32])
{
    // HTML uses ECMAScript's fixed notation thresholds. Ryū supplies the
    // shortest round-trip digits; moving the decimal point preserves them.
    if (value == 0) {
        out[0] = '0'; out[1] = '\0';
        return 1;
    }
    char scientific[32], digits[20];
    int len = d2s_buffered_n(value, scientific);
    size_t at = 0;
    int read = 0, ndigits = 0;
    if (scientific[read] == '-') {
        out[at++] = '-';
        read++;
    }
    while (read < len && scientific[read] != 'E') {
        if (scientific[read] != '.')
            digits[ndigits++] = scientific[read];
        read++;
    }
    read++;
    bool negative = read < len && scientific[read] == '-';
    if (negative)
        read++;
    int exponent = 0;
    while (read < len)
        exponent = exponent * 10 + scientific[read++] - '0';
    if (negative)
        exponent = -exponent;
    if (exponent >= -6 && exponent < 21) {
        int point = exponent + 1;
        if (point <= 0) {
            out[at++] = '0'; out[at++] = '.';
            for (int i = 0; i < -point; i++)
                out[at++] = '0';
        }
        for (int i = 0; i < ndigits; i++) {
            if (point > 0 && i == point)
                out[at++] = '.';
            out[at++] = digits[i];
        }
        for (int i = ndigits; i < point; i++)
            out[at++] = '0';
    } else {
        out[at++] = digits[0];
        if (ndigits > 1)
            out[at++] = '.';
        for (int i = 1; i < ndigits; i++)
            out[at++] = digits[i];
        out[at++] = 'e';
        out[at++] = exponent < 0 ? '-' : '+';
        unsigned e = (unsigned)(exponent < 0 ? -exponent : exponent);
        if (e >= 100)
            out[at++] = (char)('0' + e / 100);
        if (e >= 10)
            out[at++] = (char)('0' + e / 10 % 10);
        out[at++] = (char)('0' + e % 10);
    }
    out[at] = '\0';
    return at;
}
