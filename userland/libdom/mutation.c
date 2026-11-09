#include "internal.h"

/* A document has one listener. Feature rows add C consumers here so their
 * installation cannot replace one another's reset/reaction notifications. */
static void changed(void *opaque, const os64_html_change_t *record)
{
    os64_dom_t *dom = opaque;
    if (dom->closed) return;
    /* Queue observer records before consumers can run custom reactions. */
    d_observers_changed(dom, record);
    for (DMutationConsumer *c = dom->mutation_consumers; c; c = c->next)
        c->changed(dom, record);
}
void d_mutation_register(os64_dom_t *dom, DMutationConsumer *consumer,
                         void (*callback)(os64_dom_t *, const os64_html_change_t *))
{
    consumer->changed = callback;
    consumer->next = dom->mutation_consumers;
    dom->mutation_consumers = consumer;
}
void d_mutation_start(os64_dom_t *dom)
{
    os64_html_observe(dom->document, changed, dom);
    dom->mutation_installed = true;
}
void d_mutation_stop(os64_dom_t *dom)
{
    if (dom->mutation_installed) os64_html_observe(dom->document, NULL, NULL);
    dom->mutation_installed = false;
}
