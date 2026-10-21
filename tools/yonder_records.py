"""THE ONE READER OF YONDER'S PAGE FILES (YONDER_DIAGNOSTICS.md), which
tools/yonder_tally.py and tools/yonder_corpus_diff.py both read records
through, so a record is judged whole by one set of rules whichever tool
reads it.

A page file is one page load's record: line one `address: <url>`, line two
the verdict, then record lines

    MISSING <kind> <name> (<count>)
    FAILED <what>: <message> (<count>)

and plain `key: value` lines. A record a replay wrote
(tools/test_yonder_replay_host.c) puts its page's STATE second, `REPLAY
state: <state> <why>`, carries REPLAY detail lines after the window's
record, and ends with `end`. read() answers a Record, with its State, or
an Unjudged naming why the file cannot be judged; it never answers a
record of what it could read of a file that is not whole, since every
line such a file lost would read as a page that stopped asking for it.

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
    exactly. Bytes that are not UTF-8 raise UnicodeDecodeError, never a
    guess."""
    def one(m):
        e = m.group(1)
        return b'\\' if e == b'\\' else bytes([int(e[1:], 16)])
    raw = ESCAPE.sub(one, raw)
    raw = PERCENT.sub(lambda m: PERCENT_BACK[m.group(0)], raw)
    return raw.decode('utf-8')


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
    """What a page came to, in order of precedence (a page that is two is
    the later). A replay's record SAYS it, on its second line, `REPLAY
    state: <state> <why>`, decided by the runner from its own run; a
    record yonder wrote is a VERDICT unless it says it cannot be judged."""
    VERDICT = 0
    INCOMPLETE = 1      # the page and its kept fetches did not meet one for one
    UNFINISHED = 2      # the replay reached no verdict
    UNJUDGED = 3        # the record says why it cannot be judged


# The kinds of line a replay does not reproduce, being the outcome of a
# fetch it never performs (tools/test_yonder_replay_host.c's
# RP_NOT_REPRODUCED): a MISSING line's kind or a FAILED line's what. A
# `REPLAY not reproduced:` line naming anything else is refused, since it
# would keep lines of that kind out of every comparison.
NOT_REPRODUCED = {'font-fetch', 'image', 'picture-fetch'}

LINES_LOST = b'REPLAY unjudged: lines lost'
UNWRITTEN = b'verdict: unwritten'
STATE_LINE = re.compile(rb'REPLAY state: (verdict|incomplete|unfinished|unjudged)(?: (.*))?')


class Record:
    """One page file read whole: its address, its MISSING lines as {(kind,
    name): count}, its FAILED lines as {(what, message): count}, its
    `state` and, for UNJUDGED, its `reason`; whether a replay wrote it
    (`replay`), and on a replay's record its REPLAY detail lines
    (`incomplete`, `unfinished`) and the kinds its `REPLAY not reproduced:`
    line names (`not_reproduced`). A record yonder wrote that is UNJUDGED
    says why in a flag too: `lost`, it lost lines (the lines it kept are
    read, and a census counts them), or `unwritten`, it is a claimed
    name's placeholder."""

    def __init__(self, path, address):
        self.path = path
        self.address = address
        self.site = site_of(address)
        self.missing = {}
        self.failed = {}
        self.incomplete = []
        self.unfinished = []
        self.state = State.VERDICT
        self.reason = ''
        self.replay = False
        self.lost = False
        self.unwritten = False
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


class Malformed(Exception):
    """Why the lines being read are not a whole record."""


