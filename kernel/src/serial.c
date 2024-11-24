#include "io.h"
#include "time.h"

int init_serial(int port) {
    outb(COM1 + 1, 0x00); // Disable all interrupts
    outb(COM1 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(COM1 + 0, 0x03); // Divisor low byte (38400 baud)
    outb(COM1 + 1, 0x00); // Divisor high byte
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7); // Enable FIFO, clear them, 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
	// Check if serial is faulty (i.e: not same byte as sent)
	if(inb(port + 0) != 0xAE) {
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
   while (is_transmit_empty(port) == 0){}
   outb(port,a);
   wait(30);
}
