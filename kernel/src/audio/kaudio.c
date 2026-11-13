// kaudio.c — the null sink's pump (audio.h; AUDIO_SEAM.md § The ring, and
// who empties it). knet's loop where the doorbell is concerned: take the
// edge before the work, park only if it is still clear. The tick rings the
// bell while kAudioClocked says the null sink has a stream to keep time
// for, so an idle machine pays for a parked thread and nothing else.

#include "audio/audio.h"
#include "doorbell.h"
#include "smp_core.h"
#include "CONFIG.h"
#include "serial_logging.h"

doorbell_t kAudioDoorbell;
task_t    *kKaudioTask;

// The nap between rings. Never the cadence anything runs at: the backstop
// for a ring that lands in the one window the scheduler's service points do
// not cover (DOORBELL.md).
#define KAUDIO_BACKSTOP_TICKS (TICKS_PER_SECOND)

void kaudio_thread(void)
{
	thread_t *self = get_core_local_storage()->currentThread;
	doorbell_register(&kAudioDoorbell, self, "kaudio");
	printd(DEBUG_BOOT, "kaudio: running on CPU %u\n", get_core_local_storage()->cpu);

	for (;;)
	{
		(void)doorbell_take(&kAudioDoorbell);
		// Takes what the clock says is due, or nothing when the null sink
		// is not the sink or has no work: a backstop wake costs one look.
		audio_null_pump();
		(void)doorbell_park(&kAudioDoorbell, KAUDIO_BACKSTOP_TICKS);
	}
}
