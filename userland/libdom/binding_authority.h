#ifndef LIBDOM_BINDING_AUTHORITY_H
#define LIBDOM_BINDING_AUTHORITY_H

#include <dom/dom.h>
#include <way/origin.h>

/* Private native preparation for Location member checks. No WindowProxy or
 * cross-origin object publication is provided by this interface. Use a settings-
 * enabled cluster and keep the normal grouped admission/teardown obligations.
 * Tuple origins must be canonical; opaque origins must be issued by this
 * cluster (or copied from a token it issued). Tokens are not URL strings.
 * Successful admission copies the origin before installation and charges its
 * storage to the binding/cluster. There is no origin setter after admission.
 * Refusal before ownership transfer leaves document/state with the caller. */
__attribute__((visibility("hidden")))
os64_dom_admission_t d_create_in_realm_with_origin(os64_dom_cluster_t *, os64_js_realm_t *,
    os64_html_document_t *, os64_page_state_t *, const os64_dom_options_t *,
    const way_origin_t *, os64_js_outcome_t *);
__attribute__((visibility("hidden")))
bool d_cluster_opaque_origin(os64_dom_cluster_t *, way_origin_t *);
/* Allocation-free issuer; exhaustion preserves both counter and output. */
__attribute__((visibility("hidden")))
bool d_next_opaque_origin(uint64_t *, way_origin_t *);

#endif
