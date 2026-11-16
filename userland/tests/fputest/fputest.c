// fputest — the floating-point register file survives everything the
// scheduler can do to a thread, and a program cannot use its own signal
// frame to hurt the kernel.
//
// The FPU slice made x87/MMX/SSE state part of a thread's context: saved
// on every switch, restored on every load, snapshotted into every signal
// frame. Each of those is a place a register can silently come back wrong,
// and "silently" is the whole hazard — a float program with a clobbered
// XMM register does not crash, it computes the wrong picture. Worse than a
// wrong DATA register is a wrong CONTROL register: if MXCSR's rounding mode
// does not switch with the thread, every result is off by an ULP and
// nothing anywhere notices.
//
// So: several threads, each holding a DIFFERENT pattern in XMM7, a
// different arithmetic ladder in XMM8..XMM15, a different constant on the
// x87 stack, a different rounding mode in MXCSR and a different one in the
// x87 control word — all spinning on the same cores, each checking all
// five every iteration. Preemption puts thread B's state on the core
// between two of thread A's checks; if the scheduler's save/restore has a
// hole, A sees B's. Migration across cores is covered by the same test,
// because the state travels in the thread struct, not the core.
//
// The signal frame is tested at the same time, on BOTH delivery paths,
// and the fixture knows which one each delivery took. The handler wipes
// every XMM register, the x87 stack, and both control registers — but
// first it reads the x87 control word it was handed, which names the
// interrupted thread (every thread's is distinct), so each delivery is
// counted against the thread that took it:
//
//   §10, the scheduler's own delivery: a HELPER PROCESS (this program,
//   re-spawned with --helper) sends the task SIGINT from outside, during
//   a phase in which EVERY thread of this task is a syscall-free spinner
//   (main keeps time with rdtsc). A spinner is reached only by the
//   scheduler's visit, so each of those deliveries went through the full
//   frame. The fixture requires at least one. The phases are sequential
//   because a task-wide signal goes to the FIRST thread at a delivery
//   point and is cleared from the rest — a sleeping pinger or a clock
//   syscall running alongside would take them all (it did, on the P5).
//
//   §5, the syscall-exit delivery: the pinger thread loads its own pattern,
//   issues the ctl write as a RAW syscall instruction inside the same asm
//   block, and checks its registers on the far side. SIGINT published by
//   that very write is delivered to the writing thread at the syscall's
//   exit, because the dispatcher offers a pending signal to the current
//   thread before any sibling is visited — so the sender is the thread
//   that must hold a pattern.
//
//   §9, the page fault's delivery: main loads its pattern and touches page
//   zero inside the same asm block. The SIGSEGV handler wipes everything,
//   steps the frame's RIP past the faulting instruction, and returns; the
//   registers must be back on the far side.
//
// WITH AVX ON (the program asks the CPU: CPUID's OSXSAVE, then XGETBV) every
// pattern also fills the upper halves of all sixteen YMM registers, each
// register and each thread its own, and every check reads all sixteen back;
// the handlers wipe them with vzeroall. With AVX off the same phases run
// without the upper halves. A separate step spawns this program again
// pinned to ONE CPU (`--pinned`, PINCPU inherited at spawn) with two
// spinning threads, so the CPU's registers pass between exactly those two
// for two seconds — the leak an FXSAVE-only switch had (AVX.md § Proof).
//
// Then the hostile case. The frame lives on the program's own stack, and
// sigreturn restores the image out of it — xrstor64 or fxrstor64, in the
// KERNEL. A reserved MXCSR bit faults either; a header naming a component
// outside XCR0 or a nonzero reserved header byte faults xrstor64, and a
// compaction word faults it on a CPU without XSAVEC (on one with it, it
// asks for a layout the kernel never writes). So the kernel sanitizes the
// image first. The last phase forges each of the four in turn from inside a
// handler and returns: the OS must survive, which is the whole check — a
// CPU cannot hold an illegal MXCSR, because loading one IS the fault.
//
// ONE ASM BLOCK PER CHECK, on purpose. The state is loaded and checked
// inside a single asm statement, so no C code — and no function call — sits
// between load and check. XMM registers are caller-saved in the SysV ABI:
// a callee is ALLOWED to clobber XMM7, and a test that called os64_ticks
// between load and check would be testing the ABI, not the kernel. A chunk
// is a million iterations, far longer than a scheduler tick, so every
// preemption lands inside one.
//
// Exit codes name the failed step: 0xF0DE0000 is a pass.

#include "os64/os64.h"

#define FPUTEST_OK               0xF0DE0000
#define FPUTEST_NO_START         0xF0DE0001
#define FPUTEST_BAD_JOIN         0xF0DE0002
#define FPUTEST_XMM_CLOBBERED    0xF0DE0003
#define FPUTEST_X87_CLOBBERED    0xF0DE0004
#define FPUTEST_NO_HANDLER       0xF0DE0005
#define FPUTEST_NO_SIGNALS       0xF0DE0006   // no external delivery reached a spinning thread (§10 untested)
#define FPUTEST_NO_CTL           0xF0DE0007
#define FPUTEST_MXCSR_CLOBBERED  0xF0DE0008
#define FPUTEST_X87CW_CLOBBERED  0xF0DE0009
#define FPUTEST_HIGHXMM_CLOBBERED 0xF0DE000A
#define FPUTEST_FORGE_NO_SIGNAL  0xF0DE000B
#define FPUTEST_NO_HELPER        0xF0DE000D   // could not spawn the external sender
#define FPUTEST_NO_SELF_SIGNALS  0xF0DE000E   // the pinger's own syscall exits never delivered (§5 untested)
#define FPUTEST_YMM_CLOBBERED    0xF0DE000F   // an upper half of a YMM register came back wrong
#define FPUTEST_NO_SEGV          0xF0DE0010   // the page-fault delivery never ran its handler (§9 untested)
#define FPUTEST_NO_PINNED        0xF0DE0011   // could not spawn the one-CPU run

