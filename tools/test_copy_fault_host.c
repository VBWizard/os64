// The rule for which ring-0 faults a user copy recovers from
// (kernel/src/copy_fault.c), beside a model written from USER_COPY.md
// § Which faults recover: every table entry and the addresses beside it,
// the faulting address on each side of each bound, every error-code bit.
//
//   test_copy_fault

#include <stdio.h>

#include "copy_fault.h"

#define PAGE   0x1000ull
#define CEIL   0x0000800000000000ull

static unsigned long s_checks, s_failed;

static const copy_resume_t kTable[] = {
    { 0xffffffff80101000ull, 0xffffffff80101010ull },
    { 0xffffffff80101020ull, 0xffffffff80101030ull },
    { 0xffffffff80101040ull, 0xffffffff80101050ull },
};

static uint64_t model(uint64_t rip, uint64_t cr2, uint64_t err, const copy_resume_t *t, uint64_t n)
{
    bool ring3 = (err >> 2) & 1, reserved = (err >> 3) & 1;
    bool user_address = cr2 >= PAGE && cr2 < CEIL;
    if (ring3 || reserved || !user_address)
        return 0;
    for (uint64_t i = 0; i < n; i++)
        if (t[i].at == rip)
            return t[i].resume;
    return 0;
}

static void check(uint64_t rip, uint64_t cr2, uint64_t err, uint64_t n)
{
    uint64_t got = copy_fault_resume(rip, cr2, err, PAGE, CEIL, kTable, n);
    uint64_t want = model(rip, cr2, err, kTable, n);
    s_checks++;
    if (got != want) {
        s_failed++;
        if (s_failed <= 20)
            printf("FAIL: rip %#llx cr2 %#llx err %#llx entries %llu: got %#llx, want %#llx\n",
                   (unsigned long long)rip, (unsigned long long)cr2, (unsigned long long)err,
                   (unsigned long long)n, (unsigned long long)got, (unsigned long long)want);
    }
}

int main(void)
{
    const uint64_t cr2s[] = {
        0, 1, PAGE - 1, PAGE, PAGE + 1, 0x70000000ull, CEIL - PAGE, CEIL - 1, CEIL, CEIL + 1,
        0xffff800000000000ull, 0xffffffff80000000ull, ~0ull,
    };
    uint64_t rips[16];
    int nr = 0;
    for (int i = 0; i < 3; i++) {
        rips[nr++] = kTable[i].at;
        rips[nr++] = kTable[i].at + 1;
        rips[nr++] = kTable[i].at - 1;
        rips[nr++] = kTable[i].resume;
    }
    rips[nr++] = 0;
    rips[nr++] = 0xffffffff80200000ull;
    for (uint64_t n = 0; n <= 3; n++)
        for (int r = 0; r < nr; r++)
            for (unsigned c = 0; c < sizeof(cr2s) / sizeof(cr2s[0]); c++)
                for (uint64_t err = 0; err < 64; err++)
                    check(rips[r], cr2s[c], err, n);

    // Named: the plain case resumes at its own entry's exit, never another's.
    check(kTable[1].at, 0x70001000ull, 0x2, 3);
    if (copy_fault_resume(kTable[1].at, 0x70001000ull, 0x2, PAGE, CEIL, kTable, 3) != kTable[1].resume) {
        printf("FAIL: a write fault at entry 1 did not resume at entry 1's exit\n");
        s_failed++;
    }
    printf("copy fault rule: %lu checks, %lu failed\n", s_checks, s_failed);
    return s_failed != 0;
}
