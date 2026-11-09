// Measure the added cost of privilege-boundary accounting against userland.
// Compare this same fixture on the userland base and accounting branch in
// identical QEMU configurations; the minimum round discounts preemption.
#include "os64/os64.h"
#include "os64/syscall.h"

#define CALLS 20000u
#define ROUNDS 7u

static uint64_t cycles(void)
{
    uint32_t low, high;
    __asm__ volatile("lfence\n\trdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

int main(void)
{
    uint64_t pid = os64_taskid();
    if (!pid) return 1;
    uint64_t best_net = UINT64_MAX;
    volatile uint64_t sink = 0;

    for (unsigned round = 0; round < ROUNDS; round++)
    {
        uint64_t start = cycles();
        for (unsigned i = 0; i < CALLS; i++) sink += pid;
        uint64_t empty = cycles() - start;

        start = cycles();
        for (unsigned i = 0; i < CALLS; i++)
        {
            uint64_t ret;
            __asm__ volatile("syscall" : "=a"(ret) : "a"((uint64_t)SYSCALL_TASKID)
                             : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory");
            if (ret != pid) return 2;
            sink += ret;
        }
        uint64_t calls = cycles() - start;
        uint64_t net = calls >= empty ? calls - empty : 0;
        if (net < best_net) best_net = net;
        os64_printf("syscallbench round=%u calls=%u empty_cycles=%lu call_cycles=%lu net_cycles=%lu\n",
                    round, CALLS, empty, calls, net);
    }
    os64_printf("syscallbench best_net_cycles=%lu calls=%u sink=%lu\n",
                best_net, CALLS, (uint64_t)sink);
    return 0;
}
