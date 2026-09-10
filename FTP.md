# FTP client — design

Status: rulings issued 2026-09-09 (section below), and built. This is
BROWSER.md's rung 6, the last one on the ladder, and the reason it is last is
that it is the only rung whose protocol needs TWO connections at once.

Written by Opus 5 before any code, per the house rule that known deferrals
are discussed before the slice starts rather than discovered at review.

`userland/apps/ftp/wire.{c,h}` is the protocol half, `ftp.c` the half that
makes syscalls, `tools/test_ftp_host.sh` the fixture and `tools/ftptestd.py`
the deterministic server. What the acceptance pass proved, and the three
defects the harness found before the OS ran any of it, are in
VERIFICATION.md § FTP client acceptance.

## Why this rung exists

Every rung before it dialed once, said one thing, read one answer and hung
up. A browser does not work that way: it opens a connection for the page and
then a connection per image, each to an ephemeral port the stack has to draw,
use and release while the first connection is still alive. FTP is that
pattern with thirty-five years of documentation and a hundred public servers
to practise against, so it rehearses the browser's connection behaviour
before the browser exists to be debugged.

What it points at that nothing else has: **the ephemeral port bitmap under
churn** (`s_ephemeral_used`, tcp.c), a **connection closing while another
connection to the same peer is live**, and an **upload over a real round
trip** — the second of the two triggers the stop-and-wait DEBTS row named
before PR #65 paid it in advance.

## The lineage, because it is a good one

**FTP is older than TCP, older than telnet's re-specification, and older than
the internet.** Abhay Bhushan wrote RFC 114 at MIT in April 1971, for a
network of a dozen machines speaking NCP. RFC 959 (Postel and Reynolds,
October 1985) is the re-specification everyone actually implements, and it is
the document this client is built from.

**RFC 959 specifies the control connection as a Telnet connection.** Not "like
telnet" — it says the control channel follows the Telnet protocol, which is
why commands end with CRLF, why the spec discusses Telnet end-of-line, and
why the interrupt sequence in §4.1.3 is spelled in IAC codes. We built
/bin/telnet yesterday. The dependency in the specification is real and forty
years old; whether this client ever needs to act on it is answered under
"Protocol scope" below.

**Passive mode is the world's answer to a design that assumed no firewalls.**
The original mode has the SERVER dial the CLIENT back for data, which was
entirely reasonable when every host on the network had a routable address and
nothing in between dropped anything. PASV (RFC 959 §4.1.2) inverts it so the
client dials both connections. NAT made that the only mode that works, and
today it is what every client uses by default.

**FTPS is FTP over TLS** (RFC 4217, 2005): the client sends `AUTH TLS` on the
control channel and both channels become TLS from there. It is not SFTP,
which is an unrelated protocol riding SSH and shares nothing with this one but
four letters. Named here because the deferral list mentions it and the two get
confused constantly.

## Rulings (Chris, 2026-09-09)

1. **An interactive client, not a fetch.** `/bin/ftp`, the 4.2BSD shape. An
   `ftp://` scheme in os64get is a later increment that costs almost nothing
   once the protocol half exists, and it would not exercise the churn this
   rung is for: a one-shot fetch dials twice and exits.

2. **Passive mode only.** Active mode needs the client to LISTEN, which os64's
   TCP does not do and which BROWSER.md puts in the tier this slice is not.
   This is not a gap to apologise for; it is the mode the internet uses.

3. **`TYPE I` always, and no `ascii` command.** ASCII mode exists to translate
   line endings between machines that disagree about them. os64 agrees with
   Unix, and the translation can only corrupt a binary that someone forgot to
   switch modes for. DIVERGENCES gets the row; the client sends `TYPE I` once
   at login and never mentions it again.

4. **LIST output prints raw.** RFC 959 never specified what LIST returns, so
   every client that tried to parse it ended up with a table of heuristics for
   Unix, Windows, VMS, MVS and three kinds of wrong. MLSD (RFC 3659, 2007)
   fixed it twenty-two years too late for anyone to rely on. We print what the
   server sent. *Chris reserves the right to revisit this the moment he sees
   what a real server's LIST looks like on the glass, which is the correct
   place to make that decision.*

5. **The deferrals live in THIS FILE, not in DEBTS.md.** Chris's ruling when
   the list was read to him: they are scope, not debt. A DEBTS row is for
   something the system will one day be worse for lacking; a client that does
   not implement resume is just a client that does not implement resume.

## The application

`/bin/ftp [HOST [PORT]]`, default port 21. With a host it connects and logs
in immediately; without one it comes up at its own prompt and waits for
`open`. A console application, usable on a VT, inside husk, or as gterm's
seated program.

