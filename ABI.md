# os64 Syscall & Userland ABI

*The design record for the kernel↔userland contract: the register
convention, the dispatch machinery, ring-3 task lifecycle, and the agreed
(partly unbuilt) userland structure. This ABI is **ours, not Linux's** —
numbers and semantics are free to diverge; Unix ideas are adopted when they
are genuinely good (handles 1/2 mirroring stdout/stderr, `spawn` over a
fork/exec split), never for compliance's sake.*

## The register contract

```
RAX = syscall number                     (numbers: abi-to-be/syscall_numbers.h,
RDI, RSI, RDX, R10, R8, R9 = args 0-5     today kernel/include/syscall_numbers.h)
RAX = result
```

- **R10 stands in for RCX** as arg 3: the CPU burns RCX for the return RIP
  on `SYSCALL` (and R11 for RFLAGS).
- **Clobbered for the caller:** RCX, R11 (hardware), plus all C
  caller-saved registers — exactly the set a plain C call clobbers, so a C
  syscall stub needs no special saving. Callee-saved (RBX, RBP, R12-R15)
  are preserved.
- Interrupts are masked at entry by SFMASK (mandatory — see
  "Interruptibility" below). TODAY they stay masked for the whole syscall;
  that is a bring-up simplification, not the design — the committed design
  is interruptible syscall bodies, required before `read` lands.
- `syscall_numbers.h` must stay **preprocessor-only** (bare #defines): it
  is shared between C and assembly (the exit trampoline template).
- **Error convention (decided 2026-07-08; code change lands with the
  libos64 scaffolding):** syscalls return a VALUE:STATUS pair in
  **RAX:RDX** — SysV already returns 16-byte structs that way, so
  handlers are plain C: `return (sysret_t){value, 0};`. RDX == 0 means
  success; error codes get an enum in `abi/include/`. The full 64-bit
  value space stays clean (no reserved ranges), and the error check sits
  at the call site — no errno-style action at a distance. syscall.S needs
  no change (RDX already survives the exit path untouched); the ABI
  change is that RDX becomes defined output instead of clobber garbage.
  UNTIL that lands, the code uses the interim sentinels `0xFFFF...FF`
  (INVALID) / `0xFFFF...FE` (BAD_USER_DATA) — don't add syscalls that can
  legally return values in that range before the pair convention exists.

## Entry and exit (syscall.S) — the invariants

1. **Never touch the user stack in the entry path.** A hostile ring-3
   program can point RSP at garbage and execute `syscall`; a #PF before
   the kernel has a good stack double-faults. The user RSP is stashed
   GS-relative in CLS — safe precisely because IF is masked, so nothing
   else runs on this core between stash and reload.
2. The kernel stack comes from `CLS.kernel_rsp0`, kept in sync with the
   running thread by `tss_set_rsp0()` at every context switch.
3. `sysretq` reloads CS/SS from STAR[63:48] and RFLAGS from R11 — the
   user's IF comes back on. (History: an earlier `retfq`-based return left
   ring 3 running with interrupts off. If you ever see an unpreemptible
   user task, someone resurrected that path.)
