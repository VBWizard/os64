// glass.c — the kernel half of /dev/glass. gui/glass.h is the contract,
// os64/glass.h the ABI, REMOTE.md section 3 the design.

#include <stdint.h>
#include <stdbool.h>
#include "gui/glass.h"
#include "gui/gui_internal.h"
#include "gui/compositor.h"
#include "os64/glass.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "smp_core.h"
#include "scheduler.h"
#include "signals.h"
#include "kernel.h"          // kTicksSinceStart — the park's backstop and the deadline
#include "gui/input.h"       // input_inject_pointer / input_release_pointer — the view's pointer
#include "driver/system/hid_keyboard.h"   // the view's keyboard: the USB keyboard's interpreter
#include "CONFIG.h"          // TICKS_PER_SECOND

// How many separate rectangles a view keeps before it folds them into one.
// The compositor keeps the same number for the same reason: past it, the
// bookkeeping costs more than re-sending the area between them.
#define GLASS_PENDING_MAX 16

// A parked reader's longest nap. The compositor's feed wakes it the moment a
// frame lands; this is the net under a wake that found it not yet asleep, and
// the cadence at which a reader notices the screen changing hands.
#define GLASS_BACKSTOP_TICKS (TICKS_PER_SECOND / 4)

struct glass_view
{
	rect_t pending[GLASS_PENDING_MAX];   // changed since the last read, disjoint-ish
	uint32_t count;
	bool owned_reported;                 // whether the desktop held the screen, as last reported
	bool closed;                         // the handle is gone; readers leave
	uint32_t refs;                       // handle + operations in flight (atomic)
	thread_t *waiter;                    // a reader parked for the next change
	struct glass_view *next;             // s_views, under kGuiLock

	// The hands, under input_lock (gui/glass.h, INPUT).
	bool writable;                       // opened "u"
	bool input_closed;                   // the handle closed: nothing more is delivered
	spinlock_t input_lock;
	hid_keyboard_t kbd;
	input_pointer_source_t pointer;      // the buttons this view holds down
};

static glass_view_t *s_views;

static rect_t screen_rect(const surface_t *bb)
{
	return (rect_t){0, 0, (int32_t)bb->width, (int32_t)bb->height};
}

static bool rects_touch(rect_t a, rect_t b)
{
	return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}

// Record a change. A rectangle that touches one already pending grows it, so
// a window being dragged stays one rectangle rather than a trail of them; a
// full record folds everything into its union.
static void pending_add(glass_view_t *v, rect_t r)
{
	if (rect_is_empty(r))
		return;
	for (uint32_t i = 0; i < v->count; i++)
		if (rects_touch(v->pending[i], r))
		{
			v->pending[i] = rect_union(v->pending[i], r);
			return;
		}
	if (v->count < GLASS_PENDING_MAX)
	{
		v->pending[v->count++] = r;
		return;
	}
	rect_t all = r;
	for (uint32_t i = 0; i < v->count; i++)
		all = rect_union(all, v->pending[i]);
	v->pending[0] = all;
	v->count = 1;
}

glass_view_t *glass_view_open(bool writable)
{
	const surface_t *bb = gui_backbuffer();
	if (bb == NULL)
		return NULL;
	glass_view_t *v = kmalloc(sizeof(*v));
	if (v == NULL)
		return NULL;
	v->refs = 1;
	v->writable = writable;
	v->kbd.name = "glass";
	v->kbd.debug = DEBUG_GUI;
	uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
	v->pending[0] = screen_rect(bb);
	v->count = 1;
	v->owned_reported = gui_owns_glass();
	v->next = s_views;
	s_views = v;
	spinlock_release_irqrestore(&kGuiLock, flags);
	return v;
}

void glass_view_ref(glass_view_t *v)
{
	__sync_fetch_and_add(&v->refs, 1);
}

void glass_view_release(glass_view_t *v)
{
	if (__sync_sub_and_fetch(&v->refs, 1) != 0)
		return;
	// The last reference: nothing can reach this view but the list.
	uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
	for (glass_view_t **p = &s_views; *p != NULL; p = &(*p)->next)
		if (*p == v)
		{
			*p = v->next;
			break;
		}
	spinlock_release_irqrestore(&kGuiLock, flags);
	kfree(v);
}