#define WORKERS      4
#define PATTERNS     (WORKERS + 2)   // + the main thread + the pinger: every thread its own
#define MAIN_PATTERN   WORKERS
#define PINGER_PATTERN (WORKERS + 1)
#define RUN_MS       2000
#define CHUNK_ITERS  1000000UL
#define HELPER_SENDS 60              // ~2s of external SIGINTs at 30ms
#define HELPER_GAP_MS 30
#define PHASE_A_MS   3000            // longer than the helper's sends, so every one lands in phase A
#define SEGV_ROUNDS  20
#define PINNED_CPU   "1"             // a CPU that does not exist leaves the run unpinned (PINCPU's rule)

// Which checks failed inside a chunk — one bit each, OR'd across chunks.
#define BAD_XMM7    (1U << 0)
#define BAD_ST0     (1U << 1)
#define BAD_MXCSR   (1U << 2)
#define BAD_X87CW   (1U << 3)
#define BAD_HIGHXMM (1U << 4)
#define BAD_YMM     (1U << 5)

// One pattern per THREAD — workers, main, and the pinger. Every field
// differs between any two of them, so a cross-thread leak cannot pass by
// luck — two threads sharing a pattern would hide a leak between exactly
// those two. The x87 control words are
// ALL distinct on purpose: the handler identifies the interrupted thread
// by the one it finds loaded.
typedef struct {
    uint64_t lo, hi;      // XMM7; XMM8..15 hold 2x..9x of it per lane
    uint64_t sum_lo, sum_hi;  // 44x per lane — the XMM8..15 checksum
    double   x87;         // st(0)
    uint32_t mxcsr;       // all exceptions masked, a distinct rounding mode
    uint16_t x87cw;       // all exceptions masked, distinct rounding + precision
    uint16_t pad;
    uint8_t  upper[16][16];   // YMM0..15 bits 255:128, filled in main; used with AVX on
} pattern_t;

// MXCSR: 0x1F80 is every exception masked; bits 13-14 are the rounding
// mode. x87 CW: 0x037F is masked/extended/nearest; bits 10-11 rounding,
// 8-9 precision. The sums and the upper halves are filled in by
// fill_patterns.
static pattern_t kPatterns[PATTERNS] = {
    { 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, 0, 0, 1.0000001, 0x1F80U | (0U << 13), 0x037FU, 0, {{0}} },
    { 0xDEADBEEFCAFEBABEULL, 0x1122334455667788ULL, 0, 0, 2.7182818, 0x1F80U | (1U << 13), 0x077FU, 0, {{0}} },   // round down
    { 0xA5A5A5A55A5A5A5AULL, 0x0F0F0F0FF0F0F0F0ULL, 0, 0, 3.1415926, 0x1F80U | (2U << 13), 0x0A7FU, 0, {{0}} },   // round up, double precision
    { 0x8000000000000001ULL, 0x7FFFFFFFFFFFFFFFULL, 0, 0, 0.0000042, 0x1F80U | (3U << 13), 0x0F7FU, 0, {{0}} },   // toward zero
    { 0xC0FFEE0000C0FFEEULL, 0x0BADF00D0BADF00DULL, 0, 0, 6.0221408, 0x1F80U | (1U << 13), 0x027FU, 0, {{0}} },   // main: round down, single
    { 0x5EED5EED5EED5EEDULL, 0x9E3779B97F4A7C15ULL, 0, 0, 1.6180339, 0x1F80U | (2U << 13), 0x0E7FU, 0, {{0}} },   // pinger: round up, toward-zero x87
};

static volatile int64_t gHandlerRuns;
static volatile int64_t gRunsByPattern[PATTERNS];   // who was interrupted, by control word
static volatile int64_t gRunsUnknown;               // a thread with no pattern loaded
static volatile int64_t gSegvRuns;
static volatile bool    gStop;
static bool             gHaveAvx;                   // the CPU says AVX state is enabled

// Every line to the terminal AND the serial wire, so a headless harness can
// read which paths ran without a screendump.
static void say(const char *fmt, ...) OS64_PRINTF(1, 2);
static void say(const char *fmt, ...)
{
    char text[240];
    va_list args;
    va_start(args, fmt);
    os64_vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    os64_printf("%s", text);
    // The wire takes one line without its newline.
    size_t n = os64_strlen(text);
    if (n > 0 && text[n - 1] == '\n')
        text[n - 1] = '\0';
    os64_serial_log(text);
}

// Asked once, before any thread starts: CPUID.1:ECX.OSXSAVE (XGETBV is legal),
// then XCR0's SSE and AVX bits. AVX.md § What ring 3 gets.
static bool cpu_has_avx_on(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1U << 27)) || !(c & (1U << 28)))
        return false;
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 0x6U) == 0x6U;
}

// Wipe everything the frame is supposed to protect. Every YMM register
// zeroed whole when AVX is on, every XMM register zeroed, the x87 stack
// emptied and its control word reset, MXCSR reset. If sigreturn restores
// from the frame, the interrupted thread never notices; if it restores
// nothing, every check in the next iteration fails.
static void wipe_registers(void)
{
    static const uint32_t reset_mxcsr = 0x1F80U;
    if (gHaveAvx)
        __asm__ volatile("vzeroall" ::: "memory");
    __asm__ volatile(
        "pxor xmm0, xmm0\n\t"  "pxor xmm1, xmm1\n\t"  "pxor xmm2, xmm2\n\t"  "pxor xmm3, xmm3\n\t"
        "pxor xmm4, xmm4\n\t"  "pxor xmm5, xmm5\n\t"  "pxor xmm6, xmm6\n\t"  "pxor xmm7, xmm7\n\t"
        "pxor xmm8, xmm8\n\t"  "pxor xmm9, xmm9\n\t"  "pxor xmm10, xmm10\n\t" "pxor xmm11, xmm11\n\t"
        "pxor xmm12, xmm12\n\t" "pxor xmm13, xmm13\n\t" "pxor xmm14, xmm14\n\t" "pxor xmm15, xmm15\n\t"
        "fninit\n\t"
        "ldmxcsr %[m]\n\t"
        :: [m] "m"(reset_mxcsr) : "memory");
}

