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

## The prompt — `$PROMPT`

Unset, husk prompts `husk> `, exactly as it always has. Set, it is rendered
from this vocabulary:

| Escape | Prints |
|---|---|
| `\w` | working directory |
| `\W` | its last component (`/` stays `/`) |
| `\t` | time, `HH:MM:SS`, local — honours `$TZ` |
| `\d` | date, `YYYY-MM-DD` — ISO because os64 has no locale to have an opinion with, and because the one format that sorts is the one worth picking |
| `\?` | the last command's exit status |
| `\j` | background jobs being tracked |
| `\n` | newline, for a two-line prompt |
| `\e` | an escape byte for terminal control sequences, including colour |
| `\\` | a literal backslash |

The terminal reads the colour and cursor sequences an escape byte begins
(CLAUDE.md § The terminal's escape sequences), so a green prompt is:

```
export PROMPT="\e[1;32m\W\e[0m$ "
```

`\e[0m` at the end is not optional politeness — without it, everything the
next command prints inherits the colour.

```
export PROMPT="\w $ "
export PROMPT="[\t] \W \?> "
export PROMPT="$HOSTNAME:\w $ "
```

**Named `PROMPT`, not `PS1`.** The `1` in PS1 only means anything alongside
PS2, PS3 and PS4 — continuation, select, xtrace — and husk has none of them,
so the number points at siblings that do not exist. (`PATH` keeps its name for
the opposite reason: it is a good one.)

**Escapes only — the prompt is never re-expanded.** bash runs PS1 through
parameter *and command* substitution on every print, which is its most famous
foot-gun. Here the string is expanded ONCE, at the assignment, through the
ordinary path where expansions are DATA and never syntax; the escapes above
are a closed vocabulary that cannot introduce syntax at print time. That is
also why there is no `\h`: `"$HOSTNAME:\w"` already works, and brings every
other environment variable with it.

**`$?` AND `\?` ARE NOT THE SAME THING, and the difference is the whole design
in one line.** `$?` is husk's own expansion: it happens once, at the
assignment, so it freezes the status that was current when the rc ran — a
literal `0` for the rest of the session. `\?` is a prompt escape, evaluated at
every print. The frozen one is the dangerous shape precisely because it looks
alive: put a ticking `\t` beside it and the prompt updates every second while
the one number that would tell you something never moves. (Found the day
`$PROMPT` shipped, in the author's own first prompt.)

The rule the two spellings encode: **`$NAME` for what does not change**
(`$HOSTNAME`), **an escape for what does**. Anything you want frozen, freeze
with `$`; anything you want live has to be in the escape table, and if it
isn't, it can't be live — that is the price of never re-expanding.

An escape the vocabulary does not know is printed **verbatim**, backslash and
all — a prompt is cosmetic, so refusing to draw one over a typo would be
hostile, and a stray `\q` on every line is a louder tripwire than a message
you would see once.

Expanded prompts are limited to 255 bytes. A longer expansion uses `husk> `
instead, before any custom prompt bytes are written: truncating a CSI or OSC
sequence could otherwise consume the next command's echoed characters.

Absent: `\u` (no users), `\h` (above), `\$`'s `#`-for-root (no privilege
levels), and `\[ \]` (husk's rub-out counts typed characters rather than
measuring prompt columns, so it needs no zero-width delimiters).

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

## Booked: a scripting language (a FEATURE, not a debt)

Chris, 2026-08-19: he wants to write complex husk scripts eventually, and he
wants them not to look like gobbledygook — "looking at you, Bash." Booked here
rather than in DEBTS on his instruction: nothing is broken and husk does its
present job well. This is a thing to BUILD, not a thing to repay.

**The sins to avoid, named** — they are separable, and only some are inherent
to being a shell:

1. `[` is a program pretending to be syntax, whose closing `]` is an argument
   it checks for.
2. **Unquoted `$x` word-splits and globs.** `rm $files` and `rm "$files"`
   differ; `[ -n $x ]` silently becomes `[ -n ]` when x is empty. The dialect
   that survives is "quote everything, always" — a rule you follow to defeat
   a feature nobody asked for. This is the deepest one.
3. Four conditional dialects — `test`, `[ ]`, `[[ ]]`, `(( ))` — each with
   different operators and quoting rules.
4. Reversed-word terminators (`fi`, `esac` — Algol 68 by way of Bourne, an
   Algol man), except loops end in `done`, so it is not even consistently
   strange.
5. `-eq` vs `=` vs `==`; numeric and string comparison spelled incompatibly.

**What husk already has and must keep**: exit status as the condition — what
makes every program in the system a predicate, and the thing that makes a
shell a shell — plus pipelines, `&&`/`||`, and `;`.

**The prior art to steal from**: `rc` (Tom Duff, Plan 9, 1990) — the same
exercise with the same motivation, and the paper says so out loud. Four ideas:

- **Variables are LISTS, not strings.** `files = (a.txt b.txt)`; `rm $files`
  is always two arguments with no quoting decision, because there is nothing
  to re-split. `$#x` counts, `$x(1)` indexes. This one change deletes the
  entire quoting-discipline burden that sin 2 creates.
- `{ }` blocks everywhere — no `fi`, no `esac`, no `done`.
- One quoting rule: single quotes, `''` for a literal quote. Quoting becomes
  about literal characters, never about defending yourself from the shell.
- Conditions are commands: `if (test -n $GUI) { gterm & }`.

Its one wart, not worth inheriting: `if not { }` for else.

**THE DECISION THAT CANNOT WAIT: strings or lists.** Everything else here can
be added incrementally; the variable model cannot. It reaches expansion,
assignment, and argv construction, and today husk has one rc file and almost
no scripts — this is the cheapest the choice will ever be. After a few hundred
lines of husk scripts exist it is effectively permanent. Staying string-shaped
forever is a legitimate answer; defaulting into it is not.

**Open question for Chris before anything is drafted**: how big do husk
scripts actually get? "A better rc file and the occasional five-liner" and "I
want to write real programs in this" point at different languages, and every
other decision hangs off that one.

Next step when it comes up: a design chapter here, ratified before a line of
code — the pattern PTY.md and GRAPHICS.md's VT8 chapter both earned.

## Known limits, stated plainly

- Editing a line that has WRAPPED misbehaves at the wrap seam — the
  renderer's `\b` clamps at column 0. A line that long deserves a script.
- Globs cover the last path component: `/tmp/*` yes, `/*/foo` no (booked).
- No tab completion, no `%1` job notation — the task number is the handle.
