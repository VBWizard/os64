// test — evaluate an expression through its exit status.

#include "os64/os64.h"

#define TEST_MAX_DEPTH 64

typedef struct {
    int32_t argc;
    char **argv;
    int32_t at;
    int32_t depth;
    const char *error;
    const char *error_arg;
    const char *program;
} test_parser_t;

static bool token_is(const char *token, const char *expected)
{
    return token != NULL && os64_streq(token, expected);
}

// THE TABLES HOLD WHAT OS64 CAN ANSWER, nothing more. Unix test's `-r -w -x
// -u -g -k -O -G -L -h -S -p -b -c` ask about permissions, owners, setuid
// bits, symlinks and device nodes — none of which this system has. They are
// not listed as "unsupported": they are unknown here, and an operator-shaped
// word that is not in these tables is refused by name (status 2), which is
// the reminder a script pasted from elsewhere is owed.
static bool is_unary_operator(const char *token)
{
    static const char *operators[] = {
        "-d", "-e", "-f", "-s", "-n", "-z"
    };

    for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); i++)
    {
        if (token_is(token, operators[i]))
            return true;
    }
    return false;
}

static bool is_binary_operator(const char *token)
{
    static const char *operators[] = {
        "=", "==", "!=", "<", ">", "-eq", "-ne", "-gt", "-ge",
        "-lt", "-le", "-nt", "-ot"
    };

    for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); i++)
    {
        if (token_is(token, operators[i]))
            return true;
    }
    return false;
}

// A word shaped like an operator: a dash and one or two letters. Used only
// to decide that a token in an operator's POSITION which the tables do not
// know deserves "unknown operator" rather than "unexpected argument".
static bool looks_like_operator(const char *token)
{
    if (token == NULL || token[0] != '-' || token[1] == '\0')
        return false;
    if (token_is(token, "-a") || token_is(token, "-o"))
        return false;                      // the connectives, parsed above this level
    for (const char *p = token + 1; *p != '\0'; p++)
    {
        bool letter = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
        if (!letter || p - token > 2)
            return false;
    }
    return true;
}

static bool fail(test_parser_t *parser, const char *error, const char *arg)
{
    if (parser->error == NULL)
    {
        parser->error = error;
        parser->error_arg = arg;
    }
    return false;
}

