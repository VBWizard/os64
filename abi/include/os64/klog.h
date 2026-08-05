#ifndef OS64_ABI_KLOG_H
#define OS64_ABI_KLOG_H

// os64/klog.h — reading the kernel's log from ring 3.
//
// The kernel does not write log files. It keeps a per-core ring of
// entries and hands them, oldest-first across all cores, to whoever asks;
// deciding WHERE they end up — a file, a socket, a screen, nowhere — is
// policy, and policy lives in userland. That split is why this header
// exists at all, and it is the same one /proc and memory() are built on.
//
// The daemon's loop is the whole contract:
//
//     for (;;) {
//         int64_t n = os64_klog_read(entries, 64);
//         if (n > 0)  ...format and write them...
//         else        os64_sleep(100);
//     }
//
// READING IS A CLAIM. While someone is calling this, the kernel's own
// logd STOPS draining to serial — that hand-off is the point (steady-state
// logging moves from a 115200-baud wire to a memcpy-speed file). The claim
// is a HEARTBEAT, not a registration: each read stamps the current tick,
// and if that stamp goes stale the kernel resumes serial draining on its
// own. So a log daemon that crashes, hangs, or is killed cannot take the
// system's logging with it — and its own death notice necessarily lands
// in the serial log, because serial is what comes back.
//
// PANICS DO NOT COME THROUGH HERE. A dying kernel cannot wait for a
// userland process to schedule; panic writes directly to serial and force-
// drains the rings itself (see logd_emergency_flush). A log file ends at
// the last line before the panic; the panic is on the wire.

#include <stdint.h>

// One log entry, as userland sees it. Deliberately NOT the kernel's
// internal struct: this is a contract, and the kernel must stay free to
// reorganize its rings without breaking a program compiled last year.
#define OS64_LOG_MESSAGE_MAX 256

typedef struct os64_logent
{
	uint64_t ticks;      // kTicksSinceStart when the line was logged. The
	                     // stopwatch, not the calendar — a daemon that
	                     // wants wall-clock stamps calls os64_time() and
	                     // does its own arithmetic (os64/date.h), because
	                     // the format of a timestamp is policy too.
	uint64_t threadID;   // who said it
	uint16_t core;       // which CPU it was said on
	uint8_t  level;      // the printd severity/category bits, as logged
	uint8_t  category;
	uint8_t  continued;  // 1 = a continuation chunk of the previous entry;
	                     // a long line is split across entries and should
	                     // be concatenated, not re-prefixed
	uint8_t  reserved[3];
	char     message[OS64_LOG_MESSAGE_MAX];
} os64_logent_t;

#endif // OS64_ABI_KLOG_H
