// bootenv.c — apply bootenv.conf to the environment every task inherits.
// The design is in bootenv.h; this file is the mechanics.
#include "bootenv.h"

#include "BasicRenderer.h"      // printf — a malformed line belongs on the glass too
#include "conf.h"
#include "env.h"
#include "memory/kmalloc.h"
#include "serial_logging.h"
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
	bool        full;      // said once per file, then the rest is skipped
} bootenv_apply_t;

static void bootenv_line(const char *key, const char *value, void *user)
{
	bootenv_apply_t *a = (bootenv_apply_t *)user;

	if (key == NULL) {
		// Not `NAME = value`. Loud on both sinks: the commonest spelling is a
		// value with no name, which looks fine and sets nothing.
		printf("%s: not NAME = value, ignored: %s\n", a->path, value);
		printd(DEBUG_BOOT, "bootenv: %s: not NAME = value, ignored: %s\n", a->path, value);
		a->bad++;
		return;
	}

	if (value[0] == '\0') {
		// `NAME =` with nothing after it removes the variable — `unset`'s
		// idempotent shape, so unsetting the absent is not an error.
		env_unset(a->task->env, key);
		a->unset++;
		return;
	}

	if (a->full)
		return;
	if (!env_set(a->task->env, key, value)) {
		// The block the kernel task was born with is one page, and the whole
		// boot environment has to fit in it: the block is not grown here,
		// because the kernel task's window is not remapped the way
		// syscall_setenv remaps a program's, and a swapped block behind a
		// stale mapping is a lifetime bug waiting for a reader. A page holds
		// a hundred ordinary variables; a file that needs more is asking the
		// boot environment to be something it is not, and is told so.
		printf("%s: environment block full at %s — the rest of the file is ignored\n",
		       a->path, key);
		printd(DEBUG_BOOT, "bootenv: %s: environment block full at %s — the rest of the file is ignored\n",
		       a->path, key);
		a->full = true;
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
	       a.bad ? ", lines ignored" : "", a.full ? ", block full" : "");
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

	// The boot cmdline's TZ= outranks the file (bootenv.h says why), and
	// whichever zone won also becomes the kernel's own standard-time offset,
	// so the time() fallback and the kernel's few displays agree with the
	// environment every program reads.
	if (kTZString[0])
		env_set(task->env, "TZ", kTZString);
	time_set_zone(env_get(task->env, "TZ"));
}
