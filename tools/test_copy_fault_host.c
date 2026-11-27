// The rule for which ring-0 faults a user copy recovers from
// (kernel/src/copy_fault.c), beside a model written from USER_COPY.md
// § Which faults recover: every table entry and the addresses beside it,
// the faulting address on each side of each bound and of each move's user
// pointer, the kernel operand's side, every error-code bit.
//
//   test_copy_fault

#include <stdbool.h>
#include <stdio.h>

#include "copy_fault.h"

#define PAGE   0x1000ull
#define CEIL   0x0000800000000000ull

static unsigned long s_checks, s_failed;

// A read (user pointer in RSI) and a write (RDI), each a byte move and an
// 8-byte one.
static const copy_resume_t kTable[] = {
    { 0xffffffff80101000ull, 0xffffffff80101010ull, COPY_USER_IN_RSI, 1 },
    { 0xffffffff80101020ull, 0xffffffff80101030ull, COPY_USER_IN_RSI, 8 },
    { 0xffffffff80101040ull, 0xffffffff80101050ull, COPY_USER_IN_RDI, 1 },
    { 0xffffffff80101060ull, 0xffffffff80101070ull, COPY_USER_IN_RDI, 8 },
};
#define ENTRIES (sizeof(kTable) / sizeof(kTable[0]))

static uint64_t model(uint64_t rip, uint64_t cr2, uint64_t err, uint64_t rsi, uint64_t rdi,
                      const copy_resume_t *t, uint64_t n)
{
    bool ring3 = (err >> 2) & 1, reserved = (err >> 3) & 1;
    bool user_address = cr2 >= PAGE && cr2 < CEIL;
    if (ring3 || reserved || !user_address)
        return 0;
    for (uint64_t i = 0; i < n; i++) {
        if (t[i].at != rip)
            continue;
        uint64_t user = t[i].user_reg == COPY_USER_IN_RDI ? rdi : rsi;
        bool on_user_side = cr2 >= user && cr2 < user + t[i].step;
        return on_user_side ? t[i].resume : 0;
    }
    return 0;
}

static void check(uint64_t rip, uint64_t cr2, uint64_t err, uint64_t rsi, uint64_t rdi, uint64_t n)
{
    uint64_t got = copy_fault_resume(rip, cr2, err, rsi, rdi, PAGE, CEIL, kTable, n);
    uint64_t want = model(rip, cr2, err, rsi, rdi, kTable, n);
    s_checks++;
    if (got != want) {
        s_failed++;
        if (s_failed <= 20)
            printf("FAIL: rip %#llx cr2 %#llx err %#llx rsi %#llx rdi %#llx entries %llu: got %#llx, want %#llx\n",
                   (unsigned long long)rip, (unsigned long long)cr2, (unsigned long long)err,
                   (unsigned long long)rsi, (unsigned long long)rdi, (unsigned long long)n,
                   (unsigned long long)got, (unsigned long long)want);
    }
}

static void expect(const char *what, uint64_t got, uint64_t want)
{
    s_checks++;
    if (got != want) {
        s_failed++;
        printf("FAIL: %s: got %#llx, want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    }
}

int main(void)
{
    const uint64_t cr2s[] = {
        0, 1, PAGE - 1, PAGE, PAGE + 1, 0x70000000ull, CEIL - PAGE, CEIL - 1, CEIL, CEIL + 1,
        0xffff800000000000ull, 0xffffffff80000000ull, ~0ull,
    };
    // Each pointer register relative to the faulting address: at it, a byte
    // and seven bytes below (inside an 8-byte step), eight below (just past
    // one), one above, and far away (the other operand's side).
    const uint64_t offsets[] = { 0, 1, 7, 8, 9, (uint64_t)-1, 0x10000000ull };
    uint64_t rips[24];
    unsigned nr = 0;
    for (unsigned i = 0; i < ENTRIES; i++) {
        rips[nr++] = kTable[i].at;
        rips[nr++] = kTable[i].at + 1;
        rips[nr++] = kTable[i].resume;
    }
    rips[nr++] = 0;
    rips[nr++] = 0xffffffff80200000ull;
    const uint64_t errs[] = { 0, 1, 2, 3, 4, 6, 8, 0xa, 0x10, 0x12, 0x1f };
    for (uint64_t n = 0; n <= ENTRIES; n++)
        for (unsigned r = 0; r < nr; r++)
            for (unsigned c = 0; c < sizeof(cr2s) / sizeof(cr2s[0]); c++)
                for (unsigned e = 0; e < sizeof(errs) / sizeof(errs[0]); e++)
                    for (unsigned a = 0; a < sizeof(offsets) / sizeof(offsets[0]); a++)
                        for (unsigned b = 0; b < sizeof(offsets) / sizeof(offsets[0]); b++)
                            check(rips[r], cr2s[c], errs[e], cr2s[c] - offsets[a], cr2s[c] - offsets[b], n);

    // Named cases, so a failure says what broke.
    const uint64_t user = 0x70001000ull, kstack = 0x6f0ff000ull;
    expect("a read's byte move faulting at its user pointer resumes at its own exit",
           copy_fault_resume(kTable[0].at, user, 0, user, kstack, PAGE, CEIL, kTable, ENTRIES), kTable[0].resume);
    expect("a write's 8-byte move faulting 7 bytes into its step resumes at its own exit",
           copy_fault_resume(kTable[3].at, user + 7, 2, kstack, user, PAGE, CEIL, kTable, ENTRIES), kTable[3].resume);
    expect("a read that ran off its kernel buffer (RDI) into a guard page panics",
           copy_fault_resume(kTable[0].at, kstack, 2, user, kstack, PAGE, CEIL, kTable, ENTRIES), 0);
    expect("a write that ran off its kernel source (RSI) into a guard page panics",
           copy_fault_resume(kTable[2].at, kstack, 0, kstack, user, PAGE, CEIL, kTable, ENTRIES), 0);
    expect("a byte move faulting one byte past its user pointer panics",
           copy_fault_resume(kTable[0].at, user + 1, 0, user, kstack, PAGE, CEIL, kTable, ENTRIES), 0);
    expect("a fault below the user pointer panics",
           copy_fault_resume(kTable[1].at, user - 1, 0, user, kstack, PAGE, CEIL, kTable, ENTRIES), 0);
    printf("copy fault rule: %lu checks, %lu failed\n", s_checks, s_failed);
    return s_failed != 0;
}
