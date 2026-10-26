#include "internal.h"

typedef struct {
    Context *context;
    svg_string_t text;
    size_t at;
} Parser;

static bool digit(char ch)
{
    return ch >= '0' && ch <= '9';
}
static char peek(const Parser *p)
{
    return p->at < p->text.size ? p->text.data[p->at] : 0;
}
static bool take(Parser *p)
{
    if (!svg_spend(p->context, 1))
        return false;
    ++p->at;
    return true;
}
static bool whitespace(Parser *p)
{
    while (p->at < p->text.size) {
        char ch = peek(p);
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f')
            break;
        if (!take(p))
            return false;
    }
    return true;
}
static bool separator(Parser *p, bool required)
{
    size_t start = p->at;
    if (!whitespace(p))
        return false;
    if (peek(p) == ',') {
        if (!take(p) || !whitespace(p))
            return false;
    }
    return !required || start != p->at;
}
/* Locale-free decimal scanner, with bounded significant storage and bounded
 * exponent arithmetic. It accepts SVG 1.1's unambiguous trailing decimal dot.
 * Conversion uses binary64 arithmetic; it is not a correctly-rounded strtod.
 * Range loss is LIMIT, separate from malformed grammar. */
static bool number(Parser *p, double *out)
{
    bool negative = peek(p) == '-';
    if (peek(p) == '+' || negative)
        if (!take(p))
            return false;
    uint64_t mantissa = 0;
    size_t digits = 0, fraction = 0, discarded = 0;
    unsigned significant = 0;
    bool dot = false;
    while (digit(peek(p)) || (!dot && peek(p) == '.')) {
        char ch = peek(p);
        if (!take(p))
            return false;
        if (ch == '.') {
            dot = true;
            continue;
        }
        ++digits;
        if (dot)
            ++fraction;
        if (significant || ch != '0') {
            if (significant < 19) {
                mantissa = mantissa * 10 + (unsigned)(ch - '0');
                ++significant;
            } else {
                ++discarded;
            }
        }
    }
    if (!digits)
        return false;
    int64_t exponent = 0;
    /* Saturation must exceed the input-length cap: discarded mantissa digits
     * and fractional zeros can otherwise cancel a saturated exponent into a
     * false in-range value. This bound still leaves arithmetic headroom. */
    const int64_t exponent_limit = (int64_t)INT32_MAX + 1024;
    bool exp_negative = false;
    if (peek(p) == 'e' || peek(p) == 'E') {
        if (!take(p))
            return false;
        exp_negative = peek(p) == '-';
        if (peek(p) == '+' || exp_negative)
            if (!take(p))
                return false;
        if (!digit(peek(p)))
            return false;
        while (digit(peek(p))) {
            if (exponent < exponent_limit) {
                exponent = exponent * 10 + (peek(p) - '0');
                if (exponent > exponent_limit)
                    exponent = exponent_limit;
            }
            if (!take(p))
                return false;
        }
    }
    if (mantissa == 0) {
        *out = negative ? -0.0 : 0.0;
        return true;
    }
    /* Input is capped at INT32_MAX, so these differences fit int64_t. */
    int64_t scale = (exp_negative ? -exponent : exponent) + (int64_t)discarded - (int64_t)fraction;
    if (scale > 308 || scale < -343) {
        p->context->why = SVG_LIMIT;
        return false;
    }
    double value = (double)mantissa;
    if (scale < -308) {
        value *= pow(10.0, (double)(scale + 308));
        value *= 1e-308;
    } else {
        value *= pow(10.0, (double)scale);
    }
    if (!isfinite(value) || value == 0) {
        p->context->why = SVG_LIMIT;
        return false;
    }
    *out = negative ? -value : value;
    return true;
}
static char upper(char ch)
{
    return ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : ch;
}
static bool command_letter(char ch)
{
    switch (upper(ch)) {
    case 'M':
    case 'L':
    case 'H':
    case 'V':
    case 'C':
    case 'S':
    case 'Q':
    case 'T':
    case 'A':
    case 'Z':
        return true;
    default:
        return false;
    }
}
static Point point(double x, double y, Point current, bool relative)
{
    return (Point){x + (relative ? current.x : 0), y + (relative ? current.y : 0)};
}
static Point reflected(Point current, Point control)
{
    return (Point){current.x + (current.x - control.x), current.y + (current.y - control.y)};
}
static bool append(Context *c, svg_path_t *p, svg_command_t cmd)
{
    for (size_t i = 0; i < 3; ++i) {
        if (!svg_point_valid(cmd.p[i])) {
            c->why = SVG_LIMIT;
            return false;
        }
    }
    p->commands = svg_grow(c, p->commands, &p->capacity, p->count, sizeof(*p->commands),
                           c->options.limits.path_commands);
    if (c->why != SVG_OK)
        return false;
    p->commands[p->count++] = cmd;
    return true;
}
void svg_free_path(Context *c, svg_path_t *path)
{
    if (!path)
        return;
    svg_free(c, path->commands);
    svg_free(c, path);
    --c->stats.paths;
}
Status svg_parse(Context *c, svg_string_t text, svg_path_t **out, svg_parse_result_t *result)
{
    *out = NULL;
    *result = (svg_parse_result_t){SVG_OK, 0};
    if ((!text.data && text.size) || text.size > c->options.limits.input_bytes ||
        text.size > INT32_MAX) {
        result->status = !text.data && text.size ? SVG_BAD_ARGUMENT : SVG_LIMIT;
        return result->status;
    }
    svg_path_t *path = svg_alloc(c, sizeof(*path));
    if (!path) {
        result->status = c->why;
        return c->why;
    }
    *path = (svg_path_t){.owner = c};
    ++c->stats.paths;
    Parser p = {c, text, 0};
    Point current = {0, 0}, start = {0, 0}, control = {0, 0};
    char repeat = 0, previous = 0;
    bool malformed = false;
    while (whitespace(&p) && p.at < text.size) {
        if (!svg_spend(c, 1))
            break;
        char op = repeat;
        bool explicit = command_letter(peek(&p));
        if (explicit) {
            op = peek(&p);
            if (!take(&p) || !whitespace(&p))
                break;
        } else if (repeat) {
            if (!separator(&p, false))
                break;
        }
        char verb = upper(op);
        bool relative = op >= 'a' && op <= 'z';
        if (!op || (!path->count && verb != 'M')) {
            malformed = true;
            break;
        }
        svg_command_t cmd = {0};
        if (verb == 'Z') {
            cmd.verb = SVG_CLOSE;
            if (!append(c, path, cmd))
                break;
            current = start;
            previous = 'Z';
            repeat = 0;
            continue;
        }
        unsigned count = verb == 'H' || verb == 'V'   ? 1
                         : verb == 'C'                ? 6
                         : verb == 'Q' || verb == 'S' ? 4
                         : verb == 'A'                ? 7
                                                      : 2;
        double v[7] = {0};
        bool complete = true;
        for (unsigned i = 0; i < count; ++i) {
            if (i && !separator(&p, verb == 'A' && i == 3)) {
                complete = false;
                break;
            }
            if (verb == 'A' && (i == 3 || i == 4)) {
                char flag = peek(&p);
                if ((flag != '0' && flag != '1') || !take(&p)) {
                    complete = false;
                    break;
                }
                v[i] = flag - '0';
            } else if (!number(&p, &v[i])) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            malformed = true;
            break;
        }
        Point end = current;
        switch (verb) {
        case 'M':
        case 'L':
        case 'T':
            end = point(v[0], v[1], current, relative);
            cmd.verb = verb == 'M' ? SVG_MOVE : verb == 'T' ? SVG_QUADRATIC : SVG_LINE;
            if (verb == 'T') {
                cmd.p[0] =
                    previous == 'Q' || previous == 'T' ? reflected(current, control) : current;
                control = cmd.p[0];
                cmd.p[1] = end;
            } else {
                cmd.p[0] = end;
            }
            break;
        case 'H':
        case 'V':
            if (verb == 'H')
                end.x = v[0] + (relative ? current.x : 0);
            else
                end.y = v[0] + (relative ? current.y : 0);
            cmd.verb = SVG_LINE;
            cmd.p[0] = end;
            break;
        case 'C':
        case 'S':
            cmd.verb = SVG_CUBIC;
            if (verb == 'C') {
                cmd.p[0] = point(v[0], v[1], current, relative);
                cmd.p[1] = point(v[2], v[3], current, relative);
                end = point(v[4], v[5], current, relative);
            } else {
                cmd.p[0] =
                    previous == 'C' || previous == 'S' ? reflected(current, control) : current;
                cmd.p[1] = point(v[0], v[1], current, relative);
                end = point(v[2], v[3], current, relative);
            }
            control = cmd.p[1];
            cmd.p[2] = end;
            break;
        case 'Q':
            cmd.verb = SVG_QUADRATIC;
            cmd.p[0] = point(v[0], v[1], current, relative);
            control = cmd.p[0];
            end = point(v[2], v[3], current, relative);
            cmd.p[1] = end;
            break;
        case 'A':
            cmd.verb = SVG_ARC;
            cmd.rx = fabs(v[0]);
            cmd.ry = fabs(v[1]);
            cmd.rotation = v[2];
            cmd.large = v[3] != 0;
            cmd.sweep = v[4] != 0;
            end = point(v[5], v[6], current, relative);
            cmd.p[0] = end;
            break;
        default:
            malformed = true;
            break;
        }
        if (malformed || !append(c, path, cmd))
            break;
        current = end;
        if (verb == 'M')
            start = current;
        previous = verb;
        repeat = verb == 'M' ? (relative ? 'l' : 'L') : op;
    }
    result->offset = p.at;
    result->status = c->why != SVG_OK ? c->why : malformed ? SVG_MALFORMED : SVG_OK;
    if (result->status == SVG_OK || result->status == SVG_MALFORMED)
        *out = path;
    else
        svg_free_path(c, path);
    return result->status;
}
Status svg_path_parse(Context *c, svg_string_t text, svg_path_t **out, svg_parse_result_t *result)
{
    if (out)
        *out = NULL;
    if (result)
        *result = (svg_parse_result_t){SVG_BAD_ARGUMENT, 0};
    if (!out || !result)
        return SVG_BAD_ARGUMENT;
    Status s = svg_enter(c);
    if (s != SVG_OK) {
        result->status = s;
        return s;
    }
    s = svg_parse(c, text, out, result);
    return svg_leave(c, s);
}
const svg_command_t *svg_path_commands(const svg_path_t *p, size_t *count)
{
    if (count)
        *count = p ? p->count : 0;
    return p ? p->commands : NULL;
}
Status svg_path_release(Context *c, svg_path_t *p)
{
    if (!c || !p || p->owner != c)
        return SVG_BAD_ARGUMENT;
    if (c->busy || p->in_scene)
        return SVG_BUSY;
    c->busy = true;
    svg_free_path(c, p);
    return svg_leave(c, SVG_OK);
}