4. The per-core MSR block — STAR (selectors), LSTAR (`syscall_Enter`),
   SFMASK, **EFER.SCE** (without which `syscall` is #UD) — is programmed
   per core in `ap_initialization_handler`. The GDT layout SYSRET demands
   (user code = user data + 8, kernel data = kernel code + 8) is enforced
   by `_Static_assert`s there: a GDT reshuffle fails the build, not the
   first sysret.

### Interruptibility (committed design — the bodies open up before `read`)

Why entry masking is non-negotiable: a gate-based syscall (the old 32-bit
OS) got a known-good kernel stack from the TSS *atomically in hardware*,
so a trap gate could leave IF on and be interruptible from instruction
one. `SYSCALL` is faster precisely because it skips that: it changes
CS/SS and nothing else — the kernel arrives at CPL 0 **still on the
user's stack**, which ring 3 controls. An interrupt in that window pushes
a ring-0 frame onto hostile memory: double fault at best. SFMASK clearing
IF over the entry (and the symmetric exit window: between reloading the
user RSP and `sysretq`) is how every SYSCALL-based kernel closes that
hole.

Masking the whole syscall BODY, though, is just the bring-up shortcut.
Its costs arrive with real syscalls: a blocking `read` under IF=0
deadlocks (it sleeps waiting for an interrupt that cannot be delivered),
long syscalls add input latency, and — already true today — a long
syscall on the BSP collapses pending IRQ0 ticks into one delivery, so
`kTicksSinceStart` drifts. The design, standard for SYSCALL kernels:
**mask across entry, `sti` once kernel context is established, `cli`
before the exit unwind.**

Migration list (short, because `yield` already proves the hard part —
the scheduler context-switches mid-syscall today, on the syscall kernel
stack, via the pre-CR3-switch frame reads):

1. `sti` in `syscall_Enter` immediately after the user-context frame is
   built (the CLS user-RSP stash is consumed into the frame by then — the
   stash itself must stay inside the masked window); `cli` before the
   sysret unwind.
2. Move `g_saved_cr3`/`g_saved_cr3_valid` from per-CPU arrays to
   per-THREAD state — a thread preempted mid-syscall can resume on a
   different core, and per-CPU is then the wrong home (the CLS-scratch
   rule's cousin).
3. Audit handlers for "nothing can preempt me" assumptions. The user-CR3
   copy window is the known one; it becomes preemption-safe once its
   state is per-thread (the scheduler already saves/restores CR3 per
   thread).

Until this lands, treat every syscall handler as running with interrupts
off: no waiting on interrupt-delivered events, keep bodies short.

## The dispatcher (syscall.c) — the rules that bit us

- **`needs_cr3_switch` is false for every entry, on purpose.** Every task
  PML4's upper-half entries point at the KERNEL'S OWN page tables — shared,
  not copied — so under the user's CR3, ring 0 sees the entire kernel
  (kRenderer, the scheduler queues, the whole HHDM...) PLUS this task's
  lower half. Ring 3 is fenced off the kernel half by the U/S bit (kernel
  mappings lack PAGE_USER), not by absence. A handler therefore needs
  nothing the user CR3 can't see — switching to kKernelPML4 gains nothing
  (identical kernel view) and LOSES the user's buffers. Flipping
  to kKernelPML4 under a C handler is actively fatal: the thread's syscall
  kernel stack is a task-local VA that kKernelPML4 does NOT map, so the
  next C statement faults, the #PF handler pushes onto the same unmapped
  stack, and the machine triple-faults (write() proved this the first time
  the flag was exercised). A syscall that truly needs kernel context must
  switch STACK and CR3 together — `call_in_kernel_context`
  (task_exit_asm.S) is the proven pattern.
- **`user_ptr_arg_mask`: only mark args that ARE user pointers.** Unused
  argument registers carry ring-3 garbage — and right after a previous
  syscall, that "garbage" is kernel addresses our own clobber convention
  left behind. A blanket check-all-six rejects valid calls: write() was
  the first casualty (R10 held a leftover kernel pointer from yield(), so
  a perfectly valid write bounced with BAD_USER_DATA before the handler
  ever ran). The mask is correctness, not laziness.
- **User data crosses the boundary through the copy helpers only** —
  `copy_user_string` (NUL-bounded, byte-walked) and `copy_user_buffer`
  (length-exact). Both validate the range against kHHDMOffset with
  overflow-safe arithmetic and ferry through bounded kernel chunks
  (write() uses 512 bytes) so a user-supplied length can never become
  unbounded kernel stack use. The helpers run inside a "user-CR3 window"
  that re-opens the user address space if a (future) CR3-switched handler
  is mid-flight — safe today because the whole syscall runs masked; under
  the interruptible-bodies design (below) its safety moves to per-thread
  CR3 state instead.
- **The boundary tripwire:** the dispatcher records CR3 at entry and
  panics if a syscall tries to leave on a different one. Escaping to
  ring 3 with the kernel CR3 "works" (kernel maps are a superset) right up
  until it corrupts something unrelated — the panic names the culprit
  syscall instead.

## Current syscall inventory

| # | Name | Notes |
|---|---|---|
| 0 | `yield` | Genuine APIC self-IPI into the scheduler — same nesting/EOI semantics as the timer path. Returns immediately if nothing else is runnable |
| 1 | `debug_log(msg)` | Copies a user string, prints `[user] ...` |
| 2 | `exit(code)` | Stores task->retVal, `task_exit()`, never returns |
| 3 | `write(handle, buf, len)` | Dispatches on the handle's tag: console, pipe write end (whole-or-block ≤ capacity; EPIPE ⇒ SIGPIPE terminate), or file (chunked, runs under kKernelPML4). Returns bytes written; partial progress reported over error |
| 4 | `read(handle, buf, len)` | Dispatches on the tag: console keyboard (blocks), pipe read end (blocks; 0 = EOF = no writers left), or file (short near end, 0 AT end — same contract, zero file-awareness needed). Returns bytes read |
| 5 | `spawn(path, argv, in, out, err)` | Launch child, return pid (non-blocking). in/out/err = caller's handles to install as child's 0/1/2, −1 = console. NO blanket handle inheritance, by design |
| 6 | `wait(pid, *code)` | Reap a child: pid>0 = that child, 0 = first of any. Returns ended pid, writes exit code via out-param. Returns immediately if already dead |
| 7 | `pipe(int[2])` | Create pipe; [0]=read end, [1]=write end. Creator holds BOTH ends and must close its copies after handing them to children |
| 8 | `close(handle)` | Drop the handle. For pipe ends this is signalling (EOF/EPIPE refcounts); for files it closes (and may flush) the VFS file. Double-close is an error |
| 9 | `open(path, mode)` | Open a file on the root fs, return a handle (≥ 3). mode: "r"/"w"/"a"/"c", NULL ⇒ "r", validated at the boundary. The handle plugs into read/write/seek/close AND spawn redirection (`prog < file` for free) |
| 10 | `seek(handle, offset, whence)` | Files only. whence = OS64_SEEK_SET/CUR/END (abi header). Returns the NEW absolute position (yes, better than lseek). SEEK_END+0 = file size |
| 16-22 | GUI (reserved) | See GRAPHICS.md "The userland boundary" — create/destroy/get_surface/publish/event_poll/screen_info/event_wait |

File syscalls (open/seek, and read/write/close on file handles) run their VFS
work under kKernelPML4 via `call_in_kernel_context` — the ELF-load-in-spawn
lesson generalized: disk I/O bottoms out in DMA structures only the kernel
tables map. Params + bounce buffers ride in kmalloc'd (HHDM) blocks, never on
the syscall's task-local stack. The one asymmetry: closing a file from the
task-exit path is ALREADY in kernel context, and re-entering
call_in_kernel_context from the kernel interrupt stack would reset RSP onto
the live frames — handle_file_object_close checks CR3 and calls straight
through in that case.

## Ring-3 task lifecycle

**Startup handoff (register-based, and already built):** os64 does NOT use a
SysV-style initial stack (argc/argv/envp/auxv pushed as a block). Instead
`task_create()` latches the new thread's entry registers directly —
`RDI=argc`, `RSI=argv` (the argv blob is built and mapped at
`TASK_ARGV_VIRT`=0x6f000000 with task-space pointers), `RDX=envp`
(`TASK_ENV_VIRT`) — which is exactly the SysV *calling* convention for
`main(argc, argv, envp)`. So the `launch` stub does nothing but pass those
registers through to `main`; the initial user stack carries ONLY the return
address to the exit trampoline (below). When no args are given, the kernel
synthesizes `argc=1, argv[0]=path`. (Historical footgun: an early `launch`
zeroed RDI/RSI/RDX and discarded the whole handoff — a stub must PRESERVE
them.)

**Birth:** `task_create()` loads the ELF (demand-paged via VMAs), builds
the user stack, and seeds its initial return address with
`TASK_EXIT_TRAMPOLINE_VIRT` — a user-mapped, **read-only + executable**
page holding a few bytes copied from the kernel's
`user_exit_trampoline_template`. A `_start` that plainly `ret`s lands
there at CPL 3 and executes: `mov rdi, rax` (exit code = return value),
`mov eax, SYSCALL_EXIT`, `syscall`. Design constraints that keep this
working: the template stays position-independent (it runs at a different
VA than it was assembled at — no absolute references, RIP-relative jumps
only) and the page is mapped without PAGE_WRITE (a program cannot scribble
on its own exit path). Kernel threads use the simpler ring-0 trick — their
stacks are seeded with `task_exit_with_retval` directly, since ring 0 may
return into kernel text; ring 3 cannot, which is the whole reason the
trampoline exists.

**Death:** `syscall_exit` → `task_exit()`, which switches to the per-core
kernel interrupt stack and kKernelPML4 in one asm block and `call`s a
noinline continuation — the never-touch-a-C-local-across-the-switch recipe
(CLAUDE.md, context-switching chapter) — marks task and thread exited,
and yields away for good. The thread lands in qZombie; reaping happens via
`task_reap_eligible_zombies` / parent wait. Anything that gives tasks
kernel-visible resources (GUI windows!) must hook its teardown into this
path BEFORE the address space is torn down — see GRAPHICS.md's "windows
die before pages."

## Adding a syscall (recipe)

1. Number in `syscall_numbers.h` (keep the header preprocessor-only).
2. Handler in syscall.c. TODAY: static, uniform 6-arg signature, `(void)`
   the unused args. DECIDED (2026-07-08; lands together with the RAX:RDX
   refresh): natural typed signatures — `sysret_t sys_write(uint64_t
   handle, const void *buf, size_t len)` — cast into the uniform table
   type by SYSCALL_DEFINE, Linux-style. Safe by construction on our one
   target ABI: the dispatcher always loads six argument registers, and a
   three-parameter callee reading three is exactly what SysV promises.
   User data in via the copy helpers only.
3. `SYSCALL_DEFINE(NUM, "name", fn, false, PTRMASK)` — the `false` is the
   CR3 flag; read its DANGER comment before ever passing true. PTRMASK
   marks exactly the pointer args, nothing more (see above for why).
4. A ring-3 test fixture exercising it in the post-boot phase (the
   fixtures are the ABI's regression suite), and VERIFICATION.md's greps
   stay green.
5. A `libos64` wrapper once userland scaffolding exists.

## The userland plan (agreed 2026-07-05, unbuilt — the shell-first roadmap)

- **Layout (in this repo):** `abi/include/` — the kernel↔userland contract
  headers (syscall numbers, handoff spec; deliberately OUTSIDE kernel
  source, the old OS proved referencing kernel headers from userland is
  ugly); `userland/{libos64, start, apps/<app>}`; one makefile above all.
- **Names:** the startup stub is **`launch`** (`userland/start/launch.S`)
  — not crt0; the library is **libos64**, headers `<os64/...>`.
- **The fence rule:** the kernel may ship user-runnable code only if its
  entire vocabulary is kernel ABI (the exit trampoline qualifies: three
  instructions, all ABI). Anything speaking *library* vocabulary gets
  linked into the binary from userland.
- **Process model:** BOTH patterns are first-class (decided 2026-07-08):
  `spawn(path, argv)` + `wait` as the everyday app-launch path, AND a
  fully supported fork/exec split — fork/exec earns its keep for the
  cases spawn can't express (set up the child's world — handles, pipes,
  environment — in the child itself, then exec). The unfinished fork
  register-load path (SCHEDULER.md, `justForked`) is where that work
  starts.
- **Order:** scaffolding (abi/ + launch + libos64 + hello) →
  **interruptible syscall bodies** (the migration list in
  "Interruptibility" above — a HARD GATE: the next step deadlocks without
  it) → console `read` (line-discipline design first: keyboard events →
  char stream, where echo lives) → spawn + wait → **the shell** (~200
  lines; review the old 32-bit kshell first — Chris liked how it worked)
  → per-task handle table + open/close/seek, with **pipes as the first
  handle type** (Unix small-utilities-composed-by-pipes is the stance;
  `ls` is an app, not a builtin).
- PIE-by-default with loader-assigned per-binary bias is planned (debug
  ergonomics: unique addresses per binary).

## Deferred debts (all commented at their sites)

**Interruptible syscall bodies** (the "Interruptibility" section — hard
gate before `read`, also queued in the roadmap order above); FPU/SIMD
state in the context switch (until then user builds carry `-mno-sse`); a
#DF IST emergency stack (turn stack-death triple faults into panics);
drop IOPL=3 for ring 3 once fixtures stop using `out`; the page-0/VA-0
NULL guard (MEMORY.md).

Resolved-in-design, code pending (land with the libos64 scaffolding):
the RAX:RDX error convention and natural typed handler signatures (both
above).

### Memory-protection hardening (discussed 2026-07-08)

The shared kernel upper half (every task PML4 points at the kernel's own
tables, U/S-protected) is a DELIBERATE, sound decision for os64's threat
model — we run our own binaries; there is no untrusted multi-tenant code.
It is NOT a debt. What Meltdown/KPTI unshared it for (a speculative
side-channel) is out of scope. But the audit that discussion triggered
found three real items:

1. **`mp_schedStack` is mapped USER** (`scheduler.c` scheduler_init,
   flag `0x7`) — a per-core kernel scheduler stack, at its HHDM VA, is
   ring-3 read/writable because PAGE_USER is set. Almost certainly a
   paste from the user-stack mapping path. Should be `0x3`
   (PRESENT|WRITE, no USER). An actual hole (ring 3 scribbling live
   kernel scheduling state), hobby-scale only because we run trusted
   code. **Cheapest real fix in the kernel — one digit.**
2. **SMEP** (CR4 bit 20) is not enabled. Without it a corrupted kernel
   function pointer aimed at a user-half page executes attacker code at
   CPL 0; with it that faults. The kernel never legitimately executes
   user pages (the exit trampoline is user-MAPPED but runs at CPL 3), so
   this is nearly free. Enable per-core in the same MSR block as
   EFER.SCE (ap_initialization_handler), **gated on
   `CPUID.(07H,0):EBX` bit 7** — detected, not assumed (AMD Zen+ and
   Intel Ivy Bridge+ have it; a feature-less VM must degrade, not #GP).
3. **SMAP** (CR4 bit 21, `CPUID.(07H,0):EBX` bit 20) — the hardware
   backstop against stray kernel↔user access. Higher cost: every
   deliberate user access must bracket with `stac`/`clac` or the kernel
   faults itself. The copy helpers (`copy_user_string`/`_buffer`) are
   already the hand-rolled chokepoint this would formalize, so it's a
   discipline-wide change, not a missing protection. Deferred with intent;
   do it when the copy helpers are the ONLY user-access path (they nearly
   are — argv setup in task.c is the other).

## Failure fingerprints (symptom → cause)

- **#UD on a task's first `syscall`:** EFER.SCE not set on that core —
  the per-core MSR block didn't run there.
- **#GP on the first sysret:** GDT layout no longer satisfies SYSRET's
  selector math — the static_asserts were bypassed or the GDT reshuffled.
- **Triple fault on the first C statement of a handler:** someone set
  `needs_cr3_switch=true` without a stack switch.
- **Valid syscall bounces with BAD_USER_DATA, handler never runs:** the
  ptr mask marks a non-pointer arg; leftover kernel addresses in unused
  arg registers are being "validated."
- **`entered on CR3 X but is leaving on Y` panic:** the named handler (or
  something it called) switched address spaces and didn't restore —
  exactly what the tripwire exists to catch.
- **User task runs unpreemptible (IF=0 at ring 3):** a return path that
  doesn't restore RFLAGS from the frame/R11 — the retfq ghost.
- **Wild jump when a user program's main returns:** the exit trampoline
  page wasn't seeded/mapped, or the template grew an absolute reference
  and stopped being position-independent.