// Before the wipe: the live file still belongs to the interrupted thread
// (the frame is a COPY), so the control word read here says whose turn it
// was. That is how the fixture knows a delivery reached a spinning worker.
static void clobber_handler(int signo)
{
    (void)signo;
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    bool known = false;
    for (int i = 0; i < PATTERNS; i++)
        if (kPatterns[i].x87cw == cw) { gRunsByPattern[i]++; known = true; }
    if (!known)
        gRunsUnknown++;
    wipe_registers();
    gHandlerRuns++;
}

// §9: the fault is `mov rax, [rax]` with RAX = 0x10 — three bytes, emitted
// as bytes so the length is not the assembler's choice — and returning
// would retry it forever, so the handler steps the frame's RIP past it. The
// stub CALLs the handler with RSP pointing AT the frame (task_exit_asm.S);
// at -O0 (push rbp; mov rbp, rsp) that is rbp + 16, and RIP is at +16 in it
// (signal_frame_t, signals.h).
#define SEGV_FAULT_LENGTH 3
#define FRAME_RIP_OFFSET  16

static void segv_handler(int signo)
{
    (void)signo;
    uint8_t *frame = (uint8_t *)__builtin_frame_address(0) + 16;
    *(volatile uint64_t *)(frame + FRAME_RIP_OFFSET) += SEGV_FAULT_LENGTH;
    wipe_registers();
    gSegvRuns++;
}

// The load and the check, as text, so the spin loop and the syscall probe
// share ONE copy of each. Operand names are the ones both asm statements
// bind below.
#define FPU_LOAD_ASM \
        "ldmxcsr [%[pat] + 40]\n\t"            /* MXCSR <- this thread's rounding mode */ \
        "fldcw   [%[pat] + 44]\n\t"            /* x87 CW <- this thread's rounding/precision */ \
        "movdqu xmm7, [%[pat]]\n\t"      /* XMM7 <- pattern */ \
        "movdqa xmm8, xmm7\n\t"  "paddq xmm8, xmm7\n\t"    /* 2x */ \
        "movdqa xmm9, xmm8\n\t"  "paddq xmm9, xmm7\n\t"    /* 3x */ \
        "movdqa xmm10, xmm9\n\t" "paddq xmm10, xmm7\n\t"   /* 4x */ \
        "movdqa xmm11, xmm10\n\t" "paddq xmm11, xmm7\n\t"  /* 5x */ \
        "movdqa xmm12, xmm11\n\t" "paddq xmm12, xmm7\n\t"  /* 6x */ \
        "movdqa xmm13, xmm12\n\t" "paddq xmm13, xmm7\n\t"  /* 7x */ \
        "movdqa xmm14, xmm13\n\t" "paddq xmm14, xmm7\n\t"  /* 8x */ \
        "movdqa xmm15, xmm14\n\t" "paddq xmm15, xmm7\n\t"  /* 9x  (sum 44x) */ \
        "fld qword ptr [%[pat] + 32]\n\t"     /* st(0) <- constant */

// Local labels 2-9 belong to the check; a caller's own labels must avoid them.
#define FPU_CHECK_ASM \
        /* XMM7 against the pattern */ \
        "movdqu [%[scr]], xmm7\n\t" \
        "mov rax, [%[scr]]\n\t" \
        "cmp rax, [%[pat]]\n\t" \
        "jne 2f\n\t" \
        "mov rax, [%[scr] + 8]\n\t" \
        "cmp rax, [%[pat] + 8]\n\t" \
        "je 3f\n\t" \
        "2:\n\t" \
        "or %[bad], %[bxmm]\n\t" \
        "3:\n\t" \
        /* XMM8..15 summed against 44x the pattern */ \
        "movdqa xmm6, xmm8\n\t" \
        "paddq xmm6, xmm9\n\t"  "paddq xmm6, xmm10\n\t" "paddq xmm6, xmm11\n\t" \
        "paddq xmm6, xmm12\n\t" "paddq xmm6, xmm13\n\t" "paddq xmm6, xmm14\n\t" "paddq xmm6, xmm15\n\t" \
        "movdqu [%[scr]], xmm6\n\t" \
        "mov rax, [%[scr]]\n\t" \
        "cmp rax, [%[pat] + 16]\n\t" \
        "jne 5f\n\t" \
        "mov rax, [%[scr] + 8]\n\t" \
        "cmp rax, [%[pat] + 24]\n\t" \
        "je 6f\n\t" \
        "5:\n\t" \
        "or %[bad], %[bhigh]\n\t" \
        "6:\n\t" \
        /* st(0) against the constant (fst, not fstp: it stays loaded) */ \
        "fst qword ptr [%[scr] + 16]\n\t" \
        "mov rax, [%[scr] + 16]\n\t" \
        "cmp rax, [%[pat] + 32]\n\t" \
        "je 7f\n\t" \
        "or %[bad], %[bst]\n\t" \
        "7:\n\t" \
        /* MXCSR against what we loaded — the whole word, flags included */ \
        "stmxcsr [%[scr] + 24]\n\t" \
        "mov eax, [%[scr] + 24]\n\t" \
        "cmp eax, [%[pat] + 40]\n\t" \
        "je 8f\n\t" \
        "or %[bad], %[bmx]\n\t" \
        "8:\n\t" \
        /* x87 control word */ \
        "fnstcw [%[scr] + 28]\n\t" \
        "mov ax, [%[scr] + 28]\n\t" \
        "cmp ax, [%[pat] + 44]\n\t" \
        "je 9f\n\t" \
        "or %[bad], %[bcw]\n\t" \
        "9:\n\t"

