#ifndef YONDER_HOST_MENUS_H
#define YONDER_HOST_MENUS_H
/* The yonder hosts' menu session (yonder_host_menus.c): the last list a
 * dropdown opened (and its rows as last relabeled), and a pick played into
 * it. And the hosts' icon theme: its leases out, and a switch to refuse. */
#include <stdbool.h>
#include <stdio.h>
#include "image/icon_theme.h"
#include "os64/ui_action.h"
#include "os64/ui_menu.h"

#define YONDER_HOST_LIST_ROWS 16

typedef struct {
    unsigned opens, detaches, relabels, menus;     /* menus: os64_ui_menu_open's */
    bool open;
    size_t count, selected;
    char labels[YONDER_HOST_LIST_ROWS][64];
    bool enabled[YONDER_HOST_LIST_ROWS];
    void (*pick)(void *, size_t);
    void *user;
} yonder_host_list_t;

extern yonder_host_list_t yonder_host_list;

typedef struct {
    int out;            /* leases acquired and not yet released */
    bool refuse;        /* every acquire answers MISSING */
} yonder_host_icons_t;

extern yonder_host_icons_t yonder_host_icons;

/* The person commits `row`: the list closes, then its pick runs, as the
 * session does. False when no list is open. */
bool yonder_host_list_pick(size_t row);

#endif
