# Telnet client — design

Status: rulings issued 2026-09-04 (section below); the option support they
ratified is what the protocol engine implements. The kernel dependency
ruling 4 named has landed — the TCP send window merged as PR #65 — so the
writer-thread proposal is struck rather than adapted, and the session loop
is the single loop described under "Session execution".

Steps 1, 2 and 3 of the implementation order are built:
`userland/apps/telnet/telnet_protocol.{c,h}` is the engine,
`tools/test_telnet_host.sh` is its fixture, and
`userland/apps/telnet/telnet.c` is the session and command UI — one loop, as
ruling 4 settled. Step 4 is the acceptance pass against a destination Chris
picks; step 5 is the evidence.

**Ctrl+] WAS UNTYPEABLE, and that was a kernel bug this client found.** The
keyboard drivers translated Ctrl only for LETTERS, so the escape character
every telnet has used since 4.2BSD arrived as a literal `]` and the
`telnet>` prompt could not be reached from a keyboard at all. Both dialects
had their own letters-only copy of the rule. They now share one —
`keyboard_has_control_code` in keyboard.h — and it covers the column 1963
ASCII actually laid out (CLAUDE.md § Keyboard has the argument).

## Rulings (Chris + Fable, 2026-09-04)

Read before the sections below: these answer the doc's open questions and
name the one kernel dependency. Astra's draft stands as written; where a
ruling contradicts a paragraph, the ruling wins.

1. **Option scope: ECHO, SUPPRESS-GO-AHEAD and NAWS are IN.** BROWSER.md's
   refuse-all sketch was wrong for the target this client exists for: a
   refuse-all client prints your password on the glass at a Unix login and
   collects GO-AHEAD bytes from a half-duplex server. Those three are the
   floor, not a widening. TERMINAL-TYPE stays refused in v1; the honest name
   to offer later is `ANSI` — colour, absolute and relative cursor movement,
   the saved cursor, the two erases and a CP437 high half, which is very
   nearly what the word meant to a BBS in 1992 — as a one-line follow-on the
   day a real target asks.

2. **Lifecycle: disconnect returns to the `telnet>` prompt; `quit` ends the
   program.** Astra's recommendation, adopted — it is what BSD's client has
   done since 1983, and it keeps the last screen readable after a peer
   drops.

3. **The first real destination is Chris's pick** (question 1 below). A BBS
   with ANSI art works the escape parser hardest; a Unix login works the
   ECHO negotiation. Both are worth a run before this is called done.

4. **THE SESSION LOOP WAITED ON THE TCP SEND WINDOW, WHICH HAS LANDED.**
   The writer thread, the bounded queue, atomic publication,
   quiesce-before-close and the missing cancellation primitive were all a
   correct reading of a `tcp_conn_write` that did not return until the peer
   had acknowledged the last segment: a keystroke cost one round trip during
   which the client was deaf, and a dead link wedged it for the whole
   retransmit ladder. PR #65 paid that kernel debt. `tcp_conn_write` now
   queues into the connection's send ring and returns the count QUEUED,
   blocking only when the ring is full — a whole window behind. So 4.2BSD's
   shape is the right one, ONE loop with no second thread, and the writer
   thread is not to be built.

5. **Two gaps the draft missed.**
   - **Ctrl+D never reaches the remote.** The console turns it into
     end-of-input before a program sees it, so a Unix login cannot be
     logged out of by the key that does it everywhere else. v1 ships a
     local `send eof` at the prompt (Ctrl+] then `send eof`), which needs
     nothing from the kernel. The right answer — a raw tty mode where a
     program asks the console to stop interpreting Ctrl+C and Ctrl+D — is a
     small kernel slice of Fable's that ssh will want too; booked, not
     blocking.
     **NOTE FROM FABLE, 2026-09-08 — RAW MODE IS BUILT** (SIGINT.md § Raw
     mode is the design record). The shape, which this client is built
     against: it is ONE
     bit per terminal — in raw mode BOTH Ctrl+C and Ctrl+D arrive as the
     bytes 0x03 and 0x04 (Chris deferred the half-mode question to me and
     I ruled both-or-neither: those two interpretations are the console's
     whole discipline, and ssh needs the full thing anyway). A foreground
     program asks with `os64_tty_set_raw(true)` (libos64, procfs.h), which
     writes `raw` to `/proc/self/tty`; `/proc/self/tty` reports `mode` and
     `raw_task`. The KERNEL restores cooked when the holder exits, so
     telnet never has to. Works on a VT and inside a gterm's pty alike.
     Consequence for the bullet below: in raw mode telnet READS 0x03 and
     sends `IAC IP` itself — one path, bytes in and bytes out, the
     4.2BSD character-mode shape — and the SIGINT handler is not needed
     (keep it only for a telnet that chooses to stay cooked). `send eof`
     can stay as a convenience; Ctrl+D simply goes down the wire as 0x04.
     Fixture: `/tests/rawtty`.
   - **SIGINT is simple.** Catch it, send `IAC IP` (244), done. No Synch, no
     urgent data: the remote's own interrupt character does the rest, and
     nearly every telnetd ignores the Synch anyway.

