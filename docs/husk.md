# husk — the language, on one page

husk runs programs. Everything below is the glue between them: where their
bytes go, what stands in for their output, and which of them run. The rule
under all of it: **husk does nothing a program can do** — `test`, `true`,
`false`, `echo` are programs, and `if` only ever looks at an exit status.

Every keyword and operator here is recognized only when *you typed it*. A
value that arrived by expansion — `$1`, `$NAME`, `$(…)`, a glob match — is
data, never syntax: `echo $(echo "|")` prints a bar, it does not pipe.

## Redirection

| Spelling | Meaning |
|---|---|
| `< f` | stdin from f |
| `> f` / `>> f` | stdout to f — truncate / append |
| `2> f` / `2>> f` | stderr to f — truncate / append |
| `2>&1` | stderr goes wherever stdout **ends up** (file, pipe, or console) |
| `>&2` | stdout goes wherever stderr ends up |
| `&> f` / `&>> f` | both to f — the same as `> f 2>&1` |
| `a \| b` | a's stdout into b's stdin; `2>&1` on `a` sends its stderr down the pipe too |

Operators are whole tokens: `> f` works, `>f` is an argument. Per stream the
last spelling wins. Where the joins bind is the one departure from Bourne:
`cmd 2>&1 > f` puts both streams in `f`, because that is what everyone means
by it (DIVERGENCES § The shell). A program that spawns others passes its
redirection down: `testrun > log 2>&1` captures every fixture.

## Expansion

| Spelling | Becomes |
|---|---|
| `$?` | the last command's exit status |
| `$$` | husk's own task id |
| `$CWD` | the current directory, live |
| `$NAME` | the environment variable (nothing, if unset) |
| `$0` `$1`…`$9` `$*` `$#` | a script's path, its arguments, all of them, how many |
| `$(cmd)` | what `cmd` printed, trailing newlines stripped |
| `*` `?` `[…]` | filenames, last path component only — a `[` with no closing `]` is just a `[`, so `[ -d /x ]` runs `/bin/[` |

Double quotes group words and still expand; single quotes hand the bytes
through untouched. `$(…)` nests, runs *in this shell* (a `cd` inside it
moves you; a builtin's output inside it is not captured — `$(pwd)` works,
`pwd` being a program), and its own status is not `$?` — the line's is.

## Command lists

| Spelling | Meaning |
|---|---|
| `a ; b` | a, then b |
| `a && b` | b only if a succeeded |
| `a \|\| b` | b only if a failed |
| `a &` | a in the background — reported when it ends |
| `! a` | a, with its verdict inverted |
| `time a` | a, then how long it took (the whole pipeline) |

## Variables

There is one kind, and it is the environment: `NAME=value` sets it,
`$NAME` reads it, every program you run afterwards inherits it, `unset
NAME` removes it. `export NAME=value` still works and means the same thing.
A value with spaces needs quotes (`GREETING="hello there"`); `X=*` sets a
star, not a directory listing. The name and the `=` must be typed — a `$x`
holding `A=B` is a word, not an assignment.

## Control flow

```
if LIST                    while LIST          for NAME in WORDS
then …                     do …                do …
elif LIST                  done                done
then …
else …
fi
```

A LIST is any command line; its last status decides — 0 is true. `then`,
`do` and `else` may follow a `;` on the same line or open their own, so
these are the same:

```
if ls /home/flag 2> /dev/null; then echo ready; else echo waiting; fi

if ls /home/flag 2> /dev/null
then
    echo ready
else
    echo waiting
fi
```

`if ! cmd` tests for failure. Blocks nest. At the prompt, an open block
shows the `> ` continuation prompt; Ctrl+C there abandons it. Ctrl+C during
a loop stops the loop (`$?` = 130, the same as a program killed by it). A
script or rc that ends inside an open block is refused, not half-run.

```
while ! ls /home/flag 2> /dev/null
do
    sleep 1
done

for f in /home/*.log
do
    echo $f
done
```

`for` expands its list once — globs, `$NAME`, `$(…)` all work there — and
assigns each word in turn; `for NAME` with no `in` walks a script's
arguments. NAME keeps its last value when the loop ends. And because a
variable is the environment, the body's children see it with no ceremony:
`for i in 1 2; do husk -c "echo $i"; done` prints 1, then 2.

Not yet: `break`, `continue`, `case` — DEBTS § husk scripting.

## Directories

| Spelling | Meaning |
|---|---|
| `cd DIR` / `cd` | go there / go to `/` |
| `cd -` | back to the directory you were in before this one (prints it) |
| `pushd DIR` | remember here, go to DIR |
| `pushd` | swap places with the top of the stack |
| `popd` | back to the top of the stack |
| `dirs` | show the stack: here first, then newest to oldest |

## The line editor

Left/Right, Home/End, Up/Down through history, Delete, and the chords
Ctrl+A/E (home/end), Ctrl+U/K (kill to start/end), Ctrl+W (word erase).
**Tab** completes the word at the caret: the first word against builtins,
the current directory and PATH; anything else against the directory it
names. One match is inserted whole (`/` after a directory, a space after
anything else); several extend to what they share; a Tab that can extend
nothing lists them. Ctrl+D at an empty prompt is end of input.

## Builtins, and why each one is

`cd` `pushd` `popd` `dirs` (only the shell can move the shell), `export`
`unset` (the environment flows only downward), `exit`, and the prefixes
`time` and `!` (only the party at both ends of a pipeline can judge it).
That is the whole list, on purpose.
