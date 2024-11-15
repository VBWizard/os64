#include "io.h"


int init_serial(int port) {
   outb(port + 1, 0x00);    // Disable all interrupts
   outb(port + 3, 0x80);    // Enable DLAB (set baud rate divisor)
   outb(port + 0, 0x06);    // Set divisor to 6 (19200 baud if clock is 1.8432 MHz)
   outb(port + 1, 0x00);    //                  (high byte)
   outb(port + 3, 0x03);    // 8 bits, no parity, one stop bit
   outb(port + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
   outb(port + 4, 0x0B);    // IRQs enabled, RTS/DSR set
   outb(port + 4, 0x1E);    // Set in loopback mode, test the serial chip
   outb(port + 0, 0xAE);    // Test serial chip (send byte 0xAE and check if serial returns same byte)
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
   asm("nop\n");
}