6. **Half-close is not needed for v1.** The draft is right that the
   connection interface exposes close and not a write-side shutdown; telnet
   does not need one (end-of-input goes to the remote as bytes, ruling 5).
   It stays a TCP-side gap, listed on BROWSER.md's rung 5 as something the
   protocol exercises, and it is not this client's to fill.

## The application

`/bin/telnet [HOST [PORT]]`, default port 23. A console application usable
on a VT, inside an existing shell, or directly as gterm's seated program:

```
/bin/gterm /bin/telnet example.org 23
```

gterm already passes the program and its arguments to `os64_spawn_seated`;
no intermediary husk is needed. A desktop menu entry can use the same command.
With no host it opens a small `telnet>` command prompt, which is what makes
a generic Telnet menu entry useful without hardcoding a destination.

Commands: `open HOST [PORT]`, `close`, `quit`, `help`, `status`, and
`echo auto|on|off`. Ctrl+] enters the local prompt; `continue` resumes
the connection. An explicit `send escape` sends the reserved character.
Remote output continues to be drained while the prompt is active; prompt
redisplay must not lose edited input. Bound command lines and queued data.

Lifecycle (ruling 2): disconnect and connection failure return to the prompt;
`quit` ends the application (and therefore its gterm window). This keeps
errors and the last screen visible and permits reconnecting. Closing the
window ends the session via the existing PTY hangup path.

## Protocol scope

Useful interactive Telnet rather than the roadmap's refuse-all baseline
(ruling 1): accept server ECHO, negotiate SUPPRESS-GO-AHEAD in both
directions, and offer NAWS for terminal dimensions. Refuse other options.
Do not advertise a VT100/xterm terminal type that the local renderer cannot
honor. Binary mode, environment exchange, authentication, encryption, and
Telnet server support are outside this slice. Telnet traffic is plaintext.

**A REAL DESTINATION HAS SINCE ASKED FOR BINARY MODE**, and the acceptance
pass measured why: a DOS door on the target board takes a
bare CR for Return and ignores both spellings this client can send. Booked in
DEBTS § Networking rather than built, on Chris's call — the row carries the
measurements and the two shapes a fix could take.

The parser lives in `userland/apps/telnet/telnet_protocol.{c,h}`, independent
of syscalls so a host harness can exercise it. It maintains state across
arbitrary read boundaries: ordinary data, IAC, negotiation, and bounded
subnegotiation handling. Decode doubled IAC; escape outbound IAC; handle
NVT CR LF and CR NUL with state across reads. Unknown commands must not
leak onto the screen. Unsupported subnegotiations are discarded without
allocating from peer-provided lengths. EOF inside a control sequence is
reported as truncated protocol input. Option state must prevent repeated
acknowledgments and negotiation loops.

Default NVT operation uses local line editing/echo. Negotiated remote echo
and suppress-go-ahead enable character interaction; local echo then stops,
letting the server control password visibility. Manual echo override is
available for unusual peers. Enter sends NVT newline; fallback behavior
when a peer refuses options must be explicit and covered by fixture tests.

Read initial dimensions through `os64_tty_read`; SIGWINCH only marks them
dirty. Send NAWS from ordinary code after negotiation and on actual changes,
with IAC quoting in its payload. Refusal leaves remote size reporting off.

## Existing interfaces and limits

- `gterm.c` already supports direct seating and closes when the session ends.
- `os64_tty_handle` supplies timed character reads without automatic echo.
  Ctrl+C becomes SIGINT; Ctrl+D becomes EOF rather than an ordinary byte.
- gterm forwards key events with nonzero ASCII. The keyboard drivers already
  encode arrows, Home/End, Insert/Delete, and Page Up/Down as escape-byte
  events, which gterm passes through to its child. Function-key coverage
  needs a separate audit before being promised.
