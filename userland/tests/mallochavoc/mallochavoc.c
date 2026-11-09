// mallochavoc — black-box, reproducible allocator chaos.
//
// This deliberately knows nothing about heap representation.  Its oracle is
// only the public malloc contract: alignment, non-overlapping live objects,
// preserved contents, calloc zeroing, and realloc's edge cases.  The shadow
// table is static so the test does not depend on the allocator it is judging.

#include "os64/os64.h"

#define SLOT_COUNT       384U
#define DEFAULT_OPS      50000U
#define SEED_SCRAMBLE    0x6d616c6c6f63ULL
#define FULL_CHECK_EVERY 1024U

typedef struct {
    uint8_t *pointer;
    size_t size;
    uint64_t signature;
} allocation_t;

static allocation_t gAllocations[SLOT_COUNT];
static uint64_t gRandom;
static uint64_t gSeed;
static uint64_t gOperation;

static uint64_t random64(void)
{
    // xorshift64*: tiny, deterministic, and quite sufficient for havoc.
    uint64_t x = gRandom;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    gRandom = x;
    return x * 0x2545f4914f6cdd1dULL;
}

static bool parse_uint(const char *text, uint64_t *result)
{
    uint64_t value = 0;
    if (text == NULL || *text == '\0')
        return false;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return false;
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *result = value;
    return true;
}

static uint64_t expected_word(uint64_t signature, size_t word_index)
{
    return signature ^ ((uint64_t)word_index * 0x9e3779b97f4a7c15ULL);
}

static void fill(allocation_t *allocation)
{
    size_t word_count = allocation->size / sizeof(uint64_t);
    uint64_t *words = (uint64_t *)allocation->pointer;
    for (size_t i = 0; i < word_count; i++)
        words[i] = expected_word(allocation->signature, i);

    uint64_t tail = expected_word(allocation->signature, word_count);
    size_t tail_start = word_count * sizeof(uint64_t);
    for (size_t i = tail_start; i < allocation->size; i++) {
        size_t byte_index = i - tail_start;
        allocation->pointer[i] = (uint8_t)(tail >> (byte_index * 8));
    }
}

static int fail(const char *reason, uint32_t slot, size_t offset)
{
    os64_hprintf(OS64_STDERR,
                 "mallochavoc: FAIL: %s at operation %lu, slot %u, offset %lu\n",
                 reason, gOperation, slot, (uint64_t)offset);
    os64_hprintf(OS64_STDERR,
                 "mallochavoc: reproduce with: mallochavoc -s %lu -n %lu\n",
                 gSeed, gOperation + 1);
    return 1;
}

static int verify_slot(uint32_t slot)
{
    allocation_t *allocation = &gAllocations[slot];
    if (allocation->pointer == NULL)
        return 0;
    if (((uintptr_t)allocation->pointer & 15U) != 0)
        return fail("misaligned pointer", slot, 0);
    size_t word_count = allocation->size / sizeof(uint64_t);
    const uint64_t *words = (const uint64_t *)allocation->pointer;
    for (size_t i = 0; i < word_count; i++) {
        if (words[i] != expected_word(allocation->signature, i))
            return fail("live allocation contents changed", slot,
                        i * sizeof(uint64_t));
    }

    uint64_t tail = expected_word(allocation->signature, word_count);
    size_t tail_start = word_count * sizeof(uint64_t);
    for (size_t i = tail_start; i < allocation->size; i++) {
        size_t byte_index = i - tail_start;
        uint8_t expected = (uint8_t)(tail >> (byte_index * 8));
        if (allocation->pointer[i] != expected)
            return fail("live allocation contents changed", slot, i);
    }
    return 0;
}

static bool ranges_overlap(const allocation_t *a, const allocation_t *b)
{
    if (a->pointer == b->pointer)
        return true;
    if (a->size == 0 || b->size == 0)
        return false;
    uintptr_t a0 = (uintptr_t)a->pointer;
    uintptr_t b0 = (uintptr_t)b->pointer;
    return a0 < b0 + b->size && b0 < a0 + a->size;
}

