# HUSK.md — the shell's reference card

*What husk answers to, on one page. This is the quick-draw card, not the
design record — the design conversations live in husk.c's own comments,
which remain the authority when this card and the code disagree. A husk is
the outer shell of a seed; this is the outer shell of the kernel.*

## Line editing — the keys

| Key | Does |
|---|---|
| *typing* | inserts at the caret (mid-line insert since 2026-08-08) |
| **Enter** | submits the whole line, wherever the caret sits |
| **Backspace** | erases before the caret |
| **Delete** | erases at the caret *(2026-08-16)* |
| **Left / Right** | moves the caret one cell |
| **Home / End** | caret to start / end of line *(2026-08-16)* |
| **Ctrl+A / Ctrl+E** | Home / End, emacs's spelling *(2026-08-16)* |
| **Ctrl+U** | kill to start of line — V7's line-kill, promoted from `@` *(2026-08-16)* |
| **Ctrl+K** | kill to end of line *(2026-08-16)* |
| **Ctrl+W** | erase the word before the caret — 4BSD's werase *(2026-08-16)* |
| **Up / Down** | walk command history (32 deep, duplicates stored once) |
| **Ctrl+C** | at the prompt: kill the half-typed line, print `^C`, re-prompt |
| **Tab** | literal tab — it's typeable text (no completion yet, honestly) |

History browsing parks your half-typed line at the first Up and returns it
when you walk back past the newest entry. Movement keys (arrows, Home/End)
keep you browsing; the first *edit* makes the recalled line yours.

## Terminal chords — the tty's, not husk's

These are commands to the terminal stack, consumed by the kernel before any
program sees a byte:

| Chord | Does |
|---|---|
| **Alt+Left / Alt+Right** | switch virtual terminals (8 of them) |
| **Shift+PgUp / Shift+PgDn** | scrollback on the focused terminal |
| **Ctrl+C** | (with a program running) terminate the foreground task |
| **Ctrl+D** | end-of-input: the program's read() returns 0, once |
| **Ctrl+Alt+Del** | caught and declined — os64 prefers a polite `shutdown` |

## The command line

| Syntax | Does |
|---|---|
| `a \| b \| c` | pipeline, up to 4 stages — husk builds the plumbing, moves zero bytes |
| `< file` | child reads the file as input |
| `> file` | child writes the file (truncates — First Edition vocabulary, 1971) |
| `>> file` | appends instead |
| `a ; b` | run in sequence (Thompson's) |
| `a && b` | run b only if a succeeded (Bourne's) |
| `a \|\| b` | run b only if a failed |
| `cmd &` | background job: announced `[1] 57`, reaped and reported `[1]+ 57 Done` / `Exit n` at a later prompt — the task number is the handle (`echo kill > /proc/57/ctl`) |
| `*` `?` `[a-z]` `[!a-z]` | V7 wildcards, expanded by the shell (last path component only, sorted; `echo "*"` prints a star) |

## Variables — expanded by the shell, in front of every program

| Variable | Expands to |
|---|---|
| `$?` | last exit status |
| `$$` | husk's own task ID (expansion-time identity — `echo $$` names husk) |
| `$CWD` | current directory, fetched LIVE from the kernel — never a stale copy |
| `$NAME` | the environment (`os64_getenv`); unset names expand to nothing |

**Quoting is Bourne's 1977 split:** single quotes suppress expansion and
grouping wholesale (`'$CWD'` stays literal); double quotes group words but
let `$` speak. A `$` that starts no name stays a `$`.

## Builtins — only what MUST live in the shell

| Builtin | Why it can't be a program |
|---|---|
| `cd` | a child's directory change dies with the child |
| `exit` | the shell's own lifetime |
| `export NAME=value` | the shell's own environment, inherited downward |
| `unset NAME` | export's undo, same one-way valve |
| `time cmd` | a PREFIX: only the party at both ends of a run can clock it |

Everything else is a real program in `/bin` — zero feature duplication.

## The rc file, and -c

- At startup husk runs the first of: `/home/husk.rc`, `/etc/husk.rc`,
  `/fat/husk.rc`, `/husk.rc` — yours, the system's, the lifeboat's, the
  fallback. A serial beacon names which copy ran. Subshells skip it.
- In rc files, blank lines and `#` comments are skipped — file syntax, not
  shell syntax (a typed `#` at the prompt means a program named `#`,
  honestly not found).
- `husk -c "ps -e | grep husk"` runs one line and exits with its status —
  the seam that lets a program (watch(1) asked first) own a command line
  without owning a parser.

## Known limits, stated plainly

- Editing a line that has WRAPPED misbehaves at the wrap seam — the
  renderer's `\b` clamps at column 0. A line that long deserves a script.
- Globs cover the last path component: `/tmp/*` yes, `/*/foo` no (booked).
- No tab completion, no `%1` job notation — the task number is the handle.
