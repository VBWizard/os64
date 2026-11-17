// beam.c — the beam seam (driver/display/beam.h): asks each driver, measures
// the period, keeps the facts, and withdraws a beam that stops making sense.
//
// THE STATE'S LIFE. Everything is written once by beam_init, on the kernel
// task before the compositor exists. After that the compositor thread is the
// one writer: its reads keep the failure counts, and a withdrawal writes the
// reason, then clears `present`. /sys/gui reads the facts and the reason from
// any CPU without a lock; the reason's text is complete before `present`
// says to look at it.

#include "driver/display/beam.h"
#include "micros.h"
#include "printd.h"
#include "CONFIG.h"
#include "sprintf.h"

static const beam_driver_t *const s_drivers[] = { &kBeamAmdDcn };

static struct {
	volatile bool present;
	bool found;                    // a driver found a core, measured or not
	const char *driver;
	char where[48];
	fc_frame_t frame;
	uint32_t (*read_line)(void);
	const char *volatile why;
	char why_text[112];
	uint32_t fails_in_row;
	volatile uint64_t fails;
} s_beam = { .why = "not looked for: no desktop on this boot" };

static void beam_say_why(const char *fmt, uint32_t a, uint32_t b)
{
	snprintf(s_beam.why_text, sizeof(s_beam.why_text), fmt, a, b);
	__asm__ volatile("" ::: "memory");   // the text before anyone is told to read it
	s_beam.why = s_beam.why_text;
}

void beam_init(void)
{
	beam_found_t found;
	const beam_driver_t *driver = NULL;
	const char *why = "no display core this kernel can read";
	for (unsigned i = 0; i < sizeof(s_drivers) / sizeof(s_drivers[0]); i++) {
		found = (beam_found_t){ 0 };
		const char *answer = s_drivers[i]->probe(&found);
		if (answer == BEAM_NOT_MINE)
			continue;
		if (answer != NULL) {
			why = answer;
			printd(DEBUG_BOOT, "beam: %s: %s\n", s_drivers[i]->name, answer);
			break;
		}
		driver = s_drivers[i];
		break;
	}
	if (driver == NULL) {
		s_beam.why = why;
		printd(DEBUG_BOOT, "beam: none (%s)\n", why);
		return;
	}

	s_beam.found = true;
	s_beam.driver = driver->name;
	snprintf(s_beam.where, sizeof(s_beam.where), "%s", found.where);
	s_beam.read_line = found.read_line;
	s_beam.frame = (fc_frame_t){ found.lines, found.blank_start, found.blank_end, 0 };

	// The period, from the counter against micros_now, interrupts on: an
	// interrupt that delays a reading costs nothing unless it hides a wrap
	// (frame_clock.h, measuring the period).
	fc_measure_t m;
	frame_clock_measure_begin(&m, found.lines);
	uint64_t start = micros_now();
	for (;;) {
		uint32_t raw = found.read_line();
		uint64_t now = micros_now();
		if (frame_clock_measure_feed(&m, raw, now) || now - start >= FC_MEASURE_BOUND_US)
			break;
	}
	uint64_t period_ns;
	fc_measure_reason_t result = frame_clock_measure_result(&m, &period_ns);
	if (result != FC_MEASURE_OK) {
		s_beam.why = frame_clock_measure_reason_name(result);
		printd(DEBUG_BOOT, "beam: %s at %s: no beam (%s; longest gap between readings %lu us)\n",
		       driver->name, s_beam.where, s_beam.why, m.gap_max_us);
		return;
	}
	s_beam.frame.period_ns = period_ns;
	if (!frame_clock_frame_valid(&s_beam.frame)) {
		s_beam.why = "the frame the core describes makes no sense";
		printd(DEBUG_BOOT, "beam: %s at %s: no beam (%u lines, blank %u to %u)\n", driver->name,
		       s_beam.where, found.lines, found.blank_start, found.blank_end);
		return;
	}
	uint64_t hz_x100 = 100000000000ull / period_ns;
	printd(DEBUG_BOOT, "beam: %s at %s: %u lines a frame, %u shown, blank from line %u to %u, "
	       "%lu.%02lu Hz (period %lu ns, longest gap between readings %lu us)\n",
	       driver->name, s_beam.where, found.lines, frame_clock_active_rows(&s_beam.frame),
	       found.blank_start, found.blank_end, hz_x100 / 100, hz_x100 % 100, period_ns, m.gap_max_us);
	s_beam.why = NULL;
	s_beam.present = true;
}

bool beam_present(void)
{
	return s_beam.present;
}

const char *beam_driver(void)
{
	return s_beam.found ? s_beam.driver : NULL;
}

const char *beam_where(void)
{
	return s_beam.found ? s_beam.where : NULL;
}

const char *beam_why(void)
{
	return s_beam.present ? NULL : s_beam.why;
}

bool beam_frame(fc_frame_t *out)
{
	if (!s_beam.found || s_beam.frame.period_ns == 0)
		return false;
	*out = s_beam.frame;
	return true;
}

bool beam_line(uint32_t *line)
{
	if (!s_beam.present)
		return false;
	uint32_t raw = s_beam.read_line();
	if (frame_clock_line_ok(&s_beam.frame, raw)) {
		s_beam.fails_in_row = 0;
		*line = raw;
		return true;
	}
	s_beam.fails++;
	if (++s_beam.fails_in_row >= BEAM_FAILS_IN_ROW) {
		if (raw == 0xFFFFFFFFu)
			beam_say_why("withdrawn: %u readings in a row read all ones", BEAM_FAILS_IN_ROW, 0);
		else
			beam_say_why("withdrawn: %u readings in a row were beyond the frame (the last line %u)",
			             BEAM_FAILS_IN_ROW, raw);
		s_beam.present = false;
		printd(DEBUG_BOOT | DEBUG_GUI, "beam: %s\n", s_beam.why);
	}
	return false;
}

uint64_t beam_read_failures(void)
{
	return s_beam.fails;
}