// With AVX on: the upper half of every YMM register from the pattern's
// table. vinsertf128 with the register itself as the first source keeps the
// low half, so XMM0..5 (the compiler's) and XMM6..15 (the pattern above)
// are untouched. Legacy SSE never writes an upper half, so the low-half
// checks cannot disturb these either.
#define FPU_UPPER_LOAD_ASM \
        "vinsertf128 ymm0, ymm0, [%[pat] + 48], 1\n\t" \
        "vinsertf128 ymm1, ymm1, [%[pat] + 64], 1\n\t" \
        "vinsertf128 ymm2, ymm2, [%[pat] + 80], 1\n\t" \
        "vinsertf128 ymm3, ymm3, [%[pat] + 96], 1\n\t" \
        "vinsertf128 ymm4, ymm4, [%[pat] + 112], 1\n\t" \
        "vinsertf128 ymm5, ymm5, [%[pat] + 128], 1\n\t" \
        "vinsertf128 ymm6, ymm6, [%[pat] + 144], 1\n\t" \
        "vinsertf128 ymm7, ymm7, [%[pat] + 160], 1\n\t" \
        "vinsertf128 ymm8, ymm8, [%[pat] + 176], 1\n\t" \
        "vinsertf128 ymm9, ymm9, [%[pat] + 192], 1\n\t" \
        "vinsertf128 ymm10, ymm10, [%[pat] + 208], 1\n\t" \
        "vinsertf128 ymm11, ymm11, [%[pat] + 224], 1\n\t" \
        "vinsertf128 ymm12, ymm12, [%[pat] + 240], 1\n\t" \
        "vinsertf128 ymm13, ymm13, [%[pat] + 256], 1\n\t" \
        "vinsertf128 ymm14, ymm14, [%[pat] + 272], 1\n\t" \
        "vinsertf128 ymm15, ymm15, [%[pat] + 288], 1\n\t"

// Each upper half out to the scratch and XORed against the table, every
// difference ORed into R11; one branch at the end. Local label 4 belongs to
// this check.
#define FPU_UPPER_CHECK_ONE(reg, off) \
        "vextractf128 [%[scr]], " reg ", 1\n\t" \
        "mov rax, [%[scr]]\n\t" \
        "xor rax, [%[pat] + " #off "]\n\t" \
        "or r11, rax\n\t" \
        "mov rax, [%[scr] + 8]\n\t" \
        "xor rax, [%[pat] + " #off " + 8]\n\t" \
        "or r11, rax\n\t"
#define FPU_UPPER_CHECK_ASM \
        "xor r11, r11\n\t" \
        FPU_UPPER_CHECK_ONE("ymm0", 48)   FPU_UPPER_CHECK_ONE("ymm1", 64) \
        FPU_UPPER_CHECK_ONE("ymm2", 80)   FPU_UPPER_CHECK_ONE("ymm3", 96) \
        FPU_UPPER_CHECK_ONE("ymm4", 112)  FPU_UPPER_CHECK_ONE("ymm5", 128) \
        FPU_UPPER_CHECK_ONE("ymm6", 144)  FPU_UPPER_CHECK_ONE("ymm7", 160) \
        FPU_UPPER_CHECK_ONE("ymm8", 176)  FPU_UPPER_CHECK_ONE("ymm9", 192) \
        FPU_UPPER_CHECK_ONE("ymm10", 208) FPU_UPPER_CHECK_ONE("ymm11", 224) \
        FPU_UPPER_CHECK_ONE("ymm12", 240) FPU_UPPER_CHECK_ONE("ymm13", 256) \
        FPU_UPPER_CHECK_ONE("ymm14", 272) FPU_UPPER_CHECK_ONE("ymm15", 288) \
        "test r11, r11\n\t" \
        "jz 4f\n\t" \
        "or %[bad], %[bymm]\n\t" \
        "4:\n\t"

// Two base registers, not eight: the pattern's fields and the scratch
// slots are reached by offset, because the syscall probe below needs six
// more GPRs than an eight-pointer operand list leaves on x86-64. The
// offsets are asserted against the struct, so a reordered field breaks
// the build rather than the comparison.
_Static_assert(__builtin_offsetof(pattern_t, x87)    == 32, "pattern_t.x87 is read at +32");
_Static_assert(__builtin_offsetof(pattern_t, mxcsr)  == 40, "pattern_t.mxcsr is read at +40");
_Static_assert(__builtin_offsetof(pattern_t, x87cw)  == 44, "pattern_t.x87cw is read at +44");
_Static_assert(__builtin_offsetof(pattern_t, sum_lo) == 16, "pattern_t.sum_lo is read at +16");
_Static_assert(__builtin_offsetof(pattern_t, upper)  == 48, "pattern_t.upper is read at +48");
// scratch[4]: +0..15 an XMM spill, +16 the st(0) store, +24 MXCSR, +28 the x87 CW.
#define FPU_ASM_OPERANDS(p, scratch) \
          [pat] "r"(p), [scr] "r"(scratch), \
          [bxmm] "i"(BAD_XMM7), [bst] "i"(BAD_ST0), [bmx] "i"(BAD_MXCSR), \
          [bcw] "i"(BAD_X87CW), [bhigh] "i"(BAD_HIGHXMM), [bymm] "i"(BAD_YMM)

