// input.c — the unified input event queue (GUI layer 2).
//
// Raw device drivers (keyboard IRQ1, mouse IRQ12) push primitive state here;
// this file turns it into typed events (KEY_DOWN, MOUSE_MOVE, BUTTON_UP, ...)
// on one ring the compositor drains each frame. See gui/input.h for the
// producer/consumer rules. Text wheel input changes the focused VT's view
// without painting; its existing flush handles the pixels.

#include "gui/input.h"
#include "gui/gui_types.h"
#include "gui/compositor.h"   // gui_owns_glass — mouse routing (VT8 chapter)
#include "tty.h"
#include "driver/system/keyboard.h"   // keyboard_current_modifiers — the pointer's
                                      // view of the keyboard (Ctrl+Alt+drag)

#include "CONFIG.h"
#include "kernel.h"
#include "printd.h"
#include "spinlock.h"
#include "video.h"
#include "os64/mouse.h"
#include "memcpy.h"
#include "strings/strings.h"

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
// How many sources hold each button (input.h, input_pointer_source_t); a
// button is down while its count is nonzero.
static uint32_t s_button_holders[3];
static os64_mouse_device_t s_mouse_devices[OS64_MOUSE_DEVICES];
static uint32_t s_mouse_sources[OS64_MOUSE_DEVICES];
static unsigned s_mouse_device_count;
static uint64_t s_mouse_generation;

int input_mouse_snapshot(void *out, size_t capacity)
{
    if(!out || capacity<sizeof(os64_mouse_snapshot_t)) return -1;
    os64_mouse_snapshot_t *snapshot=out;
    uint64_t flags=spinlock_acquire_irqsave(&s_input_lock);
    *snapshot=(os64_mouse_snapshot_t){.version=OS64_MOUSE_VERSION,
        .count=s_mouse_device_count,.generation=s_mouse_generation};
    for(unsigned i=0;i<s_mouse_device_count;i++) snapshot->devices[i]=s_mouse_devices[i];
    spinlock_release_irqrestore(&s_input_lock,flags);
    return sizeof(*snapshot);
}

int input_mouse_apply(const void *bytes, size_t length)
{
    if(!bytes || length!=sizeof(os64_mouse_command_t)) return -1;
    os64_mouse_command_t command;
    memcpy(&command,bytes,sizeof(command));
    if(command.version!=OS64_MOUSE_VERSION || !command.count || command.count>OS64_MOUSE_DEVICES) return -1;
    for(unsigned i=0;i<command.count;i++) {
        if(!os64_mouse_setting_valid(&command.settings[i])) return -1;
        for(unsigned j=0;j<i;j++)
            if(!strcmp(command.settings[i].key,command.settings[j].key)) return -1;
    }
    uint64_t flags=spinlock_acquire_irqsave(&s_input_lock);
    unsigned slots[OS64_MOUSE_DEVICES], count=s_mouse_device_count;
    bool valid=command.expected_generation==s_mouse_generation && s_mouse_generation!=UINT64_MAX;
    for(unsigned i=0;i<command.count && valid;i++) {
        unsigned j=0;
        while(j<s_mouse_device_count && strcmp(s_mouse_devices[j].setting.key,command.settings[i].key)) j++;
        slots[i]=j==s_mouse_device_count?count++:j;
        if(count>OS64_MOUSE_DEVICES) valid=false;
    }
    if(valid) {
        for(unsigned i=0;i<command.count;i++) {
            os64_mouse_device_t *d=&s_mouse_devices[slots[i]];
            d->setting=command.settings[i];
            if(!d->name[0]) strncpy(d->name,d->setting.key,sizeof(d->name)-1);
        }
        s_mouse_device_count=count; s_mouse_generation++;
    }
    spinlock_release_irqrestore(&s_input_lock,flags);
    return valid?(int)length:-1;
}

