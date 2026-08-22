// input.c — the unified input event queue (GUI layer 2).
//
// Raw device drivers (keyboard IRQ1, mouse IRQ12) push primitive state here;
// this file turns it into typed events (KEY_DOWN, MOUSE_MOVE, BUTTON_UP, ...)
// on one ring the compositor drains each frame. See gui/input.h for the
// producer/consumer rules — the short version: ISRs enqueue, compositor polls,
// nobody wakes anybody.

#include "gui/input.h"
#include "gui/gui_types.h"
#include "gui/compositor.h"   // gui_owns_glass — mouse routing (VT8 chapter)
#include "driver/system/keyboard.h"   // keyboard_current_modifiers — the pointer's
                                      // view of the keyboard (Ctrl+Alt+drag)

#include "CONFIG.h"
#include "kernel.h"
#include "printd.h"
#include "spinlock.h"
#include "video.h"

extern struct Framebuffer kFrameBuffer;

#define INPUT_QUEUE_SIZE 256   // power of two; ~2.5s of typematic + mouse at
                               // worst case before drops — plenty for a queue
                               // drained every frame

static input_event_t s_queue[INPUT_QUEUE_SIZE];
static volatile uint32_t s_head;   // producer cursor (IRQ side)
static volatile uint32_t s_tail;   // consumer cursor (compositor side)
static spinlock_t s_input_lock = 0;

// The GUI-side cursor position, tracked here so every mouse event carries
// absolute screen coordinates and the compositor never integrates deltas.
static int32_t s_mouse_x, s_mouse_y;
static uint8_t s_mouse_buttons;

// Gate: stays false when the GUI is off so the always-on keyboard driver's
// inject calls cost one branch and nothing else.
static volatile bool s_active = false;

void input_init(void)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_input_lock);
	s_head = s_tail = 0;
	s_mouse_x = (int32_t)kFrameBuffer.width / 2;   // start centered
	s_mouse_y = (int32_t)kFrameBuffer.height / 2;
	s_mouse_buttons = 0;
	s_active = true;
	spinlock_release_irqrestore(&s_input_lock, flags);
	printd(DEBUG_GUI, "input: unified event queue active (%u slots)\n", INPUT_QUEUE_SIZE);
}

// Enqueue helper — caller must hold s_input_lock. Drop-newest when full
// (matches the keyboard driver's policy: keep the oldest, oldest-first order
// stays intact for the consumer).
static void enqueue_locked(input_event_t *ev)
{
	uint32_t next = (s_head + 1) % INPUT_QUEUE_SIZE;
	if (next == s_tail)
		return;
	ev->tick = kTicksSinceStart;
	s_queue[s_head] = *ev;
	s_head = next;
}

void input_inject_key(char ascii, uint8_t scancode, uint8_t modifiers, bool pressed)
{
	if (!s_active)
		return;

	uint64_t flags = spinlock_acquire_irqsave(&s_input_lock);
	input_event_t ev = {
		.type = pressed ? INPUT_EVENT_KEY_DOWN : INPUT_EVENT_KEY_UP,
		.key = { .ascii = ascii, .scancode = scancode, .modifiers = modifiers },
	};
	enqueue_locked(&ev);
	spinlock_release_irqrestore(&s_input_lock, flags);
}

void input_inject_mouse(int16_t dx, int16_t dy, uint8_t buttons)
{
	if (!s_active)
		return;

	// Text VTs took no mouse from 2026-08-19 until 2026-08-21, and the reason
	// given was "there is no consumer". THERE IS ONE NOW: vt_select.c, the
	// gpm-lineage console selection this comment promised. Events are
	// enqueued unconditionally; the compositor decides at the far end of the
	// ring whether they belong to a window or to the focused terminal, which
	// is the only place that knows who holds the glass at drain time.

	// The keyboard state that was true when this packet arrived. Sampled ONCE
	// for the whole packet so the move and the button edges it may also carry
	// agree with each other — a chord that is released mid-packet must not
	// produce a move that thinks it was held and a button-up that thinks it
	// was not (that disagreement is exactly how a modifier-drag gets stuck).
	uint8_t modifiers = keyboard_current_modifiers();

	uint64_t flags = spinlock_acquire_irqsave(&s_input_lock);

	// Integrate motion and clamp to the screen. PS/2 y is positive-up;
	// the DRIVER converts to screen coords (positive-down) before injecting,
	// so dy here is already screen-oriented.
	if (dx || dy) {
		s_mouse_x += dx;
		s_mouse_y += dy;
		if (s_mouse_x < 0) s_mouse_x = 0;
		if (s_mouse_y < 0) s_mouse_y = 0;
		if (s_mouse_x >= (int32_t)kFrameBuffer.width)  s_mouse_x = (int32_t)kFrameBuffer.width - 1;
		if (s_mouse_y >= (int32_t)kFrameBuffer.height) s_mouse_y = (int32_t)kFrameBuffer.height - 1;

		input_event_t ev = {
			.type = INPUT_EVENT_MOUSE_MOVE,
			.mouse = { .x = s_mouse_x, .y = s_mouse_y, .dx = dx, .dy = dy,
			           .buttons = buttons, .button = 0,
			           .modifiers = modifiers },
		};
		enqueue_locked(&ev);
	}

	// Diff button state into discrete DOWN/UP events, one per changed button,
	// so the window system routes clicks without re-deriving edges.
	uint8_t changed = buttons ^ s_mouse_buttons;
	for (uint8_t b = 0; changed && b < 3; b++) {
		uint8_t mask = (uint8_t)(1u << b);
		if (!(changed & mask))
			continue;
		input_event_t ev = {
			.type = (buttons & mask) ? INPUT_EVENT_MOUSE_BUTTON_DOWN
			                         : INPUT_EVENT_MOUSE_BUTTON_UP,
			.mouse = { .x = s_mouse_x, .y = s_mouse_y, .dx = 0, .dy = 0,
			           .buttons = buttons, .button = b,
			           .modifiers = modifiers },
		};
		enqueue_locked(&ev);
	}
	s_mouse_buttons = buttons;

	spinlock_release_irqrestore(&s_input_lock, flags);
}

bool input_pending(void)
{
	// Deliberately unlocked (see input.h): both cursors are volatile and a
	// stale answer only costs one timer period of latency.
	return s_head != s_tail;
}

bool input_pop(input_event_t *out)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_input_lock);
	if (s_head == s_tail) {
		spinlock_release_irqrestore(&s_input_lock, flags);
		return false;
	}
	*out = s_queue[s_tail];
	s_tail = (s_tail + 1) % INPUT_QUEUE_SIZE;
	spinlock_release_irqrestore(&s_input_lock, flags);
	return true;
}
