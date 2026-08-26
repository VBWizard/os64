#!/bin/bash
# stale_refs.sh — the retirement grep (2026-08-25).
#
# THE FAILURE THIS EXISTS FOR: a commit deletes or renames a thing — a
# function, a #define, a file — and a comment or a doc somewhere the diff
# never touched keeps citing it by name. Nobody re-reads the whole tree for
# every commit, so it survives until a reviewer runs the grep the author
# did not. PR #31 paid six review rounds for exactly this: the old wake
# helper's name in GRAPHICS.md and SCHEDULER.md, the deleted gui/desktop.c
# named in five headers and two docs, gui_startup cited in conf.c after it
# was gone. Every one was a two-second grep. This is that grep, run for you.
#
# WHAT IT DOES
#   1. Takes the diff (uncommitted work against HEAD by default, or any git
#      range) and collects every identifier-shaped and path-shaped token that
#      a REMOVED line carried and no ADDED CODE line carries — the names this
#      change retired. Comment text on added lines does not count as
#      re-adding: a commit that renames X and writes "this was X until today"
#      has still retired X (the first draft of this tool missed the wake
#      helper for exactly that reason).
#   2. Greps the resulting tree — every tracked file, docs included — for
#      each, whole-word. A name that still appears in CODE is live, not
#      retired, and is dropped. A name whose every surviving hit is PROSE
#      (a comment, a .md file) is reported, hit by hit: those are the lines
#      someone must READ before committing. History is allowed to name the
#      dead, but only on purpose.
#   3. Lists ADDED comment lines that carry the words that go stale first:
#      ONLY, NOTHING ELSE, THE ONE, EXACTLY ONE, EVERY, NEVER, ALWAYS, SOLE.
#      Superlatives are the claims a later change silently falsifies
#      ("the ONE whole-file reader" — there were four). For each: what would
#      make it false, and does this commit do that?
#
# It cannot judge a REASON that went stale ("the kernel walks the ladder for
# its own readers" stayed grammatical after the last reader left). That
# takes a reader. It catches the mechanical two-thirds.
#
# USAGE
#   tools/stale_refs.sh            # working tree + index vs HEAD (pre-commit)
#   tools/stale_refs.sh HEAD~1     # the last commit
#   tools/stale_refs.sh a70279a~1..a70279a
# Exit status 1 when anything was reported, so it can gate a commit.
#
# No jq (it is not on this machine); python3 does the work.

set -u
cd "$(git rev-parse --show-toplevel)" || exit 1

range=${1:-}
if [ -z "$range" ]; then
    diff_cmd=(git diff HEAD)
    tree_rev=""                       # grep the working tree
    label="working tree vs HEAD"
else
    case "$range" in
        *..*) diff_cmd=(git diff "$range"); tree_rev=${range#*..} ;;
        *)    diff_cmd=(git diff "$range~1" "$range"); tree_rev=$range ;;
    esac
    label="$range"
fi

# The diff goes through a FILE, not a pipe: `python3 -` takes its PROGRAM
# from stdin, so a heredoc program and a piped diff cannot share it (the
# second draft of this tool reported nothing at all for exactly that reason).
difffile=$(mktemp)
trap 'rm -f "$difffile"' EXIT
"${diff_cmd[@]}" > "$difffile" || exit 1

python3 - "$label" "$tree_rev" "$difffile" <<'EOF'
import re, subprocess, sys
label, tree_rev, difffile = sys.argv[1], sys.argv[2], sys.argv[3]
diff = open(difffile, errors="replace").read().splitlines()

IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]{5,}')
PATH  = re.compile(r'[A-Za-z0-9_./-]+/[A-Za-z0-9_.-]+\.(?:c|h|S|md|sh|py|conf|ld)')
SUPER = re.compile(r'\b(ONLY|NOTHING ELSE|THE ONE|EXACTLY ONE|EVERY|NEVER|ALWAYS|SOLE)\b')
PROSE_EXT = ('.md', '.txt')

def code_part(line, fname):
    """The part of a source line that is code, not comment. Prose files are
    all comment; a '//' starts one; a line that is a '*' or '#' (not a
    preprocessor directive) continuation is one."""
    if fname.endswith(PROSE_EXT):
        return ''
    s = line.lstrip()
    if s.startswith('*') or (s.startswith('#') and not s.startswith('#define')
                              and not s.startswith('#include') and not s.startswith('#if')
                              and not s.startswith('#endif') and not s.startswith('#else')):
        return ''
    return line.split('//', 1)[0]

removed, added_code, supers = set(), set(), []
fname = ''
for line in diff:
    if line.startswith('+++ '):
        fname = line[6:] if line.startswith('+++ b/') else line[4:]
        continue
    if line.startswith('--- '):
        continue
    if line.startswith('-'):
        body = line[1:]
        removed.update(IDENT.findall(body)); removed.update(PATH.findall(body))
    elif line.startswith('+'):
        body = line[1:]
        code = code_part(body, fname)
        added_code.update(IDENT.findall(code)); added_code.update(PATH.findall(code))
        if code.strip() != body.strip() and SUPER.search(body):
            supers.append(f"{fname}: {body.strip()}")

def namey(t):
    # An underscore or a path separator: that is what a NAME looks like here.
    # ALLCAPS alone is the house style for emphasis in prose (LARGER, EVERY)
    # and flagged half the tree on the first run.
    return '_' in t or '/' in t
retired = sorted(t for t in removed - added_code if namey(t))

def tree_hits(tok):
    cmd = ['git', 'grep', '-n', '-w', '-F', '--', tok] + ([tree_rev] if tree_rev else [])
    out = subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()
    hits = []
    for h in out:
        if tree_rev and h.startswith(tree_rev + ':'):
            h = h[len(tree_rev) + 1:]
        f, _, rest = h.partition(':')
        ln, _, text = rest.partition(':')
        if f == 'tools/stale_refs.sh':
            continue
        hits.append((f, ln, text))
    return hits

print(f"stale_refs: {label}\n")
reported = 0
print("== Names this change RETIRED that survive only in prose (read every line):")
for tok in retired:
    hits = tree_hits(tok)
    if not hits:
        continue
    # Still used by CODE somewhere? Then it is live, not retired.
    if any(tok in code_part(text, f) for f, _, text in hits):
        continue
    reported += 1
    print(f"-- {tok}")
    for f, ln, text in hits:
        print(f"   {f}:{ln}: {text.strip()[:140]}")
if reported == 0:
    print("   (none)")

print()
if supers:
    print("== ADDED comment lines making a claim that goes stale first — what would make each false?")
    for s in supers:
        print("   " + s[:160])
else:
    print("== No new superlative claims.")

sys.exit(1 if reported else 0)
EOF
