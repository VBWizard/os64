#ifndef MEMORY_MAPLIST_H
#define MEMORY_MAPLIST_H

// maplist.h — a task's list of mappings and each mapping's lifetime
// (docs/design/pending/MAPPING_LIST.md). Pure: no kernel headers, no locks,
// no allocation; the caller holds the task's mapLock around every call that
// takes a list (vma.c), and frees a mapping when a call says to, outside the
// lock. tools/test_maplist_host.sh drives it beside a model.
//
// A MAPPING'S LIFE: made (its node not linked), LISTED (linked; anyone may
// find it under the lock), DELISTED (unlinked; only its holders still reach
// it), and FREED, by whoever's call says so: the unlink that finds no
// holder, or the release that gives back its last hold while it is
// delisted. A node's range is fixed once it is linked.
//
// The list is linked at the HEAD, and a lookup by address that finds a node
// MOVES IT TO THE HEAD: a fresh region is the one about to fault a thousand
// times, and an old one in use (the image, a library, the first heap arena)
// would otherwise sit behind every region made since. So the list's order is
// most recently found or made first, and a reader that copies only part of
// the list (/proc's maps past its walk guard) gets those.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct maplist_node {
    struct maplist_node *prev, *next;
    uintptr_t start, end;           // [start, end)
    uint32_t holds;                 // callers using it outside the lock
    bool listed;
} maplist_node_t;

typedef struct {
    maplist_node_t *head;
    uint32_t count;
    // The walk: lookups made, lookups that found nothing (each walked the
    // whole list), nodes stepped over in all, and the longest single walk,
    // since the list began; and where the last read left them
    // (maplist_read_walks), with the longest walk since.
    uint64_t lookups, misses, steps, steps_most;
    uint64_t read_lookups, read_misses, read_steps, window_steps_most;
} maplist_t;

typedef struct {
    uint64_t lookups, misses, steps, steps_most;
} maplist_walks_t;

void maplist_init(maplist_t *l);

// `n` linked at the head over [start, end), listed, with no holds.
void maplist_link(maplist_t *l, maplist_node_t *n, uintptr_t start, uintptr_t end);

// The listed node whose range holds `addr`, NULL for none, moved to the
// head when found. Counted.
maplist_node_t *maplist_find(maplist_t *l, uintptr_t addr);

// maplist_find, and a hold on what it found: the caller may use it after
// the lock, and gives it back with maplist_release.
maplist_node_t *maplist_find_hold(maplist_t *l, uintptr_t addr);

// The listed node starting exactly at `start`, NULL for none; not moved
// (its caller is about to delist it). Counted.
maplist_node_t *maplist_find_start(maplist_t *l, uintptr_t start);

// A hold taken on `n`, which the caller already reaches (listed, or held).
void maplist_hold(maplist_node_t *n);

// `n` unlinked and delisted: no lookup finds it from now on. True when
// nothing holds it, and the caller frees it.
bool maplist_unlink(maplist_t *l, maplist_node_t *n);

// One hold on `n` given back. True when that was the last, and `n` is
// delisted: the caller frees it. A release with no hold to give back is
// refused (false, nothing changes), so a double release cannot free twice.
bool maplist_release(maplist_node_t *n);

// The walks since the list began (`all`) and since the last read (`since`);
// the read starts the window again.
void maplist_read_walks(maplist_t *l, maplist_walks_t *all, maplist_walks_t *since);

// Every node unlinked and delisted at once, for a task's burial, returned
// as a chain through `next` in the list's order; the list is
// left empty. Each is freed by the caller whatever its holds, since no
// thread of the task is left to give one back; `*held` counts the holds
// found outstanding.
maplist_node_t *maplist_take_all(maplist_t *l, uint64_t *held);

#endif