#define FPU_ASM_CLOBBERS \
          "rax", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", \
          "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc"
// The AVX variants also write the upper halves of XMM0..5 and use R11.
// vzeroupper at the end puts every upper half back to zero, as the SysV
// ABI expects of SSE code, and leaves every low half alone.
#define FPU_AVX_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5"

// Load the pattern, then spin CHUNK_ITERS times comparing what the
// registers hold against what was loaded. Returns the BAD_* bits.
static uint32_t run_chunk_sse(const pattern_t *p)
{
    uint32_t bad = 0;
    uint64_t scratch[4];
    __asm__ volatile(
        FPU_LOAD_ASM
        "mov rcx, %[iters]\n\t"
        "1:\n\t"
        FPU_CHECK_ASM
        "dec rcx\n\t"
        "jnz 1b\n\t"
        "fstp st(0)\n\t"                 // leave the x87 stack as we found it
        : [bad] "+r"(bad)
        : FPU_ASM_OPERANDS(p, scratch), [iters] "i"(CHUNK_ITERS)
        : "rcx", FPU_ASM_CLOBBERS);
    return bad;
}

static uint32_t run_chunk_avx(const pattern_t *p)
{
    uint32_t bad = 0;
    uint64_t scratch[4];
    __asm__ volatile(
        FPU_LOAD_ASM
        FPU_UPPER_LOAD_ASM
        "mov rcx, %[iters]\n\t"
        "1:\n\t"
        FPU_CHECK_ASM
        FPU_UPPER_CHECK_ASM
        "dec rcx\n\t"
        "jnz 1b\n\t"
        "fstp st(0)\n\t"
        "vzeroupper\n\t"
        : [bad] "+r"(bad)
        : FPU_ASM_OPERANDS(p, scratch), [iters] "i"(CHUNK_ITERS)
        : "rcx", "r11", FPU_AVX_CLOBBERS, FPU_ASM_CLOBBERS);
    return bad;
}

static uint32_t run_chunk(const pattern_t *p)
{
    return gHaveAvx ? run_chunk_avx(p) : run_chunk_sse(p);
}

// Load the pattern, make ONE syscall — the ctl write that publishes SIGINT
// to this very thread — and check on the far side. The `syscall` is inline
// so nothing but the kernel stands between load and check: the frame is
// built from the live file at the syscall's exit, the handler wipes it,
// and sigreturn must put it back before this instruction stream resumes.
//
// The three payload arguments travel in a struct behind ONE pointer: the
// check already binds eight register operands, and eight plus three plus
// the six registers the syscall clobbers is more than x86-64 has. Write's
// fourth argument, infinite patience, is loaded as an immediate.
typedef struct { uint64_t handle; const char *text; uint64_t length; } syscall_args_t;

// The raw write, shared by both variants. R11 is free again after the
// syscall (the hardware put RFLAGS in it), so the upper check may use it.
#define FPU_SYSCALL_ASM \
        "mov rax, %[nr]\n\t" \
        "mov rdi, [%[a]]\n\t" \
        "mov rsi, [%[a] + 8]\n\t" \
        "mov rdx, [%[a] + 16]\n\t" \
        "mov r10, -1\n\t"       /* OS64_WAIT_FOREVER: this raw write bypasses libos64. */ \
        "syscall\n\t"
// A syscall clobbers what a C call clobbers (ABI.md § register contract):
// every caller-saved GPR, not just the hardware's RCX/R11 — the kernel
// zeroes them so nothing of its own leaks out. Name them all, or the
// compiler parks an operand in R10 and the first store after `syscall`
// goes through a null pointer.
#define FPU_SYSCALL_CLOBBERS "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10"

static uint32_t run_syscall_checked(const pattern_t *p, int64_t handle,
                                    const char *text, uint64_t length)
{
    uint32_t bad = 0;
    uint64_t scratch[4];
    syscall_args_t args = { (uint64_t)handle, text, length };
    if (gHaveAvx)
        __asm__ volatile(
            FPU_LOAD_ASM
            FPU_UPPER_LOAD_ASM
            FPU_SYSCALL_ASM
            FPU_CHECK_ASM
            FPU_UPPER_CHECK_ASM
            "fstp st(0)\n\t"
            "vzeroupper\n\t"
            : [bad] "+r"(bad)
            : FPU_ASM_OPERANDS(p, scratch),
              [nr] "i"(SYSCALL_WRITE), [a] "r"(&args)
            : FPU_SYSCALL_CLOBBERS, FPU_AVX_CLOBBERS, FPU_ASM_CLOBBERS);
    else
        __asm__ volatile(
            FPU_LOAD_ASM
            FPU_SYSCALL_ASM
            FPU_CHECK_ASM
            "fstp st(0)\n\t"
            : [bad] "+r"(bad)
            : FPU_ASM_OPERANDS(p, scratch),
              [nr] "i"(SYSCALL_WRITE), [a] "r"(&args)
            : FPU_SYSCALL_CLOBBERS, FPU_ASM_CLOBBERS);
    return bad;
}

// Load the pattern, fault on page zero, check. The SIGSEGV handler steps
// RIP past the three bytes of `mov rax, [rax]` (segv_handler); everything
// else must come back from the frame (§9).
#define FPU_SEGV_ASM \
        "mov rax, 0x10\n\t" \
        ".byte 0x48, 0x8b, 0x00\n\t"     /* mov rax, [rax] — SEGV_FAULT_LENGTH bytes */

