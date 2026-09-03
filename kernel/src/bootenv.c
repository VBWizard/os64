// bootenv.c — apply bootenv.conf to the environment every task inherits.
// The design is in bootenv.h; this file is the mechanics.
#include "bootenv.h"

#include "BasicRenderer.h"      // printf — a malformed line belongs on the glass too
#include "conf.h"
#include "env.h"
#include "memory/kmalloc.h"
#include "panic.h"
#include "serial_logging.h"
#include "strcmp.h"
#include "time.h"

extern char kTZString[];

// The file name, and the cap every os64 config reader uses.
#define BOOTENV_NAME     "bootenv.conf"
#define BOOTENV_MAX      8192

typedef struct {
	task_t     *task;
	const char *path;      // the file being applied, for the complaints
	unsigned    set;
	unsigned    unset;
	unsigned    bad;
	bool        stopped;   // failure was reported once; the rest is skipped
} bootenv_apply_t;

// bootenv_apply runs once on ktask before any program is spawned. ktask has
// no user-side TASK_ENV_VIRT mapping (only ELF tasks receive one), so growing
// its kernel-owned block needs no page-table remap: later children simply
// inherit however many pages the completed seed owns. Ordinary running tasks
// grow through syscall_setenv, whose remap is the other half of this rule.
static bool bootenv_set(task_t *task, const char *key, const char *value)
{
	while (!env_set(task->env, key, value)) {
		envpage_t *bigger = env_grow(task->env, TASK_ENV_MAX_BYTES / PAGE_SIZE);
		if (bigger == NULL)
			return false;

		envpage_t *old = task->env;
		task->env = bigger;
		kfree(old);
	}
	return true;
}

static void bootenv_line(const char *key, const char *value, void *user)
{
	bootenv_apply_t *a = (bootenv_apply_t *)user;

	if (a->stopped)
		return;

	if (key == NULL) {
		// Not `NAME = value`. Loud on both sinks: the commonest spelling is a
		// value with no name, which looks fine and sets nothing.
		printf("%s: not NAME = value, ignored: %s\n", a->path, value);
		printd(DEBUG_BOOT, "bootenv: %s: not NAME = value, ignored: %s\n", a->path, value);
		a->bad++;
		return;
	}

	if (kTZString[0] && strcmp(key, "TZ") == 0) {
		// create_kernel_task already seeded the command-line TZ into ktask's
		// parent environment. A file assignment OR unset has lower precedence,
		// so it never gets to displace that reserved pair; keeping it resident
		// is what makes the final precedence guarantee independent of capacity.
		printd(DEBUG_BOOT, "bootenv: %s: TZ ignored — command-line TZ is in force\n",
		       a->path);
		return;
	}

	if (value[0] == '\0') {
		// `NAME =` with nothing after it removes the variable — `unset`'s
		// idempotent shape, so unsetting the absent is not an error.
		env_unset(a->task->env, key);
		a->unset++;
		return;
	}

	if (!bootenv_set(a->task, key, value)) {
		// The normal 64KB ceiling or allocator exhaustion is a refusal, not
		// permission to publish a partial replacement. env_set leaves the old
		// value intact; stop this file so its applied prefix is deterministic.
		printf("%s: environment cannot grow at %s — the rest of the file is ignored\n",
		       a->path, key);
		printd(DEBUG_BOOT, "bootenv: %s: environment cannot grow at %s — the rest of the file is ignored\n",
		       a->path, key);
		a->stopped = true;
		return;
	}
	a->set++;
}

static void bootenv_apply_file(task_t *task, const char *path)
{
	size_t   len  = 0;
	uint8_t *text = conf_read_file(path, BOOTENV_MAX, &len);
	if (text == NULL) {
		// Found by the walk a moment ago and unreadable now, or past the cap.
		// Either way the file's variables are not in force, and a boot that
		// silently ran without them would present as "my TZ stopped working".
		printf("%s: unreadable or larger than %u bytes — ignored\n", path, (unsigned)BOOTENV_MAX);
		printd(DEBUG_BOOT, "bootenv: %s: unreadable or larger than %u bytes — ignored\n",
		       path, (unsigned)BOOTENV_MAX);
		return;
	}

	bootenv_apply_t a = { .task = task, .path = path };
	conf_parse((char *)text, bootenv_line, &a);
	kfree(text);

	printd(DEBUG_BOOT, "bootenv: %s: %u set, %u unset%s%s\n", path, a.set, a.unset,
	       a.bad ? ", lines ignored" : "", a.stopped ? ", incomplete" : "");
}

void bootenv_apply(task_t *task)
{
	if (task == NULL || task->env == NULL)
		return;

	// Every copy on the ladder, in ladder order. The walk is resumable for
	// exactly this (conf_find_from — the hosts reader's shape).
	char   paths[CONF_MAX_DIRS][CONF_PATH_MAX];
	size_t found = 0;
	size_t from  = 0;
	while (found < CONF_MAX_DIRS) {
		int at = conf_find_from(BOOTENV_NAME, from, paths[found], CONF_PATH_MAX);
		if (at < 0)
			break;
		found++;
		from = (size_t)at + 1;
	}

	// Applied LAST TO FIRST, so the file earliest on the ladder has the final
	// word on every name it sets — env_set replaces, which is what makes
	// "layers over" one line of code rather than a merge.
	for (size_t i = found; i-- > 0;)
		bootenv_apply_file(task, paths[i]);

	if (found == 0)
		printd(DEBUG_BOOT, "bootenv: no %s on the search path — the built-in environment stands\n",
		       BOOTENV_NAME);

	// The boot cmdline's TZ= outranks the file (bootenv.h says why). File TZ
	// lines were skipped above, so this is a same-size reassertion of the pair
	// create_kernel_task seeded before the filesystem existed; it cannot need
	// capacity. Failure means that invariant broke, and booting in the wrong
	// zone would be the dishonest outcome. Whichever zone won also becomes the
	// kernel's standard-time offset so its displays agree with userland.
	if (kTZString[0] && !bootenv_set(task, "TZ", kTZString))
		panic("bootenv: command-line TZ could not be retained in the environment\n");
	time_set_zone(env_get(task->env, "TZ"));
}