static int verify_all(void)
{
    for (uint32_t i = 0; i < SLOT_COUNT; i++) {
        if (verify_slot(i) != 0)
            return 1;
        if (gAllocations[i].pointer == NULL)
            continue;
        for (uint32_t j = i + 1; j < SLOT_COUNT; j++) {
            if (gAllocations[j].pointer != NULL &&
                ranges_overlap(&gAllocations[i], &gAllocations[j]))
                return fail("live allocations overlap", j, 0);
        }
    }
    if (os64_heap_verify() != 0)
        return fail("heap verifier reported damage", 0, 0);
    return 0;
}

static size_t random_size(void)
{
    static const size_t edges[] = {
        0, 1, 2, 7, 8, 15, 16, 17, 23, 24, 31, 32, 33,
        63, 64, 65, 127, 128, 129, 255, 256, 257,
        511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049,
        4095, 4096, 4097, 8191, 8192, 8193, 65535, 65536, 65537
    };
    uint64_t choice = random64() % 16;
    if (choice < 10)
        return edges[random64() % (sizeof(edges) / sizeof(edges[0]))];
    if (choice == 10)
        return (size_t)(random64() % (1024U * 1024U + 1U));
    return (size_t)(random64() % 16385U);
}

static int install_allocation(uint32_t slot, void *pointer, size_t size)
{
    if (pointer == NULL)
        return 0; // Resource exhaustion is legal; corruption is not.
    if (((uintptr_t)pointer & 15U) != 0)
        return fail("new allocation is misaligned", slot, 0);

    allocation_t candidate = {
        .pointer = pointer,
        .size = size,
        .signature = random64()
    };
    for (uint32_t i = 0; i < SLOT_COUNT; i++) {
        if (i != slot && gAllocations[i].pointer != NULL &&
            ranges_overlap(&candidate, &gAllocations[i]))
            return fail("new allocation overlaps a live allocation", slot, 0);
    }
    gAllocations[slot] = candidate;
    fill(&gAllocations[slot]);
    return 0;
}

static int allocate_slot(uint32_t slot, bool use_calloc)
{
    size_t size = random_size();
    void *pointer;

    if (use_calloc && size != 0) {
        size_t count = (size_t)(random64() % 31U) + 1U;
        size_t width = size / count + (size % count != 0 ? 1U : 0U);
        size = count * width;
        pointer = os64_calloc(count, width);
        if (pointer != NULL) {
            uint8_t *bytes = pointer;
            for (size_t i = 0; i < size; i++) {
                if (bytes[i] != 0)
                    return fail("calloc returned non-zero data", slot, i);
            }
        }
    } else {
        pointer = os64_malloc(size);
    }
    return install_allocation(slot, pointer, size);
}