void input_pointer_register(input_pointer_source_t *src,const char *key,const char *name)
{
    char bounded[OS64_MOUSE_KEY]={0};
    if(!src || !key || !name || strlen(key)>=sizeof(bounded)) return;
    strcpy(bounded,key);
    if(!os64_mouse_key_valid(bounded)) return;
    uint64_t flags=spinlock_acquire_irqsave(&s_input_lock);
    if(src->settings_slot) {
        spinlock_release_irqrestore(&s_input_lock,flags); return;
    }
    unsigned slot=0;
    while(slot<s_mouse_device_count && strcmp(s_mouse_devices[slot].setting.key,key)) slot++;
    if(slot<OS64_MOUSE_DEVICES) {
        if(slot==s_mouse_device_count) {
            s_mouse_device_count++;
            strcpy(s_mouse_devices[slot].setting.key,key);
            s_mouse_devices[slot].setting.speed=100;
        }
        strncpy(s_mouse_devices[slot].name,name,OS64_MOUSE_NAME-1);
        s_mouse_devices[slot].name[OS64_MOUSE_NAME-1]=0;
        s_mouse_sources[slot]++; s_mouse_devices[slot].connected=1;
        src->settings_slot=slot+1;
    }
    spinlock_release_irqrestore(&s_input_lock,flags);
}

// Gate for the GUI ring; text wheel input does not depend on it.
static volatile bool s_active = false;

void input_init(void)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_input_lock);
	s_head = s_tail = 0;
	s_mouse_x = (int32_t)kFrameBuffer.width / 2;   // start centered
	s_mouse_y = (int32_t)kFrameBuffer.height / 2;
	for (int b = 0; b < 3; b++)
		s_button_holders[b] = 0;
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

static uint8_t buttons_held(void)
{
	uint8_t held = 0;
	for (int b = 0; b < 3; b++)
		if (s_button_holders[b] != 0)
			held |= (uint8_t)(1u << b);
	return held;
}

// Move by (dx, dy), clamped to the screen, and turn `src`'s new buttons into
// the machine's DOWN/UP edges. Caller holds s_input_lock. Both pointers —
// the mice, which move relatively, and /dev/glass, which says where — end
// here, so a grab, a drag and a chord see the same events from either.
static void pointer_locked(input_pointer_source_t *src, int32_t dx, int32_t dy, uint8_t buttons,
                           uint8_t modifiers)
{
	buttons &= 0x07;
	uint8_t before = buttons_held();
	for (int b = 0; b < 3; b++)
	{
		uint8_t mask = (uint8_t)(1u << b);
		if ((buttons & mask) && !(src->buttons & mask))
			s_button_holders[b]++;
		else if (!(buttons & mask) && (src->buttons & mask))
			s_button_holders[b]--;
	}
	src->buttons = buttons;
	uint8_t held = buttons_held();

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
			.mouse = { .x = s_mouse_x, .y = s_mouse_y, .dx = (int16_t)dx, .dy = (int16_t)dy,
			           .buttons = held, .button = 0,
			           .modifiers = modifiers },
		};
		enqueue_locked(&ev);
	}

	// Diff the machine's button state into discrete DOWN/UP events, one per
	// changed button, so the window system routes clicks without re-deriving
	// edges.
	uint8_t changed = held ^ before;
	for (uint8_t b = 0; changed && b < 3; b++) {
		uint8_t mask = (uint8_t)(1u << b);
		if (!(changed & mask))
			continue;
		input_event_t ev = {
			.type = (held & mask) ? INPUT_EVENT_MOUSE_BUTTON_DOWN
			                      : INPUT_EVENT_MOUSE_BUTTON_UP,
			.mouse = { .x = s_mouse_x, .y = s_mouse_y, .dx = 0, .dy = 0,
			           .buttons = held, .button = b,
			           .modifiers = modifiers },
		};
		enqueue_locked(&ev);
	}
}

static int32_t scale_motion(int16_t delta,uint32_t speed,int32_t *fraction)
{
    int32_t value=(int32_t)delta*(int32_t)speed+*fraction;
    *fraction=value%100;
    return value/100;
}

