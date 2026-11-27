// maplist.c — a task's list of mappings and each mapping's lifetime
// (memory/maplist.h).

#include "memory/maplist.h"

void maplist_init(maplist_t *l)
{
    *l = (maplist_t){0};
}

void maplist_link(maplist_t *l, maplist_node_t *n, uintptr_t start, uintptr_t end)
{
    n->start = start;
    n->end = end;
    n->holds = 0;
    n->listed = true;
    n->prev = NULL;
    n->next = l->head;
    if (l->head != NULL)
        l->head->prev = n;
    l->head = n;
    l->count++;
}

// Off the list where it is, onto its head. `n` is listed and not the head.
static void move_to_head(maplist_t *l, maplist_node_t *n)
{
    n->prev->next = n->next;
    if (n->next != NULL)
        n->next->prev = n->prev;
    n->prev = NULL;
    n->next = l->head;
    l->head->prev = n;
    l->head = n;
}

// The walk, counted: `start_only` matches an exact start, otherwise a
// range holding `addr`, which moves what it finds to the head.
static maplist_node_t *walk(maplist_t *l, uintptr_t addr, bool start_only)
{
    uint64_t steps = 0;
    maplist_node_t *found = NULL;
    for (maplist_node_t *n = l->head; n != NULL; n = n->next) {
        steps++;
        if (start_only ? n->start == addr : (addr >= n->start && addr < n->end)) {
            found = n;
            break;
        }
    }
    l->lookups++;
    if (found == NULL)
        l->misses++;
    else if (!start_only && found != l->head)
        move_to_head(l, found);
    l->steps += steps;
    if (steps > l->steps_most)
        l->steps_most = steps;
    if (steps > l->window_steps_most)
        l->window_steps_most = steps;
    return found;
}

void maplist_read_walks(maplist_t *l, maplist_walks_t *all, maplist_walks_t *since)
{
    *all = (maplist_walks_t){ l->lookups, l->misses, l->steps, l->steps_most };
    *since = (maplist_walks_t){ l->lookups - l->read_lookups, l->misses - l->read_misses, l->steps - l->read_steps,
                                l->window_steps_most };
    l->read_lookups = l->lookups;
    l->read_misses = l->misses;
    l->read_steps = l->steps;
    l->window_steps_most = 0;
}

maplist_node_t *maplist_find(maplist_t *l, uintptr_t addr)
{
    return walk(l, addr, false);
}

maplist_node_t *maplist_find_hold(maplist_t *l, uintptr_t addr)
{
    maplist_node_t *n = walk(l, addr, false);
    if (n != NULL)
        n->holds++;
    return n;
}

maplist_node_t *maplist_find_start(maplist_t *l, uintptr_t start)
{
    return walk(l, start, true);
}

void maplist_hold(maplist_node_t *n)
{
    n->holds++;
}

bool maplist_unlink(maplist_t *l, maplist_node_t *n)
{
    if (!n->listed)
        return false;
    if (n->prev != NULL)
        n->prev->next = n->next;
    else
        l->head = n->next;
    if (n->next != NULL)
        n->next->prev = n->prev;
    n->prev = n->next = NULL;
    n->listed = false;
    l->count--;
    return n->holds == 0;
}

bool maplist_release(maplist_node_t *n)
{
    if (n->holds == 0)
        return false;
    n->holds--;
    return n->holds == 0 && !n->listed;
}

maplist_node_t *maplist_take_all(maplist_t *l, uint64_t *held)
{
    maplist_node_t *chain = l->head;
    *held = 0;
    for (maplist_node_t *n = chain; n != NULL; n = n->next) {
        n->listed = false;
        n->prev = NULL;     // `next` stays: it is the chain handed back
        *held += n->holds;
    }
    l->head = NULL;
    l->count = 0;
    return chain;
}
