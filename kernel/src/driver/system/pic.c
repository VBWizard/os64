#include "driver/system/pic.h"
#include "io.h"

void pic_remap(int offset1, int offset2) {
    // Save masks
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    // Start initialization sequence
    outb(PIC1_COMMAND, ICW1_INIT);
    outb(PIC2_COMMAND, ICW1_INIT);

    // Set vector offsets
    outb(PIC1_DATA, offset1);
    outb(PIC2_DATA, offset2);

    // Tell PICs how they are wired together
    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    // Set PICs to 8086/88 mode
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    // Restore masks
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);

	outb(PIC1_DATA, inb(0x21) & ~0x01); // Unmask IRQ0 on the master PIC
	outb(PIC2_DATA, inb(0xA1));         // Keep the slave PIC mask unchanged

}

uint8_t get_master_pic_offset() {
    return inb(PIC1_DATA);
}

uint8_t get_slave_pic_offset() {
    return inb(PIC2_DATA);
}

int pic_irq0_mapping() {
    uint8_t master_offset = get_master_pic_offset();
	return master_offset;
}
