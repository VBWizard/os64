// sbc_frame.h — an SBC stream's settings, and the arithmetic they decide
// (docs/design/pending/AUDIO_A2DP.md § Part E).
//
// PURE: no kernel headers. The encoder's boundary (kernel/ports/sbc/port/
// sbc_port.h) checks its settings and its frames against these. They are
// written for AVDTP and the A2DP pump as well, which need to know what may
// be configured and how long a frame is without knowing the encoder.
// tools/test_sbc_host.sh proves them; tools/test_sbc_frame_mutants.py
// breaks each rule.

#ifndef SBC_FRAME_H
#define SBC_FRAME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct sbc_settings
{
	uint32_t rate;        // 44100 or 48000
	uint8_t  channels;    // 2
	bool     joint;       // joint stereo; false is plain stereo
	uint8_t  blocks;      // 16
	uint8_t  subbands;    // 8
	bool     snr;         // false: loudness
	uint8_t  bitpool;     // SBC_BITPOOL_MIN to SBC_BITPOOL_MAX
} sbc_settings_t;

// The range an A2DP SBC configuration may name (the A2DP specification's
// minimum and maximum bitpool fields; BlueZ's a2dp-codecs.h spells them
// SBC_MIN_BITPOOL and SBC_MAX_BITPOOL). It is inside what the frame itself
// allows for two channels (32 × subbands, which libsbc and FFmpeg's decoder
// both enforce), so every value here is a frame a decoder takes. What a
// particular pair of headphones takes is AVDTP's business: they name their
// own maximum, and it is usually 53.
#define SBC_BITPOOL_MIN 2u
#define SBC_BITPOOL_MAX 250u

// The longest frame any accepted setting gives: joint stereo at the largest
// bitpool, sbc_frame_bytes' formula at 8 subbands, 2 channels, 16 blocks.
#define SBC_FRAME_BYTES_MAX (4u + (4u * 8u * 2u) / 8u + (8u + 16u * SBC_BITPOOL_MAX + 7u) / 8u)

// What the encoder's boundary takes: 44.1 or 48 kHz, two channels, joint
// or plain stereo, 16 blocks, 8 subbands, loudness or SNR, a bitpool in
// range. Everything else is refused, including what the specification
// allows and A.3 does not use (mono, dual channel, 4 subbands, fewer
// blocks), so that nothing the proof did not cover can reach the encoder.
bool sbc_settings_valid(const sbc_settings_t *settings);

// One frame's length in bytes, for stereo and joint stereo (the A2DP
// specification's SBC appendix; libsbc's sbc_get_frame_length agrees):
//   4 + (4 × subbands × channels) / 8 + ceil((join × subbands + blocks × bitpool) / 8)
// 115 at 48 kHz joint stereo, 16 blocks, 8 subbands, bitpool 51.
// 0 for settings sbc_settings_valid refuses.
uint32_t sbc_frame_bytes(const sbc_settings_t *settings);

// PCM frames (one sample per channel) one SBC frame carries: blocks ×
// subbands, 128 for every accepted setting. 0 for refused settings.
uint32_t sbc_frame_samples(const sbc_settings_t *settings);

// Bits a second: 8 × frame bytes × rate / (subbands × blocks), rounded
// down (at 44.1 kHz it is not a whole number). 345000 at the settings above.
// 0 for refused settings.
uint32_t sbc_bit_rate(const sbc_settings_t *settings);

#endif
