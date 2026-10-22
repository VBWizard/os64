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

# Every escape, tried at each place in ONE pass from the left (REPLAY.md
# § 2): what one decodes is never read again, so `\\x25` and `25` side by
# side are `%25`, never `%`. The runner's rp_unescape_n is the same pass.
ESCAPE = re.compile(rb'\\\\|\\x[0-9A-Fa-f]{2}|M%49SSING|F%41ILED|%25')
ESCAPE_BACK = {b'\\\\': b'\\', b'M%49SSING': b'MISSING', b'F%41ILED': b'FAILED', b'%25': b'%'}


def unescape_bytes(raw, tokens=True):
    """A field's bytes as the page wrote them: diag.c spells a backslash
    `\\\\`, a byte below 0x20 or 0x7F `\\xHH`, and, with `tokens` (a record's
    fields; an index's have no tokens), a token in data with its second
    letter percent-encoded and a `%` of the data's own `%25`. One pass from
    the left gives back the data's bytes exactly."""
    def one(m):
        e = m.group(0)
        if e.startswith(b'\\x'):
            return bytes([int(e[2:], 16)])
        if not tokens and e not in (b'\\\\',):
            return e
        return ESCAPE_BACK[e]
    return ESCAPE.sub(one, raw)


def unescape(raw):
    """unescape_bytes() decoded: bytes that are not UTF-8 raise
    UnicodeDecodeError, never a guess."""
    return unescape_bytes(raw).decode('utf-8')


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

END = b'end'
UNWRITTEN = b'verdict: unwritten'


class Record:
    """One page file read whole: its address, its MISSING lines as {(kind,
    name): count}, its FAILED lines as {(what, message): count}, its
    `state` and, for UNJUDGED, its `reason`; whether a replay wrote it
    (`replay`), and on a replay's record its REPLAY detail lines
    (`incomplete`, `unfinished`), the kinds its `REPLAY not reproduced:`
    line names (`not_reproduced`), and, for a page of version 1, what it
    took from outside its record: the inputs the harness gave
    (`defaulted`), those `--window` stated, {key: value} (`stated`), and
    whether it was assumed kept whole (`assumed`). A record yonder wrote
    that is UNJUDGED says why in a flag too: `marked`, it says so itself,
    and `lost`, it lost lines (the lines it kept are read, and a census
    counts them), or `unwritten`, it is a claimed name's placeholder."""

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
        self.marked = False
        self.unwritten = False
        self.defaulted = set()
        self.stated = {}
        self.assumed = False
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


CLOCK_MAX = (1 << 53) - 1


def decimal(digits, limit=UINT32_MAX):
    """THE ONE NUMBER PARSER (REPLAY.md § 2): a whole decimal, ASCII digits
    and nothing else, no sign, no leading zero past a lone `0`, at most
    `limit`: UINT32_MAX, which no number diag.c writes passes (a count, a
    byte count, an index line's number, a zoom), or CLOCK_MAX for a clock's
    milliseconds. Malformed otherwise, however long."""
    if (not digits or len(digits) > len(str(limit)) or not all(0x30 <= c <= 0x39 for c in digits)
            or (len(digits) > 1 and digits[0] == 0x30)):
        raise Malformed('not a whole page file: a count no record holds')
    n = int(digits)
    if n > limit:
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
    b'agent': rb'.+', b'cookies': rb'.*', b'clock': rb'([0-9]+)', b'tz': rb'.+',
}
HEAD = {k: re.compile(v) for k, v in HEAD.items()}


# The inputs version 2 of a record added (REPLAY.md § 3): a record that
# says any of them is of version 2, and one that says none of version 1.
V2_FACTS = (b'agent: ', b'cookies: ', b'clock: ', b'tz: ')

# What a replay of a version 1 page takes from outside its record
# (REPLAY.md § 3), never silently: DEFAULTED, from the harness, the
# cookies and the clock, which nobody can state for a page recorded
# before them; STATED, by the person running the replay (`--window`), the
# window's inputs, the agent and the zone.
DEFAULTABLE = {'cookies', 'clock'}
STATABLE = {'zoom', 'appearance', 'motion', 'animate', 'modern', 'scripts', 'script-seconds', 'laid-out-at',
            'agent', 'tz'}