void input_inject_mouse(input_pointer_source_t *src, int16_t dx, int16_t dy,
                        uint8_t buttons, int16_t wheel)
{
	// Text scrollback follows the focused VT at arrival, like Shift+PgUp.
	// This changes state only; tty_flush_if_dirty paints outside the IRQ.
	if (wheel && !gui_owns_glass()) {
		tty_view_wheel(wheel);
		wheel = 0;
	}
	if (!s_active)
		return;

	// Motion and buttons share the compositor's routing to windows or text
	// selection. Text wheel input above also works without that consumer.

	// The keyboard state that was true when this packet arrived. Sampled ONCE
	// for the whole packet so the move and the button edges it may also carry
	// agree with each other — a chord that is released mid-packet must not
	// produce a move that thinks it was held and a button-up that thinks it
	// was not (that disagreement is exactly how a modifier-drag gets stuck).
	uint8_t modifiers = keyboard_current_modifiers();

	uint64_t flags = spinlock_acquire_irqsave(&s_input_lock);
	uint32_t speed=100; bool right=false;
	if(src->settings_slot && src->settings_slot<=s_mouse_device_count) {
		const os64_mouse_setting_t *setting=&s_mouse_devices[src->settings_slot-1].setting;
		speed=setting->speed; right=setting->right_primary!=0;
	}
	if(src->speed!=speed) { src->fraction_x=src->fraction_y=0; src->speed=speed; }
	// A preference change cannot turn a held primary press into a secondary
	// release. Adopt the mapping at the start of the next physical chord.
	if(!src->raw_buttons) src->right_primary=right;
	src->raw_buttons=buttons&7;
	if(src->right_primary) buttons=(buttons&4)|((buttons&1)<<1)|((buttons&2)>>1);
	int32_t mx=scale_motion(dx,speed,&src->fraction_x);
	int32_t my=scale_motion(dy,speed,&src->fraction_y);
	// Event deltas are signed 16-bit even though cursor integration is wider.
	if(mx>32767) mx=32767; else if(mx<-32768) mx=-32768;
	if(my>32767) my=32767; else if(my<-32768) my=-32768;
	pointer_locked(src, mx, my, buttons, modifiers);
	if (wheel) {
		input_event_t ev = {
			.type = INPUT_EVENT_MOUSE_WHEEL,
			.mouse = { .x = s_mouse_x, .y = s_mouse_y, .dy = wheel,
			           .buttons = buttons_held(), .modifiers = modifiers },
		};
		enqueue_locked(&ev);
	}
	spinlock_release_irqrestore(&s_input_lock, flags);
}

void input_inject_pointer(input_pointer_source_t *src, int32_t x, int32_t y, uint8_t buttons)
{
	if (!s_active)
		return;
	// The same one-sample rule as a mouse packet (above).
	uint8_t modifiers = keyboard_current_modifiers();
	uint64_t flags = spinlock_acquire_irqsave(&s_input_lock);
	pointer_locked(src, x - s_mouse_x, y - s_mouse_y, buttons, modifiers);
	spinlock_release_irqrestore(&s_input_lock, flags);
}

void input_release_pointer(input_pointer_source_t *src)
{
	if (!s_active)
		return;
	uint8_t modifiers = keyboard_current_modifiers();
	uint64_t flags = spinlock_acquire_irqsave(&s_input_lock);
	pointer_locked(src, 0, 0, 0, modifiers);
	src->raw_buttons=0; src->fraction_x=src->fraction_y=0;
	spinlock_release_irqrestore(&s_input_lock, flags);
}

void input_pointer_unregister(input_pointer_source_t *src)
{
    uint8_t modifiers=keyboard_current_modifiers();
    uint64_t flags=spinlock_acquire_irqsave(&s_input_lock);
    if(s_active) pointer_locked(src,0,0,0,modifiers);
    if(src->settings_slot && src->settings_slot<=s_mouse_device_count) {
        unsigned slot=src->settings_slot-1;
        if(s_mouse_sources[slot]) s_mouse_sources[slot]--;
        s_mouse_devices[slot].connected=s_mouse_sources[slot]!=0;
    }
    *src=(input_pointer_source_t){0};
    spinlock_release_irqrestore(&s_input_lock,flags);
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