- The terminal reads SGR, absolute cursor positioning (CUP/HVP), the
  relative moves (CUU/CUD/CUF/CUB) with the saved-cursor pair (SCP/RCP),
  erase display, erase line, OSC 11 background colour, and the character-set
  selections `ESC ( U` / `ESC ( B`. It does NOT implement scrolling regions,
  insert/delete line, the alternate screen, or DEC private modes; a peer
  that needs one of those needs a slice of its own, and the rule stays what
  it has always been — an escape is implemented when something asks for it.
- TCP reads take deadlines and writes queue into the send ring and return,
  so a single loop that writes directly is the right shape. A write blocks
  only when the ring is full, which is a peer a whole window behind — not
  the per-keystroke round trip the earlier design had to work around.
- The public connection interface exposes close, not a separate write-side
  shutdown. Peer EOF can be drained and closed; independent local half-close
  cannot be promised by this client design.

These are current-tree observations, not permissions to change the kernel.
Do not hide missing terminal capabilities inside a Telnet-only emulator.

## Session execution

ONE LOOP, no second thread — 4.2BSD's shape, and what the send window makes
correct. Each pass does a short timed read of the terminal and a short timed
read of the connection, and does a bounded amount of work with whatever each
returned. Neither read can starve the other, because neither blocks longer
than its deadline.

The engine holds all the protocol state and never touches a syscall, so the
loop is the only thing that reads and writes. Bytes the engine wants sent —
a negotiation reply, a NAWS payload, the escaped form of what was typed —
are taken from `telnet_pending` and written; `telnet_sent` retires what the
write accepted, so a short write is a fact the engine already knows how to
survive. That pending buffer is the back-pressure: it is bounded, and the
engine stops consuming input rather than growing it, which is the property
the fixture drives directly.

Signal handlers set a flag and nothing else. SIGINT sends `IAC IP` and does
not kill the client (ruling 5); SIGHUP and SIGTERM ask the session to end;
SIGWINCH marks the dimensions dirty and NAWS is sent from ordinary code.

## Acceptance and implementation order

1. DONE — the rulings above settle target use, options and lifecycle; the
   terminal prerequisites landed as the relative moves, the saved cursor and
   the CP437 character set.
2. DONE — `userland/apps/telnet/telnet_protocol.{c,h}` and
   `tools/test_telnet_host.sh`: every split point, IAC quoting, NVT
   conversion, repeated and crossed negotiations, refused options, malformed
   subnegotiations, EOF, back-pressure, a differential comparison against
   Python's `telnetlib`, and live loopback scenarios judged from both ends.
3. DONE — `userland/apps/telnet/telnet.c`: one loop, the `telnet>` prompt,
   unsolicited output, echo transitions, failed connections, reconnect,
   queue pressure and stalled writes.
4. DONE — exercised on a VT and as `gterm /bin/telnet x-bit.org 23230`, the
   target Chris picked: SPITFIRE BBS 3.7, whose full-colour opening art
   drew as CP437 in a window. What the launch proves beyond typing is that
   no husk stands between the window and the program — `quit` ends telnet
   and the window goes with it, through the ordinary hangup path.

   **RAW MODE IS WHAT THIS PASS WAS WAITING FOR**, and the two keys behave
   the way 4.2BSD's character mode always did. Against a byte-naming peer:
   Ctrl+C leaves as `IAC IP`, Ctrl+D as the byte `0x04`, an ordinary key as
   itself — neither is a signal and neither is an end of input any more.
   Against the board, both travel and the session carries on (a DOS-era
   BBS ignores an interrupt, which is its right). Ctrl+] still reaches the
   `telnet>` prompt from inside a raw session, which is the whole reason
   the escape character needed a kernel fix first.

   **WHAT THE PASS FOUND, AND IT WAS THE RESIZE.** `os64_read_for` on the
   connection answers `OS64_INTERRUPTED` when a caught signal ends the park,
   and this client read every negative that was not a timeout as a broken
   connection — so the FIRST window resize of a session hung up on it. One
   Ctrl+Alt+M in a gterm, reproducibly. os64 has no `SA_RESTART` on purpose
   (DIVERGENCES § Signals): the caller decides, and deciding "the peer is
   gone" is the wrong decision. `wait_came_back_empty` is the rule written
   down once — a timeout and an interruption both mean "nothing happened,
   ask again" — and the read, the write and the keyboard all ask it.

   **THE DIAL IS NOT INTERRUPTIBLE**, and that is the kernel's gap rather
   than this client's: `tcp_connect` polls and naps to its own deadline
   without asking `signal_park_must_end`, so a Ctrl+C at
   `Trying host...` is not seen until the handshake ends. Booked in DEBTS
   § Networking, together with the reason it cannot simply be fixed — the
   dial vocabulary's -4 is `OS64_NET_ERR_BAD_ADDRESS` and the signal
   sentinel is also -4.
