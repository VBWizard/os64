#include "io.h"
#include "time.h"

static volatile int serial_lock = 0;

static inline void serial_lock_acquire() {
    while (__sync_lock_test_and_set(&serial_lock, 1)) {
        /* spin */
    }
}

static inline void serial_lock_release() {
    __sync_lock_release(&serial_lock);
}

static inline void serial_write_char(int port, char a) {
    outb(port, a);
    __asm__("nop\nnop\nnop\n");
}

int init_serial(int port) {
    serial_lock_acquire();
    outb(COM1 + 1, 0x00); // Disable all interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(COM1 + 0, 0x03); // Divisor low byte (38400 baud)
    outb(COM1 + 1, 0x00); // Divisor high byte
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
    // Check if serial is faulty (i.e: not same byte as sent)
    if (inb(port + 0) != 0xAE) {
        serial_lock_release();
        return 1;
    }

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    outb(port + 4, 0x0F);
    serial_lock_release();
    return 0;
}

int is_transmit_empty(int port) {
    return inb(port + 5) & 0x20;
}

void write_serial(int port, char a) {
    serial_lock_acquire();
    serial_write_char(port, a);
    serial_lock_release();
}

// Implemented to handle processing a string and writing all the bytes via write_serial()
void serial_print_string(const char *message) {
    serial_lock_acquire();
    for (const char *c = message; *c; c++) {
        serial_write_char(COM1, *c);
    }
    serial_lock_release();
}
