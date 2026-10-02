# js — run a JavaScript program

Status: RULED by Chris, 2026-10-02 (C1). The grammar, argument binding,
output conventions and exit statuses below are the runner's half of the R0
contract (`userland/libjs/CONTRACT.md`), accepted by Quinn as R0's owner in
the C1 review. `tools/test_js_cli_host.sh` holds the runner to the tables here
(all but the library's own fatal exit), on a stand-in for the library and on
the real one. `make` builds `js` and `libjs.so` into `userland/bin`, and
`os64get js libjs.so` installs them; the image gains `/bin/js` together with
`/lib/libjs.so` (DEBTS.md § Userland utilities).

```
js [options] FILE [ARG...]
js [options] -e SOURCE [ARG...]
js [options] - [ARG...]
```

`-` reads the whole script from standard input: `echo 'print(6 * 7)' | js -`,
`js - < build.js`. At a terminal it reads until Ctrl+D.

`js` runs one classic script (not a module) to completion: the script, then
every Promise job it queues, until none is runnable. `print(...)` and
`console.log(...)` write to standard output, so ordinary shell redirection
works (`js report.js > out.txt`, `js gen.js | grep x`). Diagnostics go to
standard error.

## Options

| Option | Meaning | Default |
|---|---|---|
| `-e`, `--eval SOURCE` | run SOURCE instead of a file | |
| `-m`, `--memory SIZE` | memory budget for the engine | 64M |
| `-t`, `--time MS` | execution budget, in milliseconds, for the script and its jobs | 60000 |
| `--stack SIZE` | JavaScript stack budget | 256K |
| `--source SIZE` | largest script accepted | 4M |
| `--jobs N` | most Promise jobs one run may execute | 2^64-1 |
| `-h`, `--help` | usage | |
| `--` | end of `js`'s options | |

SIZE takes a byte count with an optional `K` or `M` suffix. Every budget must
be positive: R0 has no "unlimited" setting, deliberately. `--stack` is at most
768K, a quarter below the 1 MiB thread stack the engine actually runs on, so a
runaway recursion ends as the script's own `InternalError: stack overflow`
(exit 1, like any uncaught exception), never as a fault.

A job is one Promise continuation: each `.then` callback that runs, and each
resumption of an `async` function after an `await`. The job budget is the one
that does not depend on machine speed, so a runaway Promise chain stops at the
same point on every machine. Its default is the largest count the field holds,
because an ordinary async program can legitimately run millions of jobs and the
time budget already stops one that never ends. `--jobs N` is there for a test
that wants the deterministic cap.

The defaults are the runner's, provisional until J2 measures real programs on
the guest and the library publishes production defaults.

**The first operand ends `js`'s options.** Everything after the file name
(or after `-e SOURCE`, or `-`) belongs to the script, flags included, so
`js tool.js -v --out x` hands `-v --out x` to `tool.js`. Write `--` when a file
name itself starts with `-`.

## Arguments

The script sees its arguments in `scriptArgs`, an array of strings:

| Command | `scriptArgs` |
|---|---|
| `js tool.js a b` | `["tool.js", "a", "b"]` |
| `js -e 'print(scriptArgs)' a b` | `["-e", "a", "b"]` |
| `cat tool.js \| js - a b` | `["-", "a", "b"]` |

Element zero always names what ran (the path as typed, `-e`, or `-`), so a
script's own arguments always start at index one.

## Exit status

| Status | Meaning |
|---|---|
| 0 | the script and all its jobs finished |
| 1 | the script failed: an uncaught exception or an unhandled Promise rejection |
| 2 | usage: unknown option, bad value, no script |
| 3 | input or output failed: the script (file or standard input) could not be read, the runner had no memory to hold it or its arguments, or output could not be written |
| 4 | a budget was exceeded (the message names which: memory, time, source or jobs) |
| 5 | the library refused the runtime (header/library mismatch, or a runtime that could not be created), or answered with a status the runner never asks for; the message names the status |
| 0x4A534641 | the engine detected a broken invariant and ended the process (`OS64_JS_FATAL_EXIT`, "JSFA"); this is the library's, not the runner's |

Ctrl+C ends `js` the way it ends any program. R0's cancellation call is not
safe from a signal handler, and the runner does not pretend otherwise.

## Diagnostics

One line, then the stack trace when the engine has one; the trace is where
the line and column are:

```
js: tool.js: TypeError: not a function
    at main (tool.js:12:5)
js: tool.js: unhandled promise rejection: boom
    at <eval> (tool.js:3:16)
js: tool.js: time budget exceeded (60000 ms)
js: nosuch.js: no such file
```

A trace too long for the library's diagnostic buffer ends with
`[truncated]`.

## Not supported

An interactive prompt, modules and `import`, and any filesystem, network or
process API inside the script. `js` with no script prints usage and exits 2;
it is where the interactive prompt will live when it exists (DEBTS.md).
