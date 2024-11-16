#include "driver/system/irq.h"
#include "CONFIG.h"
#include "io.h"

uint64_t kTicksSinceStart;
uint64_t kSystemCurrentTime;

/// @brief Handles the PIT timer signal
__attribute__((used))
void handler_irq0_c()
{
	kTicksSinceStart++;
	if (kTicksSinceStart % TICKS_PER_SECOND == 0)
		kSystemCurrentTime++;
	return;
}