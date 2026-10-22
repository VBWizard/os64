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


def display(text):
    """`text` as it may reach a terminal: a control character (C0, DEL and
    C1) re-spelled `\\xHH` and a backslash `\\\\`, so text a page wrote can
    neither drive the terminal (`\\x1b[2J`) nor pass for a line of this
    tool's own. Every page-controlled string a tool prints goes through
    it."""
    return ''.join(f'\\x{ord(c):02X}' if ord(c) < 0x20 or 0x7F <= ord(c) < 0xA0
                   else '\\\\' if c == '\\' else c for c in str(text))


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
END = b'end'
UNWRITTEN = b'verdict: unwritten'


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


UINT32_MAX = 0xFFFFFFFF


def decimal(digits):
    """THE ONE NUMBER PARSER: a whole decimal, digits and nothing else, at
    most UINT32_MAX, which no number diag.c writes passes (a count, a byte
    count, an index line's number, a zoom). Malformed otherwise, however
    long."""
    if not digits or len(digits) > 10 or not digits.isdigit():
        raise Malformed('not a whole page file: a count no record holds')
    n = int(digits)
    if n > UINT32_MAX:
        raise Malformed('not a whole page file: a count no record holds')
    return n


count_of = decimal


# THE GRAMMAR of a record's lines and an index's (tools/yonder_record_grammar.txt
# holds its vectors, which this reader and the replay runner's C reader both
# run and must agree on). A line is exactly one kind or REFUSED; a line that
# begins like a kind and does not match its grammar exactly is REFUSED, never
# read as some other kind, and never accepted because the record's counts
# happen to reconcile.
RE_VERDICT = re.compile(rb'verdict: (?:(clean)|(unwritten)|([0-9]+) MISSING, ([0-9]+) FAILED|'
                        rb'([0-9]+) MISSING|([0-9]+) FAILED)')
RE_MISSING = re.compile(rb'MISSING ([^ ]+) (.+) \(([0-9]+)\)')
RE_FAILED = re.compile(rb'FAILED (.+?): (.+) \(([0-9]+)\)')
RE_STATE = re.compile(rb'REPLAY state: (verdict|incomplete|unfinished|unjudged)(?: (.+))?')
RE_NOT_REPRODUCED = re.compile(rb'REPLAY not reproduced: ([^ ]+(?: [^ ]+)*)')
# The facts whose values a reader parses: each value matched whole, every
# number in it through decimal().
HEAD = {
    b'bytes': rb'([0-9]+)', b'kept lines': rb'([0-9]+)', b'zoom': rb'([0-9]+)', b'script seconds': rb'([0-9]+)',
    b'status': rb'([0-9]+)(?: .*)?',
    b'laid out at': rb'([0-9]+)(?:x([0-9]+))? px(?: in ([0-9]+) ms)?',
    b'body': rb'html|text|image', b'encrypted': rb'yes|no', b'scripts': rb'on|off', b'modern': rb'on|off',
    b'appearance': rb'light|dark', b'motion': rb'full|reduce', b'animate': rb'on|off', b'kept': rb'whole|not whole: .+',
    b'final address': rb'.*', b'content-type': rb'.*', b'charset': rb'.*', b'TLS': rb'.*',
}
HEAD = {k: re.compile(v) for k, v in HEAD.items()}


def classify(line, index=False):
    """(kind, fields) for one line of a record, or of an index (`index`):
    the kind's name as tools/yonder_record_grammar.txt spells it, or
    ('REFUSED', why) / ('index:REFUSED', why)."""
    def numbers(m):
        return [decimal(g) if g is not None and g.isdigit() else g for g in m.groups()]
    try:
        if index:
            if line.startswith(b'# yonder kept index '):
                return 'index:version', decimal(line[len(b'# yonder kept index '):])
            if line.startswith(b'#'):
                return 'index:comment', None
            f = line.split(b'\t')
            if (len(f) != 7 or f[1] not in (b'script', b'sheet') or not f[2] or f[3] not in (b'came', b'failed')
                    or decimal(f[0]) < 1):
                return 'index:REFUSED', 'a line that is not a script\'s or a sheet\'s whole'
            return 'index:entry', (decimal(f[0]), f[1], f[2], f[3] == b'came', f[4], f[5], decimal(f[6]))
        if line == END:
            return 'end', None
        if line == LINES_LOST:
            return 'lines-lost', None
        if line.startswith(b'address: '):
            return 'address', line[len(b'address: '):]
        if line.startswith(b'verdict: '):
            m = RE_VERDICT.fullmatch(line)
            if m is None:
                return 'REFUSED', 'a verdict of no spelling yonder writes'
            g = numbers(m)
            if g[1] is not None:
                return 'verdict', 'unwritten'
            counts = (g[2] or g[4] or 0, g[3] or g[5] or 0)
            if any(n == 0 for n in g[2:] if n is not None):
                return 'REFUSED', 'a verdict that names a token with no records'
            return 'verdict', counts
        if line.startswith(b'MISSING '):
            m = RE_MISSING.fullmatch(line)
            if m is None:
                return 'REFUSED', 'a record line with no count, or no kind and name'
            return 'missing', (m.group(1), m.group(2), decimal(m.group(3)))
        if line.startswith(b'FAILED '):
            m = RE_FAILED.fullmatch(line)
            if m is None:
                return 'REFUSED', 'a record line with no count, or no what and message'
            return 'failed', (m.group(1), m.group(2), decimal(m.group(3)))
        if line.startswith(b'REPLAY '):
            m = RE_STATE.fullmatch(line)
            if m is not None:
                if (m.group(1) == b'verdict') != (m.group(2) is None):
                    return 'REFUSED', 'a REPLAY state with no why, or a verdict with one'
                return 'state', (m.group(1), m.group(2) or b'')
            for head, kind in ((b'REPLAY incomplete ', 'incomplete'), (b'REPLAY unfinished ', 'unfinished')):
                if line.startswith(head) and len(line) > len(head):
                    return kind, line[len(head):]
            m = RE_NOT_REPRODUCED.fullmatch(line)
            if m is not None:
                words = set(m.group(1).decode('utf-8').split(' '))
                if not words <= NOT_REPRODUCED:
                    return 'REFUSED', ('a REPLAY not reproduced line naming what a replay does reproduce: '
                                       + ' '.join(sorted(words - NOT_REPRODUCED)))
                return 'not-reproduced', words
            return 'REFUSED', 'a REPLAY line of no kind this reader knows'
        key, sep, value = line.partition(b': ')
        if not sep or not key:
            return 'REFUSED', 'a line of no kind a record holds'
        if key in HEAD:
            m = HEAD[key].fullmatch(value)
            if m is None:
                return 'REFUSED', f'a `{key.decode("utf-8", "replace")}` of no value yonder writes'
            numbers(m)
            return 'head', (key, value)
        return 'fact', (key, value)
    except Malformed as e:
        return ('index:REFUSED' if index else 'REFUSED'), str(e).replace('not a whole page file: ', '')