**No terminal mode is changed anywhere, and this is the interesting
difference from telnet.** Telnet needed one loop watching two mouths because
either end may speak at any moment, and raw mode so that Ctrl+C and Ctrl+D
could reach the peer as bytes. FTP's control channel is strictly
request/response and the person drives every request, so the client reads a
line, sends a command, and reads the reply. Nothing arrives unbidden except a
`421` when the server times the session out, and that is discovered on the
next command.

**THE PROMPT PAINTS ITSELF, because in os64 echo is the reader's job** —
console.c's discipline is "the caller echoes", which PTY.md § What os64
already built without meaning to records as the reason a pty slave needs no
echo machinery. It is a good rule and it costs every program with a prompt one
loop: `prompt_line` reads a byte, paints it, handles Backspace by overprinting
and swallows an arrow key's escape sequence rather than letting it into a file
name. This client shipped without it for an afternoon and the prompt answered
commands nobody could see themselves typing.

The same loop with the painting switched off IS the password prompt. Hiding a
password takes no terminal mode here, because nothing was going to show it —
an earlier draft asked for raw mode to "hide" it and warned the user when it
could not get it, which was a warning about a danger that did not exist.

### The verbs

| Command | Wire | Notes |
|---|---|---|
| `open HOST [PORT]` | connect + login | refuses if already connected |
| `close` | `QUIT` | back to the prompt, connection down |
| `quit` / `bye` | `QUIT` | ends the program |
| `user [NAME]` | `USER`/`PASS` | re-authenticate on a live connection |
| `ls` / `dir [PATH]` | `LIST` | data connection, printed raw |
| `cd PATH` | `CWD` | |
| `cdup` | `CDUP` | |
| `pwd` | `PWD` | prints the 257 reply's quoted path |
| `get REMOTE [LOCAL]` | `RETR` | data connection, staged, see below |
| `put LOCAL [REMOTE]` | `STOR` | data connection |
| `del NAME` | `DELE` | |
| `mkdir NAME` | `MKD` | |
| `rmdir NAME` | `RMD` | |
| `status` | none | what is connected, and the transfer counters |
| `quote WORDS...` | verbatim | the traditional escape hatch |
| `help` / `?` | none | |

Anonymous is the default when the server accepts it: `USER anonymous` with
`PASS os64@` — the convention since the 1980s is an email address, and a
machine that does not have one should not invent a plausible-looking lie.

### Why the transfer buffer is 64KB and gets filled before it is written

Chris measured `get` at about 11 Mb/s on the P5 against os64get's 179 Mb/s
over the same wifi, which is far too large a gap to be the link. The cause was
that the transfer loop **wrote every read**. A read of a connection answers
with what has ARRIVED — one segment, or a scheduler pass's worth — so an
ordinary download handed write-through ext2 a block or two per TCP segment,
and every one of those is a disk transaction the wire waits behind.

os64get had already learned this and written it down at `GET_CHUNK`: "at 4KB
the file was written a block per syscall, and the transfer waited on the disk,
not the wire." The lesson was in the tree and this client did not reuse it.

Two things fix it, and 64KB is the size because it is three things at once —
the receive ring (`TCP_RCV_BUF`), the send ring, and the block cache's line.
One read can drain everything that arrived; one write is a run the disk takes
in a single pass.

There is a second effect that only shows on a real link, and it is the one
that probably explains the size of the gap. While the old loop sat in a
synchronous disk write, the receive ring filled and the window we advertise
went to zero. Recovering from a shut window costs a round trip — free on
QEMU's loopback, not free over wifi through ICS. So the penalty is the disk's
slowness MULTIPLIED by the link's latency, which is why QEMU understates it.

The cost of the change is that a LISTING now appears when it is complete
rather than line by line, because stdout takes the same path. A directory is
bounded by what a person will read; a download is not.

### What a downloaded file is called before it is a file

`get` writes to `<local>.part` and renames over `<local>` when the transfer
is complete AND the control channel has confirmed it. Interrupted, refused or
truncated, the `.part` stays and nothing is published.

This is os64get's rule and it is here for os64get's reason: **a truncated
download that keeps the real name looks exactly like a successful one.** The
classic ftp(1) writes straight through and leaves you whatever arrived, which
was a reasonable trade when the alternative was retyping the transfer and the
file was on a tape somewhere. It is not a reasonable trade now.

## Protocol scope

Spoken: `USER`, `PASS`, `TYPE`, `PASV`, `LIST`, `RETR`, `STOR`, `CWD`,
`CDUP`, `PWD`, `DELE`, `MKD`, `RMD`, `QUIT`, plus anything `quote` is handed.