void glass_view_close(glass_view_t *v)
{
	// Lift every finger first, under the input lock, so no write can land
	// after the release: an empty report ends every key and modifier the
	// keyboard held (with their release events), and releasing the view's
	// pointer lifts the buttons it held, and no other device's. A full GUI
	// event queue drops those events like any others (DEBTS § Remote access).
	uint64_t iflags = spinlock_acquire_irqsave(&v->input_lock);
	v->input_closed = true;
	if (v->writable)
	{
		static const uint8_t none[8];
		hid_keyboard_report(&v->kbd, none);
		input_release_pointer(&v->pointer);
	}
	spinlock_release_irqrestore(&v->input_lock, iflags);

	uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
	v->closed = true;
	// The slot is the waiter's to clear (gui_client.c's event wait has the
	// reason); this only makes sure it wakes to see `closed`.
	if (v->waiter != NULL)
		scheduler_wake_isleep_thread(v->waiter);
	spinlock_release_irqrestore(&kGuiLock, flags);
	glass_view_release(v);
}

long glass_view_read(glass_view_t *v, void *out, size_t cap, uint64_t deadline)
{
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls != NULL ? cls->currentThread : NULL;
	const surface_t *bb = gui_backbuffer();
	if (self == NULL || bb == NULL)
		return GLASS_ERR_CLOSED;

	for (;;)
	{
		uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
		// Awake at the top: a registration left by the park we came out of
		// is void (the signals doctrine, CLAUDE.md).
		if (v->waiter == self)
			v->waiter = NULL;
		if (v->closed)
		{
			spinlock_release_irqrestore(&kGuiLock, flags);
			return GLASS_ERR_CLOSED;
		}
		if (signal_park_must_end(self))
		{
			spinlock_release_irqrestore(&kGuiLock, flags);
			return GLASS_ERR_INTERRUPTED;
		}

		// The screen changed hands: everything is owed again, flagged the
		// new way, so the viewer learns of it on this read.
		bool owned = gui_owns_glass();
		if (owned != v->owned_reported)
		{
			v->pending[0] = screen_rect(bb);
			v->count = 1;
			v->owned_reported = owned;
		}

		if (v->count > 0)
		{
			rect_t r = v->pending[0];
			size_t row = (size_t)r.w * sizeof(uint32_t);
			if (cap < sizeof(os64_glass_rect_t) + row)
			{
				spinlock_release_irqrestore(&kGuiLock, flags);
				return GLASS_ERR_SMALL;
			}
			// A band of whole rows from the top; the rest stays first in line.
			uint32_t rows = (uint32_t)((cap - sizeof(os64_glass_rect_t)) / row);
			if (rows >= (uint32_t)r.h)
			{
				rows = (uint32_t)r.h;
				v->count--;
				for (uint32_t i = 0; i < v->count; i++)
					v->pending[i] = v->pending[i + 1];
			}
			else
			{
				v->pending[0].y += (int32_t)rows;
				v->pending[0].h -= (int32_t)rows;
			}
			spinlock_release_irqrestore(&kGuiLock, flags);

			// Copy outside the lock. A frame composited meanwhile may tear
			// these pixels, and its damage — recorded after our take — brings
			// them back on the next read (os64/glass.h, EVENTUALLY exact).
			os64_glass_rect_t head = {
				.x = (uint16_t)r.x, .y = (uint16_t)r.y,
				.w = (uint16_t)r.w, .h = (uint16_t)rows,
				.screen_w = (uint16_t)bb->width, .screen_h = (uint16_t)bb->height,
				.flags = owned ? 0 : OS64_GLASS_TEXT_VT,
			};
			uint8_t *dst = (uint8_t *)out;
			memcpy(dst, &head, sizeof(head));
			dst += sizeof(head);
			for (uint32_t y = 0; y < rows; y++, dst += row)
				memcpy(dst, bb->pixels + ((size_t)(r.y + (int32_t)y) * bb->pitch_px + (size_t)r.x), row);
			return (long)(sizeof(head) + (size_t)rows * row);
		}

		if (deadline != 0 && kTicksSinceStart >= deadline)
		{
			spinlock_release_irqrestore(&kGuiLock, flags);
			return GLASS_ERR_TIMEOUT;
		}
		// Empty-handed: register in the same hold as the failed take, so a
		// frame either landed before it (taken above) or will see us.
		v->waiter = self;
		spinlock_release_irqrestore(&kGuiLock, flags);
		uint64_t wake = kTicksSinceStart + GLASS_BACKSTOP_TICKS;
		if (deadline != 0 && deadline < wake)
			wake = deadline;
		signal_raise(SIGSLEEP, wake, self);
	}
}

void glass_damage_locked(const rect_t *rects, uint32_t count)
{
	for (glass_view_t *v = s_views; v != NULL; v = v->next)
	{
		if (v->closed)
			continue;
		bool changed = false;
		for (uint32_t i = 0; i < count; i++)
			if (!rect_is_empty(rects[i]))
			{
				pending_add(v, rects[i]);
				changed = true;
			}
		if (changed && v->waiter != NULL)
			scheduler_wake_isleep_thread(v->waiter);
	}
}