static int compare_strings(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right)
    {
        left++;
        right++;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

static bool parse_integer(test_parser_t *parser, const char *text,
                          int64_t *value)
{
    const char *p = text;
    bool negative = false;

    if (*p == '+' || *p == '-')
    {
        negative = *p == '-';
        p++;
    }
    if (*p == '\0')
        return fail(parser, "integer expression expected", text);

    uint64_t magnitude = 0;
    uint64_t limit = negative ? 9223372036854775808ULL
                              : 9223372036854775807ULL;
    while (*p != '\0')
    {
        if (*p < '0' || *p > '9')
            return fail(parser, "integer expression expected", text);
        uint64_t digit = (uint64_t)(*p - '0');
        if (magnitude > (limit - digit) / 10)
            return fail(parser, "integer out of range", text);
        magnitude = magnitude * 10 + digit;
        p++;
    }

    if (negative)
        *value = magnitude == 9223372036854775808ULL
            ? -(int64_t)(magnitude - 1) - 1
            : -(int64_t)magnitude;
    else
        *value = (int64_t)magnitude;
    return true;
}

static bool stat_path(const char *path, os64_dirent_t *entry)
{
    os64_memset(entry, 0, sizeof(*entry));
    return os64_stat(path, entry) == 0;
}

static bool evaluate_unary(test_parser_t *parser, const char *op,
                           const char *operand)
{
    if (token_is(op, "-n"))
        return operand[0] != '\0';
    if (token_is(op, "-z"))
        return operand[0] == '\0';

    os64_dirent_t entry;
    bool exists = stat_path(operand, &entry);
    if (token_is(op, "-e"))
        return exists;
    if (token_is(op, "-d"))
        return exists && (entry.flags & OS64_DE_DIR) != 0;
    if (token_is(op, "-f"))
        return exists && (entry.flags & OS64_DE_DIR) == 0;
    if (token_is(op, "-s"))
        return exists && entry.size != 0;

    return fail(parser, "unknown operator", op);
}

static bool evaluate_binary(test_parser_t *parser, const char *left,
                            const char *op, const char *right)
{
    if (token_is(op, "=") || token_is(op, "=="))
        return os64_streq(left, right);
    if (token_is(op, "!="))
        return !os64_streq(left, right);
    if (token_is(op, "<"))
        return compare_strings(left, right) < 0;
    if (token_is(op, ">"))
        return compare_strings(left, right) > 0;

    if (token_is(op, "-eq") || token_is(op, "-ne") ||
        token_is(op, "-gt") || token_is(op, "-ge") ||
        token_is(op, "-lt") || token_is(op, "-le"))
    {
        int64_t a;
        int64_t b;
        if (!parse_integer(parser, left, &a) ||
            !parse_integer(parser, right, &b))
            return false;
        if (token_is(op, "-eq")) return a == b;
        if (token_is(op, "-ne")) return a != b;
        if (token_is(op, "-gt")) return a > b;
        if (token_is(op, "-ge")) return a >= b;
        if (token_is(op, "-lt")) return a < b;
        return a <= b;
    }

    if (token_is(op, "-nt") || token_is(op, "-ot"))
    {
        os64_dirent_t a;
        os64_dirent_t b;
        bool a_exists = stat_path(left, &a);
        bool b_exists = stat_path(right, &b);

        if (token_is(op, "-nt"))
            return a_exists && (!b_exists ||
                   (a.mtime != 0 && b.mtime != 0 && a.mtime > b.mtime));
        return b_exists && (!a_exists ||
               (a.mtime != 0 && b.mtime != 0 && a.mtime < b.mtime));
    }

    return fail(parser, "unknown operator", op);
}

static bool parse_or(test_parser_t *parser);

static bool parse_primary(test_parser_t *parser)
{
    if (parser->at >= parser->argc)
        return fail(parser, "expression expected", NULL);

    // A binary expression wins before operator-shaped operands are
    // interpreted. This preserves test's old but useful `test -n = -n` rule.
    if (parser->at + 2 < parser->argc &&
        is_binary_operator(parser->argv[parser->at + 1]))
    {
        const char *left = parser->argv[parser->at++];
        const char *op = parser->argv[parser->at++];
        const char *right = parser->argv[parser->at++];
        return evaluate_binary(parser, left, op, right);
    }
    if (parser->at + 2 < parser->argc &&
        looks_like_operator(parser->argv[parser->at + 1]) &&
        !is_unary_operator(parser->argv[parser->at + 1]))
        return fail(parser, "unknown operator", parser->argv[parser->at + 1]);

    const char *token = parser->argv[parser->at++];
    if (token_is(token, "("))
    {
        if (++parser->depth > TEST_MAX_DEPTH)
            return fail(parser, "expression nesting is too deep", token);
        bool value = parse_or(parser);
        parser->depth--;
        if (parser->error != NULL)
            return false;
        if (parser->at >= parser->argc ||
            !token_is(parser->argv[parser->at], ")"))
            return fail(parser, "closing ')' expected", NULL);
        parser->at++;
        return value;
    }

    if (is_unary_operator(token))
    {
        if (parser->at >= parser->argc)
            return fail(parser, "operand expected after", token);
        return evaluate_unary(parser, token, parser->argv[parser->at++]);
    }
    if (looks_like_operator(token) && parser->at < parser->argc)
        return fail(parser, "unknown operator", token);

    return token[0] != '\0';
}

static bool parse_not(test_parser_t *parser)
{
    if (parser->at < parser->argc &&
        token_is(parser->argv[parser->at], "!"))
    {
        const char *op = parser->argv[parser->at++];
        if (++parser->depth > TEST_MAX_DEPTH)
            return fail(parser, "expression nesting is too deep", op);
        bool value = parse_not(parser);
        parser->depth--;
        return parser->error == NULL && !value;
    }
    return parse_primary(parser);
}

static bool parse_and(test_parser_t *parser)
{
    bool value = parse_not(parser);
    while (parser->error == NULL && parser->at < parser->argc &&
           token_is(parser->argv[parser->at], "-a"))
    {
        parser->at++;
        bool right = parse_not(parser);
        value = value && right;
    }
    return value;
}

static bool parse_or(test_parser_t *parser)
{
    bool value = parse_and(parser);
    while (parser->error == NULL && parser->at < parser->argc &&
           token_is(parser->argv[parser->at], "-o"))
    {
        parser->at++;
        bool right = parse_and(parser);
        value = value || right;
    }
    return value;
}

static const char *base_name(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++)
    {
        if (*p == '/')
            name = p + 1;
    }
    return name;
}

int main(int argc, char **argv)
{
    const char *program = argc > 0 && argv != NULL && argv[0] != NULL
        ? base_name(argv[0]) : "test";
    int32_t expression_argc = argc > 0 ? argc - 1 : 0;
    char **expression_argv = argc > 0 ? argv + 1 : argv;

    if (token_is(program, "["))
    {
        if (expression_argc == 0 ||
            !token_is(expression_argv[expression_argc - 1], "]"))
        {
            os64_hprintf(OS64_STDERR, "[: missing closing ]\n");
            return 2;
        }
        expression_argc--;
    }

    if (expression_argc == 0)
        return 1;
    // With one argument, even a word shaped like an operator is just a
    // string. Historical shell scripts rely on `test -n` being true.
    if (expression_argc == 1)
        return expression_argv[0][0] == '\0' ? 1 : 0;
    // The two-argument form predates the general expression grammar: `!`
    // negates the following string as a one-argument expression. Thus
    // `test ! !` is false rather than an incomplete double negation.
    if (expression_argc == 2 && token_is(expression_argv[0], "!"))
        return expression_argv[1][0] == '\0' ? 0 : 1;

    test_parser_t parser = {
        .argc = expression_argc,
        .argv = expression_argv,
        .program = program
    };
    bool value = parse_or(&parser);
    if (parser.error == NULL && parser.at != parser.argc)
        fail(&parser, "unexpected argument", parser.argv[parser.at]);

    if (parser.error != NULL)
    {
        if (parser.error_arg != NULL)
            os64_hprintf(OS64_STDERR, "%s: %s '%s'\n", parser.program,
                         parser.error, parser.error_arg);
        else
            os64_hprintf(OS64_STDERR, "%s: %s\n", parser.program,
                         parser.error);
        return 2;
    }
    return value ? 0 : 1;
}
