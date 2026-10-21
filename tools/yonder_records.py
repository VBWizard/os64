"""THE ONE READER OF YONDER'S PAGE FILES (YONDER_DIAGNOSTICS.md), which
tools/yonder_tally.py and tools/yonder_corpus_diff.py both read records
through, so a record is judged whole by one set of rules whichever tool
reads it.

A page file is one page load's record: line one `address: <url>`, line two
the verdict, then record lines

    MISSING <kind> <name> (<count>)
    FAILED <what>: <message> (<count>)

plain `key: value` lines, and, in a record a replay wrote
(tools/test_yonder_replay_host.c), `REPLAY` lines saying what the replay
reached. read() answers a Record or an Unjudged naming why the file cannot
be judged; it never answers a record of what it could read of a file that
is not whole, since every line such a file lost would read as a page that
stopped asking for it.

Imported by path from beside the tool (`python3 -I` puts no directory on
the path); it imports nothing but the standard library.
"""
import enum
import os
import pathlib
import re

COUNT = re.compile(rb' \((\d+)\)$')
VERDICT = re.compile(rb'verdict: (?:clean|(\d+) MISSING, (\d+) FAILED|(\d+) MISSING|(\d+) FAILED)')
ESCAPE = re.compile(rb'\\(\\|x[0-9A-Fa-f]{2})')
PERCENT = re.compile(rb'M%49SSING|F%41ILED|%25')
PERCENT_BACK = {b'M%49SSING': b'MISSING', b'F%41ILED': b'FAILED', b'%25': b'%'}


def unescape(raw):
    """A field as the page wrote it: diag.c spells a backslash `\\\\`, a
    byte below 0x20 or 0x7F `\\xHH`, a token in data with its second
    letter percent-encoded, and a `%` of the data's own `%25`, which this
    reads back in one pass from the left, so the data's bytes come back
    exactly."""
    def one(m):
        e = m.group(1)
        return b'\\' if e == b'\\' else bytes([int(e[1:], 16)])
    raw = ESCAPE.sub(one, raw)
    raw = PERCENT.sub(lambda m: PERCENT_BACK[m.group(0)], raw)
    return raw.decode('utf-8', 'replace')


def site_of(address):
    """The host an address names, lower case, without userinfo or port;
    the scheme for an address with none (`file`, `about`)."""
    m = re.match(r'^([A-Za-z][A-Za-z0-9+.-]*):(//([^/?#]*))?', address)
    if not m:
        return address
    host = m.group(3)
    if not host:
        return m.group(1).lower()
    host = host.rsplit('@', 1)[-1]
    if host.startswith('['):
        return host.split(']', 1)[0].lower() + ']'
    return host.split(':', 1)[0].lower()


class State(enum.IntEnum):
    """What a record says its page reached, in order of precedence: a
    record carrying the lines of two states is in the later one, so a
    replay that reached no verdict is UNFINISHED whatever else it says.
    A record yonder wrote, with no `REPLAY` line, reached its VERDICT."""
    VERDICT = 0
    INCOMPLETE = 1      # `REPLAY incomplete <kind> <url>`: asked for what the corpus never kept
    UNFINISHED = 2      # `REPLAY unfinished <why>`: the replay reached no verdict


class Record:
    """One page file read whole: its address, its MISSING lines as {(kind,
    name): count}, its FAILED lines as {(what, message): count}, its
    `REPLAY` lines by state (`incomplete`, `unfinished`), its `state`, and
    the kinds its `REPLAY not reproduced:` line names (`not_reproduced`):
    a replay's record says which kinds of line are not the page's, since
    the replay never fetches what they are about.
    `lost`: the record says some lines had no memory to be named, which a
    count cannot show."""

    def __init__(self, path, address):
        self.path = path
        self.address = address
        self.site = site_of(address)
        self.missing = {}
        self.failed = {}
        self.incomplete = []
        self.unfinished = []
        self.lost = False
        self.state = State.VERDICT
        self.not_reproduced = set()


class Unjudged:
    """A file that cannot be judged, and why: one that cannot be read, is
    not a page file, or is not whole."""

    def __init__(self, path, reason):
        self.path = path
        self.reason = reason

    def __str__(self):
        return f'{self.path}: {self.reason}'


