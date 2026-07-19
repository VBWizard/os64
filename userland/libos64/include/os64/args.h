#ifndef OS64_ARGS_H
#define OS64_ARGS_H

// libos64 argument parsing — pulled into existence by ls, the first flagged
// app (consumer-driven, exactly as agreed when upper's tripwire went in).
//
// This is deliberately NOT getopt. getopt's sins, avoided by construction:
//   - Global mutable state (optind/optarg/opterr): here the caller owns an
//     os64_args_t on its own stack — reentrant, restartable, two parsers can
//     run at once without a whisper of interference.
//   - GNU getopt silently PERMUTES argv as a side effect: argv here is never
//     touched. Not one byte.
//   - The "ab:c::" optstring mini-language: options are declared in a plain
//     table you can read, and --help text is GENERATED from that same table,
//     so the help can never drift from the truth.
//
// Usage — the whole thing:
//     static const os64_optspec_t specs[] = {
//         { 'l', "long",  0, "one entry per line with sizes" },
//         { 'a', "all",   0, "include entries starting with ." },
//     };
//     os64_args_t a;
//     os64_args_init(&a, argc, argv, specs, 2);
//     int r;
//     while ((r = os64_args_next(&a)) != OS64_ARG_END)
//         switch (r) {
//             case 'l': ...; break;
//             case 'a': ...; break;
//             case OS64_ARG_POSITIONAL: /* a.value is the path */ break;
//             case OS64_ARG_HELP: os64_args_help(&a, "ls [-la] [path]");
//                                 return 0;
//             default:  os64_args_help(&a, "ls [-la] [path]");
//                       return 1;   // OS64_ARG_ERROR: a.value = bad token
//         }
//
// Grammar: "-l", combined "-la", long "--long", values as "-o name" or
// "--out=name", "--" ends option parsing (everything after is positional),
// and a lone "-" is positional (it conventionally means stdin). -h/--help
// yield OS64_ARG_HELP unless a spec claims 'h' for itself.
//
// A program with NO options (looking at you, pwd) passes NULL, 0:
//     os64_args_init(&a, argc, argv, NULL, 0);
// -h/--help still work (they're unclaimed by definition), positionals and
// errors flow normally, and os64_args_help prints just the usage line.
// (NOT `specs[] = {{}}` — that is ONE spec, all zeroes, and it used to
// print as "-" plus character zero in the help. The printer now skips
// blank rows, but say what you mean: NULL, 0.)

#include <stddef.h>

typedef struct {
    char        letter;       // the short form: 'l' for -l (required, unique)
    const char *name;         // the long form: "long" for --long (or NULL)
    int         takes_value;  // option consumes a value (next token or =...)
    const char *help;         // one line, shown by os64_args_help
} os64_optspec_t;

typedef struct {
    // set by init — treat as read-only after
    int argc;
    char **argv;
    const os64_optspec_t *specs;
    int nspecs;
    // OPTIONAL, yours to set after init (the caller owns this struct — that's
    // the design): a one-line details of the program, printed by
    // os64_args_help under the usage line. NULL = no details.
    //     os64_args_init(&a, argc, argv, specs, 1);
    //     a.about = "list directory contents";
    const char *about;
    // OPTIONAL, same contract as about: a second line printed after it.
    const char *details;
    // iteration state — the parser's, not yours
    int index;
    const char *bundle;       // mid "-la": pointer to the next letter
    int no_more_opts;         // a "--" was seen
    // per-result output
    const char *value;        // option's value / positional text / bad token
} os64_args_t;

#define OS64_ARG_END        (-1)  // no more arguments
#define OS64_ARG_POSITIONAL (-2)  // .value = the argument
#define OS64_ARG_ERROR      (-3)  // .value = the offending token
#define OS64_ARG_HELP       (-4)  // user asked for -h/--help

void os64_args_init(os64_args_t *a, int argc, char **argv,
                    const os64_optspec_t *specs, int nspecs);

// Returns a spec's letter, or one of the OS64_ARG_* results above.
int os64_args_next(os64_args_t *a);

// Print "usage: <usage>" plus one generated line per option, to handle 1.
void os64_args_help(const os64_args_t *a, const char *usage);

#endif // OS64_ARGS_H
