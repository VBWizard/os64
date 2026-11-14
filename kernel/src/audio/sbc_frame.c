// sbc_frame.c — an SBC stream's settings and the arithmetic they decide.
// sbc_frame.h is the contract; nothing here knows the encoder.

#include <stddef.h>

#include "audio/sbc_frame.h"

bool sbc_settings_valid(const sbc_settings_t *settings)
{
	if (settings == NULL)
		return false;
	if (settings->rate != 44100 && settings->rate != 48000)
		return false;
	if (settings->channels != 2 || settings->blocks != 16 || settings->subbands != 8)
		return false;
	return settings->bitpool >= SBC_BITPOOL_MIN && settings->bitpool <= SBC_BITPOOL_MAX;
}

uint32_t sbc_frame_bytes(const sbc_settings_t *settings)
{
	if (!sbc_settings_valid(settings))
		return 0;
	uint32_t subbands = settings->subbands;
	uint32_t join_bits = settings->joint ? subbands : 0;
	uint32_t sample_bits = join_bits + (uint32_t)settings->blocks * settings->bitpool;
	return 4 + (4 * subbands * settings->channels) / 8 + (sample_bits + 7) / 8;
}

uint32_t sbc_frame_samples(const sbc_settings_t *settings)
{
	if (!sbc_settings_valid(settings))
		return 0;
	return (uint32_t)settings->blocks * settings->subbands;
}

uint32_t sbc_bit_rate(const sbc_settings_t *settings)
{
	if (!sbc_settings_valid(settings))
		return 0;
	uint64_t bits = 8ull * sbc_frame_bytes(settings) * settings->rate;
	return (uint32_t)(bits / sbc_frame_samples(settings));
}
