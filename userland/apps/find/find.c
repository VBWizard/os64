// find.c — select and act on entries beneath directory trees.

#include "os64/os64.h"

#define FIND_MAX_PATHS    512
#define FIND_MAX_CLAUSES   96
#define FIND_MAX_EXEC_ARGS 128

typedef enum {
    FIND_NAME,
    FIND_PATH,
    FIND_TYPE,
    FIND_SIZE,
    FIND_MTIME,
    FIND_PRINT,
    FIND_EXEC
} find_kind_t;

typedef enum {
    FIND_EQUAL,
    FIND_LESS,
    FIND_GREATER
} find_comparison_t;

typedef struct {
    find_comparison_t comparison;
    uint64_t value;
} find_quantity_t;

typedef struct {
    find_kind_t kind;
    bool negate;
    union {
        const char *pattern;
        char type;
        find_quantity_t quantity;
        struct {
            char **arguments;
            int32_t count;
        } execute;
    } value;
} find_clause_t;

typedef struct {
    find_clause_t clauses[FIND_MAX_CLAUSES];
    int32_t clause_count;
    uint32_t min_depth;
    uint32_t max_depth;
    int64_t now;
    bool failed;
} find_state_t;

static void help(void)
{
    os64_puts(
        "usage: find [PATH ...] [EXPRESSION]\n"
        "Walk PATH (default .) and evaluate each entry. Adjacent tests are ANDed.\n"
        "\n"
        "tests:\n"
        "  -name PATTERN       match the final path component\n"
        "  -path PATTERN       match the complete path as visited\n"
        "  -type f|d           match regular files or directories\n"
        "  -size [+|-]N[cKMG]  compare size in bytes (or binary units)\n"
        "  -mtime [+|-]N       compare age in whole 24-hour periods\n"
        "  -mindepth N         do not evaluate entries shallower than N\n"
        "  -maxdepth N         do not descend below N\n"
        "  ! TEST, -not TEST   negate the following test or action\n"
        "  -a, -and            explicit AND (AND is otherwise implied)\n"
        "\n"
        "actions:\n"
        "  -print              print matching paths (the default action)\n"
        "  -exec CMD ... ;     run CMD once per match, replacing {} arguments\n"
        "\n"
        "PATTERN uses *, ?, ranges such as [a-z], and negated sets [!a-z].\n"
        "For numeric tests +N means greater than N and -N means less than N.\n");
}

static bool expression_word(const char *word)
{
    return word != NULL && (word[0] == '-' || os64_streq(word, "!") ||
                            os64_streq(word, "("));
}

static bool parse_unsigned(const char *text, uint64_t *value,
                           const char **end)
{
    if (text == NULL || *text < '0' || *text > '9')
        return false;

    uint64_t result = 0;
    const char *p = text;
    while (*p >= '0' && *p <= '9')
    {
        uint64_t digit = (uint64_t)(*p - '0');
        if (result > (UINT64_MAX - digit) / 10)
            return false;
        result = result * 10 + digit;
        p++;
    }
    *value = result;
    if (end != NULL)
        *end = p;
    return true;
}

static bool parse_depth(const char *text, uint32_t *depth)
{
    uint64_t value;
    const char *end;
    if (!parse_unsigned(text, &value, &end) || *end != '\0' ||
        value > UINT32_MAX)
        return false;
    *depth = (uint32_t)value;
    return true;
}

static bool parse_quantity(const char *text, bool units,
                           find_quantity_t *quantity)
{
    if (text == NULL)
        return false;

    find_comparison_t comparison = FIND_EQUAL;
    if (*text == '+')
    {
        comparison = FIND_GREATER;
        text++;
    }
    else if (*text == '-')
    {
        comparison = FIND_LESS;
        text++;
    }

    uint64_t value;
    const char *end;
    if (!parse_unsigned(text, &value, &end))
        return false;

    uint64_t multiplier = 1;
    if (*end != '\0')
    {
        if (!units || end[1] != '\0')
            return false;
        switch (*end)
        {
            case 'c': multiplier = 1; break;
            case 'K': case 'k': multiplier = 1024ULL; break;
            case 'M': case 'm': multiplier = 1024ULL * 1024; break;
            case 'G': case 'g': multiplier = 1024ULL * 1024 * 1024; break;
            case 'T': case 't': multiplier = 1024ULL * 1024 * 1024 * 1024; break;
            default: return false;
        }
    }
    if (value > UINT64_MAX / multiplier)
        return false;

    quantity->comparison = comparison;
    quantity->value = value * multiplier;
    return true;
}

