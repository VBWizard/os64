#ifndef OS64_BT_A2DP_SEAM_H
#define OS64_BT_A2DP_SEAM_H

// bt_a2dp_seam.h — the A2DP pump's way out of the Bluetooth host: the audio
// seam, the SBC encoder and the clock, as the bt_a2dp_ops_t the host hands
// the pump (docs/design/pending/AUDIO_A2DP.md part P). The host is pure and
// builds in every host suite with no kernel header; this table is the
// kernel's half, and the transport (xhci.c) gives it to the host
// (bt_host_audio) at bring-up. Every call is made from kusb's pass, under
// USB's serializing flag, with no lock of the Bluetooth host's held.
//
// The seam knows this sink as "a2dp" (/sys/audio). Its nudge rings kusb's
// doorbell and does nothing else: it runs under the stream's lock with
// interrupts off, and the pump decides from state at its next pass.

#include "driver/system/usb/bt_a2dp.h"

extern const bt_a2dp_ops_t kBtA2dpSeamOps;

#endif
