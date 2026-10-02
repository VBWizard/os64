# J2 guest measurements

`make -C userland -j8 js-measure-test` builds
`userland/obj/js/measure/consumer`. Put it at `/home/jsmeasure` on a disposable
home image and boot the checkout's normal root image. It uses the reserved
`jsembedtest` address in its own process. It remains outside the normal image
inventory and uses the already delivered libjs/libmath/libos64 libraries.

After boot and late kernel tests settle, run:

```text
/home/jsmeasure > /home/j2-measure.txt
echo $? > /home/j2-measure-status.txt
sync
```

Wait for the consumer to finish before capturing its status. Expect status 0
and `J2: 105 checks, 0 failures`. Repeat with one virtual CPU and with multiple
CPUs. A single CPU ensures the two workers compete for the same floating-point
hardware; multiple CPUs exercise the ordinary SMP scheduler configuration.

The consumer reads `/proc/self/maps` and finds the usable native stack containing
its current stack pointer. A public native callback samples RSP and recursion
depth for simple frames, eight live locals, accessor recursion and a bounded
64 KiB native frame, at 128/256/768 KiB engine budgets. Each run must end as an
engine exception, keep at least 128 KiB **sampled** physical headroom and allow
runtime reuse. CSV rows report the mapped-stack use at sampled callbacks,
not the lowest RSP over every instruction or a universal per-frame cost.

Representative workloads check primes through 10,000, 10,000-record JSON,
an 8 MiB typed array, a 10,000-step BigInt Fibonacci calculation, 100,000
Promise jobs and a comment-heavy source at the 4 MiB input ceiling. They use
`os64_js_default_limits()`, apart from the recursion budget sweep. Rows report
elapsed monotonic microseconds and engine allocation charges sampled through
the public `JS_ComputeMemoryUsage` API. Samples do not establish a global heap
peak, include wrapper/host input memory or measure process RSS. Source generation
is fixture-owned host memory outside the engine budget. These workloads justify
a configurable standalone starting profile; they do not characterize all scripts
or P5 performance.

Two threads create and own separate runtimes. Native callbacks place distinct
sentinels in all sixteen XMM registers and two occupied x87 registers, with
different x87/MXCSR rounding controls. The assembly helper keeps them live
across existing yield/sleep syscalls, compares the occupied x87 values, control/
status fields and XMM values, and restores the caller's full saved state before
returning. This avoids a C caller-saved-register assumption and keeps the script's
normal rounding mode. Empty x87 registers and reserved FXSAVE bytes are not
compared. An intentional disturbance of XMM, x87 and MXCSR proves all three
comparison classes reject changed state. No syscall or kernel implementation
is added. AVX/XSAVE state is outside this FXSAVE/SSE2 profile.

Script arithmetic, signed zero/NaN, maths, Promise processing, formatting and
JSON must remain correct across the callbacks. Atomic peer counts demonstrate
overlap; `/proc` switch counts confirm actual scheduler activity. Each worker
must complete 129 pauses without a register/control mismatch and join cleanly.
Precise captured results and limitations belong to `userland/libjs/VALIDATION.md`.
