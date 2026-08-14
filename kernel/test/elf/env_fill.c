// env_fill.c — environment growth test fixture (setenv past one page).
//
// The kernel launches this as /bin/env_fill (test_env_growth, test_main.c).
// It exists because the environment block is BORN one page and, since
// 2026-08-14, GROWS when setenv fills it: the kernel swaps a doubled block
// under the fixed TASK_ENV_VIRT window, up to the TASK_ENV_MAX_BYTES (64KB)
// ceiling. That swap is the risky part — the task's read-only mapping is
// remapped onto new physical pages MID-RUN, from the task's own syscall —
// so this fixture attacks it end to end, from ring 3, through the real ABI:
//
//   1. fill far past one page (600 pairs ≈ 11KB → two growth events, 1→2→4
//      pages) and demand every single setenv succeed;
//   2. verify page_count in the block header actually grew — read through
//      the TASK_ENV_VIRT window, which only shows the new header if the
//      REMAP worked;
//   3. read back every pair through that same window (content survived the
//      copy) and confirm the inherited PATH seed also survived;
//   4. keep setting until the 64KB ceiling refuses (bounded loop — if the
//      refusal never comes, the cap is broken and the block would pave the
//      exit trampoline one page at a time);
//   5. after the refusal, prove the block was not corrupted by the failed
//      set, that REPLACING an existing key still works at a full block
//      (compact-then-append), and that unset frees room a new set can use.
//
// Every check returns a distinct 0xE27Fxxxx code so a regression names the
// invariant that died; success returns ENV_FILL_OK.

#include <stdint.h>
#include "os64/env.h"
#include "os64/syscall_numbers.h"
#include "os64/syscall.h"

// Must match kernel/include/task.h (the fixture cross-checks the ABI address
// the same way arg_echo does).
#define TASK_ENV_VIRT  0x6f100000UL

#define ENV_FILL_OK        0x0E27600DUL   // "ENV GOOD"
#define FAIL_ENV_PTR       0xE27F0001UL   // env not at TASK_ENV_VIRT (or NULL)
#define FAIL_SET           0xE27F0002UL   // a fill-phase setenv failed early
#define FAIL_NO_GROWTH     0xE27F0003UL   // page_count never grew past 1
#define FAIL_READBACK      0xE27F0004UL   // a filled pair read back wrong
#define FAIL_PATH          0xE27F0005UL   // inherited PATH lost in the copies
#define FAIL_NO_CEILING    0xE27F0006UL   // 64KB cap never refused
#define FAIL_POST_REFUSAL  0xE27F0007UL   // a refusal corrupted existing data
#define FAIL_REPLACE_FULL  0xE27F0008UL   // replace-at-full-block failed
#define FAIL_UNSET_ROOM    0xE27F0009UL   // unset didn't free usable room

#define FILL_PAIRS         600            // ≈11KB of pairs → two growths
#define CEILING_MAX_TRIES  1000           // ~130B/pair → cap hits near ~420

static int64_t sys_setenv(const char *key, const char *val)
{
    return (int64_t)os64_syscall2(SYSCALL_SETENV, (uint64_t)key, (uint64_t)val);
}

static int64_t sys_unsetenv(const char *key)
{
    return (int64_t)os64_syscall2(SYSCALL_SETENV, (uint64_t)key, 0);
}

static int local_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Write prefix + 4 decimal digits of n into buf ("EF0042"). No libc here.
static void make_key(char *buf, char p0, char p1, uint32_t n)
{
    buf[0] = p0; buf[1] = p1;
    buf[2] = (char)('0' + (n / 1000) % 10);
    buf[3] = (char)('0' + (n / 100) % 10);
    buf[4] = (char)('0' + (n / 10) % 10);
    buf[5] = (char)('0' + n % 10);
    buf[6] = '\0';
}