static bool compare_quantity(uint64_t actual, const find_quantity_t *wanted)
{
    if (wanted->comparison == FIND_LESS)
        return actual < wanted->value;
    if (wanted->comparison == FIND_GREATER)
        return actual > wanted->value;
    return actual == wanted->value;
}

static find_clause_t *new_clause(find_state_t *state, find_kind_t kind,
                                 bool negate)
{
    if (state->clause_count >= FIND_MAX_CLAUSES)
        return NULL;
    find_clause_t *clause = &state->clauses[state->clause_count++];
    os64_memset(clause, 0, sizeof(*clause));
    clause->kind = kind;
    clause->negate = negate;
    return clause;
}

static bool need_argument(int argc, int32_t at, const char *predicate)
{
    if (at + 1 < argc)
        return true;
    os64_hprintf(OS64_STDERR, "find: %s requires an argument\n", predicate);
    return false;
}

static int32_t parse_expression(int argc, char **argv, int32_t first,
                                find_state_t *state)
{
    bool negate = false;
    bool has_action = false;

    for (int32_t i = first; i < argc; i++)
    {
        const char *word = argv[i];
        if (os64_streq(word, "!") || os64_streq(word, "-not"))
        {
            negate = !negate;
            continue;
        }
        if (os64_streq(word, "-a") || os64_streq(word, "-and"))
        {
            if (negate)
            {
                os64_hprintf(OS64_STDERR, "find: expected an expression after !\n");
                return -1;
            }
            continue;
        }
        if (os64_streq(word, "-mindepth") || os64_streq(word, "-maxdepth"))
        {
            if (negate || !need_argument(argc, i, word))
            {
                if (negate)
                    os64_hprintf(OS64_STDERR,
                                 "find: depth controls cannot be negated\n");
                return -1;
            }
            uint32_t depth;
            if (!parse_depth(argv[++i], &depth))
            {
                os64_hprintf(OS64_STDERR, "find: invalid depth: %s\n", argv[i]);
                return -1;
            }
            if (os64_streq(word, "-mindepth"))
                state->min_depth = depth;
            else
                state->max_depth = depth;
            continue;
        }

        find_clause_t *clause = NULL;
        if (os64_streq(word, "-name") || os64_streq(word, "-path"))
        {
            if (!need_argument(argc, i, word))
                return -1;
            clause = new_clause(state, os64_streq(word, "-name")
                                       ? FIND_NAME : FIND_PATH, negate);
            if (clause != NULL)
                clause->value.pattern = argv[++i];
        }
        else if (os64_streq(word, "-type"))
        {
            if (!need_argument(argc, i, word))
                return -1;
            const char *type = argv[++i];
            if (!((type[0] == 'f' || type[0] == 'd') && type[1] == '\0'))
            {
                os64_hprintf(OS64_STDERR,
                             "find: -type expects f or d, not '%s'\n", type);
                return -1;
            }
            clause = new_clause(state, FIND_TYPE, negate);
            if (clause != NULL)
                clause->value.type = type[0];
        }
        else if (os64_streq(word, "-size") || os64_streq(word, "-mtime"))
        {
            if (!need_argument(argc, i, word))
                return -1;
            bool size = os64_streq(word, "-size");
            find_quantity_t quantity;
            if (!parse_quantity(argv[++i], size, &quantity))
            {
                os64_hprintf(OS64_STDERR, "find: invalid %s value: %s\n",
                             size ? "size" : "mtime", argv[i]);
                return -1;
            }
            clause = new_clause(state, size ? FIND_SIZE : FIND_MTIME, negate);
            if (clause != NULL)
                clause->value.quantity = quantity;
        }
        else if (os64_streq(word, "-print"))
        {
            clause = new_clause(state, FIND_PRINT, negate);
            has_action = true;
        }
        else if (os64_streq(word, "-exec"))
        {
            int32_t command = i + 1;
            int32_t end = command;
            while (end < argc && !os64_streq(argv[end], ";"))
                end++;
            if (command == end || end == argc)
            {
                os64_hprintf(OS64_STDERR,
                             "find: -exec requires a command terminated by ';'\n");
                return -1;
            }
            if (end - command > FIND_MAX_EXEC_ARGS)
            {
                os64_hprintf(OS64_STDERR,
                             "find: -exec command has too many arguments\n");
                return -1;
            }
            clause = new_clause(state, FIND_EXEC, negate);
            if (clause != NULL)
            {
                clause->value.execute.arguments = &argv[command];
                clause->value.execute.count = end - command;
            }
            has_action = true;
            i = end;
        }
        else
        {
            os64_hprintf(OS64_STDERR,
                         "find: unsupported expression: %s\n", word);
            return -1;
        }

        if (clause == NULL)
        {
            os64_hprintf(OS64_STDERR, "find: expression is too complex\n");
            return -1;
        }
        negate = false;
    }

    if (negate)
    {
        os64_hprintf(OS64_STDERR, "find: expected an expression after !\n");
        return -1;
    }
    if (!has_action)
    {
        if (new_clause(state, FIND_PRINT, false) == NULL)
        {
            os64_hprintf(OS64_STDERR, "find: expression is too complex\n");
            return -1;
        }
    }
    return 0;
}