KEPT_ASSUMED = b'REPLAY kept: assumed whole (version 1)'


def classify(line, index=False):
    """(kind, fields) for one line of a record, or of an index (`index`):
    the kind's name as tools/yonder_record_grammar.txt spells it, or
    ('REFUSED', why) / ('index:REFUSED', why). A line holding a control
    byte (a carriage return, a NUL, any byte below 0x20 but the tab), or a
    field that decodes to a NUL, is no line of either, and read()
    classifies every line of a record, so one anywhere refuses it whole."""
    def numbers(m, limit=UINT32_MAX):
        return [decimal(g, limit) if g is not None and g[:1].isdigit() else g for g in m.groups()]
    try:
        if any(c < 0x20 for c in line if c != 0x09):
            return ('index:REFUSED' if index else 'REFUSED'), 'a control byte'
        # Nor one spelled `\x00`: a field that decodes to a NUL would read
        # as shorter than it is wherever a string ends at one.
        if b'\0' in unescape_bytes(line, tokens=not index):
            return ('index:REFUSED' if index else 'REFUSED'), 'a NUL in a field'
        if index:
            if line.startswith(b'# yonder kept index '):
                return 'index:version', decimal(line[len(b'# yonder kept index '):])
            if line.startswith(b'#'):
                return 'index:comment', None
            f = line.split(b'\t')
            if (len(f) not in (6, 7) or f[1] not in (b'script', b'sheet') or not f[2]
                    or f[3] not in (b'came', b'failed') or decimal(f[0]) < 1):
                return 'index:REFUSED', 'a line that is not a script\'s or a sheet\'s whole'
            if len(f) == 6:
                return 'index:entry-v1', (decimal(f[0]), f[1], f[2], f[3] == b'came', f[4], f[5], None)
            return 'index:entry', (decimal(f[0]), f[1], f[2], f[3] == b'came', f[4], f[5], decimal(f[6]))
        if line == END:
            return 'end', None
        if line.startswith(b'REPLAY unjudged: ') and len(line) > len(b'REPLAY unjudged: '):
            return 'marked', line[len(b'REPLAY unjudged: '):]
        if line.startswith(b'REPLAY inputs defaulted: '):
            words = line[len(b'REPLAY inputs defaulted: '):].split(b' ')
            if not all(w.decode('ascii', 'replace') in DEFAULTABLE for w in words) or len(set(words)) != len(words):
                return 'REFUSED', 'a REPLAY inputs defaulted line naming no input a replay defaults'
            return 'defaulted', {w.decode('ascii') for w in words}
        if line.startswith(b'REPLAY inputs stated: '):
            stated = {}
            for item in line[len(b'REPLAY inputs stated: '):].split(b' '):
                key, eq, value = item.partition(b'=')
                key = key.decode('ascii', 'replace')
                if not eq or not value or key not in STATABLE or key in stated:
                    return 'REFUSED', 'a REPLAY inputs stated line naming no input a person states, or none twice'
                stated[key] = value
            return 'stated', stated
        if line == KEPT_ASSUMED:
            return 'assumed', None
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
            numbers(m, CLOCK_MAX if key == b'clock' else UINT32_MAX)
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
        elif kind == 'marked' and not replay:
            # One line for every reason a record yonder wrote says it cannot
            # be judged: its lines lost, a setting changed during the load.
            rec.marked = True
            if f == b'lines lost':
                rec.lost = True
            if rec.state != State.UNJUDGED:
                rec.state = State.UNJUDGED
                rec.reason = ('its record lost lines it could not keep' if f == b'lines lost'
                              else 'its record says: ' + unescape(f))
        elif kind == 'defaulted' and replay:
            rec.defaulted |= f
        elif kind == 'stated' and replay:
            rec.stated.update({k: unescape(v) for k, v in f.items()})
        elif kind == 'assumed' and replay:
            rec.assumed = True
        elif kind == 'incomplete' and replay:
            rec.incomplete.append(unescape(f))
        elif kind == 'unfinished' and replay:
            rec.unfinished.append(unescape(f))
        elif kind == 'not-reproduced' and replay:
            rec.not_reproduced |= f
        elif kind == 'REFUSED':
            raise Malformed(f'not a whole page file: {f}')
        elif kind in ('incomplete', 'unfinished', 'not-reproduced', 'marked', 'defaulted', 'stated', 'assumed', 'state'):
            raise Malformed('a REPLAY line of no kind this reader knows')
        else:
            raise Malformed(f'not a whole page file: {kind} out of its place')
    if (seen_missing, seen_failed) != (want_missing, want_failed):
        raise Malformed(f'not a whole page file: its verdict counts {want_missing} MISSING and '
                        f'{want_failed} FAILED lines, and it holds {seen_missing} and {seen_failed}')


