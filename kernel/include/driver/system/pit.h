#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_COMMAND_PORT 0x43
#define PIT_CHANNEL0_PORT 0x40
#define PIT_FREQUENCY 1193180

void initialize_pit_timer(uint32_t frequency);

#endif