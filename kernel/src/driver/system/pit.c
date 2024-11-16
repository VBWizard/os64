#include "driver/system/pit.h"
#include "io.h"

void initialize_pit_timer(uint32_t frequency) 
{
	//Lowest frequency that the pit can handle is 19
	if (frequency < 19)
		frequency = 19;

    uint16_t divisor = (uint16_t)(PIT_FREQUENCY / frequency);
    
    // Send command byte: Channel 0, Access Mode (lobyte/hibyte), Mode 2 (rate generator)
    outb(PIT_COMMAND_PORT, 0x36);
    // Send low byte of divisor
    outb(PIT_CHANNEL0_PORT, divisor & 0xFF);
    // Send high byte of divisor
    outb(PIT_CHANNEL0_PORT, (divisor >> 8) & 0xFF);
}
