// Minimal PIE executable that dynamically links against libtest.so (see
// kernel/test/shlib/libtest.c), used to regression-test os64's dynamic
// linking support end to end: DT_NEEDED loading, symbol resolution,
// GLOB_DAT/JUMP_SLOT relocation, and CoW privatization of a shared
// library's writable .data on write.
//
// Calls shlib_add twice and packs both results into one 32-bit value so a
// single task retVal can prove both halves of the CoW story:
//   - call 1: shlib_counter starts at the library's pristine value (42,
//     shared with every other task that hasn't written to it yet) and
//     becomes 43 in THIS task's now-privatized copy.
//   - call 2: reads back 43 from that same private copy (not the original
//     42), proving the privatized page persists correctly within a task.
// Two different tasks should therefore see IDENTICAL packed results,
// despite writing to what started out as the same physical page — if CoW
// isolation were broken, the second task to run would instead see
// call 1 start from the first task's already-incremented value.

extern int shlib_add(int a, int b);

// COM1 is port-mapped I/O, not memory-mapped — the `out` instruction, not a
// pointer dereference to 0x3f8 (which isn't a valid memory address at all
// and page-faults). Matches serial_ping.S's `out dx, al`. Intel syntax
// (dest, src), like all asm in this project — DYN_CFLAGS passes -masm=intel.
static inline void outb(unsigned short port, unsigned char val)
{
    __asm__ volatile ("out dx, al" : : "a"(val), "d"(port));
}

static void putc_serial(char c)
{
    outb(0x3f8, (unsigned char)c);
}

static void puts_serial(const char *s)
{
    while (*s) {
        putc_serial(*s++);
    }
}

static void print_hex(unsigned int v)
{
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        unsigned int nib = (v >> (i * 4)) & 0xF;
        buf[7 - i] = nib < 10 ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[8] = 0;
    puts_serial(buf);
}

// RAX on return is captured by task_exit_with_retval, same convention as
// serial_ping.S — a plain `return` from a C _start naturally puts the
// value in RAX before the compiler's generated `ret`.
int _start(void)
{
    int first = shlib_add(2, 3);
    int second = shlib_add(2, 3);
    unsigned int packed = ((unsigned int)first << 16) | (unsigned int)(second & 0xFFFF);

    puts_serial("dyn_consumer: first=0x");
    print_hex((unsigned int)first);
    puts_serial(" second=0x");
    print_hex((unsigned int)second);
    puts_serial(" packed=0x");
    print_hex(packed);
    puts_serial("\n");

    return (int)packed;
}
