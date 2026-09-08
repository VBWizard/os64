# Where random numbers come from

*A lesson, not a design record. RANDOM.md says what os64 built and why each
decision went the way it did; this page is for the person who wants to
understand the problem before reading the answer. Nothing here is specific
to os64 until the last sections.*

## A computer cannot roll dice

Everything a processor does is determined by what came before. Given the
same memory and the same instructions, it produces the same result every
time, which is the whole point of a computer and the whole problem with
asking one for a random number. A program that "generates random numbers"
is really running a formula, and a formula's output is only as
unpredictable as its starting value. That starting value is called the
SEED, and the seed has to come from somewhere outside the formula: from the
physical world, which is the only place genuine unpredictability lives.

The word for that unpredictability is ENTROPY, borrowed from physics. When
someone says a source "has 256 bits of entropy" they mean an attacker who
knows everything else would still face two-to-the-256 equally likely
possibilities. That is the target, because 256 bits is more than any
attacker can search before the sun burns out, and the keys that protect a
TLS connection are that size.

## Two places the physical world leaks in

**A noise circuit on the chip.** Since 2012 Intel processors, and since
2015 AMD's, have carried a small circuit that amplifies thermal noise in a
pair of transistors and samples it. Thermal noise is quantum-mechanical at
bottom and nobody can predict it. Two instructions read it: `RDRAND` gives
you the output of a small generator the chip seeds from that noise, fast
and plentiful; `RDSEED` gives you the conditioned noise itself, slower,
and it is allowed to say "not ready yet" by clearing the carry flag, which
is why every reader retries it. The P5's chip needed up to eight tries per
word on the day we measured it (RNG_PROBE.md). Those instructions are the
first source, and on every machine this house owns they are healthy.

They can also lie. In 2019 some Ryzen 3000 boards came back from suspend
with `RDRAND` returning the same value forever, all ones, with the carry
flag set as if all were well. A source that answers with a constant is
worse than no source, because it looks like one. So the kernel checks:
eight draws at boot, and if they all agree the instruction is marked
failed and not trusted, which is the same check Linux performs.

**Timing.** The other leak is older and works on any hardware: things
happen at moments the computer did not choose. A network frame arrives
when the far machine and every switch between decided; a timer interrupt
lands at a moment set by a crystal oscillator whose phase drifts with
temperature. If you read the CPU's cycle counter at the instant an
interrupt arrives, the lowest few bits of that count are effectively a
dice roll. One roll is nearly worthless. Thousands, stirred together, are
a seed. os64 reads the counter on every tick and every network interrupt,
for the whole time the machine runs, and folds the result into the pool.
On a machine with the noise circuit this is belt and braces; on one
without, it is the belt.

There is a third trick for the first second of boot, before enough
interrupts have arrived: time the same short loop of memory accesses over
and over, and keep only the timings that are not a pattern. The work is
identical every time, so any difference between one run and the next is
the machine's own noise — cache misses, bus contention, and on a virtual
machine the host's own scheduling — which no one can predict. A timing
that repeats the last one, or drifts from it at a steady rate, is thrown
away: that is what a clock with no noise in it looks like. Linux does this
too. It seeds the pool in milliseconds on the one QEMU model that hides
the instructions.

## Why the raw bits are not handed out

You might expect `/dev/random` to just pass along whatever the noise
circuit said. Nobody does that, for three reasons.

First, raw sources are uneven. Timing bits are biased, hardware bits can
be stuck, and a program reading them directly would inherit every flaw.
Feeding everything through a HASH function fixes that: a hash takes any
amount of input and produces a fixed-size output in which every bit
depends on every input bit, so a few good bits anywhere in the input make
the whole output unpredictable, and a bad source cannot steer it. os64
uses BLAKE2s, published in 2012 and used by Linux's pool since 2022.

Second, a machine can want megabytes of random bytes and the noise
circuit is not that fast. So the hash output is not served directly
either; it becomes the KEY of a stream cipher, and the stream is what is
served. ChaCha20, published in 2008 and the cipher inside most of the TLS
connections your browser makes today, turns a 256-bit key into an
endless sequence of bytes that no one can distinguish from coin flips
without the key. The key is the secret; the stream is the product.

