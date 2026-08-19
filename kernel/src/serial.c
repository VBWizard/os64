#include <stdbool.h>
#include "io.h"
#include "time.h"
#include "CONFIG.h"

static volatile int serial_lock = 0;

static inline void serial_write_char(int port, char a) {
    outb(port, a);
}

// Returns 0 if a real UART answered, non-zero if nothing is there.
//
// THE PROBE USED TO BE A NO-OP, in two ways that hid each other (found
// 2026-08-18): it compared the receive register against 0xAE without ever
// entering loopback or WRITING 0xAE — so it was reading whatever happened to
// be in RBR — and every caller discarded the result anyway. The kernel
// therefore had no idea whether a serial port existed, which mattered
// enormously the day it started draining the log into one that didn't (see
// kSerialPresent's use in log.c).
//
// The real test is the classic one: put the UART in loopback, send a byte,
// and see whether it comes back. Nothing there means the read returns 0xFF
// (open bus) and we say so.
//
// Note the register writes below use `port`, not COM1. The old code mixed
// them, which worked only because the single caller passes COM1.
int init_serial(int port) {
    outb(port + 1, 0x00); // Disable all interrupts
    outb(port + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x01); // Divisor low byte (115200 baud)
    outb(port + 1, 0x00); // Divisor high byte
    outb(port + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold
    outb(port + 4, 0x0B); // IRQs enabled, RTS/DSR set

    // Loopback: MCR bit 4, with OUT1/OUT2/RTS set so the byte has a path home.
    outb(port + 4, 0x1E);
    outb(port + 0, 0xAE);
    if (inb(port + 0) != 0xAE) {
        return 1;   // nothing answered — no UART at this address
    }

    // Real device: back to normal operation (not-loopback, IRQs enabled,
    // OUT#1 and OUT#2 set).
    outb(port + 4, 0x0F);
    return 0;
}

int is_transmit_empty(int port) {
    return inb(port + 5) & 0x20;
}

void write_serial(int port, char a) {
    serial_write_char(port, a);
}

// Implemented to handle processing a string and writing all the bytes via write_serial()
// Set from init_serial's probe (or forced by SERIAL= on the cmdline), and read
// by everything that would otherwise talk to a port that isn't there. Defaults
// TRUE so that every write before the probe still goes out — those few lines
// are the ones you need when the probe itself is what went wrong.
bool kSerialPresent = true;

void serial_print_string(const char *message) {
    // No UART, no writing. This is not an optimization: on a machine with no
    // serial port (the P5), every byte sent here is a byte destroyed, and the
    // log drainer used to consume the rings into exactly this hole. Refusing
    // here is the backstop; log.c refuses to DRAIN in the first place.
    if (!kSerialPresent)
        return;

#if SERIAL_WAIT_FOR_TRANSMIT
    // Burst up to 16 bytes per wait (matches the 16550 TX FIFO depth).
    // THRE (LSR bit 5) in FIFO mode means the TX FIFO is completely empty,
    // so after each wait there are exactly 16 free slots — no overflow risk.
    // count resets to 0 at the start of each call so back-to-back calls
    // also wait before their first byte.
    int count = 0;
    for (const char *c = message; *c; c++) {
        if (count == 0)
            while (!is_transmit_empty(COM1));
        serial_write_char(COM1, *c);
        count = (count + 1) % 16;
    }
#else
    for (const char *c = message; *c; c++)
        serial_write_char(COM1, *c);
#endif
}
