# PR #65: fresh-eyes review

Reviewed 2026-09-05 at `dad6a2766440241946c6ddde44aef15b4a998ea4`, against
merge base `474e68089fdcf37f6cc813b5bef9bc09467da123` on `userland`.
At review time the PR was open and all 22 review threads were resolved.
The findings below describe that snapshot; the approved refactor is documented
in `TCP_SENDER.md` and the follow-through section at the end.

**Recommendation: keep the PR open, but stop repairing it one review comment
at a time.** The send ring is a sound foundation. The unresolved design problem
is the interaction between the send cursor, outstanding data, local submission
failures, and timer ownership. There are also independent congestion-control
errors. Deleting the local-refusal machinery alone will not finish the work.

I read the supplied `project_tcp_send_window.md`, the full PR diff and commit
history, all inline threads and discussion, the surrounding TCP implementation,
IPv4/ARP submission and NIC queue paths, syscall consumers, diagnostics, and
verification changes. The findings below concern executable behavior; prose-only
issues are not filed as findings.

## Findings at the reviewed head

All seven are P2. The first two should be fixed before merging because they
affect connection lifetime and queued data. The others are congestion-control
and recovery defects, with concrete triggers rather than general conformance
objections. Source locations refer to the reviewed head.

### 1. A persistent local refusal can retain a detached connection indefinitely

Locations: `tcp.c:462`, `tcp.c:1578`, `tcp.c:1678`, `tcp.c:1715`.

Queue data with an open peer window, have the lower layer refuse transmission,
then close the handle. `tcp_output` leaves `snd_nxt == snd_una`; the poll's
ordinary timeout branch requires `tcp_unacked`, and persist requires a zero
window. The sweep retries every tick without spending a budget or reaching a
terminal state. The receive buffer, send buffer, and port remain owned.

The host reproducer leaves the connection in FIN_WAIT_1 after 100 simulated
seconds: detached, unstripped, 1,000 bytes queued, zero retries, 10,001 refused
attempts. Continued refusal leaves the same state indefinitely. This also
affects a refused bare FIN.

There is a second entry: send 10,000 bytes, lose the response, then refuse the
RTO retransmission. The timeout rewinds `snd_nxt` to `snd_una`; refusal leaves it
there. The reproducer still has 10,000 bytes previously sent and unacknowledged,
but `tcp_unacked` is false and the retry count remains one after another 100
seconds. A transient full queue recovers; sustained refusal exposes the missing
lifetime bound. IPv4's refusal class includes both queue pressure and errors
such as an over-MTU packet, so eventual success is not a valid invariant.

### 2. Detached persist can discard queued data after 60 ms of local refusal

Locations: `tcp.c:612`, `tcp.c:548`, `tcp.c:1693`.

The round-9 fix spends `retries` before attempting a probe. The round-8 fix
reschedules a refused probe for the next tick. Together, they spend the
connection's entire close budget on local attempts at 10 ms intervals.

Reproduction: close with queued data and a zero peer window, then refuse the
probe transmissions. From the first persist deadline, six attempted probes and
six ticks later the seventh budget check closes and strips the connection.
No probe was accepted by the lower layer. This is 60 ms after the first attempt,
not the intended roughly half-minute grace period. Data waiting for an orderly
close is discarded because of brief local pressure.

An elapsed-time policy for detached connections avoids both findings 1 and 2.
Its deadline must be independent of network retransmission counts and the
frequency of local attempts.

### 3. A sub-MSS partial ACK adds a full MSS during NewReno recovery

Location: `tcp.c:1172`.

The partial-ACK branch subtracts acknowledged bytes and unconditionally adds
`snd_mss`. RFC 6582 adds that credit only when the partial ACK acknowledges at
least one SMSS. The reproducer enters recovery with MSS 1,000 and cwnd 8,000;
an ACK advancing one byte raises cwnd to 8,999 instead of reducing it to 7,999.
Repeated small advancing ACKs can inflate recovery substantially. The ordinary
growth helper's ring-size cap does not apply in this branch.

