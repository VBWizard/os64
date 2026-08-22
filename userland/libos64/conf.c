// conf.c — the `key = value` reader. Contract and dialect in os64/conf.h.
//
// Lifted from logd.c's logd_conf_value on 2026-08-22, the day os64get became
// the second program with a config file. The shape is the same: read the
// whole file to EOF (a short read is legal and a file's meaning can live in
// its last line), then walk it a line at a time — chop the comment, find the
// key, insist on the '=', trim the value. What changed is that every line is
// HANDED OUT rather than matched against one wanted key, so a caller whose
// file is a list of rules (os64get) gets them in order, and a caller who
// wants one setting (logd) picks it out in the callback.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "os64/conf.h"
#include "os64/io.h"

int64_t os64_conf_read(const char *path, os64_conf_fn fn, void *user)
{
	int64_t fd = os64_open(path, "r");
	if (fd < 0)
		return OS64_CONF_NO_FILE;

	// static: 8KB would blow a user stack (logd learned this first).
	static char buf[OS64_CONF_MAX];
	size_t got = 0;
	bool truncated = false;
	for (;;) {
		int64_t n = os64_read((int32_t)fd, buf + got, sizeof(buf) - 1 - got);
		if (n <= 0)
			break;
		got += (size_t)n;
		if (got >= sizeof(buf) - 1) {
			truncated = true;
			break;
		}
	}
	os64_close((int32_t)fd);
	buf[got] = '\0';

	int64_t delivered = 0;
	size_t i = 0;
	while (buf[i] != '\0') {
		// One line: find its bounds, then chop the comment off.
		size_t start = i;
		while (buf[i] != '\0' && buf[i] != '\n')
			i++;
		size_t end = i;
		if (buf[i] == '\n')
			i++;
		for (size_t c = start; c < end; c++)
			if (buf[c] == '#') { end = c; break; }

		// key: the first token
		size_t k = start;
		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		size_t ks = k;
		while (k < end && buf[k] != ' ' && buf[k] != '\t' && buf[k] != '=')
			k++;
		size_t ke = k;
		if (ks == ke)
			continue;   // blank, or comment-only

		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		if (k >= end || buf[k] != '=') {
			// Not `key = value`. Hand the caller the line as written (less
			// its comment and trailing whitespace) and let it complain in
			// its own voice.
			size_t le = end;
			while (le > ks && (buf[le - 1] == ' ' || buf[le - 1] == '\t' || buf[le - 1] == '\r'))
				le--;
			char saved = buf[le];
			buf[le] = '\0';
			bool go = fn(NULL, &buf[ks], user);
			buf[le] = saved;
			if (!go)
				return delivered;
			continue;
		}
		k++;   // past '='

		// value: to end of line, trimmed both sides, never split on spaces.
		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		size_t vs = k, ve = end;
		while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t' || buf[ve - 1] == '\r'))
			ve--;

		// Terminate both in place for the call, then put the bytes back:
		// the walk above still needs the line intact (ke may equal vs's
		// preceding '=', never the same byte as ve, so the two NULs are
		// independent).
		char savedK = buf[ke], savedV = buf[ve];
		buf[ke] = '\0';
		buf[ve] = '\0';
		bool go = fn(&buf[ks], &buf[vs], user);
		buf[ke] = savedK;
		buf[ve] = savedV;
		delivered++;
		if (!go)
			return delivered;
	}

	return truncated ? OS64_CONF_TRUNCATED : delivered;
}

int64_t os64_conf_read_first(const char *const *paths, size_t count,
                             os64_conf_fn fn, void *user, size_t *which)
{
	for (size_t i = 0; i < count; i++) {
		// Open first, read only if it opens — os64_conf_read does exactly
		// that and reports NO_FILE for the misses, so "first that opens"
		// is just "first that doesn't say NO_FILE".
		int64_t rc = os64_conf_read(paths[i], fn, user);
		if (rc != OS64_CONF_NO_FILE) {
			if (which)
				*which = i;
			return rc;
		}
	}
	return OS64_CONF_NO_FILE;
}