// The value for pair n: "V" + the same 4 digits, so readback can verify the
// pair survived growth by CONTENT, not just by key presence.
static void make_val(char *buf, uint32_t n)
{
    buf[0] = 'V';
    buf[1] = (char)('0' + (n / 1000) % 10);
    buf[2] = (char)('0' + (n / 100) % 10);
    buf[3] = (char)('0' + (n / 10) % 10);
    buf[4] = (char)('0' + n % 10);
    buf[5] = '\0';
}

// Walk the block at env (the live TASK_ENV_VIRT window) for key; returns the
// value pointer or 0. Deliberately a local walker, not os64_getenv: the
// fixture must not depend on libos64 to test the kernel.
static const char *env_lookup(const os64_env_block_t *env, const char *key)
{
    const char *ptr = env->data;
    const char *end = env->data + env->data_end;
    while (ptr < end)
    {
        const char *k = ptr;
        while (ptr < end && *ptr) ptr++;
        ptr++;
        const char *v = ptr;
        while (ptr < end && *ptr) ptr++;
        ptr++;
        if (local_streq(k, key))
            return v;
    }
    return 0;
}

uint64_t _start(int argc, char **argv, os64_env_block_t *env)
{
    (void)argc; (void)argv;

    // ── 1: the ABI address, same cross-check arg_echo performs ──
    if (env == 0 || (uint64_t)env != TASK_ENV_VIRT)
        return FAIL_ENV_PTR;

    uint32_t initial_pages = env->page_count;

    // ── 2: fill far past one page; every set must succeed ──
    char key[8], val[8];
    for (uint32_t i = 0; i < FILL_PAIRS; i++)
    {
        make_key(key, 'E', 'F', i);
        make_val(val, i);
        if (sys_setenv(key, val) != 0)
            return FAIL_SET;
    }

    // ── 3: growth really happened, visible through the remapped window ──
    if (env->page_count <= initial_pages)
        return FAIL_NO_GROWTH;

    // Every pair, by content — proves the grow-copy chain preserved data and
    // the window now shows the final block.
    for (uint32_t i = 0; i < FILL_PAIRS; i++)
    {
        make_key(key, 'E', 'F', i);
        make_val(val, i);
        const char *got = env_lookup(env, key);
        if (got == 0 || !local_streq(got, val))
            return FAIL_READBACK;
    }
    // The PATH seed rode along from the very first inherit; losing it here
    // would mean growth dropped inherited data.
    if (env_lookup(env, "PATH") == 0)
        return FAIL_PATH;

    // ── 4: the ceiling must refuse, and before this loop gives up ──
    // 120-byte values converge on 64KB fast; pad[] is static so the pattern
    // is the same every run.
    static char pad[121];
    for (int i = 0; i < 120; i++)
        pad[i] = (char)('a' + (i % 26));
    pad[120] = '\0';

    int refused = 0;
    for (uint32_t i = 0; i < CEILING_MAX_TRIES; i++)
    {
        make_key(key, 'E', 'G', i);
        if (sys_setenv(key, pad) != 0)
        {
            refused = 1;
            break;
        }
    }
    if (!refused)
        return FAIL_NO_CEILING;

    // ── 5: a refusal must leave the block intact... ──
    make_key(key, 'E', 'F', 0);
    const char *still = env_lookup(env, key);
    if (still == 0 || !local_streq(still, "V0000"))
        return FAIL_POST_REFUSAL;

    // ...replacing an existing key at a FULL block must still work (env_set
    // compacts the old pair out before appending, so same-size fits)...
    if (sys_setenv(key, "V9999") != 0)
        return FAIL_REPLACE_FULL;
    still = env_lookup(env, key);
    if (still == 0 || !local_streq(still, "V9999"))
        return FAIL_REPLACE_FULL;

    // ...and unset must free room a new set can then use.
    if (sys_unsetenv(key) != 0)
        return FAIL_UNSET_ROOM;
    if (sys_setenv("EFNEW", "yes") != 0)
        return FAIL_UNSET_ROOM;
    const char *fresh = env_lookup(env, "EFNEW");
    if (fresh == 0 || !local_streq(fresh, "yes"))
        return FAIL_UNSET_ROOM;

    return ENV_FILL_OK;
}
