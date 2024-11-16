#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

extern uint64_t kTicksSinceStart;
extern uint64_t kSystemCurrentTime;

void handler_irq0_c();

#endif