def read_body(lines, rec, replay):
    """The record lines from the verdict on into `rec`: the verdict, its
    lines exactly that many of each, each counted, no other verdict, and,
    on a replay's record (`replay`), its REPLAY detail lines; a REPLAY line
    of no kind this reader knows is refused. A yonder record's
    YONDER_DIAG_LINES_LOST, or its never-written placeholder, makes it
    UNJUDGED."""
    where = 'third' if replay else 'second'
    if not lines:
        raise Malformed(f'not a whole page file: its {where} line is not a verdict')
    if lines[0] == UNWRITTEN and not replay:
        rec.state, rec.reason = State.UNJUDGED, 'its name was claimed and its record never written'
        rec.unwritten = True
        return
    verdict = VERDICT.fullmatch(lines[0])
    if verdict is None:
        raise Malformed(f'not a whole page file: its {where} line is not a verdict')
    want_missing = int(verdict.group(1) or verdict.group(3) or 0)
    want_failed = int(verdict.group(2) or verdict.group(4) or 0)
    seen_missing = seen_failed = 0
    for line in lines[1:]:
        m = COUNT.search(line)
        if line.startswith(b'MISSING ') and m:
            seen_missing += 1
            kind, _, name = line[len(b'MISSING '):m.start()].partition(b' ')
            key = (unescape(kind), unescape(name))
            rec.missing[key] = rec.missing.get(key, 0) + int(m.group(1))
        elif line.startswith(b'FAILED ') and m:
            seen_failed += 1
            what, _, message = line[len(b'FAILED '):m.start()].partition(b': ')
            key = (unescape(what), unescape(message))
            rec.failed[key] = rec.failed.get(key, 0) + int(m.group(1))
        elif line.startswith(b'MISSING ') or line.startswith(b'FAILED '):
            raise Malformed('not a whole page file: a record line with no count')
        elif line.startswith(b'verdict: '):
            raise Malformed('not a whole page file: a second verdict line')
        elif line == LINES_LOST and not replay:
            rec.state, rec.reason, rec.lost = State.UNJUDGED, 'its record lost lines it could not keep', True
        elif replay and line.startswith(b'REPLAY incomplete '):
            rec.incomplete.append(unescape(line[len(b'REPLAY incomplete '):]))
        elif replay and line.startswith(b'REPLAY unfinished '):
            rec.unfinished.append(unescape(line[len(b'REPLAY unfinished '):]))
        elif replay and line.startswith(b'REPLAY not reproduced: '):
            kinds = set(unescape(line[len(b'REPLAY not reproduced: '):]).split())
            if not kinds <= NOT_REPRODUCED:
                raise Malformed(f'a REPLAY not reproduced line naming what a replay does reproduce: '
                                f'{" ".join(sorted(kinds - NOT_REPRODUCED))}')
            rec.not_reproduced |= kinds
        elif line.startswith(b'REPLAY '):
            raise Malformed('a REPLAY line of no kind this reader knows')
    if (seen_missing, seen_failed) != (want_missing, want_failed):
        raise Malformed(f'not a whole page file: its verdict counts {want_missing} MISSING and '
                        f'{want_failed} FAILED lines, and it holds {seen_missing} and {seen_failed}')


def read(path):
    """The page file at `path`, judged whole: a Record, or an Unjudged
    naming why not. Every file is UTF-8 with no NUL, no byte order mark and
    no carriage return, and its first line is `address: `. A REPLAY'S
    RECORD says its state on its second line (STATE_LINE) and ends with
    `end`; an UNJUDGED one is its address, its state and `end`, and any
    other carries the window's record from its verdict on, whose REPLAY
    detail lines must agree with the state (a VERDICT has none, an
    INCOMPLETE page an incomplete line and no unfinished one, an UNFINISHED
    page an unfinished line) and which says what it does not reproduce. A
    RECORD YONDER WROTE has its verdict second (or the placeholder's
    `verdict: unwritten`), and no REPLAY line but the one saying it lost
    lines."""
    path = pathlib.Path(path)
    try:
        if not path.is_file():
            return Unjudged(path, 'not a file')
        data = path.read_bytes()
    except OSError as e:
        return Unjudged(path, f'cannot be read: {e.strerror or e}')
    if data.startswith(b'\xef\xbb\xbf'):
        return Unjudged(path, 'not a page file: it begins with a byte order mark')
    if b'\0' in data:
        return Unjudged(path, 'not a whole page file: it holds a NUL')
    try:
        data.decode('utf-8')
    except UnicodeDecodeError:
        return Unjudged(path, 'not a page file: it is not UTF-8')
    lines = data.split(b'\n')
    if lines and lines[-1] == b'':
        lines.pop()
    if not lines or not lines[0].startswith(b'address: '):
        return Unjudged(path, 'not a page file: its first line is not `address: `')
    if any(line.endswith(b'\r') for line in lines):
        return Unjudged(path, 'not a whole page file: a line ends in a carriage return')
    try:
        rec = Record(path, unescape(lines[0][len(b'address: '):]))
        state = STATE_LINE.fullmatch(lines[1]) if len(lines) > 1 else None
        if state is None:
            read_body(lines[1:], rec, replay=False)
            return rec
        if lines[-1] != b'end' or len(lines) < 3:
            raise Malformed("a replay's record without its `end`: it was cut short")
        rec.replay = True
        rec.state = State[state.group(1).decode().upper()]
        rec.reason = unescape(state.group(2) or b'')
        if b'end' in lines[2:-1]:
            raise Malformed("a replay's record with lines past its `end`")
        if rec.state == State.UNJUDGED:
            return rec
        read_body(lines[2:-1], rec, replay=True)
        agrees = {State.VERDICT: not rec.incomplete and not rec.unfinished,
                  State.INCOMPLETE: bool(rec.incomplete) and not rec.unfinished,
                  State.UNFINISHED: bool(rec.unfinished)}[rec.state]
        if not agrees:
            raise Malformed("a replay's record whose REPLAY lines do not agree with its state")
        if not rec.not_reproduced:
            raise Malformed("a replay's record that does not say what it does not reproduce")
    except Malformed as e:
        return Unjudged(path, str(e))
    except UnicodeDecodeError:
        return Unjudged(path, 'not a page file: a field escapes to bytes that are not UTF-8')
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