static bool run_command(const find_clause_t *clause, const char *path,
                        bool *system_error)
{
    char *arguments[FIND_MAX_EXEC_ARGS + 1];
    for (int32_t i = 0; i < clause->value.execute.count; i++)
    {
        char *argument = clause->value.execute.arguments[i];
        arguments[i] = os64_streq(argument, "{}") ? (char *)path : argument;
    }
    arguments[clause->value.execute.count] = NULL;

    char resolved[OS64_WALK_PATH_MAX];
    const char *program = os64_resolve_command(arguments[0], resolved,
                                               sizeof(resolved));
    int64_t task = os64_spawn(program, arguments);
    if (task < 0)
    {
        os64_hprintf(OS64_STDERR, "find: cannot execute '%s' for '%s'\n",
                     arguments[0], path);
        *system_error = true;
        return false;
    }

    int32_t status = 0;
    if (os64_wait(task, &status) < 0)
    {
        os64_hprintf(OS64_STDERR, "find: cannot wait for '%s'\n", arguments[0]);
        *system_error = true;
        return false;
    }
    return status == 0;
}

static bool evaluate(const os64_walk_visit_t *visit, find_state_t *state,
                     bool *system_error)
{
    for (int32_t i = 0; i < state->clause_count; i++)
    {
        const find_clause_t *clause = &state->clauses[i];
        bool result = false;
        switch (clause->kind)
        {
            case FIND_NAME:
                result = os64_glob_match(clause->value.pattern,
                                         visit->entry->name);
                break;
            case FIND_PATH:
                result = os64_glob_match(clause->value.pattern, visit->path);
                break;
            case FIND_TYPE:
                result = clause->value.type == 'd'
                       ? (visit->entry->flags & OS64_DE_DIR) != 0
                       : (visit->entry->flags & OS64_DE_DIR) == 0;
                break;
            case FIND_SIZE:
                result = compare_quantity(visit->entry->size,
                                          &clause->value.quantity);
                break;
            case FIND_MTIME:
                if (visit->entry->mtime != 0 &&
                    (uint64_t)state->now >= visit->entry->mtime)
                {
                    uint64_t days = ((uint64_t)state->now -
                                     visit->entry->mtime) / 86400;
                    result = compare_quantity(days, &clause->value.quantity);
                }
                break;
            case FIND_PRINT:
                os64_printf("%s\n", visit->path);
                result = true;
                break;
            case FIND_EXEC:
                result = run_command(clause, visit->path, system_error);
                break;
        }
        if (clause->negate)
            result = !result;
        if (!result)
            return false;
    }
    return true;
}

