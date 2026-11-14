// bt_a2dp_seam.c — the A2DP pump's ops: the audio seam (audio.h), the SBC
// encoder's boundary (sbc_port.h) and the microsecond clock. bt_a2dp_seam.h
// is the contract. This object is the one tools/test_sbc_audit.py names as
// the boundary's caller, which is why the table lives in a file of its own
// rather than in xhci.c.

#include "driver/system/usb/bt_a2dp_seam.h"
#include "audio/audio.h"
#include "kusb.h"
#include "micros.h"
// The boundary's header lives with the port, outside the kernel's include
// path (kernel/ports/sbc/README.md): reached by its place in the tree.
#include "../../../../ports/sbc/port/sbc_port.h"

static void seam_nudge(void *ctx)
{
	(void)ctx;
	doorbell_ring(&kUsbDoorbell);
}

static const audio_sink_t s_sink = { .name = "a2dp", .nudge = seam_nudge, .ctx = NULL };

static bt_a2dp_pulled_t seam_pull(void *ctx, int16_t *out, uint32_t frames)
{
	(void)ctx;
	audio_pulled_t r = audio_pull(&s_sink, out, frames);
	return (bt_a2dp_pulled_t){ .frames = r.frames, .generation = r.generation, .paused = r.paused, .yours = r.yours };
}

static bool seam_has_sound(void *ctx)
{
	(void)ctx;
	return audio_sink_has_sound(&s_sink);
}

static bool seam_sound(void *ctx, bool *paused)
{
	(void)ctx;
	return audio_sink_sound(&s_sink, paused);
}

static void seam_attach(void *ctx)
{
	(void)ctx;
	audio_sink_attach(&s_sink);
}

static void seam_detach(void *ctx)
{
	(void)ctx;
	audio_sink_detach(&s_sink);
}

// `why` is a string literal (bt_a2dp.h), so it outlives the sink's
// attachment as audio_sink_failed asks.
static void seam_failed(void *ctx, const char *why)
{
	(void)ctx;
	audio_sink_failed(&s_sink, why);
}

static void seam_delay(void *ctx, uint32_t delay_us)
{
	(void)ctx;
	audio_sink_delay(&s_sink, delay_us);
}

static bool seam_encoder_begin(void *ctx, const sbc_settings_t *settings)
{
	(void)ctx;
	return sbc_port_begin(settings);
}

static bool seam_encoder_encode(void *ctx, const int16_t *pcm, uint8_t *out, size_t capacity)
{
	(void)ctx;
	return sbc_port_encode(pcm, out, capacity);
}

static bool seam_owns_volume(void *ctx, bool owned)
{
	(void)ctx;
	return audio_sink_owns_volume(&s_sink, owned);
}

static bool seam_volume_set(void *ctx, uint8_t volume)
{
	(void)ctx;
	return audio_sink_volume_set(&s_sink, volume);
}

static uint8_t seam_volume_now(void *ctx)
{
	(void)ctx;
	return (uint8_t)audio_sink_volume_now(&s_sink);   // 0..100
}

static uint64_t seam_clock_us(void *ctx)
{
	(void)ctx;
	return micros_now();
}

const bt_a2dp_ops_t kBtA2dpSeamOps = {
	.pull = seam_pull,
	.has_sound = seam_has_sound,
	.attach = seam_attach,
	.detach = seam_detach,
	.failed = seam_failed,
	.delay = seam_delay,
	.encoder_begin = seam_encoder_begin,
	.encoder_encode = seam_encoder_encode,
	.clock_us = seam_clock_us,
	.sound = seam_sound,
	.owns_volume = seam_owns_volume,
	.volume_set = seam_volume_set,
	.volume_now = seam_volume_now,
};
