// env — the environment: listed, or changed for ONE program.
//
//   env                                    every variable, one per line
//   env [-i] [-u NAME]... [NAME=value]... command [args...]
//                                          run command with those changes
//
// A child's environment is a copy of its spawner's, so env is a courier: it
// applies the changes to its OWN copy, finds the command through the $PATH
// that results, spawns it (the child inherits the changed copy), waits, and
// answers with the command's exit code. The shell's copy is never touched —
// which is the point, and why husk refuses `NAME=value command` and sends
// you here. (Unix's env exec'd the command in its own process; os64 has no
// exec, so this one stays alive as the waiter. The kernel follows that wait
// down when it aims Ctrl+C, so the program dies and env reports 130.)
//
// Changes apply left to right, exactly as typed: `env -i PATH=/bin ls`
// empties the block, then sets PATH, then finds ls with it.

#include "os64/os64.h"

static bool is_name_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

// `NAME=value` with NAME spelled the way husk spells one: the `=`, or NULL
// when the token is a command instead.
static const char *assignment_eq(const char *arg)
{
    if (!is_name_start(arg[0]))
        return NULL;
    const char *p = arg + 1;
    while (is_name_char(*p))
        p++;
    return *p == '=' ? p : NULL;
}

// One line per variable. The value goes out by POINTER (os64_env_next hands
// back a bounded copy, and a $(...) capture can be longer than the bound)
// with its control bytes spelled as escapes — a captured `ls` carries
// newlines, and a listing is only a listing while one line is one variable.
static int32_t list_environment(void)
{
    os64_envent_t e = { .index = 0 };
    int32_t verdict;
    while ((verdict = os64_env_next(&e)) == 0)
    {
        const char *value = os64_getenv(e.key);
        os64_write(OS64_STDOUT, e.key, os64_strlen(e.key));
        os64_write(OS64_STDOUT, "=", 1);
        os64_write_escaped(OS64_STDOUT, value != NULL ? value : "");
        os64_write(OS64_STDOUT, "\n", 1);
    }
    return verdict == 1 ? 0 : 2;    // 1 is the walk running off the end: the happy path
}

// -i: remove everything. Each unset rewrites the block under any walk in
// flight (proc.h), so the walk restarts from the top after every removal.
// A key the walk could not hand back whole cannot be unset by name, so a
// first entry that survives its own removal ends the loop rather than
// spinning on it.
static void empty_environment(void)
{
    os64_envent_t e = { .index = 0 };
    while (os64_env_next(&e) == 0)
    {
        os64_unsetenv(e.key);
        os64_envent_t next = { .index = 0 };
        if (os64_env_next(&next) == 0 && os64_streq(next.key, e.key))
            break;
        e.index = 0;
    }
}

int main(int argc, char **argv)
{
    // No destinations: options are applied in the manual loop below, in
    // the order typed, because their ORDER is their meaning.
    static const os64_optspec_t specs[] = {
        { .letter = 'i', .name = "empty", .takes_value = false,
          .help = "start the program from an empty environment" },
        { .letter = 'u', .name = "unset", .takes_value = true,
          .help = "remove NAME (may be repeated)" },
    };
    static const char *usage =
        "env [-i] [-u NAME]... [NAME=value]... [command [args...]]";
    os64_args_t a;
    os64_args_init(&a, argc, argv, specs, 2);
    a.about = "Print the environment, or run one program with changes to it "
              "that only that program sees.";

    // The command and everything after it belong to the command — the
    // parser stops the moment a positional is not an assignment. It never
    // permutes argv, so the token it hands back is argv[k] for the k found
    // by address.
    char **cmd_argv = NULL;
    int32_t r;
    while (cmd_argv == NULL && (r = os64_args_next(&a)) != OS64_ARG_END)
    {
        switch (r)
        {
        case 'i':
            empty_environment();
            break;
        case 'u':
            if (os64_unsetenv(a.value) != 0)
            {
                os64_hprintf(OS64_STDERR, "env: cannot unset %s\n", a.value);
                return 1;
            }
            break;
        case OS64_ARG_POSITIONAL:
        {
            const char *eq = assignment_eq(a.value);
            if (eq == NULL)
            {
                for (int32_t k = 1; k < argc; k++)
                    if (argv[k] == a.value)
                        cmd_argv = argv + k;
                break;
            }
            char name[OS64_ENV_STR_MAX + 1];
            size_t len = (size_t)(eq - a.value);
            if (len > OS64_ENV_STR_MAX)
            {
                os64_hprintf(OS64_STDERR, "env: variable name too long: %s\n", a.value);
                return 1;
            }
            os64_memcpy(name, a.value, len);
            name[len] = '\0';
            if (os64_setenv(name, eq + 1) != 0)
            {
                os64_hprintf(OS64_STDERR, "env: cannot set %s (environment full?)\n", name);
                return 1;
            }
            break;
        }
        case OS64_ARG_HELP:
            os64_args_help(&a, usage);
            return 0;
        default:                                // OS64_ARG_ERROR: a.value = the bad token
            os64_args_help(&a, usage);
            return 1;
        }
    }

    if (cmd_argv == NULL)
        return list_environment();

    char resolved[256];
    const char *path = os64_resolve_command(cmd_argv[0], resolved, sizeof(resolved));
    int64_t tid = os64_spawn(path, cmd_argv);
    if (tid < 0)
    {
        os64_hprintf(OS64_STDERR, "env: cannot run %s\n", cmd_argv[0]);
        return 1;
    }

    // A caught signal cuts the wait short with nothing collected and the
    // child still running — wait again (SIGNALS.md §8: the caller loops).
    int32_t code = 0;
    int64_t ended;
    do
        ended = os64_wait(tid, &code);
    while (ended == OS64_INTERRUPTED);
    if (ended <= 0)
    {
        os64_hprintf(OS64_STDERR, "env: lost track of %s\n", cmd_argv[0]);
        return 1;
    }
    return code;
}
