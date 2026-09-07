# CPU randomness survey

`/tests/rngprobe` reports CPU vendor, brand, raw CPUID signature, hypervisor
presence, and RDRAND/RDSEED support. It executes an instruction only when its
feature bit is advertised. It prints to stdout and the serial log, without
printing generated values. Run it directly or through `/tests/testrun rngprobe`.

For each supported instruction, it requests 256 64-bit samples, allowing up to
1024 attempts per sample. A failed carry flag does not contribute a sample.
The report includes successful samples, total attempts, exhausted sample
budgets, maximum attempts for one sample, adjacent repeats, zero values, and
all-ones values. Constant successful output is flagged as suspicious. These
are bounded operational checks, not entropy estimation or certification.
A feature bit plus varied samples is not evidence of a trustworthy source.

The 1024-attempt cap is a survey budget, not a production retry policy or a
permanent hardware-failure verdict. Intel's [DRNG guide, sections 5.2.1 and
5.3.1](https://www.intel.com/content/www/us/en/developer/articles/guide/intel-digital-random-number-generator-drng-software-implementation-guide.html)
recommends ten retries for RDRAND, but gives no fixed success bound for RDSEED.
Keep the counters when interpreting a completed survey; completion alone does
not establish suitability for a tighter production budget.

Exit status:

| Code | Meaning |
|---|---|
| 0 | At least one instruction advertised; advertised instructions completed sampling without constant output. |
| 1 | An advertised instruction exhausted a retry budget or produced constant successful output. Inspect both reports. |
| 3 | Neither instruction advertised; testrun reports SKIP. |

An instruction that is advertised but faults terminates the probe according
to os64's exception handling. The pre-execution log identifies which one was
being sampled. This is deliberately a userland diagnostic, not a change to
kernel CPU detection or a random-byte service. It does not test virtio-rng.

## Target inventory

| Target | Identification | Evidence still needed |
|---|---|---|
| Bosgame P5 | Ryzen 5 6600H with Radeon Graphics; user-supplied os64 probe screenshot, hypervisor=no | Firmware revision and boot/resume history |
| Desktop | Ryzen 9 3900X; the Linux/WSL environment advertises RDRAND and RDSEED | Direct os64 boot results and firmware revision |
| QEMU | Local QEMU 8.2.2; the repo's default qemu64 model hides both features | CPU instruction runs completed below; virtio-rng service validation remains separate |
| VirtualBox | Installed version 7.2.8r173730 on Windows; user-supplied os64 probe screenshot identifies Ryzen 9 3900X, hypervisor=yes | Actual VM configuration and virtualization backend |

CPU models and hypervisor configuration can change guest-visible features.
Record the exact boot configuration with each report. RDRAND and RDSEED on
one CPU are not independent physical entropy sources. Source approval also
requires the applicable processor/hypervisor documentation and errata review.

For physical machines, retain firmware revision and boot/resume history with
the report. Where suspend/resume is supported, repeat after resume: a cold-boot
sample cannot establish post-resume behavior. Constant-output detection covers
any repeated 64-bit value, including zero and all ones. Linux also performs a
[boot-time RDRAND variation check](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/cpu/rdrand.c);
this survey does not claim to duplicate Linux's thresholds.

## Ownership and follow-up

Quinn owns the userland survey and BearSSL port. Fable owns the entropy pool,
source drivers, and `/dev/random` interface that the production TLS adapter
will read. The CPU survey establishes guest-visible availability and catches
some operational failures; validating pool readiness, source loss, and the
random-byte interface on all four targets is a separate integration step.

QEMU's default-model absence is an absent feature, not an advertised instruction
failure. Adding a `virtio-rng-pci` device does not change that CPUID result;
consuming its bytes requires a guest driver. QEMU documents the device's
[host entropy backends](https://www.qemu.org/docs/master/system/invocation.html).
The exposed-instruction run below uses TCG, not KVM CPU passthrough.

Interrupt, disk-completion, and NIC-arrival timings need observation at the
kernel event sites if assessed as possible pool inputs. This userland probe
does not measure them or assign entropy credit to timing variation. Source
credit and readiness policy belong in the pool design.

## Verification

`bash tools/test_rngprobe_host.sh` checks feature-leaf gating, unsupported
instruction suppression, bounded retries (including success on the final
allowed attempt), rejection of failed-instruction destination values, and
constant-output detection under ASan/UBSan. Add `--live` for the host CPU
survey; its output describes the Linux environment, not a bare-metal os64 run.

For guest verification use isolated disk copies and the headless recipe in
[VERIFICATION.md](VERIFICATION.md). Compare `-cpu qemu64` with
`-cpu qemu64,+rdrand,+rdseed`, retaining q35 and the other boot parameters.
This changes only the RNG feature exposure rather than selecting the broader
`max` CPU model. Run `/tests/testrun rngprobe` and retain serial output.

### Results recorded 2026-09-07

Strict userland and full-image builds passed. Injected host checks passed
with ASan/UBSan; LeakSanitizer was disabled because it cannot run under this
environment's ptrace setup. The checks include mixed success/exhaustion and
constant all-ones output with a successful carry flag.

| Environment | RDRAND | RDSEED | Result |
|---|---|---|---|
| Linux/WSL, Ryzen 9 3900X, hypervisor present | 256 successes in 256 attempts | 256 successes in 256 attempts | COMPLETE |
| os64, QEMU 8.2.2 TCG, `-cpu qemu64` | Absent; not executed | Absent; not executed | testrun: 0 passed, 0 failed, 1 skipped |
| os64, QEMU 8.2.2 TCG, `-cpu qemu64,+rdrand,+rdseed` | 256 successes in 256 attempts | 256 successes in 256 attempts | testrun: 1 passed, 0 failed, 0 skipped |
| os64, Bosgame P5, Ryzen 5 6600H, hypervisor absent | 256 successes in 256 attempts | 256 successes in 1423 attempts; maximum 8 attempts per sample | COMPLETE |
| os64, VirtualBox, Ryzen 9 3900X, hypervisor present | 256 successes in 256 attempts | 256 successes in 256 attempts | COMPLETE |

The completed samples had no adjacent repeats, zeros, or all-ones values.
Both guest runs reached boot complete using q35, 8 vCPUs, 8 GiB RAM, and
isolated copies of the ext2 root and home disks.

Chris supplied the physical P5 and VirtualBox results as screenshots, read
from `/mnt/c/temp/rngprobe_P5.jfif` and `/mnt/c/temp/rngprobe_vbox.PNG`.
The P5 reports signature `0x00a40f41`; VirtualBox reports `0x00870f10`.
Both advertise both instructions and report zero exhausted sample budgets.
Maximum attempts per sample is 1 for both VirtualBox instructions and P5
RDRAND. P5 RDSEED incurred 1167 unsuccessful attempts across 256 completed
samples; its maximum of 8 attempts includes the successful attempt. This
demonstrates retry handling on that run, not a guaranteed future retry bound.

These results do not cover bare-metal 3900X, suspend/resume, or virtio-rng.
The VirtualBox screenshot establishes guest-visible behavior, not native
3900X behavior or the specific host virtualization backend.

Chris agreed to defer the bare-metal 3900X run until a convenient reboot.
RDRAND/RDSEED availability on that target remains a working assumption, not
native os64 validation. Pool development and the BearSSL port can proceed;
production source selection still requires runtime checks and failure handling.