This is especially relevant to a sender explicitly supporting small writes;
advancing ACKs need not acknowledge a full segment. Fix the credit condition
and specify how recovery growth is bounded. [RFC 6582 §3.2](https://www.rfc-editor.org/rfc/rfc6582.html#section-3.2).

### 4. Limited-transmit bytes incorrectly increase the loss threshold

Locations: `tcp.c:415`, `tcp.c:631`, `tcp.c:1250`.

With 10,000 bytes initially outstanding, the first two duplicate ACKs send
2,000 extra bytes through limited transmit. At the third duplicate,
`tcp_congestion_loss` halves the resulting 12,000-byte span and sets ssthresh
to 6,000. It should exclude those extra bytes and use 5,000. The current
connection state records no limited-transmit byte credit to subtract.

This makes the congestion response too permissive precisely when the feature
is recovering a loss. [RFC 5681 §3.2, step 2](https://www.rfc-editor.org/rfc/rfc5681.html#section-3.2).

### 5. Repeated RTOs reduce ssthresh again for the same unacknowledged head

Locations: `tcp.c:631`, `tcp.c:1648`.

The first timeout of a 10,000-byte flight sets ssthresh to 5,000 and resends
one MSS. If that retransmission also gets no ACK, the next timeout recomputes
the threshold from the rewound cursor's 1,000-byte span and lowers it to the
2,000-byte floor. The reproducer confirms 5,000 → 2,000 without any ACK
progress. The threshold should remain unchanged on subsequent RTOs of that
same segment. Recovery after the outage becomes unnecessarily slow.

This also exposes why a retransmission cursor is not a sufficient definition
of outstanding flight. [RFC 5681 §3.1](https://www.rfc-editor.org/rfc/rfc5681.html#section-3.1).

### 6. An old recovery mark suppresses fast retransmit after 2 GiB of progress

Locations: `tcp.c:1224`, `tcp.c:1915`.

`recover` starts at ISS and changes only on fast retransmit or RTO. After
more than 2^31 bytes of clean progress, the signed modular comparison reads
the stale mark as ahead of `snd_una`. Three duplicate ACKs then reset the
counter instead of initiating fast retransmit. A loss that would normally be
recovered by duplicate ACKs waits for an RTO.

The reproducer uses a reachable state after that amount of progress; it does
not transmit 2 GiB in the test. An ordinary control produces one fast
retransmit; the otherwise identical advanced sequence state produces none.
This can happen without crossing numeric zero: the problem is the distance
from a stale reference point. Retire or advance inactive recovery marks while
they are still in the valid comparison range, with an explicit recovery-state
rule. Ordinary sequence-wrap tests alone do not catch this.

### 7. An idle connection retains its previous full congestion window

Locations: `tcp.c:404`, `tcp.c:321`.

There is no last-data-transmission/idle-restart decision. Once cwnd reaches
the ring limit, it remains available after arbitrary idle periods. The
reproducer waits 60 simulated seconds and then emits 65,535 bytes in one
output call, versus the 10,000-byte initial window in that fixture.

The ACK clock and evidence of path capacity have expired. Add an idle-restart
rule; `snd_sent_at` is an RTT stopwatch and does not record each data send.
This is a SHOULD requirement with a concrete burst consequence, rather than
an interoperability failure. [RFC 5681 §4.1](https://www.rfc-editor.org/rfc/rfc5681.html#section-4.1).

## Why the reviews keep producing repairs

The code uses `snd_nxt - snd_una` for both a position in the retransmission
pass and outstanding flight. Those agree before the first rewind. Afterwards,
`snd_max` preserves what was submitted, but timers and congestion calculations
still ask the cursor. Findings 1 and 5 are two consequences of that mismatch.
Mechanically replacing `snd_nxt` with `snd_max` is not a complete fix either:
`snd_max` also includes a submitted persist probe whose answer may leave
`snd_una` unchanged. Probe acknowledgment eligibility and retransmission-timer
ownership need distinct rules.

Timer roles are similarly implicit. A one-unit span and a zero window identify
a probe; an RTO deadline also schedules local SYN attempts and unanswered
probes; the retry count measures network failures and detached persistence.
Each new exception must be understood at every caller. Finding 2 is a direct
composition of two individually targeted review fixes.

The reviewer contributed to this loop. [Round 4's refusal finding](https://github.com/VBWizard/os64/pull/65#discussion_r3939319493)
identified a real performance concern but prescribed local retry as though
it were required for reliable TCP. Treating a lower-layer drop as packet loss
is a viable design when retransmission state and timers retain responsibility
for it. Once local retry was promised, later reviewers correctly found holes
in that promise. The diagnosis deserved attention; the prescribed mechanism
needed a design decision before adoption.

Fable's proposed simplification is directionally useful, with two corrections:

- Queue depth does not establish that refusals are rare. A 64 KiB flight needs
  about 45 packets at MSS 1,460, 123 at MSS 536, or 1,366 at the accepted minimum
  MSS 48. e1000's 64-slot ring has 63 usable slots; multiple connections also
  share it. virtio does not reclaim TX completions in its transmit function.
- The cited BSD implementation does more than ignore failures. It commits
  sequence/timer state before `ip_output`; ENOBUFS invokes `tcp_quench`, and
  certain established-connection errors are retained as soft errors. It also
  represents persist and retransmission timers explicitly. Adopt a coherent
  policy, not a historical shorthand. [4.4BSD-Lite2 tcp_output.c](https://raw.githubusercontent.com/sergev/4.4BSD-Lite2/master/usr/src/sys/netinet/tcp_output.c).

## Proposed exit from the loop

1. **Settle the submission contract.** My preference for this slice is to
   treat transient queue refusal as local packet loss, stop the output pass,
   retain retransmission responsibility, and let a defined timer recover it.
   Keep the ARP stop-pass behavior. Classify permanent construction errors
   separately so an impossible packet does not retry forever. If immediate
   local retry is retained, give it one explicit pending-operation mechanism
   and bounded lifetime rather than separate SYN/data/FIN/probe exceptions.
2. **Write the sender invariants before changing it.** Distinguish the oldest
   unacknowledged sequence, highest submitted sequence, retransmission cursor,
   queued end, and FIN sequence. Rewinding a cursor cannot erase outstanding
   responsibility. State which timer owns idle, retransmission, persist, and
   local retry, and give detached no-progress cleanup its own elapsed-time
   bound. This does not require replacing the ring or implementing SACK.
3. **Complete the congestion event table together.** Cover first and repeated
   RTO, limited transmit, recovery entry, sub-MSS/full-MSS partial ACK,
   recovery exit, old recovery marks, and restart after idle. Specify the
   state/byte credit each transition needs; fix the five related findings as
   one coherent change.
4. **Make transition coverage durable.** Use the host harness as a starting
   point, convert observations to expected-correct assertions, and add the
   cross-product of SYN/data/FIN/probe with sent/ARP-pending/refused outcomes,
   ACK progress/nonprogress, and owned/detached lifetime. Include refusal
   lasting longer than an RTO, and recovery after refusal stops. Then run the
   QEMU loss/reorder/stall rigs and actual NIC queue-pressure cases. Review
   that final implementation once the matrix passes.

I would not book repeated lifetime bugs as debt merely to end the review
count. Nor would I restart from scratch: preserve the ring, ACK retirement,
queued-close semantics, and the useful integration work.

## Coordination with Fable's PHY work

On 2026-09-05 Chris supplied Fable's five-step PHY plan. The existing
`.worktrees/r8125-phy` worktree is on `fable/r8125-phy`, based on `b469b0e`.
At inspection its edits were in `r8125.c`, `test_r8125_host.c`, and new
`r8125_phy.c/.h` files. The plan also includes seam diagnostics, e1000 STATUS
reporting, and runtime link-change reporting. This is work in progress, not
an independently reviewed PHY implementation.

The PHY plan does not change the TCP architecture proposed here:

- Fable owns PHY access, advertisement/autonegotiation, boot/link reporting,
  LINKCHG handling, and the r8125 host tests. The TCP refactor does not need
  to edit those files or change the NIC transmit callback contract.
- The existing PR and the planned PHY work share `sysfs.c` and `e1000.c`.
  TCP connection diagnostics and NIC link diagnostics are separate sections;
  PR #65's e1000 edit concerns TX ring capacity, while Fable's concerns STATUS
  reporting. Preserve both when integrating. Any `net_device_t` additions
  require recompiling consumers against the combined header.
- A temporary link outage is not a permanent packet-construction error.
  Classify a known impossible packet at the IPv4 validation site; do not
  infer permanence from a generic driver negative return (r8125 also uses
  `-1` for a full queue).
- Link-up notification is not TCP acknowledgment progress. It must not free
  queued bytes, refill the no-progress budget, or justify a larger congestion
  window. The proposed sender remains independent of PHY speed/status and
  recovers through its protocol timers and actual ACKs.
- Add unplug/replug acceptance coverage for both owned and detached
  connections, including reconnection before the cleanup deadline and an
  outage that outlasts it. Each branch can be validated independently first;
  test the combined tree on the P5 before attributing throughput changes to
  either the PHY or the sender.

## Evidence and limits

The historical observations from `dad6a27` are retained in
`tools/review_pr65/observed-dad6a27.txt`. The maintained acceptance suite is
now `tools/test_tcp_host.sh`; it compiles current TCP bodies and checksummed
input against deterministic platform/time/submission stubs. Run it with
`ASAN_OPTIONS=detect_leaks=0 tools/test_tcp_host.sh` in this environment.
The results below describe the original review snapshot, not the refactor.

ASan and UBSan completed without diagnostics. LeakSanitizer was disabled
because this environment reported that it cannot run under ptrace; leak
findings above come from connection state/lifetime, not an LSan result.
Positive controls passed for ring and sequence wrap, data-plus-FIN close and
TIME_WAIT stripping, cumulative ACK past a rewound cursor, normal fast
retransmit, and answered persist followed by window reopening.

All five changed kernel C translation units were freshly compiled into
scratch objects with the repository's cross-compiler and strict `-Werror`
flags. The normal kernel build was already up to date. PR-range
`git diff --check` and `tools/stale_refs.sh` passed. No new QEMU or physical
NIC run was performed; the PR's throughput measurements remain historical
evidence, not results independently reproduced in this review.

The syscall copy/partial-write path, writer registration under the connection
lock, ACK ring retirement, FIN ACK handling, sysfs snapshot locking, and IPv4
disposition return did not yield additional introduced correctness findings in
this pass. That is a review result, not a proof of concurrency correctness.
Inherited receive-segment validation and detached FIN_WAIT_2 cleanup remain
outside the specific new findings; an orphan-lifetime design should consider
the latter too.

The revised sink verdict usefully measures receipt of the announced bytes,
but it does not test close-with-data-outstanding: netsend waits for the verdict
before closing. Its overrun check catches extra bytes in the final `recv`;
bytes arriving in a later read are not examined. Keep that timing instrument,
and add a separate EOF/close acceptance test. The PR adds no durable sender
state-transition tests; the existing kernel TCP test principally exercises
connection refusal. This explains why green bulk transfers did not prevent
the review regressions.

## Follow-through

The approved refactor addresses the seven findings above. `TCP_SENDER.md`
defines the implemented state and timer invariants; `VERIFICATION.md` records
new acceptance evidence separately from historical measurements. Detached
FIN_WAIT_2 is now included in the 30-second no-ACK-progress cleanup bound.
The maintained host suite and separate `netclose` EOF test replace the
observation-only harness. Fable's PHY branch remains separate.
