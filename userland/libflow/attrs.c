// attrs.c — HTML's attribute grammars: numbers, dimensions and colours as
// a page wrote them, read by the standard's own algorithms.

#include "internal.h"

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

bool f_eq_nocase(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return false;
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

// ── Numbers ─────────────────────────────────────────────────────────────

bool f_parse_integer(const char *s, int32_t *out)
{
    if (s == NULL)
        return false;
    while (space(*s))
        s++;
    bool negative = false;
    if (*s == '-' || *s == '+') {
        negative = *s == '-';
        s++;
    }
    if (!digit(*s))
        return false;
    int64_t value = 0;
    for (; digit(*s); s++)
        if (value <= F_INT_MAX)
            value = value * 10 + (*s - '0');
    if (value > F_INT_MAX)
        value = F_INT_MAX;
    *out = (int32_t)(negative ? -value : value);
    return true;
}

bool f_parse_nonnegative(const char *s, int32_t *out)
{
    int32_t value;
    if (!f_parse_integer(s, &value) || value < 0)
        return false;
    *out = value;
    return true;
}

f_dim_kind_t f_parse_dimension(const char *s, bool nonzero, int32_t *out)
{
    if (s == NULL)
        return F_DIM_NONE;
    while (space(*s))
        s++;
    if (!digit(*s))
        return F_DIM_NONE;
    // Whole units, then the fraction in 1/64ths: the unit layout keeps.
    int64_t whole = 0;
    for (; digit(*s); s++)
        if (whole <= F_INT_MAX)
            whole = whole * 10 + (*s - '0');
    if (whole > F_INT_MAX)
        whole = F_INT_MAX;
    int64_t frac64 = 0;
    if (*s == '.' && digit(s[1])) {
        s++;
        // Enough digits to settle a 1/64th; the rest cannot move it.
        int64_t num = 0, den = 1;
        for (; digit(*s); s++)
            if (den < 1000000) {
                num = num * 10 + (*s - '0');
                den *= 10;
            }
        frac64 = (num * 64 + den / 2) / den;
    }
    int64_t value = whole * 64 + frac64;
    if (nonzero && value == 0)
        return F_DIM_NONE;
    *out = (int32_t)value;
    return *s == '%' ? F_DIM_PERCENT : F_DIM_PX;
}

// ── Colours ─────────────────────────────────────────────────────────────

// CSS's named colours, which a legacy colour attribute honours before it
// tries to read the value as hex. Sorted, for the binary search.
static const struct {
    const char *name;
    uint32_t rgb;
} s_named[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff},
    {"aquamarine", 0x7fffd4}, {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4},
    {"black", 0x000000}, {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a}, {"burlywood", 0xdeb887},
    {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
    {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc},
    {"crimson", 0xdc143c}, {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b},
    {"darkgoldenrod", 0xb8860b}, {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400},
    {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b}, {"darkmagenta", 0x8b008b},
    {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0},
    {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xadff2f}, {"grey", 0x808080}, {"honeydew", 0xf0fff0},
    {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c}, {"indigo", 0x4b0082}, {"ivory", 0xfffff0},
    {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2},
    {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3},
    {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00},
    {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee},
    {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead},
    {"navy", 0x000080}, {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9},
    {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6},
    {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xff0000},
    {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513},
    {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090},
    {"snow", 0xfffafa}, {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4}, {"tan", 0xd2b48c},
    {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0},
    {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5},
    {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
};

static bool named_color(const char *s, size_t len, uint32_t *out)
{
    char key[24];
    if (len == 0 || len >= sizeof(key))
        return false;
    for (size_t i = 0; i < len; i++)
        key[i] = lower(s[i]);
    key[len] = '\0';
    int32_t lo = 0, hi = F_ARRAY(s_named) - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        int c = 0;
        const char *a = key, *b = s_named[mid].name;
        while (*a != '\0' && *a == *b) {
            a++;
            b++;
        }
        c = (unsigned char)*a - (unsigned char)*b;
        if (c == 0) {
            *out = s_named[mid].rgb;
            return true;
        }
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return false;
}

// Any string but an empty one or `transparent` is some colour. This is the
// algorithm that made `bgcolor="chucknorris"` red in every browser, and a
// page written against those browsers is drawn in the colours it was
// looked at in.
bool f_parse_legacy_color(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0')
        return false;
    size_t len = os64_strlen(s);
    while (len > 0 && space(*s)) {
        s++;
        len--;
    }
    while (len > 0 && space(s[len - 1]))
        len--;
    if (len == 11) {
        const char *t = "transparent";
        size_t i = 0;
        while (i < len && lower(s[i]) == t[i])
            i++;
        if (i == len)
            return false;
    }
    if (named_color(s, len, out))
        return true;
    if (len == 4 && s[0] == '#' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0 && hexval(s[3]) >= 0) {
        *out = (uint32_t)(hexval(s[1]) * 17) << 16 | (uint32_t)(hexval(s[2]) * 17) << 8 |
               (uint32_t)(hexval(s[3]) * 17);
        return true;
    }

    // Per code point: one outside the Basic Multilingual Plane becomes
    // "00", the rest are themselves for now; 128 characters at most.
    char in[130];
    size_t n = 0;
    for (size_t at = 0; at < len && n < 128;) {
        uint32_t cp;
        at += os64_utf8_decode(s + at, len - at, &cp);
        if (cp > 0xFFFF) {
            in[n++] = '0';
            if (n < 128)
                in[n++] = '0';
        } else {
            in[n++] = cp < 0x80 ? (char)cp : 'x';
        }
    }
    size_t from = n > 0 && in[0] == '#' ? 1 : 0;
    char hex[132];
    size_t h = 0;
    for (size_t i = from; i < n; i++)
        hex[h++] = hexval(in[i]) >= 0 ? in[i] : '0';
    while (h == 0 || h % 3 != 0)
        hex[h++] = '0';
    size_t length = h / 3;
    const char *comp[3] = {hex, hex + length, hex + 2 * length};
    if (length > 8) {
        for (int c = 0; c < 3; c++)
            comp[c] += length - 8;
        length = 8;
    }
    while (length > 2 && comp[0][0] == '0' && comp[1][0] == '0' && comp[2][0] == '0') {
        for (int c = 0; c < 3; c++)
            comp[c]++;
        length--;
    }
    uint32_t rgb = 0;
    for (int c = 0; c < 3; c++) {
        uint32_t v = (uint32_t)hexval(comp[c][0]);
        if (length >= 2)
            v = v * 16 + (uint32_t)hexval(comp[c][1]);
        rgb = rgb << 8 | v;
    }
    *out = rgb;
    return true;
}