**IAC is NOT interpreted on the control channel.** RFC 959 says the control
connection is a Telnet connection, and a strict reading means running every
control byte through a telnet engine. In practice no FTP server has
negotiated an option in decades, and the one Telnet mechanism the spec
genuinely uses — `IAC IP` before `ABOR`, to interrupt a transfer in progress —
is not in this client's scope (see the deferrals). A stray 0xFF in a reply
line is passed through as the byte it is. If a server ever negotiates at us,
the engine to answer it is next door in telnet_protocol.c and the seam is a
function call; that day has not come since 1985 and is not planned for.

## The three traps

These are the whole slice, and each one has a corresponding test in the host
harness before the OS runs any of it.

**1. Multiline replies.** A reply is `NNN<space>text` when it is one line, and
`NNN-text` ... `NNN<space>text` when it is several, where the closing line
must repeat the SAME three digits. A client that scans for "three digits then
a space" is fooled the first time a server's banner art contains a line
starting `220 `, and RFC 959 §4.2 knew this well enough to tell servers not
to do it — which is not the same as servers not doing it. The parser tracks
the opening code and accepts only that code as the close.

**2. Connect, then command.** The order for every transfer is: `PASV`, parse
the reply, DIAL the data connection, and only then send `RETR`/`STOR`/`LIST`.
Reversed, it deadlocks against a server that will not proceed until the data
connection exists. This is the dance a browser does for every image on a page
and it is the reason this rung is on the ladder.

**3. Two channels, one truth.** The data connection ending at EOF does not
mean the transfer succeeded; only the control channel's `226` says that. A
server whose disk fills mid-transfer closes the data connection and reports
`451`. A client that trusts EOF alone publishes a truncated file and calls it
done. So: read to EOF, close the data handle, THEN read the control reply,
and let that reply decide whether the `.part` is promoted.

The corollary is that `150` must be read BEFORE the data is read, not after,
because a refusal arrives there instead: `550 No such file` comes back on the
control channel with no data connection activity at all, and a client waiting
on the data handle first waits for a connection nobody is going to feed.

## The 227 reply, and why it is parsed defensively

`227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)` encodes a four-byte address
and a two-byte port as decimal text, because the spec wanted every reply
legible to a human reading a Telnet session. The text around the tuple is not
specified: servers write `(10,0,2,2,195,80)`, or `=10,0,2,2,195,80`, or the
same with commentary either side. The parser scans for six comma-separated
numbers in 0..255 and takes the last such run on the line, ignoring
everything else, and refuses a reply that has no such run rather than dialing
a guess.

**The address the server names is NOT necessarily trusted.** A server behind
its own NAT can advertise a private address that means nothing to us, and a
malicious one can name a third party's address to make our machine dial it.
v1 dials the port the server named at the address WE ARE ALREADY CONNECTED
TO, and says so on the glass when the two differ. That is what curl and every
modern client do, for the same reason.

## Existing interfaces this is built on

Everything needed exists. No kernel change is planned for this slice.

- `os64_dial("tcp!host!port")` — dial.h, the string door, names resolved in
  the library. Two live handles from one task is ordinary: the connection
  table is keyed by the four-tuple and the ephemeral port is drawn per
  connection.
- `os64_read_for(h, buf, len, ms)` — the control channel wants a deadline so a
  silent server is a message rather than a hang. Honoured on dialed net
  handles.
- `os64_write(h, buf, len)` — since PR #65 this queues into the send ring and
  returns what was queued, blocking only when the ring is full. `put` is the
  first program in the tree to care.
- `os64_open` with `"w"`, `os64_rename`, `os64_unlink` — the staging.
- `os64_dial_reason(err)` — the shared refusal vocabulary, so a dial failure
  reads the same here as in os64get and ping.
- `/sys/net/tcp` — the instrument. Two rows during a transfer, and the port
  churn visible across a session, is the evidence this rung was added to
  produce.

## What is deliberately absent

Documented here, per ruling 5. None of these is a DEBTS row.

- **Active mode (`PORT`, `EPRT`).** Needs a listener. Ruling 2.
- **FTPS (`AUTH TLS`).** The trust store and libtls exist as of this week, so
  this is a real future increment rather than an impossibility — but it wants
  the control and data channels wrapped independently, and it is not what this
  rung is testing.
- **`REST` and resume.** A resumed transfer needs the `.part` staging to
  become a first-class resumable object with its own bookkeeping, and the
  question of whether the remote file changed underneath it.
