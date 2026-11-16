// media_session.c — the lock, the waits and the names around the media keys.
// media_session.h is the contract, media_keys.c the rule, MEDIA_KEYS.md the
// design.

#include "audio/media_session.h"
#include "audio/media_keys.h"
#include "audio/audio.h"     // audio_toggle: the kernel's own answer
#include "gui/window.h"      // wm_focus_owner
#include "spinlock.h"
#include "kmalloc.h"
#include "memset.h"
#include "memcpy.h"
#include "memory/memcmp.h"
#include "smp_core.h"
#include "scheduler.h"
#include "signals.h"
#include "task.h"
#include "kernel.h"          // kTicksSinceStart
#include "CONFIG.h"          // TICKS_PER_SECOND
#include "micros.h"
#include "strings/sprintf.h"
#include "strings/strings.h"
#include "driver/filesystem/vfs/synthfs.h"   // synth_text_escape: the program's name is data

struct media_session
{
	media_keys_session_t keys;   // first: the rule hands back this pointer
	uint32_t  refs;              // atomic: the handle, and each operation in flight
	bool      closed;            // under s_lock: the handle went
	thread_t *reader;            // under s_lock: the one thread parked in a read
	uint64_t  task_id;           // copied at open, for the report
	char      name[32];
};

// The session lock. Taken irqsave (kusb arrives with interrupts on, a sysfs
// writer may not), held for the rule, a ring and a wake, never across a wait
// or an allocation. A wake takes the queue lock under it, the audio
// stream's order; nothing that runs under the queue lock takes this one.
static spinlock_t         s_lock;
static media_keys_table_t s_table;
static bool               s_ready;

// What the kernel did with the last press no session would take.
typedef enum { KERNEL_NONE, KERNEL_PAUSED, KERNEL_RESUMED, KERNEL_REFUSED } kernel_answer_t;
static const char *const kKernelAnswers[] = { "none", "paused", "resumed", "refused" };
static kernel_answer_t s_kernel_last;
static uint64_t        s_kernel_paused;
static uint64_t        s_kernel_resumed;

// How long a park lasts with nobody to end it. Every event that can end a
// read wakes it; this is for the one that should have and did not.
#define MEDIA_SESSION_BACKSTOP_TICKS TICKS_PER_SECOND

static void ready_locked(void)
{
	if (!s_ready)
	{
		media_keys_table_init(&s_table);
		s_ready = true;
	}
}

// ── The handle ──────────────────────────────────────────────────────────────

media_session_t *media_session_open(long *refusal)
{
	*refusal = -1;
	media_session_t *s = kmalloc(sizeof(*s));
	if (s == NULL)
		return NULL;
	memset(s, 0, sizeof(*s));

	task_t *task = (task_t *)get_core_local_storage()->currentThread->ownerTask;
	uint64_t lineage[MEDIA_KEYS_LINEAGE];
	uint32_t n = task_lineage(task, lineage, MEDIA_KEYS_LINEAGE);
	media_keys_session_init(&s->keys, lineage, n);
	s->refs = 1;
	s->task_id = task->taskID;
	strncpy(s->name, task->exename, sizeof(s->name) - 1);

	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	ready_locked();
	int slot = media_keys_add(&s_table, &s->keys);
	spinlock_release_irqrestore(&s_lock, flags);
	if (slot < 0)
	{
		kfree(s);
		*refusal = OS64_MEDIA_KEYS_FULL;
		return NULL;
	}
	return s;
}

void media_session_ref(media_session_t *s)
{
	__sync_fetch_and_add(&s->refs, 1);
}

void media_session_release(media_session_t *s)
{
	if (__sync_sub_and_fetch(&s->refs, 1) == 0)
		kfree(s);
}

void media_session_close(media_session_t *s)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	s->closed = true;
	media_keys_remove(&s_table, &s->keys);
	if (s->reader != NULL)
		scheduler_wake_or_owe(s->reader);
	s->reader = NULL;
	spinlock_release_irqrestore(&s_lock, flags);
	media_session_release(s);
}

// ── Read and write ──────────────────────────────────────────────────────────

