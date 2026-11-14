// sbc_port.h — everything the kernel may call of the SBC encoder
// (docs/design/pending/AUDIO_A2DP.md § The boundary; kernel/ports/sbc/
// README.md is the port's record).
//
// There is ONE encoder in the machine: upstream keeps its filter history in
// file-scope statics and globals, so a second sbc_port_begin does not make a
// second encoder, it restarts the one there is with the new settings. The
// caller serializes every call: nothing here takes a lock.

#ifndef SBC_PORT_H
#define SBC_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/sbc_frame.h"

// Starts (or restarts) the encoder. False: the settings are not ones
// sbc_settings_valid accepts, or the encoder did not settle on exactly the
// bitpool asked for; the encoder is then not usable until a begin succeeds.
bool sbc_port_begin(const sbc_settings_t *settings);

// One frame: exactly sbc_frame_samples(settings) frames of interleaved
// 16-bit PCM in (left, right, left, ...), exactly sbc_frame_bytes(settings)
// bytes out at `out`. The PCM is only read. False: there is no begun
// encoder, a buffer is NULL, `capacity` is less than a frame, or the
// encoder produced a frame of another length or wrote past its buffer;
// nothing is copied to `out`, and every call is false until the next
// successful sbc_port_begin.
bool sbc_port_encode(const int16_t *pcm, uint8_t *out, size_t capacity);

#endif