- **`mget` / `mput` and globbing.** Wildcard expansion against a remote
  listing means parsing the listing, which ruling 4 declines.
- **`ABOR` and `IAC IP`.** Interrupting a transfer at the protocol level. What
  v1 does instead is under "Interrupting a transfer" below.
- **`MLSD` / `MLST`.** The structured listing. Ruling 4.
- **`ascii` / `TYPE A`.** Ruling 3.
- **A shell escape (`!`).** husk is one Ctrl+Alt away.

## Interrupting a transfer

Ctrl+C during a `get` of something enormous must not kill the client. A SIGINT
handler raises a flag; the transfer loop sees it, closes the data handle,
leaves the `.part`, and returns to the prompt.

**The hard half is the reply the server still owes.** A transfer that opened
with a `150` is promised exactly one more reply, and walking away does not
cancel that promise. A control channel carrying an unclaimed reply answers
every later command with the one before it, which is the classic way an FTP
client goes quietly insane.

Waiting for it is not an option: **os64 absorbs data arriving on a closed
connection rather than resetting it**, so a server dribbling into an abandoned
data connection runs to the end of the file before it notices — half a minute,
on the fixture that found this. Two mechanisms, and the first alone was
measured to be insufficient:

- **A short drain**, right after the interrupt, so a prompt server's verdict
  is printed where it belongs.
- **A COUNT of what is owed** (`s_owed`), which the drain decrements and the
  next reply read spends before it believes anything it is told. The drain can
  therefore afford to be impatient, and a slow server's `426` lands as its own
  line rather than as the answer to whatever was typed next.

That count is why `FTP_REPLY_STALLED` had to be a different answer from
`FTP_REPLY_FAILED`: a caller has to be able to ask "is it here yet?" without
breaking the thing it is asking about. Sharing one flag between a deadline and
a dead connection is a bug this client shipped for an afternoon.

**Ctrl+C while WAITING FOR A REPLY is a different matter and ends the
session.** The read may already have taken part of the reply, and there is no
resynchronising a stream of unframed text from the middle of one — so the
connection goes rather than every later command being answered by the
wreckage. It is a rare key to press: a reply arrives in a round trip, and the
long waits are all inside transfers, where the count above covers it.

`ABOR` is the protocol's own answer and it is not sent, because sending it
correctly means `IAC IP` + `IAC DM` with the data mark as urgent data, and
os64's TCP has no urgent pointer. Closing the data connection is what every
client falls back to anyway when the server ignores the Synch, which is most
of them.

## Implementation order

1. **The protocol half.** `userland/apps/ftp/wire.{c,h}` — reply lines,
   multiline assembly, the 227 tuple, the 257 quoted path, command
   formatting. No syscall in either file, fed by a source function exactly as
   gopher's wire.h and telnet_protocol.c are.
2. **The host harness.** `tools/test_ftp_host.c` + `.sh` — plain `cc` under
   ASan, driving the parser at every chunk size from one byte up, because a
   stream parser's bugs live where a token straddles two reads. The three
   traps each get a case, and so does every malformed 227 anyone has seen.
3. **The client.** `userland/apps/ftp/ftp.c` — dialing, login, the prompt, the
   transfer dance, the staging, the signal handler.
4. **The deterministic server.** `tools/ftptestd.py`, the way httptestd and
   gophertestd made their protocols testable from the guest: multiline
   banners, a 227 with commentary in it, a `550`, a data connection that
   closes early, a `451` after a good-looking transfer, and a slow dribble.
5. **The acceptance pass.** A public anonymous server over slirp, then the
   P5. `/sys/net/tcp` read during and after, for the churn evidence.

## Open questions

1. **Which public server for the acceptance pass?** It wants anonymous FTP
   with a directory worth looking at. Chris's pick, as telnet's target was.
2. **Does `ls` with no argument want paging?** A big directory scrolls off. The
   honest v1 answer is that it prints and husk has `| less`, but the client is
   the seated program in a gterm where there is no husk to pipe to.
3. **Ruling 4's revisit**, once the LIST output is on the glass.

## Protocol references

- RFC 114 (1971) — the original, over NCP.
- RFC 959 (1985) — the specification implemented here.
- RFC 1123 §4.1 (1989) — the requirements clarifications, including the
  advice on parsing 227 defensively.
- RFC 2428 (1998) — EPSV/EPRT, the IPv6 extensions. Not implemented; noted
  because EPSV is the modern spelling of PASV and the day os64 speaks IPv6 is
  the day it matters.
- RFC 3659 (2007) — MLSD, the listing that should have been in 1985.
- RFC 4217 (2005) — FTPS.