long media_session_read(media_session_t *s, os64_media_key_t *out, uint64_t deadline)
{
	thread_t *self = get_core_local_storage()->currentThread;
	for (;;)
	{
		uint64_t flags = spinlock_acquire_irqsave(&s_lock);
		// Awake at the top: a registration left by the park we came out of
		// is void, and so is a wake owed for it (WAKE_OWED.md).
		if (s->reader == self)
			s->reader = NULL;
		scheduler_forget_owed_wake(self);

		long end = 0;
		if (s->closed)
			end = MEDIA_SESSION_ERR_CLOSED;
		else if (signal_park_must_end(self))
			end = MEDIA_SESSION_ERR_INTERRUPTED;
		else if (s->reader != NULL)
			end = OS64_MEDIA_KEYS_BUSY;
		else if (media_keys_take(&s->keys, out))
			end = 1;
		else if (deadline != 0 && kTicksSinceStart >= deadline)
			end = MEDIA_SESSION_ERR_TIMEOUT;
		if (end != 0)
		{
			spinlock_release_irqrestore(&s_lock, flags);
			return end;
		}

		// Nothing to read: register in the same hold that found the ring
		// empty, so a press either landed before it (taken above) or will
		// find us.
		s->reader = self;
		spinlock_release_irqrestore(&s_lock, flags);   // never park holding the lock

		uint64_t wake = kTicksSinceStart + MEDIA_SESSION_BACKSTOP_TICKS;
		if (deadline != 0 && deadline < wake)
			wake = deadline;
		signal_raise(SIGSLEEP, wake, self);
		// Woken by a press, a close, the deadline, a signal or the
		// backstop: loop and look again.
	}
}

void media_session_undelivered(media_session_t *s)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	media_keys_undelivered(&s->keys);
	spinlock_release_irqrestore(&s_lock, flags);
}

long media_session_write(media_session_t *s, const char *text, size_t length)
{
	uint8_t state;
	if (!media_keys_parse_state(text, length, &state))
		return -1;
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	long verdict = s->closed ? -1 : 0;
	if (!s->closed)
		media_keys_set_state(&s_table, &s->keys, state);
	spinlock_release_irqrestore(&s_lock, flags);
	return verdict;
}

// ── The presses ─────────────────────────────────────────────────────────────

void media_key_pressed(uint32_t key)
{
	uint64_t focus = wm_focus_owner();
	uint64_t now = micros_now();

	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	ready_locked();
	media_keys_route_t r = media_keys_press(&s_table, key, focus, now);
	if (r.target != NULL)
	{
		media_session_t *s = (media_session_t *)r.target;
		if (s->reader != NULL)
			scheduler_wake_or_owe(s->reader);
	}
	spinlock_release_irqrestore(&s_lock, flags);

	if (!r.kernel_toggle)
		return;
	// No session would take it. Decided under the lock, carried out after
	// it: audio_toggle takes the stream's lock and wakes under it, and is
	// one step there, so two presses at once are two toggles.
	audio_toggle_t toggled = audio_toggle();
	kernel_answer_t did = toggled == AUDIO_TOGGLE_PAUSED ? KERNEL_PAUSED
	                    : toggled == AUDIO_TOGGLE_RESUMED ? KERNEL_RESUMED : KERNEL_REFUSED;
	flags = spinlock_acquire_irqsave(&s_lock);
	s_kernel_last = did;
	if (did == KERNEL_PAUSED)
		s_kernel_paused++;
	else if (did == KERNEL_RESUMED)
		s_kernel_resumed++;
	else
		media_keys_kernel_failed(&s_table);
	spinlock_release_irqrestore(&s_lock, flags);
}

static const char *const kKeyNames[] = { "none", "play", "pause", "next", "previous", "stop" };

static const char *key_name(uint32_t key)
{
	return key < sizeof(kKeyNames) / sizeof(kKeyNames[0]) ? kKeyNames[key] : "?";
}

