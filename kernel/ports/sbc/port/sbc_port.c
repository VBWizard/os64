// sbc_port.c — the SBC encoder's boundary: sbc_port.h is the contract,
// kernel/ports/sbc/README.md the record of what was read upstream to write
// it. The settings the encoder is given are checked first, and every frame
// it gives back is checked before anyone sees it.

#include "sbc_switches.h"

#include "sbc_encoder.h"
#include "sbc_port.h"

// SBC_Encode is handed no capacity, so it writes into this buffer, sized for
// the longest frame any accepted setting gives, and never into the caller's.
// After it lie guard bytes the encoder must leave alone. A test build makes
// the room smaller (tools/test_sbc_host.sh) to prove the guard is read.
#ifndef SBC_PORT_FRAME_ROOM
#define SBC_PORT_FRAME_ROOM SBC_FRAME_BYTES_MAX
#endif
#define SBC_PORT_GUARD_BYTES 16u
#define SBC_PORT_GUARD_FILL  0xA5u

// The top of the bit-rate search, in kbit/s. At it every accepted bitpool is
// exceeded (Init clamps to 255 there), and Init's 16-bit intermediates (the
// frame length and the bit rate it computes back) stay in range. A test
// build lowers it so that the search cannot reach the bitpool asked, to
// prove begin then refuses.
#ifndef SBC_PORT_RATE_CEILING_KBPS
#define SBC_PORT_RATE_CEILING_KBPS 4096u
#endif

static SBC_ENC_PARAMS s_params;
static uint8_t s_frame[SBC_PORT_FRAME_ROOM + SBC_PORT_GUARD_BYTES];
static uint32_t s_frame_bytes;
static bool s_ready;

#ifdef SBC_PORT_TEST_HOOKS
// The host proof reaches the encoder's parameters, to make it produce a
// frame of the wrong length, and its frame buffer, to compare what was
// copied out with what was made. Never in the kernel's build.
SBC_ENC_PARAMS *sbc_port_test_params(void);
SBC_ENC_PARAMS *sbc_port_test_params(void)
{
	return &s_params;
}
const uint8_t *sbc_port_test_frame(void);
const uint8_t *sbc_port_test_frame(void)
{
	return s_frame;
}
#endif

static void settle(uint32_t kbps)
{
	s_params.u16BitRate = (uint16_t)kbps;
	SBC_Encoder_Init(&s_params);
}

bool sbc_port_begin(const sbc_settings_t *settings)
{
	s_ready = false;
	if (!sbc_settings_valid(settings))
		return false;

	uint8_t *raw = (uint8_t *)&s_params;
	for (size_t i = 0; i < sizeof(s_params); i++)
		raw[i] = 0;
	// sbc_settings_valid admitted only the values these name.
	s_params.s16SamplingFreq = settings->rate == 48000 ? SBC_sf48000 : SBC_sf44100;
	s_params.s16ChannelMode = settings->joint ? SBC_JOINT_STEREO : SBC_STEREO;
	s_params.s16NumOfSubBands = settings->subbands;
	s_params.s16NumOfBlocks = settings->blocks;
	s_params.s16AllocationMethod = settings->snr ? SBC_SNR : SBC_LOUDNESS;
	s_params.Format = SBC_FORMAT_GENERAL;

	// The encoder's knob is a bit rate: SBC_Encoder_Init computes the bitpool
	// from u16BitRate, corrects it once and clamps it, and writes it over
	// s16BitPool. Rather than restate that arithmetic here, ask Init itself
	// for the least rate whose bitpool is at least the one asked, by halving
	// (a higher rate never gives a smaller bitpool). Each Init also clears
	// the filter history, so the last one leaves a fresh encoder.
	uint32_t lo = 1;
	uint32_t hi = SBC_PORT_RATE_CEILING_KBPS;
	while (lo < hi)
	{
		uint32_t mid = lo + (hi - lo) / 2;
		settle(mid);
		if (s_params.s16BitPool >= settings->bitpool)
			hi = mid;
		else
			lo = mid + 1;
	}
	settle(lo);
	// Whatever the search found, the encoder runs at the bitpool it now
	// holds, and that must be the one asked for.
	if (s_params.s16BitPool != settings->bitpool)
		return false;

	s_frame_bytes = sbc_frame_bytes(settings);
	for (uint32_t i = 0; i < SBC_PORT_GUARD_BYTES; i++)
		s_frame[SBC_PORT_FRAME_ROOM + i] = SBC_PORT_GUARD_FILL;
	s_ready = true;
	return true;
}

static bool fail(void)
{
	s_ready = false;
	return false;
}

bool sbc_port_encode(const int16_t *pcm, uint8_t *out, size_t capacity)
{
	if (!s_ready)
		return false;
	if (pcm == NULL || out == NULL || capacity < s_frame_bytes)
		return fail();

	// SBC_Encode takes the PCM as int16_t *, but only reads it: the analysis
	// filters copy each sample into their own history (s16X) and nothing is
	// stored through the pointer. So the caller's const buffer is passed as it
	// is, not copied.
	uint32_t length = SBC_Encode(&s_params, (int16_t *)pcm, s_frame);
	if (length != s_frame_bytes)
		return fail();
	for (uint32_t i = 0; i < SBC_PORT_GUARD_BYTES; i++)
		if (s_frame[SBC_PORT_FRAME_ROOM + i] != SBC_PORT_GUARD_FILL)
			return fail();

	for (uint32_t i = 0; i < length; i++)
		out[i] = s_frame[i];
	return true;
}
