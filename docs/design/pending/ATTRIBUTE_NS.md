# Namespaced attributes: setAttributeNS and its family

The Quick wins row "`setAttributeNS` and the other namespaced attribute
verbs are absent" (DEBTS.md), with the `document.links` namespace row it
names. One PR.

## 1. Why

React DOM, Vue and most SVG icon-sprite loaders set `xlink:href` on a `use`
with `setAttributeNS`; 92 of the corpus's 323 hosts call it. The first icon
throws inside the framework's commit and the page goes to its error view.
The verbs are `setAttributeNS`, `getAttributeNS`, `hasAttributeNS` and
`removeAttributeNS` on Element. `getAttributeNodeNS`, `setAttributeNodeNS`,
`getNamedItemNS` and `removeNamedItemNS` existed, but before this slice an
Attr whose namespace the tree would lose was refused with NotSupportedError.

## 2. The identity changes

DOM finds an attribute two ways: by qualified name (first match, any
namespace), and by namespace and local name (unique on an element). libhtml
and libdom know only the first, and libdom's table of attached Attrs is keyed
by element and qualified name on the premise that "no verb makes two of one
name". `setAttributeNS` breaks the premise: `setAttribute('foo')` then
`setAttributeNS('urn:x', 'foo')` leaves two attributes named `foo`.

So the identity becomes DOM's own, **element + namespace + local name**,
throughout:

- **libhtml.** `os64_html_attr_change_t` gains `ns` and `by_ns`. A change
  `by_ns` addresses the record by namespace and the local part of `name`. A
  set keeps an existing record's qualified name (DOM's "change an attribute"
  keeps its prefix), and a new one takes `name` and `ns`. A change without
  `by_ns` behaves exactly as today. `os64_html_attr_ns(element, ns, local)`
  is the namespace lookup.
- **libpage.** `os64_page_node_set_attr_ns` beside `os64_page_node_set_attr`,
  one implementation underneath: its prospective list finds the attribute
  by namespace and local name for the first and by the first of a qualified
  name for the second, as libhtml will apply each, so control state is
  staged from what the tree will hold. Its readers (`p_attr`) read the
  attribute in no namespace, so a namespaced change stages nothing and goes
  straight to the tree: it changes no control's state, as other browsers'
  controls ignore it (WPT namespaced-attribute-mutations).
- **libdom.** The Attr table hashes and compares namespace + local name,
  and an attached Attr reads its value by that pair. The door keeps its
  qualified-name form, `d_set_attr` (the first attribute of the name, the
  Attr made for that record detached on a removal), and gains a twin by
  namespace, `d_set_attr_ns`, which every path that knows its Attr's
  namespace takes. The backstop asks for the Attr's namespace and local
  name, not its qualified name.

`setAttributeNode` of a namespaced Attr is no longer refused: the door can
put it where the Attr says it belongs. One refusal stays, an Attr whose
prefix differs from the attribute of its namespace and local name already
there (a set keeps that one's qualified name). The classic row "No
namespaced attribute verbs" is paid with it.

## 3. The verbs

DOM § 4.9. `setAttributeNS(ns, qualifiedName, value)`: "validate and
extract" for an attribute (the steps createAttributeNS takes, now one
function both share), then "set an attribute value" by (ns, local). `getAttributeNS`
and `hasAttributeNS` look up by (ns, local). `removeAttributeNS` removes by
(ns, local). A namespace of `""` is none. Each is [CEReactions]
(ce_reactions.inc already lists them; the wrappers take them once they
exist), and a MutationRecord carries `attributeNamespace` (libhtml's
listener already passes it).

## 4. HTML reads an attribute in no namespace

HTML's semantics ("has an `href` attribute", a reflected IDL attribute)
read the attribute in no namespace. Until now only a namespaced attribute
the parser made (an SVG `xlink:href`) could exist, so a qualified-name read
was never wrong for HTML. With `setAttributeNS('urn:x', 'href', …)` it can
be. This slice changes the readers whose answer is HTML's own to
`os64_html_attr_ns(element, NULL, …)`: "has an `href`" for
`document.links`, focus, activation and following a link; `id` and `name`
for the window's named properties; and an event handler content
attribute; and libpage's controls. attributeChangedCallback's new value
is read by the change's own namespace and local name. Every other reader
(reflection at large, the anchor's `href` reflection with it, the
cascade's attribute selectors) stays a qualified-name read, booked as one DEBTS row:
a page that sets an unprefixed namespaced attribute that shares an HTML
attribute's name.

## 5. Proof

- Host cases for each verb against DOM's rules: validate and extract's
  NamespaceErrors, a set that keeps the existing prefix, two attributes of
  one qualified name in two namespaces held apart (each verb finds the
  right one, each Attr its own), `setAttributeNode` of a namespaced Attr,
  `removeAttributeNS`, and MutationRecord's `attributeNamespace`.
- libhtml's change-by-namespace path (`os64_html_set_attrs`, its listener report)
  and libpage's namespaced verb are driven through those libdom cases.
- WPT dom/nodes: attributes.html, attributes-namednodemap.html and
  Element-setAttributeNodeNS.html gain passes.
- An icon sprite: yonder's SVG renderer finds a `use`'s XLink `href` by
  namespace and local name (svg.c `href_target`), so the attribute
  `setAttributeNS(XLINK, 'xlink:href', …)` makes is the one it draws from;
  the host case checks the attribute lands in the XLink namespace.
