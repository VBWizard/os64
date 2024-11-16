#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>
#include "time.h"

extern uint64_t kTicksSinceStart;
extern time_t kSystemCurrentTime;

void handler_irq0_c();

#endif