def read_body(lines, rec, replay):
    """The record lines from the verdict on into `rec`, every one a kind of
    the grammar (classify): the verdict, its record lines exactly that many
    of each, no other verdict, and, on a replay's record (`replay`), its
    REPLAY detail lines; any line the grammar refuses, or a kind out of its
    place, is Malformed. A yonder record's lines-lost marker, or its
    never-written placeholder, makes it UNJUDGED."""
    where = 'third' if replay else 'second'
    kind, f = classify(lines[0]) if lines else ('REFUSED', None)
    if kind != 'verdict':
        raise Malformed(f'not a whole page file: its {where} line is not a verdict'
                        + (f' ({f})' if kind == 'REFUSED' and f else ''))
    if f == 'unwritten':
        if replay or len(lines) > 1:
            raise Malformed('not a whole page file: a placeholder with lines')
        rec.state, rec.reason = State.UNJUDGED, 'its name was claimed and its record never written'
        rec.unwritten = True
        return
    want_missing, want_failed = f
    seen_missing = seen_failed = 0
    for line in lines[1:]:
        kind, f = classify(line)
        if kind == 'missing':
            seen_missing += 1
            key = (unescape(f[0]), unescape(f[1]))
            rec.missing[key] = rec.missing.get(key, 0) + f[2]
        elif kind == 'failed':
            seen_failed += 1
            key = (unescape(f[0]), unescape(f[1]))
            rec.failed[key] = rec.failed.get(key, 0) + f[2]
        elif kind in ('fact', 'head'):
            pass
        elif kind == 'verdict':
            raise Malformed('not a whole page file: a second verdict line')
        elif kind == 'lines-lost' and not replay:
            rec.state, rec.reason, rec.lost = State.UNJUDGED, 'its record lost lines it could not keep', True
        elif kind == 'incomplete' and replay:
            rec.incomplete.append(unescape(f))
        elif kind == 'unfinished' and replay:
            rec.unfinished.append(unescape(f))
        elif kind == 'not-reproduced' and replay:
            rec.not_reproduced |= f
        elif kind == 'REFUSED':
            raise Malformed(f'not a whole page file: {f}')
        elif kind in ('incomplete', 'unfinished', 'not-reproduced', 'lines-lost', 'state'):
            raise Malformed('a REPLAY line of no kind this reader knows')
        else:
            raise Malformed(f'not a whole page file: {kind} out of its place')
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
    `verdict: unwritten`), no REPLAY line but the one saying it lost
    lines, and ends with `end`, which a kept page's (one that says `kept:`
    or `kept lines:`) must; a census record written before yonder wrote it may lack it.
    Every count is at most UINT32_MAX."""
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
        kind, state = classify(lines[1]) if len(lines) > 1 else ('REFUSED', None)
        if kind != 'state' and lines[1:2] and lines[1].startswith(b'REPLAY state: '):
            raise Malformed(f'not a whole page file: {state}')
        if kind != 'state':
            # Yonder ends every record with `end` now; a kept page's must
            # have it, and only a census record written before it (one
            # that says nothing of being kept) may lack it.
            ended = lines[-1] == END
            body = lines[1:-1] if ended else lines[1:]
            if END in body:
                raise Malformed('not a whole page file: lines past its `end`')
            # A kept page's record says so from its claim on (`kept lines:`,
            # written then; `kept:` comes when its bytes are whole), so one
            # cut short anywhere after its claim is caught.
            if not ended and any(line.startswith((b'kept: ', b'kept lines: ')) for line in body):
                raise Malformed("a kept page's record without its `end`: it was cut short")
            read_body(body, rec, replay=False)
            return rec
        if lines[-1] != END or len(lines) < 3:
            raise Malformed("a replay's record without its `end`: it was cut short")
        rec.replay = True
        rec.state = State[state[0].decode().upper()]
        rec.reason = unescape(state[1])
        if END in lines[2:-1]:
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