static void walk_error(const os64_walk_visit_t *visit)
{
    switch (visit->error)
    {
        case OS64_WALK_ERROR_STAT:
            os64_hprintf(OS64_STDERR, "find: cannot access '%s'\n", visit->path);
            break;
        case OS64_WALK_ERROR_OPEN:
            os64_hprintf(OS64_STDERR, "find: cannot open directory '%s'\n",
                         visit->path);
            break;
        case OS64_WALK_ERROR_READ:
            os64_hprintf(OS64_STDERR, "find: cannot read directory '%s'\n",
                         visit->path);
            break;
        case OS64_WALK_ERROR_CLOSE:
            os64_hprintf(OS64_STDERR, "find: cannot close directory '%s'\n",
                         visit->path);
            break;
        case OS64_WALK_ERROR_PATH_TOO_LONG:
            os64_hprintf(OS64_STDERR, "find: path too long beneath '%s': %s\n",
                         visit->path,
                         visit->entry == NULL ? "?" : visit->entry->name);
            break;
        case OS64_WALK_ERROR_DEPTH:
            os64_hprintf(OS64_STDERR,
                         "find: directory nesting limit reached at '%s'\n",
                         visit->path);
            break;
    }
}

static os64_walk_action_t find_visit(const os64_walk_visit_t *visit,
                                     void *context)
{
    find_state_t *state = context;
    if (visit->event == OS64_WALK_ERROR)
    {
        walk_error(visit);
        state->failed = true;
        return OS64_WALK_CONTINUE;
    }
    if (visit->event == OS64_WALK_LEAVE)
        return OS64_WALK_CONTINUE;

    os64_walk_action_t action = OS64_WALK_CONTINUE;
    if (visit->depth >= state->min_depth)
    {
        bool system_error = false;
        evaluate(visit, state, &system_error);
        if (system_error)
        {
            state->failed = true;
            action |= OS64_WALK_FAIL;
        }
    }
    if ((visit->entry->flags & OS64_DE_DIR) != 0 &&
        visit->depth >= state->max_depth)
        action |= OS64_WALK_PRUNE;
    return action;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (os64_streq(argv[1], "-h") ||
                     os64_streq(argv[1], "--help")))
    {
        help();
        return 0;
    }

    const char *paths[FIND_MAX_PATHS];
    int32_t path_count = 0;
    int32_t expression = 1;
    while (expression < argc && !expression_word(argv[expression]))
    {
        if (path_count >= FIND_MAX_PATHS)
        {
            os64_hprintf(OS64_STDERR, "find: too many starting paths\n");
            return 2;
        }
        paths[path_count++] = argv[expression++];
    }
    if (path_count == 0)
        paths[path_count++] = ".";

    find_state_t state = {
        .max_depth = UINT32_MAX
    };
    if (parse_expression(argc, argv, expression, &state) < 0)
    {
        os64_hprintf(OS64_STDERR, "find: try 'find --help'\n");
        return 2;
    }
    if (state.min_depth > state.max_depth)
    {
        os64_hprintf(OS64_STDERR,
                     "find: -mindepth cannot exceed -maxdepth\n");
        return 2;
    }

    bool needs_time = false;
    for (int32_t i = 0; i < state.clause_count; i++)
        if (state.clauses[i].kind == FIND_MTIME)
            needs_time = true;
    if (needs_time)
    {
        os64_time_t now = {0};
        if (os64_time(&now) < 0 || now.epoch < 0)
        {
            os64_hprintf(OS64_STDERR, "find: cannot read the system clock\n");
            return 1;
        }
        state.now = now.epoch;
    }

    os64_walk_options_t walk_options = {
        .open_depth_limit = OS64_WALK_MAX_DEPTH
    };
    for (int32_t i = 0; i < path_count; i++)
        if (os64_walk(paths[i], &walk_options, find_visit, &state) < 0)
            state.failed = true;
    return state.failed ? 1 : 0;
}
