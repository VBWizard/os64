#ifndef WAY_INTERNAL_H
#define WAY_INTERNAL_H

// The seam between libway's two halves.

#include "way/way.h"

// Sets session->status.
void way_say(way_session_t *session, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Whether an address can be offered inside single quotes as a command.
bool way_shell_quotable(const char *address);

// JSON by its own type or a `+json` suffix (application/ld+json and the
// rest). RFC 8259 makes it UTF-8, so no charset still means UTF-8. Lives
// with the pure half because way_text_utf8 asks it; the loader's "is this
// text at all" asks it too. A media type from libfetch is already
// lowercased, so the compare is verbatim.
bool way_type_is_json(const char *type);

// The cache's store (way/cache.h § Entries) is way_fetch_whole's as it is
// the fetch job's; way_cache_count is the counts a person is shown, and
// only libway's fetches are counted.
void way_cache_count(way_cache_t *cache, way_served_t how);

#endif