long media_keys_command(const char *text, size_t length)
{
	static const char kVerb[] = "press ";
	size_t verb = sizeof(kVerb) - 1;
	if (length <= verb || memcmp(text, kVerb, verb) != 0)
		return -1;
	const char *word = text + verb;
	size_t n = length - verb;
	while (n > 0 && (word[n - 1] == ' ' || word[n - 1] == '\t' || word[n - 1] == '\n' || word[n - 1] == '\r'))
		n--;
	for (uint32_t key = OS64_MEDIA_KEY_PLAY; key <= OS64_MEDIA_KEY_STOP; key++)
	{
		const char *name = kKeyNames[key];
		if (strlen(name) == n && memcmp(word, name, n) == 0)
		{
			media_key_pressed(key);
			return 0;
		}
	}
	return -1;
}

// ── /sys/mediakeys ──────────────────────────────────────────────────────────

static const char *state_name(uint8_t state)
{
	return state == MEDIA_KEYS_PLAYING ? "playing" : state == MEDIA_KEYS_PAUSED ? "paused" : "stopped";
}

static const char *rule_name(uint32_t rule)
{
	switch (rule)
	{
		case MEDIA_KEYS_RULE_PLAYING: return "playing";
		case MEDIA_KEYS_RULE_FOCUS:   return "focus";
		case MEDIA_KEYS_RULE_ACTIVE:  return "most recently active";
		case MEDIA_KEYS_RULE_KERNEL:  return "kernel";
		default:                      return "none yet";
	}
}

typedef struct
{
	uint64_t task_id, active_seq, delivered, lost;
	uint32_t serial, unread;
	uint8_t  state;
	bool     passed_full, last;
	char     name[32];
} session_row_t;

size_t media_keys_report(char *out, size_t capacity)
{
	session_row_t *rows = kmalloc(sizeof(session_row_t) * MEDIA_KEYS_SESSIONS);
	if (rows == NULL || capacity == 0)
	{
		if (rows) kfree(rows);
		return 0;
	}
	uint32_t count = 0;
	// Copied under the lock, formatted after it.
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	ready_locked();
	media_keys_table_t t = s_table;
	const char *kernel_last = kKernelAnswers[s_kernel_last];
	uint64_t paused = s_kernel_paused, resumed = s_kernel_resumed;
	for (uint32_t i = 0; i < MEDIA_KEYS_SESSIONS; i++)
	{
		media_session_t *s = (media_session_t *)s_table.slot[i];
		if (s == NULL)
			continue;
		session_row_t *row = &rows[count++];
		row->task_id = s->task_id;
		memcpy(row->name, s->name, sizeof(row->name));
		row->state = s->keys.state;
		row->active_seq = s->keys.active_seq;
		row->serial = s->keys.serial;
		row->delivered = s->keys.delivered;
		row->lost = s->keys.lost;
		row->unread = s->keys.count;
		row->passed_full = s->keys.passed_full;
		row->last = s_table.last_slot == (int32_t)i;
	}
	spinlock_release_irqrestore(&s_lock, flags);

	size_t used = 0;
	int n = snprintf(out, capacity,
		"sessions: %u of %u\n"
		"focus_owner: %lu\n"
		"presses: %lu\n"
		"last_press: %s\n"
		"last_rule: %s\n"
		"last_given: %s\n"
		"last_focus: %lu\n"
		"kernel: last %s, paused %lu, resumed %lu\n"
		"unanswered: %lu\n",
		count, MEDIA_KEYS_SESSIONS, wm_focus_owner(), t.presses,
		key_name(t.last_key), rule_name(t.last_rule), key_name(t.last_delivered),
		t.last_focus, kernel_last, paused, resumed, t.unanswered);
	if (n > 0)
		used = (size_t)n < capacity ? (size_t)n : capacity - 1;
	for (uint32_t i = 0; i < count && used + 1 < capacity; i++)
	{
		session_row_t *row = &rows[i];
		char name[sizeof(row->name) * 4 + 1];
		synth_text_escape(row->name, name, sizeof(name));
		n = snprintf(out + used, capacity - used,
			"session: task %lu %s state %s active %lu presses %u read %lu lost %lu unread %u%s%s\n",
			row->task_id, name, state_name(row->state), row->active_seq, row->serial,
			row->delivered, row->lost, row->unread,
			row->passed_full ? " passed-over-full" : "", row->last ? " last-press" : "");
		if (n > 0)
			used += (size_t)n < capacity - used ? (size_t)n : capacity - used - 1;
	}
	kfree(rows);
	return used;
}