static int reallocate_slot(uint32_t slot)
{
    allocation_t old = gAllocations[slot];
    if (verify_slot(slot) != 0)
        return 1;

    size_t new_size = random_size();
    uint8_t *replacement = os64_realloc(old.pointer, new_size);
    if (new_size == 0) {
        if (replacement != NULL)
            return fail("realloc(p, 0) did not return NULL", slot, 0);
        gAllocations[slot] = (allocation_t){0};
        return 0;
    }
    if (replacement == NULL)
        return verify_slot(slot); // Failure must leave the old object alive.

    gAllocations[slot].pointer = replacement;
    gAllocations[slot].size = old.size < new_size ? old.size : new_size;
    if (verify_slot(slot) != 0)
        return 1;

    gAllocations[slot].size = new_size;
    gAllocations[slot].signature = random64();
    for (uint32_t i = 0; i < SLOT_COUNT; i++) {
        if (i != slot && gAllocations[i].pointer != NULL &&
            ranges_overlap(&gAllocations[slot], &gAllocations[i]))
            return fail("reallocated object overlaps a live allocation", slot, 0);
    }
    fill(&gAllocations[slot]);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *seed_text = NULL;
    const char *ops_text = NULL;
    const os64_optspec_t specs[] = {
        {'s', "seed", true, "decimal random seed (printed for reproduction)",
         .value_out = &seed_text},
        {'n', "operations", true, "number of random operations (default 50000)",
         .value_out = &ops_text},
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Hammer malloc/calloc/realloc/free as a black-box state machine.";
    args.details = "Live contents, alignment, overlap, calloc zeroing, and heap integrity are checked throughout.";
    int32_t parsed = os64_args_parse(&args, "mallochavoc [-s SEED] [-n OPERATIONS]", NULL, 0);
    if (parsed == OS64_ARG_HELP)
        return 0;
    if (parsed < 0)
        return 2;

    uint64_t operations = DEFAULT_OPS;
    if (seed_text != NULL) {
        if (!parse_uint(seed_text, &gSeed) || gSeed == 0) {
            os64_hprintf(OS64_STDERR, "mallochavoc: seed must be a non-zero decimal integer\n");
            return 2;
        }
    } else {
        // Different boot timing gives an exploratory run a fresh path.  The
        // printed seed still turns every discovery into a deterministic run.
        os64_ticks_t ticks = {0};
        gSeed = SEED_SCRAMBLE;
        if (os64_ticks(&ticks) >= 0)
            gSeed ^= ticks.ticks + ((uint64_t)ticks.per_second << 32);
        if (gSeed == 0)
            gSeed = SEED_SCRAMBLE;
    }
    if (gSeed == 0) {
        os64_hprintf(OS64_STDERR, "mallochavoc: seed must be a non-zero decimal integer\n");
        return 2;
    }
    if (ops_text != NULL && (!parse_uint(ops_text, &operations) || operations == 0)) {
        os64_hprintf(OS64_STDERR, "mallochavoc: operations must be a positive decimal integer\n");
        return 2;
    }
    gRandom = gSeed;

    os64_printf("mallochavoc: seed=%lu operations=%lu slots=%u\n",
                gSeed, operations, SLOT_COUNT);

    // Contract edges before chaos makes their failure easy to identify.
    os64_free(NULL);
    void *zero = os64_malloc(0);
    if (zero == NULL || ((uintptr_t)zero & 15U) != 0)
        return fail("malloc(0) was NULL or misaligned", 0, 0);
    os64_free(zero);
    if (os64_calloc(SIZE_MAX, 2) != NULL)
        return fail("calloc multiplication overflow succeeded", 0, 0);
    void *realloc_zero = os64_realloc(NULL, 0);
    if (realloc_zero == NULL || ((uintptr_t)realloc_zero & 15U) != 0)
        return fail("realloc(NULL, 0) was NULL or misaligned", 0, 0);
    os64_free(realloc_zero);

    for (gOperation = 0; gOperation < operations; gOperation++) {
        uint32_t slot = (uint32_t)(random64() % SLOT_COUNT);
        allocation_t *allocation = &gAllocations[slot];
        if (allocation->pointer == NULL) {
            if (allocate_slot(slot, (random64() & 3U) == 0) != 0)
                return 1;
        } else {
            uint64_t action = random64() % 10;
            if (action < 4) {
                if (verify_slot(slot) != 0)
                    return 1;
                os64_free(allocation->pointer);
                *allocation = (allocation_t){0};
            } else if (action < 8) {
                if (reallocate_slot(slot) != 0)
                    return 1;
            } else if (verify_slot(slot) != 0) {
                return 1;
            }
        }

        // Probe an unrelated live object after every mutation; periodically
        // walk every byte and ask the public heap checker for its own verdict.
        if (verify_slot((uint32_t)(random64() % SLOT_COUNT)) != 0)
            return 1;
        if ((gOperation + 1) % FULL_CHECK_EVERY == 0 && verify_all() != 0)
            return 1;
    }

    if (verify_all() != 0)
        return 1;

    // Keep the completed workload and its heap report observable.  The heap
    // has passed its final in-use verification, but its live allocation mix
    // remains intact until the key arrives; then cleanup gets tested too.
    os64_puts("mallochavoc: iterations complete; press a key to clean up and end\n");
    char key;
    (void)os64_read(OS64_STDIN, &key, 1);

    for (uint32_t i = 0; i < SLOT_COUNT; i++) {
        os64_free(gAllocations[i].pointer);
        gAllocations[i] = (allocation_t){0};
    }
    if (os64_heap_verify() != 0)
        return fail("heap damage after final cleanup", 0, 0);

    os64_printf("mallochavoc: PASS seed=%lu operations=%lu\n", gSeed, operations);
    os64_debug_log("mallochavoc: PASS");
    return 0;
}
