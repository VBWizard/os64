#ifndef PIC_H
#define PIC_H

#include <stdint.h>
#include <stdbool.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define ICW1_INIT    0x11
#define ICW4_8086    0x01
#define PIC1_DATA 	 0x21
#define PIC2_DATA 	 0xA1

void pic_remap(int offset1, int offset2);
int pic_irq0_mapping();

#endif