uint32_t glass_view_count(void)
{
	uint32_t n = 0;
	uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
	for (glass_view_t *v = s_views; v != NULL; v = v->next)
		n++;
	spinlock_release_irqrestore(&kGuiLock, flags);
	return n;
}

long glass_view_write(glass_view_t *v, const void *data, size_t len)
{
	if (!v->writable)
		return GLASS_ERR_READ_ONLY;
	const uint8_t *p = (const uint8_t *)data;
	if (len == 0)
		return GLASS_ERR_RECORD;

	if (p[0] == OS64_GLASS_KEYBOARD)
	{
		if (len != sizeof(os64_glass_keyboard_t))
			return GLASS_ERR_RECORD;
		os64_glass_keyboard_t k;
		memcpy(&k, p, sizeof(k));
		// A keyboard never reports one key twice: a report that does is
		// refused, or each copy would be its own press (and a Caps Lock
		// toggled once per copy).
		for (int i = 2; i < 8; i++)
			for (int j = i + 1; j < 8; j++)
				if (k.report[i] != 0 && k.report[i] == k.report[j])
					return GLASS_ERR_RECORD;
		uint64_t flags = spinlock_acquire_irqsave(&v->input_lock);
		if (v->input_closed)
		{
			spinlock_release_irqrestore(&v->input_lock, flags);
			return GLASS_ERR_CLOSED;
		}
		hid_keyboard_report(&v->kbd, k.report);
		spinlock_release_irqrestore(&v->input_lock, flags);
		return (long)len;
	}

	if (p[0] == OS64_GLASS_POINTER)
	{
		if (len != sizeof(os64_glass_pointer_t))
			return GLASS_ERR_RECORD;
		os64_glass_pointer_t m;
		memcpy(&m, p, sizeof(m));
		const surface_t *bb = gui_backbuffer();
		// Off the screen is refused, not moved: a viewer that believes the
		// screen is another size is wrong about everything it will send.
		if (bb == NULL || m.x >= bb->width || m.y >= bb->height ||
		    (m.buttons & ~(OS64_GLASS_BUTTON_LEFT | OS64_GLASS_BUTTON_RIGHT | OS64_GLASS_BUTTON_MIDDLE)))
			return GLASS_ERR_RECORD;
		uint64_t flags = spinlock_acquire_irqsave(&v->input_lock);
		if (v->input_closed)
		{
			spinlock_release_irqrestore(&v->input_lock, flags);
			return GLASS_ERR_CLOSED;
		}
		input_inject_pointer(&v->pointer, m.x, m.y, m.buttons);
		spinlock_release_irqrestore(&v->input_lock, flags);
		return (long)len;
	}

	return GLASS_ERR_RECORD;
}

// How many views one frame's tick serves. More writable views than this is
// more remote keyboards than there are people; the start rotates through
// them, so every held key is served within a few frames and repeats more
// slowly while there are that many, rather than not at all.
#define GLASS_TICK_MAX 8

static bool glass_tick_due(const glass_view_t *v)
{
	return v->writable && !v->closed && v->kbd.rpt_usage != 0;
}

void glass_input_tick(void)
{
	// Take the views with a key held under kGuiLock, each with a reference
	// so it cannot be freed, then tick them with kGuiLock released (the
	// delivery's locks may not nest under it). rpt_usage is read here
	// without the input lock: it is a hint, and a stale one costs a frame.
	// The scan starts where the last one stopped, counted among the due
	// views, so a cap reached every frame still reaches them all.
	static uint32_t s_rotation;
	glass_view_t *due[GLASS_TICK_MAX];
	uint32_t n = 0, eligible = 0;
	uint64_t flags = spinlock_acquire_irqsave(&kGuiLock);
	for (glass_view_t *v = s_views; v != NULL; v = v->next)
		if (glass_tick_due(v))
			eligible++;
	uint32_t start = eligible ? s_rotation % eligible : 0;
	for (uint32_t pass = 0; pass < 2 && n < GLASS_TICK_MAX; pass++)
	{
		uint32_t index = 0;
		for (glass_view_t *v = s_views; v != NULL && n < GLASS_TICK_MAX; v = v->next)
		{
			if (!glass_tick_due(v))
				continue;
			if (pass == 0 ? index >= start : index < start)
			{
				glass_view_ref(v);
				due[n++] = v;
			}
			index++;
		}
	}
	s_rotation = start + n;
	spinlock_release_irqrestore(&kGuiLock, flags);

	for (uint32_t i = 0; i < n; i++)
	{
		uint64_t iflags = spinlock_acquire_irqsave(&due[i]->input_lock);
		if (!due[i]->input_closed)
			hid_keyboard_tick(&due[i]->kbd);
		spinlock_release_irqrestore(&due[i]->input_lock, iflags);
		glass_view_release(due[i]);
	}
}