class Unreadable(Exception):
    """A path given that is not there, or a directory that cannot be
    listed: read as empty, it would read as a corpus that asked for
    nothing."""


# The `REPLAY` lines a record may carry, by the state each says.
REPLAY_STATES = ((b'REPLAY incomplete ', State.INCOMPLETE), (b'REPLAY unfinished ', State.UNFINISHED))


def read(path):
    """The page file at `path`, judged whole: a Record, or an Unjudged
    naming why not. Its first line must be `address: `, its second
    `verdict: clean` or the counts of the tokens with records, its record
    lines exactly that many of each, each with its count, no other line a
    verdict, and every `REPLAY` line one of the states this reader knows or
    the kinds a replay does not reproduce.
    A file that cannot be read, a byte order mark, and a line ending in a
    carriage return are each named."""
    path = pathlib.Path(path)
    try:
        if not path.is_file():
            return Unjudged(path, 'not a file')
        data = path.read_bytes()
    except OSError as e:
        return Unjudged(path, f'cannot be read: {e.strerror or e}')
    if data.startswith(b'\xef\xbb\xbf'):
        return Unjudged(path, 'not a page file: it begins with a byte order mark')
    lines = data.split(b'\n')
    if lines and lines[-1] == b'':
        lines.pop()
    if not lines or not lines[0].startswith(b'address: '):
        return Unjudged(path, 'not a page file: its first line is not `address: `')
    if any(line.endswith(b'\r') for line in lines):
        return Unjudged(path, 'not a whole page file: a line ends in a carriage return')
    verdict = VERDICT.fullmatch(lines[1]) if len(lines) > 1 else None
    if verdict is None:
        return Unjudged(path, 'not a whole page file: its second line is not a verdict')
    want_missing = int(verdict.group(1) or verdict.group(3) or 0)
    want_failed = int(verdict.group(2) or verdict.group(4) or 0)
    rec = Record(path, unescape(lines[0][len(b'address: '):]))
    seen_missing = seen_failed = 0
    for line in lines[2:]:
        m = COUNT.search(line)
        if line.startswith(b'MISSING ') and m:
            seen_missing += 1
            body = line[len(b'MISSING '):m.start()]
            kind, _, name = body.partition(b' ')
            key = (unescape(kind), unescape(name))
            rec.missing[key] = rec.missing.get(key, 0) + int(m.group(1))
        elif line.startswith(b'FAILED ') and m:
            seen_failed += 1
            body = line[len(b'FAILED '):m.start()]
            what, _, message = body.partition(b': ')
            key = (unescape(what), unescape(message))
            rec.failed[key] = rec.failed.get(key, 0) + int(m.group(1))
        elif line.startswith(b'MISSING ') or line.startswith(b'FAILED '):
            return Unjudged(path, 'not a whole page file: a record line with no count')
        elif line.startswith(b'verdict: '):
            return Unjudged(path, 'not a whole page file: a second verdict line')
        elif line.startswith(b'REPLAY not reproduced: '):
            rec.not_reproduced |= set(unescape(line[len(b'REPLAY not reproduced: '):]).split())
        elif line.startswith(b'REPLAY '):
            for head, state in REPLAY_STATES:
                if line.startswith(head):
                    said = unescape(line[len(head):])
                    (rec.incomplete if state == State.INCOMPLETE else rec.unfinished).append(said)
                    rec.state = max(rec.state, state)
                    break
            else:
                return Unjudged(path, 'a REPLAY line of a state this reader does not know')
        elif line.startswith(b'some lines not kept: '):
            rec.lost = True
    if (seen_missing, seen_failed) != (want_missing, want_failed):
        return Unjudged(path, f'not a whole page file: its verdict counts {want_missing} MISSING and '
                              f'{want_failed} FAILED lines, and it holds {seen_missing} and {seen_failed}')
    return rec


def page_files(directory):
    """The `*.txt` names in a directory, sorted, as paths: every one, so a
    `*.txt` that is not a page file is read, and named, rather than passed
    over. Unreadable when the directory cannot be listed."""
    try:
        with os.scandir(directory) as entries:
            names = sorted(e.name for e in entries if e.name.endswith('.txt'))
    except OSError as e:
        raise Unreadable(f'{directory}: cannot be listed: {e.strerror or e}') from e
    return [pathlib.Path(directory) / n for n in names]