5. Run appropriate host tests, strict build, diff/comment checks, and capture
   serial/fixture evidence. Chris tests before commits per BROWSER.md.

## Questions for review

Questions 2 and 3 of the original draft are answered by rulings 1 and 2.
What is still open:

1. **ANSWERED — x-bit.org:23230, Chris's pick**, a SPITFIRE BBS 3.7 behind a
   Bot-Gate frontdoor. It worked the escape parser exactly as ruling 3
   predicted: full-colour CP437 art, absolute and relative cursor moves,
   the saved cursor for the height probe, and `ESC[2J` clearing and
   homing — the last of which is why the art lands at the top of the glass
   instead of in its bottom third. A Unix login was worked by
   `telnettestd.py --scenario unix` rather than by a real host: ECHO
   changing hands at a password prompt is the thing that matters and the
   fixture takes it away on demand, which no public board will do twice.
   The original question, kept because the reasoning stands: a BBS with
   ANSI art
   works the escape parser hardest; a Unix login works the ECHO negotiation.
2. **ANSWERED — the explicit one.** `charset cp437|latin1` at the prompt and
   `-8` on the command line, which is the ranking below in the order it was
   written. Keying off the port was rejected for the reason given there: 23
   is a Unix login as often as it is a board. The set is handed back when the
   program exits and NOT when a session ends — a board drops you often, and
   re-typing `charset cp437` before every reconnect would be its own small
   misery; the prompt is ASCII, which both sets agree about.

   **THE PEN IS HANDED BACK ON THE OPPOSITE SCHEDULE — at every disconnect,
   and at the prompt** (Chris, 2026-09-09, after running this on the P5:
   "depending on what the screen looks like when exiting a login session,
   you can end up with interesting color choices"). A board signs off in
   whatever colours its last screen used, and a set and a colour go wrong
   differently: a character set is a way of READING bytes nobody is sending
   any more, so keeping it costs nothing between sessions, while a colour is
   on the glass in front of you and outlives the program. `ESC[0m` before
   the closing notice, and again whenever the `telnet>` prompt draws itself
   — a line the prompt owns, it owns the colours of.

   **AND `ESC[0m` RATHER THAN A SNAPSHOT**, which is what was asked for.
   Reading the terminal's pen at startup would need a new procfs line and a
   new libos64 call (`os64_tty_info_t` is ABI and cannot grow a field), and
   it would buy nothing today: nothing in os64 sets a terminal colour and
   leaves it set, so the pen at startup IS the default in every case a
   person can currently produce. The stronger reason is where the knowledge
   belongs — `ESC[0m` asks for THE TERMINAL'S OWN colours, so the day os64
   lets somebody choose those, every program that resets is already asking
   for the right thing, and every program that snapshotted would be
   restoring a copy. A snapshot is only better than the default when a
   program cannot ask, and here it can.

   The paper — `ESC]11`, the terminal's own background — is NOT handed back,
   because there is no spelling to hand it back with: os64 reads OSC 11 and
   has no "reset" form of it, and telnet does not know the colour it would
   name. Nothing on a 1992 board sends OSC 11; the day something does, the
   fix is a reset spelling in the terminal, not a remembered colour here.

   The original question, kept because it is the argument:

   **WHO SAYS CP437?** A board sends the art and never announces the set —
   `ESC ( U` is os64's own spelling, not something a 1992 BBS has heard of,
   and the terminal's default is Latin-1 because a gopher menu needs it.
   So something has to declare it on the board's behalf. Three candidates,
   in the order I would rank them: a `charset cp437|latin1` command at the
   `telnet>` prompt plus a `-8` style argument (explicit, no guessing, and
   it can be changed mid-session when a peer turns out to be UTF-8); a
   default keyed on the port (23 is a board more often than not — cheap,
   and wrong for a Unix login that sends Latin-1); or the client never
   deciding and the user running `ansiprobe`-style selection by hand
   (honest, and nobody will do it). The client must also hand the set back
   on exit, or every later command on that VT is drawn in CP437.

## Protocol references

- [RFC 854: Telnet](https://www.rfc-editor.org/rfc/rfc854)
- [RFC 857: Echo](https://www.rfc-editor.org/rfc/rfc857)
- [RFC 858: Suppress Go Ahead](https://www.rfc-editor.org/rfc/rfc858)
- [RFC 1073: Window Size](https://www.rfc-editor.org/rfc/rfc1073)