def read(path):
    """The page file at `path`, judged whole: a Record, or an Unjudged
    naming why not. Every file is UTF-8 with no byte order mark, its first
    line is `address: `, and every line of it is a kind of the grammar
    (classify), so a control byte anywhere (a carriage return, a NUL)
    refuses it. A REPLAY'S RECORD says its state on its second line and
    ends with `end`; an UNJUDGED one is its address, its state and `end`
    and nothing else, and any other carries the window's record from its
    verdict on, whose REPLAY detail lines must agree with the state (a
    VERDICT has none, an INCOMPLETE page an incomplete line and no
    unfinished one, an UNFINISHED page an unfinished line), may say the
    inputs it took from the harness, and says what it does not reproduce.
    A RECORD YONDER WROTE has its verdict second (or the placeholder's
    `verdict: unwritten`), no REPLAY line but a `REPLAY unjudged:` mark,
    and ends with `end`, which a kept page's of version 2 (one that says
    `kept:` or `kept lines:`, and an input of V2_FACTS) must; a census
    record, or a kept page of version 1, written before yonder wrote it
    may lack it. Every count is at most UINT32_MAX, and a clock at most
    CLOCK_MAX."""
    path = pathlib.Path(path)
    try:
        if not path.is_file():
            return Unjudged(path, 'not a file')
        data = path.read_bytes()
    except OSError as e:
        return Unjudged(path, f'cannot be read: {e.strerror or e}')
    if data.startswith(b'\xef\xbb\xbf'):
        return Unjudged(path, 'not a page file: it begins with a byte order mark')
    try:
        data.decode('utf-8')
    except UnicodeDecodeError:
        return Unjudged(path, 'not a page file: it is not UTF-8')
    lines = data.split(b'\n')
    if lines and lines[-1] == b'':
        lines.pop()
    if not lines or not lines[0].startswith(b'address: '):
        return Unjudged(path, 'not a page file: its first line is not `address: `')
    try:
        kind, why = classify(lines[0])
        if kind != 'address':
            raise Malformed(f'not a whole page file: {why}')
        rec = Record(path, unescape(lines[0][len(b'address: '):]))
        kind, state = classify(lines[1]) if len(lines) > 1 else ('REFUSED', None)
        if kind != 'state' and lines[1:2] and lines[1].startswith(b'REPLAY state: '):
            raise Malformed(f'not a whole page file: {state}')
        if kind != 'state':
            # Yonder ends every record with `end`; a kept page's record of
            # version 2 (it says an input version 2 added, V2_FACTS, which
            # the claim writes first) must have it, from its claim on (`kept lines:`, written then;
            # `kept:` when its bytes are whole), so one cut short anywhere
            # after its claim is caught. A census record written before
            # `end` existed, and a kept page of version 1, may lack it
            # (REPLAY.md § 3).
            ended = lines[-1] == END
            body = lines[1:-1] if ended else lines[1:]
            if END in body:
                raise Malformed('not a whole page file: lines past its `end`')
            kept = any(line.startswith((b'kept: ', b'kept lines: ')) for line in body)
            if not ended and kept and any(line.startswith(V2_FACTS) for line in body):
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
            if len(lines) != 3:
                raise Malformed("an unjudged replay's record with lines beside its state")
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
