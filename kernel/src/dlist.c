#include "dlist.h"
#include "kmalloc.h"

void dlist_init(dlist_t* list) {
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}

/// @brief Adds a new node to the end of a dlist_t
/// @param The list to add the node to
/// @param Pointer to the data the node will hold
dlist_node_t* dlist_add(dlist_t* list, volatile void* data) {
    dlist_node_t* new_node = kmalloc(sizeof(dlist_node_t));
    new_node->data = data;
    new_node->next = NULL;
    if (list->tail == NULL) { // Empty list
        new_node->prev = NULL;
        list->head = new_node;
        list->tail = new_node;
    } else {
        new_node->prev = list->tail;
        list->tail->next = new_node;
        list->tail = new_node;
    }
    list->size++;
	return new_node;
}

void dlist_remove(dlist_t* list, dlist_node_t* node) {
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else { // Removing the head
        list->head = node->next;
    }

    if (node->next != NULL) {
        node->next->prev = node->prev;
    } else { // Removing the tail
        list->tail = node->prev;
    }

    kfree(node);
    list->size--;
}

/// @brief Destroy a dlist_t.  Caller remains responsible for freeing node data.
/// @param list 
void dlist_destroy(dlist_t* list) 
{
    dlist_node_t* current = list->head;
    dlist_node_t* next;

    while (current != NULL) 
	{
        next = current->next;
        kfree(current);

        current = next;
    }

    // Reset the list structure
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}
