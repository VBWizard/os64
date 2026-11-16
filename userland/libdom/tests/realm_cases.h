/* Shared host/guest adoption oracle; raw realm exchange is trusted fixture setup. */
#ifndef DOM_REALM_CASES_H
#define DOM_REALM_CASES_H
static const char dom_realm_adoption_oracle[] =
                "globalThis.container=document.createElement('section');container.expando=42;\n"
                "document.body.appendChild(container);\n"
                "assert(B.document.adoptNode(container)===container);assert(container.ownerDocument===B.document);\n"
                "B.document.body.appendChild(container);\n"
                "container.innerHTML='<p>born in B</p><!--B-->';\n"
                "assert(C.document.adoptNode(container)===container);C.document.body.appendChild(container);\n"
                "assert(container instanceof A.HTMLElement && !(container instanceof C.HTMLElement));\n"
                "assert(container.firstChild instanceof B.HTMLElement && !(container.firstChild instanceof A.HTMLElement));\n"
                "assert(container.lastChild instanceof B.Comment);\n"
                "assert(container.firstChild.ownerDocument===C.document&&container.expando===42);\n"
                "assert(C.document.querySelector('section')===container);\n"
                "globalThis.fresh=A.Document.prototype.createElement.call(B.document,'em');\n"
                "assert(fresh instanceof B.HTMLElement&&fresh.ownerDocument===B.document);\n"
                "globalThis.copy=B.document.importNode(container,true);\n"
                "assert(copy!==container&&copy instanceof B.HTMLElement&&copy.firstChild instanceof B.HTMLElement);\n"
                "assert(copy.ownerDocument===B.document&&container.ownerDocument===C.document);";
#endif
