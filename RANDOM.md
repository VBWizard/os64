# RANDOM.md — the entropy pool, and /dev/random as its door

*Design record, Fable, 2026-09-07, written BEFORE code per the known-debt
rule. Chris marks this up; the code follows the marked-up version. The
lesson behind it — why a computer cannot roll dice, what the sources are,
why the raw bits are never served — is docs/random_explained.md. The
consumer that demanded it is BearSSL (Quinn's port, BROWSER.md boss #1),
which needs a seed it cannot make for itself; the customers that were
waiting are the TCP initial sequence number, the ephemeral port draw, the
DHCP transaction id and the resolver's query id, each of which draws from
the tick counter today and says so in its own comment.*

## What the probe said (RNG_PROBE.md, Quinn, 2026-09-07)

| Target | RDRAND | RDSEED |
|---|---|---|
| P5, Ryzen 5 6600H | healthy | healthy, up to 8 attempts per sample |
| VirtualBox on the 3900X | healthy | healthy |
| QEMU TCG, `-cpu qemu64` (the harness default) | absent | absent |
| QEMU TCG, `-cpu qemu64,+rdrand,+rdseed` | healthy | healthy |
| Bare-metal 3900X | not yet run; WSL advertises both | |

So every machine this OS actually runs on has a hardware seed, the P5's
needs a retry loop, and the harness hides it unless asked. That decides
the shape: the hardware instruction seeds the pool at boot and reseeds it
after; timing noise is the second source, stirred in always and relied on
only when the hardware is absent; and a virtio-rng driver is a debt, not a
dependency, because no real target needs one.

## The design in one paragraph

Linux's post-5.17 construction, which is the smallest sound one: a
256-bit pool KEY, a BLAKE2s hash to fold new entropy into it, and a
ChaCha20 stream to generate from it with FAST KEY ERASURE — every
generate call runs the stream, keeps the first 32 bytes as the next key,
and hands the rest out, so a captured state cannot reproduce what was
already served. Reseeding folds fresh hardware output and the timing
pools into the key through BLAKE2s. Both primitives have RFC test vectors
(7693 and 8439) and a host harness diffs them against Python's `hashlib`
and the RFC tables before the kernel ever runs them. No invented
cryptography: the mixing function and the generator are the two named
algorithms, used the way their RFCs describe, and nothing else.

## The pieces

**`kernel/src/random.c`, `random.h`.** The pool, its lock, the sources,
the kernel API and the generator.

- `random_init()` runs in `kernel_init` before anything dials or leases:
  it reads CPUID for RDSEED and RDRAND, runs the VARIATION CHECK Linux
  runs (eight draws, each with the retry budget — a slow RDSEED is not a
  lying one; all equal means the instruction lies, the Zen 2
  post-resume bug), draws 256 bits from RDSEED (retrying, ten attempts per
  word, RDRAND if RDSEED is absent or exhausted), and folds them in. With
  no trusted instruction it runs the JITTER LOOP: the SAME short memory
  walk, timed by the cycle counter round after round, keeping a delta only
  when it passes the Jitter RNG's stuck test (the delta, the delta of
  deltas and the delta of those all non-zero — a constant delta and one
  drifting at a steady rate are what a noiseless clock produces, and the
  fixed workload is what makes any other shape the machine's own noise),
  until 256 are in or a bounded number of walks is spent — bounded by
  iterations rather than ticks, because it runs under the pool's lock with
  interrupts off. On a hypervisor that seeds in milliseconds. Only if that
  fails too does the pool start UNSEEDED, and the boot line says so;
  interrupt timing then finishes the job at 1024 folded events that passed
  the same stuck test.
- `random_bytes(buf, n)` is the kernel's verb. It never fails and never
  blocks: kernel callers (the ISN, the port draw, DHCP) take what the pool
  has, seeded or not, because a boot must not hang on entropy, and an
  unseeded pool still beats the tick counter they draw from today. It
  serves in 1KB chunks and takes the pool's lock PER CHUNK — the lock is
  irqsave (the ISN is drawn under TCP's), so a megabyte read must not hold
  a core's interrupts off for the whole megabyte — and checks the reseed
  cadence at every chunk, so a request cannot carry itself past the byte
  threshold on one key.
  `random_seeded()` answers whether a hardware source has contributed 256
  bits or the timing source has crossed its threshold.
- `random_add_timing(tag)` is the interrupt-side verb: lock-free, a few
  instructions, called from the interrupt top halves (the NIC ISRs before
  they ring the doorbell, the tick) and from knet's drain loop. It
  xor-rotates the TSC and the tag into a PER-CORE fast pool (one cache
  line each, no lock, the same reason the doorbell's top half takes none),
  and runs the jitter loop's stuck test on the arrival: a sample whose
  delta, delta of deltas, or delta of those is zero is mixed in but not
  counted toward seeding (`timing_rejected` in `/sys/random`).
  The pool folds the fast pools in under its lock at every reseed and
  every 64 events, whichever first — knet's wake is the usual folder, and
  a draw or a `/dev/random` read that finds the pool unseeded folds them
  too, so a machine with no NIC (and so no knet) still seeds from the
  tick.
- Reseed: after 300 seconds or 1 MB served, whichever first, from RDSEED
  plus the fast pools. A reseed that finds RDSEED exhausted takes the fast
  pools alone and counts the miss.

**`/dev/random`.** One node, in devfs beside null and zero. A read
returns exactly the bytes asked for, never short, once the pool is
seeded; before that it is REFUSED and counted (`reads_refused` in
`/sys/random`). The design first said "park the reader", and the code
said no: a filesystem read runs on the core's interrupt stack through
`call_in_kernel_context` and may not sleep there (syscall.c, the handle
alias; the stack poisoner caught exactly that on 2026-08-13). Parking
would need a waiter in the syscall layer, a mechanism for a case the
boot's own length already covers — the tick alone seeds the pool long
before userland starts, on any machine — so a refusal is a tripwire for a
source that failed, not a state a program meets. There is no `/dev/urandom`:
the split is a fossil of the days when the pool's estimator gated the
"real" device, Linux made them the same generator in 5.6, and a second
name would be one more thing to explain. A write to `/dev/random` mixes
the bytes in with no credit, so a program that knows something the
kernel does not (a saved seed, a probe's samples) can contribute.

**`/sys/random`.** The eyes: `seeded:` and by what, which instructions
CPUID advertises and which passed the variation check, hardware draws
and retries and exhaustions, timing events by source, reseeds, bytes
served, readers parked. Read it FIRST when a TLS handshake stalls.

**The customers, in this slice.** `tcp_initial_seq` becomes RFC 6528's
shape, a random 32-bit base plus the per-dial serial; the ephemeral port
sequence starts at a random offset at boot; `dhcp` draws its transaction
id; `resolve.c` reads two bytes from `/dev/random` for its query id (a
predictable id is the Kaminsky cache-poisoning surface, and the resolver
is ring 3, so the door is the file). Each of their "no entropy yet"
comments comes out in the same commit.

**The harness.** `tools/test_random_host.c` compiles `random.c` with
stubs the way `test_tcp_host` does: BLAKE2s against `hashlib.blake2s`
over a table of lengths and keys, ChaCha20 against RFC 8439's vectors,
then the pool's behaviour — fast key erasure (the state after a call
cannot regenerate the call's output), reseed cadence, the unseeded park,
the variation check refusing a stuck instruction, the retry budget. QEMU
gets `-cpu qemu64,+rdrand,+rdseed` in the harness flags so the default
boot has the source every real target has; a second boot on the bare
model proves the timing-only path seeds and serves.

## Deliberately not in this slice (each a DEBTS row on landing)

- **virtio-rng.** Every real target has RDSEED; the bare QEMU model is
  the only machine without one, and the timing source covers it. The
  device is a small driver the day a VM without CPU randomness matters.
- **Entropy ESTIMATION.** No per-event credit arithmetic. "Seeded" is a
  threshold: 256 hardware bits, or 4096 timing events on a machine with
  no hardware source, and `/sys/random` names which. Linux's estimator
  was a decade of argument that ended with roughly this.
- **A getrandom syscall.** The file is the door (the `/proc/self/tty`
  doctrine). If a consumer proves the open costs it something, a syscall
  can arrive then.
- **Reseeding from a saved seed across boots.** The P5 has RDSEED; a
  saved seed is for machines that do not.
- **Disk-completion timing.** The NIC and the tick are the sources this
  slice wires; the NVMe completion path joins when it has an interrupt to
  hang the call on.

## Rulings (Chris, 2026-09-07)

1. `/dev/random` alone. A port that needs the `urandom` name is the
   consumer that earns it.
2. Block before seeded rather than refuse — ruled, and then overtaken by
   the code: the read path may not sleep (above), and the jitter loop
   makes the wait a matter of milliseconds at boot on the one model that
   has no instruction, so the refusal is what shipped, with the reason
   written where the read is.
3. The tick is a timing source alongside the NICs, for the whole
   session: a seed is a moment, reseeding is what protects the future,
   and the cost is ~20 cycles a hundred times a second per core.

## Proof

`tools/test_random_host.sh`: BLAKE2s against hashlib (1,656 cases, three
feeds), ChaCha20 against the `cryptography` package and RFC 8439's block
vector, then the pool with its hardware replaced by hooks — seeding by
RDSEED, by RDRAND behind a stuck RDSEED, by the jitter loop, by interrupt
timing; the jitter loop refusing a frozen clock and a perfectly linear
one; a draw folding the fast pools when nothing else has (the no-NIC
boot); the variation check; the retry budget; fast key erasure and the
exact construction; the reseed cadence by bytes and by ticks. In the OS:
`random_pool` in the pre-boot suite, `/tests/randtest` in the ring-3
suite, and two headless boots (VERIFICATION.md), one with the instructions
exposed and one on the bare model.