static uint32_t run_segv_checked(const pattern_t *p)
{
    uint32_t bad = 0;
    uint64_t scratch[4];
    if (gHaveAvx)
        __asm__ volatile(
            FPU_LOAD_ASM
            FPU_UPPER_LOAD_ASM
            FPU_SEGV_ASM
            FPU_CHECK_ASM
            FPU_UPPER_CHECK_ASM
            "fstp st(0)\n\t"
            "vzeroupper\n\t"
            : [bad] "+r"(bad)
            : FPU_ASM_OPERANDS(p, scratch)
            : "r11", FPU_AVX_CLOBBERS, FPU_ASM_CLOBBERS);
    else
        __asm__ volatile(
            FPU_LOAD_ASM
            FPU_SEGV_ASM
            FPU_CHECK_ASM
            "fstp st(0)\n\t"
            : [bad] "+r"(bad)
            : FPU_ASM_OPERANDS(p, scratch)
            : FPU_ASM_CLOBBERS);
    return bad;
}

static int64_t worker(void *arg)
{
    const pattern_t *p = &kPatterns[(int64_t)arg];
    uint32_t bad = 0;
    while (!gStop)
        bad |= run_chunk(p);
    return (int64_t)bad;
}

static int64_t open_ctl(uint64_t taskid)
{
    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/ctl", taskid);
    return os64_open(path, "w");
}

// The §5 probe: every 20ms, the pinger sends ITSELF SIGINT through a raw
// syscall with a pattern loaded, and checks. Returns the BAD_* bits, or
// FPUTEST_NO_CTL.
static int64_t pinger(void *arg)
{
    (void)arg;
    int64_t h = open_ctl(os64_taskid());
    if (h < 0)
        return FPUTEST_NO_CTL;
    uint32_t bad = 0;
    while (!gStop)
    {
        bad |= run_syscall_checked(&kPatterns[PINGER_PATTERN], h, "interrupt", 9);
        os64_sleep(20);
    }
    os64_close((int32_t)h);
    return (int64_t)bad;
}

// The §10 sender: a separate PROCESS, so its syscalls are its own and the
// only threads that can take the signal are the fixture's. `fputest
// --helper <taskid>` sends HELPER_SENDS SIGINTs, HELPER_GAP_MS apart.
static int helper_main(const char *taskid_text)
{
    uint64_t taskid = 0;
    for (const char *c = taskid_text; *c >= '0' && *c <= '9'; c++)
        taskid = taskid * 10 + (uint64_t)(*c - '0');
    int64_t h = open_ctl(taskid);
    if (h < 0)
        return 1;
    for (int i = 0; i < HELPER_SENDS; i++)
    {
        os64_write((int32_t)h, "interrupt", 9);
        os64_sleep(HELPER_GAP_MS);
    }
    os64_close((int32_t)h);
    return 0;
}

// ── The hostile frame ──────────────────────────────────────────────────────
//
// The stub CALLs the handler with RSP pointing AT the frame (task_exit_asm.S),
// so at -O0 — push rbp; mov rbp, rsp — the frame is rbp + 16. Its register
// image starts at +64 (signal_frame_t, signals.h), the standard XSAVE layout
// in both modes: MXCSR at +24 inside it, the header at +512 — XSTATE_BV,
// then XCOMP_BV at +8, then reserved bytes to +63.
#define FRAME_IMAGE        64
#define FRAME_MXCSR_OFFSET (FRAME_IMAGE + 24)
#define FRAME_XSTATE_BV    (FRAME_IMAGE + 512)
#define FRAME_XCOMP_BV     (FRAME_IMAGE + 512 + 8)
#define FRAME_RESERVED     (FRAME_IMAGE + 512 + 40)

// The four ways a frame's image can trouble the kernel's restore, one per
// signal: three are a kernel #GP unless sigreturn sanitizes them first, and
// the compaction word is one too on a CPU without XSAVEC.
enum { FORGE_MXCSR, FORGE_COMPONENTS, FORGE_COMPACTED, FORGE_RESERVED, FORGE_KINDS };
static const char *const kForgeNames[FORGE_KINDS] = {
    "MXCSR 0xFFFFFFFF", "XSTATE_BV naming every component", "XCOMP_BV's compaction bit",
    "a reserved header byte",
};
static volatile int gForgeKind;
static volatile int64_t gForgeRuns;

static void forge_handler(int signo)
{
    (void)signo;
    uint8_t *frame = (uint8_t *)__builtin_frame_address(0) + 16;
    switch (gForgeKind)
    {
    case FORGE_MXCSR:      *(volatile uint32_t *)(frame + FRAME_MXCSR_OFFSET) = 0xFFFFFFFFU; break;
    case FORGE_COMPONENTS: *(volatile uint64_t *)(frame + FRAME_XSTATE_BV) = ~0ULL; break;
    case FORGE_COMPACTED:  *(volatile uint64_t *)(frame + FRAME_XCOMP_BV) = (1ULL << 63) | 7; break;
    case FORGE_RESERVED:   *(volatile uint8_t *)(frame + FRAME_RESERVED) = 0x5A; break;
    }
    gForgeRuns++;
}

