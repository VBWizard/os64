#ifndef DLIST_H
#define DLIST_H

//Doubly linked list structure and related methods

#include <stddef.h>

typedef struct dlist_node dlist_node_t;

struct dlist_node {
    dlist_node_t* prev;
    dlist_node_t* next;
	int major;
	int minor;
    volatile void* data; // Pointer to the actual data (can be any type)
};

typedef struct DList {
    dlist_node_t* head;
    dlist_node_t* tail;
    size_t size; // Optional: Track size of the list
} dlist_t;

void dlist_init(dlist_t* list);
dlist_node_t* dlist_add(dlist_t* list, volatile void* data);
void dlist_remove(dlist_t* list, dlist_node_t* node);
void dlist_destroy(dlist_t* list);

#endif