Third, and this is the subtle one: what if someone reads the key out of
memory later, through a bug or a debugger? They could then recompute every
byte the pool had ever served, which might include the TLS key you made
this morning. The defence is called FAST KEY ERASURE, from Daniel
Bernstein: every time the pool generates, it runs the stream, keeps the
first 32 bytes as the NEXT key, and hands out the rest. The key that made
this morning's bytes no longer exists anywhere. A key stolen at noon can
only compute what comes after noon, and only until the next reseed folds
in fresh noise it does not know.

That is the whole machine: sources, a hash that folds them into a key, a
cipher that stretches the key into a stream, and erasure so the past is
safe. Linux arrived at the same shape in its 5.17 rewrite after twenty
years of more complicated designs; os64 borrowed the shape and the two
algorithms, and built nothing new, because inventing cryptography is how
you lose.

## What "seeded" means

The pool starts empty and becomes SEEDED when it has taken in enough
unpredictability to be trusted: 256 bits from a trusted hardware
instruction, or 256 timings from the boot jitter loop that were not a
pattern, or 1024 folded interrupt timings that were not a pattern either. `/sys/random` says which door it came through
and how many of each it has seen. On every machine we own the answer is
`rdseed`, decided microseconds into boot. Before the pool is seeded, a
read of `/dev/random` is refused rather than served, on the principle that
handing out numbers you cannot vouch for is worse than saying no.

## `/dev/random`, and why `cat` runs forever

`/dev/random` is a door onto the pool. A read returns exactly the number
of bytes you asked for, never fewer; there is no end of file because the
stream has no end, which is why `cat /dev/random` prints until you stop
it, exactly as `cat /dev/zero` does. A write puts your bytes into the pool
without claiming any credit for them, so a program that knows something
the kernel cannot (a saved seed, a probe's samples) can contribute.

There is no `/dev/urandom`. On older Unix systems there were two files
because the kernel kept a running estimate of how much entropy it had and
`/dev/random` would BLOCK when the estimate ran low, while `/dev/urandom`
would not. The estimate was never very meaningful, the blocking caused
decades of hung programs and bad workarounds, and in 2020 Linux quietly
made both files the same generator. One name is the honest number of
things there are.

## Who uses it, and what goes wrong without it

**TLS.** Every encrypted connection begins with both sides picking random
values; if a client's are predictable, so is the session key. In 1995 two
Berkeley students, Ian Goldberg and David Wagner, broke Netscape's SSL in
minutes because its seed was the time of day and the process id. In 2008
a Debian maintainer removed two lines from OpenSSL to silence a warning
and, without realising it, removed the seeding: for two years every key
generated on a Debian system was one of 32,768 possibilities. This is the
consumer os64 built the pool for; BearSSL reads its seed from the door.

**TCP.** Every connection starts at an initial sequence number, and a
peer who can guess yours can forge packets into your connection without
seeing it. Robert Morris described the attack in 1985; Kevin Mitnick used
it in 1994. os64's sequence numbers now come from the pool, and so does
the starting point of its port numbers, which are the other half of what
a forger has to guess.

**DNS.** A resolver matches answers to questions by a 16-bit id. In 2008
Dan Kaminsky showed that a forger who can guess the id can feed a resolver
false answers faster than the real server replies, and poison every
lookup after. os64's resolver draws its id from the pool.

## Reading the eyes

`cat /sys/random` shows the pool's account of itself: what the CPU
advertises and whether each instruction passed the boot check; what
seeded the pool; how many hardware words, retries and give-ups; how many
timing events from the tick, the network cards and the drainer; how many
times the fast pools were folded in and the key reseeded; bytes served,
contributions taken, and reads refused. If a TLS handshake ever stalls,
that file is the first thing to read. A healthy machine shows `seeded:
yes`, `seeded_by: rdseed`, the tick counter climbing at a hundred a
second, and `reads_refused: 0`.

## Further reading

- RFC 4086, *Randomness Requirements for Security*, the plain-language
  standard on what a seed needs to be.
- RFC 7693 (BLAKE2) and RFC 8439 (ChaCha20), the two algorithms, with the
  test vectors os64's host harness checks against.
- Intel's *Digital Random Number Generator Software Implementation Guide*,
  on RDRAND and RDSEED and why the carry flag matters.
- Daniel Bernstein, *Fast-key-erasure random-number generators* (2017), a
  short blog post that is the clearest statement of the erasure idea.
- The Linux kernel's `drivers/char/random.c` after 5.17, the design os64's
  pool is modelled on, and Jason Donenfeld's writeups of that rewrite.
