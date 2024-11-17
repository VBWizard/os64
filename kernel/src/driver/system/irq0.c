#include "driver/system/irq.h"
#include "CONFIG.h"
#include "io.h"
#include "time.h"

volatile uint64_t kTicksSinceStart;
/// @brief Uptime in seconds
volatile uint64_t kUptime;
extern volatile time_t kSystemCurrentTime;

/// @brief Handles the PIT timer signal
__attribute__((used))
void handler_irq0_c()
{
	kTicksSinceStart++;
	if (kTicksSinceStart % TICKS_PER_SECOND == 0)
	{
		kUptime++;
		kSystemCurrentTime++;
	}
	return;
}