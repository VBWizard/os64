#ifndef YONDER_HOST_MENUS_H
#define YONDER_HOST_MENUS_H
/* The yonder hosts' menu session (yonder_host_menus.c): the last list a
 * dropdown opened, and a pick played into it. */
#include <stdbool.h>
#include <stdio.h>
#include "os64/ui_menu.h"

#define YONDER_HOST_LIST_ROWS 16

typedef struct {
    unsigned opens, detaches;
    bool open;
    size_t count, selected;
    char labels[YONDER_HOST_LIST_ROWS][64];
    bool enabled[YONDER_HOST_LIST_ROWS];
    void (*pick)(void *, size_t);
    void *user;
} yonder_host_list_t;

extern yonder_host_list_t yonder_host_list;

/* The person commits `row`: the list closes, then its pick runs, as the
 * session does. False when no list is open. */
bool yonder_host_list_pick(size_t row);

#endif