// ── One CPU, two threads ───────────────────────────────────────────────────
//
// `fputest --pinned`: spawned with PINCPU set, so this task and both its
// workers live on one CPU, and every switch there hands the registers from
// one worker to the other. Patterns 0 and 1; returns FPUTEST_OK or the
// first BAD_* code either saw.
static int pinned_main(void)
{
    gHaveAvx = cpu_has_avx_on();
    int64_t a = os64_thread(worker, (void *)0);
    int64_t b = os64_thread(worker, (void *)1);
    if (a < 0 || b < 0)
        return FPUTEST_NO_START;
    os64_sleep(RUN_MS);
    gStop = true;
    int64_t abad = -1, bbad = -1;
    if (os64_thread_join((int32_t)a, &abad) < 0 || abad < 0 ||
        os64_thread_join((int32_t)b, &bbad) < 0 || bbad < 0)
        return FPUTEST_BAD_JOIN;
    uint32_t bad = (uint32_t)abad | (uint32_t)bbad;
    if (bad & BAD_YMM)     return FPUTEST_YMM_CLOBBERED;
    if (bad & BAD_XMM7)    return FPUTEST_XMM_CLOBBERED;
    if (bad & BAD_HIGHXMM) return FPUTEST_HIGHXMM_CLOBBERED;
    if (bad & BAD_ST0)     return FPUTEST_X87_CLOBBERED;
    if (bad & BAD_MXCSR)   return FPUTEST_MXCSR_CLOBBERED;
    if (bad & BAD_X87CW)   return FPUTEST_X87CW_CLOBBERED;
    return FPUTEST_OK;
}

