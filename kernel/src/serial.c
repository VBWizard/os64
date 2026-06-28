#include "io.h"
#include "time.h"
#include "CONFIG.h"

static volatile int serial_lock = 0;

static inline void serial_write_char(int port, char a) {
    outb(port, a);
}

int init_serial(int port) {
    outb(COM1 + 1, 0x00); // Disable all interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(COM1 + 0, 0x01); // Divisor low byte (115200 baud)
    outb(COM1 + 1, 0x00); // Divisor high byte
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
    // Check if serial is faulty (i.e: not same byte as sent)
    if (inb(port + 0) != 0xAE) {
        return 1;
    }

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
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
void serial_print_string(const char *message) {
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
