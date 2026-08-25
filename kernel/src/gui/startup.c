// startup.c — what starts with the desktop. Contract and the ruling behind
// it are in gui/startup.h; this file is the reader.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gui/startup.h"

#include "CONFIG.h"
#include "conf.h"          // conf_find / conf_read_file / conf_parse
#include "kmalloc.h"
#include "printd.h"
#include "strings/strcmp.h"
#include "strings/strlen.h"

#define GUI_STARTUP_CONF_MAX 8192   // the 8KB cap every os64 config reader uses

// The built-in list, used ONLY when no gui.conf is found anywhere: the two
// demo apps the GUI has launched since they were ported to ring 3
// (2026-08-17). An absent config file must leave the machine behaving exactly
// as it did before the config file existed — that is the same promise
// /etc/os64.conf makes about the search path itself.
static const char *const kDefaultApps[] = { "/bin/gbounce", "/bin/gkeys" };

static char   s_apps[GUI_STARTUP_MAX_APPS][GUI_STARTUP_PATH_MAX];
static size_t s_app_count = 0;
static bool   s_hello = true;
static bool   s_from_file = false;

static void startup_line(const char *key, const char *value, void *user)
{
	const char *path = (const char *)user;

	if (key == NULL) {
		// The commonest config mistake there is: a value written with no key.
		// Said out loud, because the alternative is a setting that looks
		// right and does nothing (logd's lesson, and gclock.conf's the same
		// afternoon this file was written).
		printd(DEBUG_BOOT, "gui.conf: %s: not 'key = value' — ignored: %s\n",
		       path, value);
		return;
	}

	if (strcmp(key, "start") == 0) {
		if (value[0] == '\0') {
			printd(DEBUG_BOOT, "gui.conf: %s: 'start' with no program — ignored\n", path);
			return;
		}
		if (s_app_count >= GUI_STARTUP_MAX_APPS) {
			printd(DEBUG_BOOT, "gui.conf: %s: more than %u start lines — '%s' ignored\n",
			       path, (uint32_t)GUI_STARTUP_MAX_APPS, value);
			return;
		}
		size_t len = strlen(value);
		if (len >= GUI_STARTUP_PATH_MAX) {
			printd(DEBUG_BOOT, "gui.conf: %s: start path over %u characters — ignored: %s\n",
			       path, (uint32_t)GUI_STARTUP_PATH_MAX - 1, value);
			return;
		}
		for (size_t i = 0; i <= len; i++)
			s_apps[s_app_count][i] = value[i];
		s_app_count++;
		return;
	}

	if (strcmp(key, "hello") == 0) {
		// yes/no, because the value is an ANSWER to "should the hello window
		// be shown". (gclock.conf's `Pinned = true` is a different question
		// shape; both spellings live in os64 and neither is wrong. What is
		// wrong is guessing — an unrecognized value is refused, loudly.)
		if (strcmp(value, "yes") == 0)
			s_hello = true;
		else if (strcmp(value, "no") == 0)
			s_hello = false;
		else
			printd(DEBUG_BOOT, "gui.conf: %s: hello wants yes or no — ignored: %s\n",
			       path, value);
		return;
	}

	printd(DEBUG_BOOT, "gui.conf: %s: unknown setting '%s' — ignored\n", path, key);
}

void gui_startup_load(void)
{
	char found[CONF_PATH_MAX];
	if (!conf_find("gui.conf", found, sizeof(found))) {
		// No file anywhere: the built-in list, and hello stays.
		for (size_t i = 0; i < sizeof(kDefaultApps) / sizeof(kDefaultApps[0]); i++) {
			size_t len = strlen(kDefaultApps[i]);
			for (size_t j = 0; j <= len; j++)
				s_apps[s_app_count][j] = kDefaultApps[i][j];
			s_app_count++;
		}
		printd(DEBUG_BOOT, "gui: no gui.conf — starting the built-in demos, hello window on\n");
		return;
	}

	size_t   len = 0;
	uint8_t *text = conf_read_file(found, GUI_STARTUP_CONF_MAX, &len);
	if (text == NULL) {
		// It opened for the walker and not for us — gone since, or over the
		// cap. NOT a reason to fall back to the demos: the operator has a
		// gui.conf, and quietly starting things they did not ask for because
		// their file was unreadable is a worse answer than starting nothing.
		printd(DEBUG_BOOT, "gui: %s could not be read (over %u bytes?) — starting nothing\n",
		       found, (uint32_t)GUI_STARTUP_CONF_MAX);
		s_from_file = true;
		return;
	}

	s_from_file = true;
	conf_parse((char *)text, startup_line, found);
	kfree(text);

	// One line of boot news naming what the desktop is about to do, in the
	// same breath as which file said so.
	printd(DEBUG_BOOT, "gui: %s — %lu app(s) to start, hello window %s\n",
	       found, (uint64_t)s_app_count, s_hello ? "on" : "off");
	for (size_t i = 0; i < s_app_count; i++)
		printd(DEBUG_BOOT, "gui:   start %s\n", s_apps[i]);
}

bool gui_startup_hello(void)
{
	return s_hello;
}

size_t gui_startup_app_count(void)
{
	return s_app_count;
}

const char *gui_startup_app(size_t index)
{
	return (index < s_app_count) ? s_apps[index] : NULL;
}