// Each register and each thread its own upper half: a register that came
// back as another register's, or another thread's, fails the compare.
static void fill_patterns(void)
{
    for (int i = 0; i < PATTERNS; i++)
    {
        pattern_t *p = &kPatterns[i];
        p->sum_lo = p->lo * 44ULL;   // paddq wraps per lane, so does this
        p->sum_hi = p->hi * 44ULL;
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 16; b++)
            {
                uint64_t word = b < 8 ? p->lo : p->hi;
                p->upper[r][b] = (uint8_t)((word >> (8 * (b & 7))) ^ (uint8_t)(r * 0x1D + b * 0x07 + 1));
            }
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && os64_streq(argv[1], "--helper"))
        return helper_main(argv[2]);

    fill_patterns();
    if (argc == 2 && os64_streq(argv[1], "--pinned"))
        return pinned_main();

    gHaveAvx = cpu_has_avx_on();
    say("fputest: AVX %s - the upper halves of YMM0..15 are %s\n",
                gHaveAvx ? "on" : "off", gHaveAvx ? "checked too" : "not in play");

    if (os64_signal_set_handler(OS64_SIGINT, clobber_handler) < 0)
    {
        say("fputest: could not install the SIGINT handler\n");
        return FPUTEST_NO_HANDLER;
    }

    int64_t handles[WORKERS];
    for (int64_t i = 0; i < WORKERS; i++)
    {
        handles[i] = os64_thread(worker, (void *)i);
        if (handles[i] < 0)
        {
            say("fputest: could not start worker %ld\n", i);
            return FPUTEST_NO_START;
        }
    }
    // ── Phase A: external signals, and NOBODY to take them but a spinner ──
    //
    // A task-wide signal goes to the first thread that reaches a delivery
    // point, and it is then cleared from the siblings. A syscall exit is a
    // delivery point; so is the end of a park (a sleeping thread wakes for
    // any caught signal). A spinning thread on a tickless core is visited
    // only at its backstop lease — SCHED_BACKSTOP_MS apart. So while the
    // helper is sending, this task must contain nothing but spinners, or a
    // sleeping pinger and a clock syscall take every signal first. Main
    // therefore bounds this phase with rdtsc — no clock syscall —
    // calibrated against one sleep beforehand.
    uint64_t tsc_per_ms;
    {
        uint64_t t0 = __builtin_ia32_rdtsc();
        os64_sleep(200);
        tsc_per_ms = (__builtin_ia32_rdtsc() - t0) / 200;
        if (tsc_per_ms == 0)
            tsc_per_ms = 1;
    }

    char taskid_text[24];
    os64_snprintf(taskid_text, sizeof(taskid_text), "%lu", os64_taskid());
    char *const helper_argv[] = { "/tests/fputest", "--helper", taskid_text, NULL };
    int64_t helper = os64_spawn("/tests/fputest", helper_argv);
    if (helper < 0)
    {
        say("fputest: could not spawn the external sender\n");
        return FPUTEST_NO_HELPER;
    }

    uint32_t bad = 0;
    {
        uint64_t phase_start = __builtin_ia32_rdtsc();
        uint64_t phase_end = phase_start + (uint64_t)PHASE_A_MS * tsc_per_ms;
        while (__builtin_ia32_rdtsc() < phase_end)
            bad |= run_chunk(&kPatterns[MAIN_PATTERN]);
    }
    int32_t helper_code = 0;
    while (os64_wait(helper, &helper_code) == OS64_INTERRUPTED)
        ;   // a straggler ends the wait early; wait again
    int64_t main_runs_a = gRunsByPattern[MAIN_PATTERN];
    int64_t spinner_runs = main_runs_a;
    for (int i = 0; i < WORKERS; i++)
        spinner_runs += gRunsByPattern[i];

    // ── Phase B: the pinger's syscall-exit probe, main keeping time ──────
    int64_t ping = os64_thread(pinger, NULL);
    if (ping < 0)
        return FPUTEST_NO_START;
    os64_ticks_t start, now;
    os64_ticks(&start);
    do {
        bad |= run_chunk(&kPatterns[MAIN_PATTERN]);
        os64_ticks(&now);
    } while ((now.ticks - start.ticks) * 1000 / now.per_second < RUN_MS);
    gStop = true;

    for (int64_t i = 0; i < WORKERS; i++)
    {
        int64_t wbad = -1;
        if (os64_thread_join((int32_t)handles[i], &wbad) < 0 || wbad < 0)
        {
            say("fputest: join of worker %ld failed\n", i);
            return FPUTEST_BAD_JOIN;
        }
        bad |= (uint32_t)wbad;
    }
    int64_t ping_result = 0;
    os64_thread_join((int32_t)ping, &ping_result);
    if (ping_result == FPUTEST_NO_CTL)
    {
        say("fputest: could not open /proc/self/ctl to send SIGINT\n");
        return FPUTEST_NO_CTL;
    }
    bad |= (uint32_t)ping_result;

    say("fputest: %ld handler runs — %ld on syscall-free spinners (scheduler-delivered), %ld on the pinger's own syscall (syscall-exit), %ld on main afterwards, %ld unidentified\n",
                gHandlerRuns, spinner_runs, gRunsByPattern[PINGER_PATTERN],
                gRunsByPattern[MAIN_PATTERN] - main_runs_a, gRunsUnknown);
    say("fputest: bad checks: xmm7 %s, xmm8-15 %s, ymm upper halves %s, st(0) %s, mxcsr %s, x87cw %s\n",
                (bad & BAD_XMM7) ? "YES" : "none", (bad & BAD_HIGHXMM) ? "YES" : "none",
                (bad & BAD_YMM) ? "YES" : (gHaveAvx ? "none" : "not checked"),
                (bad & BAD_ST0) ? "YES" : "none", (bad & BAD_MXCSR) ? "YES" : "none",
                (bad & BAD_X87CW) ? "YES" : "none");
    if (bad & BAD_YMM)     return FPUTEST_YMM_CLOBBERED;
    if (bad & BAD_XMM7)    return FPUTEST_XMM_CLOBBERED;
    if (bad & BAD_HIGHXMM) return FPUTEST_HIGHXMM_CLOBBERED;
    if (bad & BAD_ST0)     return FPUTEST_X87_CLOBBERED;
    if (bad & BAD_MXCSR)   return FPUTEST_MXCSR_CLOBBERED;
    if (bad & BAD_X87CW)   return FPUTEST_X87CW_CLOBBERED;
    // A pass that never exercised a path must say so rather than claim it.
    if (spinner_runs == 0)
    {
        say("fputest: no external SIGINT ever reached a spinning thread — the scheduler-delivered frame went untested\n");
        return FPUTEST_NO_SIGNALS;
    }
    if (gRunsByPattern[PINGER_PATTERN] == 0)
    {
        say("fputest: the pinger's own syscalls never delivered — the syscall-exit frame went untested\n");
        return FPUTEST_NO_SELF_SIGNALS;
    }

    // ── §9: the page fault's delivery, main alone ─────────────────────────
    if (os64_signal_set_handler(OS64_SIGSEGV, segv_handler) < 0)
        return FPUTEST_NO_HANDLER;
    uint32_t segv_bad = 0;
    for (int i = 0; i < SEGV_ROUNDS; i++)
        segv_bad |= run_segv_checked(&kPatterns[MAIN_PATTERN]);
    os64_signal_set_handler(OS64_SIGSEGV, OS64_SIG_DEFAULT);   // a real fault must kill, not skip
    say("fputest: %ld of %d page faults delivered to the SIGSEGV handler (page-fault path), bad bits 0x%x\n",
                gSegvRuns, SEGV_ROUNDS, segv_bad);
    if (segv_bad & BAD_YMM)     return FPUTEST_YMM_CLOBBERED;
    if (segv_bad & BAD_XMM7)    return FPUTEST_XMM_CLOBBERED;
    if (segv_bad & BAD_HIGHXMM) return FPUTEST_HIGHXMM_CLOBBERED;
    if (segv_bad & BAD_ST0)     return FPUTEST_X87_CLOBBERED;
    if (segv_bad & BAD_MXCSR)   return FPUTEST_MXCSR_CLOBBERED;
    if (segv_bad & BAD_X87CW)   return FPUTEST_X87CW_CLOBBERED;
    if (gSegvRuns != SEGV_ROUNDS)
        return FPUTEST_NO_SEGV;

    // ── Two threads, one CPU ──────────────────────────────────────────────
    // PINCPU is read at birth from the environment the child inherits, and
    // unset again at once so nothing else this program spawns is pinned.
    os64_setenv("PINCPU", PINNED_CPU);
    char *const pinned_argv[] = { "/tests/fputest", "--pinned", NULL };
    int64_t pinned = os64_spawn("/tests/fputest", pinned_argv);
    os64_unsetenv("PINCPU");
    if (pinned < 0)
        return FPUTEST_NO_PINNED;
    int32_t pinned_code = 0;
    while (os64_wait(pinned, &pinned_code) == OS64_INTERRUPTED)
        ;
    say("fputest: two threads sharing CPU " PINNED_CPU " for %d ms: exit 0x%x\n",
                RUN_MS, (uint32_t)pinned_code);
    if ((uint32_t)pinned_code != FPUTEST_OK)
        return pinned_code;

    // Now the forgery. Only this thread is left, so the signal is ours.
    // Delivery rides the ctl write's own syscall exit (§5), so the handler
    // has run by the time the write returns; the sleep is a backstop.
    os64_signal_set_handler(OS64_SIGINT, forge_handler);
    for (int kind = 0; kind < FORGE_KINDS; kind++)
    {
        gForgeKind = kind;
        int64_t before = gForgeRuns;
        int64_t h = open_ctl(os64_taskid());
        if (h < 0)
            return FPUTEST_NO_CTL;
        os64_write((int32_t)h, "interrupt", 9);
        os64_close((int32_t)h);
        for (int i = 0; i < 50 && gForgeRuns == before; i++)
            os64_sleep(10);
        if (gForgeRuns == before)
        {
            say("fputest: the forging handler never ran for %s\n", kForgeNames[kind]);
            return FPUTEST_FORGE_NO_SIGNAL;
        }
        // Still here: the kernel survived the restore, which is the pass.
        // The MXCSR it handed back is printed for the record only.
        uint32_t mxcsr_now;
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr_now));
        say("fputest: forged %s and lived; MXCSR is 0x%x\n", kForgeNames[kind], mxcsr_now);
    }
    return FPUTEST_OK;
}
