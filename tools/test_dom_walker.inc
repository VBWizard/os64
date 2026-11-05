/* TRAVERSAL (libdom/walker.c, DOM § 6): NodeFilter's constants and the two
 * interfaces; every TreeWalker move, with a filter that skips and one that
 * rejects, a function and an object, one that throws and one that walks
 * again; a current node outside the root; NodeIterator's two moves and the
 * steps a removal takes it through; and a filter that empties the tree the
 * walk stands in. */

#define WALK_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let caught=false;try{f()}catch(e){caught=true;" \
    "assert(e.name===n,'wanted '+n+' got '+e.name+': '+e.message)}assert(caught,'no '+n);}" \
    "function ids(w,step){const out=[];let n;while((n=w[step]()))out.push(n.id||n.nodeName);return out.join(',')}"

static void dom_walker_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, WALK_ASSERT "{"
        "assert(typeof NodeFilter==='function'&&NodeFilter.FILTER_ACCEPT===1&&NodeFilter.FILTER_REJECT===2&&"
        "NodeFilter.FILTER_SKIP===3&&NodeFilter.SHOW_ALL===0xFFFFFFFF&&NodeFilter.SHOW_ELEMENT===1&&"
        "NodeFilter.SHOW_TEXT===4&&NodeFilter.SHOW_COMMENT===0x80&&NodeFilter.SHOW_DOCUMENT_FRAGMENT===0x400,"
        "'the constants');"
        "throws('TypeError',()=>new NodeFilter());throws('TypeError',()=>NodeFilter());"
        "throws('TypeError',()=>new TreeWalker());throws('TypeError',()=>new NodeIterator());"
        "const d=Object.getOwnPropertyDescriptor(NodeFilter,'SHOW_TEXT');"
        "assert(!d.writable&&!d.configurable&&d.enumerable,'constants are fixed');"
        "throws('TypeError',()=>document.createTreeWalker());throws('TypeError',()=>document.createTreeWalker({}));"
        "throws('TypeError',()=>document.createTreeWalker(document.body,1,'no'));"
        "throws('TypeError',()=>document.createTreeWalker.call(document.body,document.body));"
        "const w0=document.createTreeWalker(document.body);"
        "assert(w0 instanceof TreeWalker&&w0.root===document.body&&w0.currentNode===document.body&&"
        "w0.whatToShow===0xFFFFFFFF&&w0.filter===null,'defaults');"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(TreeWalker.prototype,'root').get.call({}));"
        "throws('TypeError',()=>TreeWalker.prototype.nextNode.call(document.createNodeIterator(document.body)));"
        /* the tree every case walks */
        "const r=document.createElement('div');"
        "r.innerHTML='<p id=a><b id=a1>x</b>t</p><!--c--><span id=c><i id=c1></i></span>tail';"
        "document.body.appendChild(r);"
        "const E=NodeFilter.SHOW_ELEMENT;"
        "let w=document.createTreeWalker(r,E);"
        "assert(ids(w,'nextNode')==='a,a1,c,c1','nextNode in tree order');"
        "assert(ids(w,'previousNode')==='c,a1,a,DIV','previousNode back to the root');"
        "assert(w.currentNode===r&&w.firstChild().id==='a'&&w.nextSibling().id==='c'&&w.nextSibling()===null&&"
        "w.previousSibling().id==='a'&&w.lastChild().id==='a1'&&w.parentNode().id==='a'&&w.parentNode()===r&&"
        "w.parentNode()===null,'the five relative moves');"
        "assert(document.createTreeWalker(r,NodeFilter.SHOW_TEXT|NodeFilter.SHOW_COMMENT).nextNode().data==='x',"
        "'whatToShow picks the kinds');"
        /* SKIP walks into a subtree, REJECT does not */
        "const skipP=document.createTreeWalker(r,E,n=>n.id==='a'?NodeFilter.FILTER_SKIP:NodeFilter.FILTER_ACCEPT);"
        "assert(ids(skipP,'nextNode')==='a1,c,c1','skip');"
        "const rejectP=document.createTreeWalker(r,E,{acceptNode(n){return n.id==='a'?2:1}});"
        "assert(ids(rejectP,'nextNode')==='c,c1','reject, an object\\'s acceptNode');"
        "let self=null;const obj={acceptNode(n){self=this;return 1}};document.createTreeWalker(r,E,obj).nextNode();"
        "assert(self===obj,'acceptNode is called on its object');"
        "let thisFn=1;document.createTreeWalker(r,E,function(){'use strict';thisFn=this;return 1}).nextNode();"
        "assert(thisFn===undefined,'a function is called with this undefined');"
        "assert(document.createTreeWalker(r,E,n=>n.id==='c'?1:3).firstChild().id==='c','firstChild through skipped');"
        /* a throwing filter, the active flag, and a filter that is no filter */
        "const boom=document.createTreeWalker(r,E,()=>{throw new RangeError('x')});"
        "throws('RangeError',()=>boom.nextNode());throws('RangeError',()=>boom.nextNode());"
        "let inner;const again=document.createTreeWalker(r,E,n=>{try{again.nextNode()}catch(e){inner=e.name}return 1});"
        "again.nextNode();assert(inner==='InvalidStateError','a filter that walks again');"
        "throws('TypeError',()=>document.createTreeWalker(r,E,{acceptNode:5}).nextNode());"
        "assert(document.createTreeWalker(r,E,()=>7).nextNode()===null,'another number is no acceptance');"
        /* currentNode can leave the root; the walk does not hang there */
        "const out=document.createTreeWalker(document.getElementById('a'),E);"
        "out.currentNode=document.getElementById('c1');"
        "assert(out.nextNode()===null&&out.currentNode.id==='c1','past the root, nothing');"
        "throws('TypeError',()=>{out.currentNode=null});"
        /* NodeIterator */
        "const it=document.createNodeIterator(r,E);"
        "assert(it instanceof NodeIterator&&it.referenceNode===r&&it.pointerBeforeReferenceNode===true);"
        "assert(it.nextNode()===r&&it.nextNode().id==='a'&&it.nextNode().id==='a1'&&"
        "it.pointerBeforeReferenceNode===false&&it.previousNode().id==='a1'&&it.pointerBeforeReferenceNode===true,"
        "'next, then previous, turns on the reference');"
        "it.detach();assert(it.referenceNode.id==='a1','detach does nothing');"
        "const all=document.createNodeIterator(r);assert(ids(all,'nextNode')==='DIV,a,a1,#text,#text,#comment,c,c1,#text');"
        "r.remove();}",
        "walker");
    /* The pre-removing steps. */
    dom_script(&f, WALK_ASSERT "{"
        "const r=document.createElement('div');document.body.appendChild(r);"
        "r.innerHTML='<p id=a><b id=a1></b></p><p id=b></p><p id=c></p>';"
        "const it=document.createNodeIterator(r,NodeFilter.SHOW_ELEMENT);"
        "it.nextNode();it.nextNode();it.nextNode();"
        "assert(it.referenceNode.id==='a1'&&!it.pointerBeforeReferenceNode);"
        "document.getElementById('a').remove();"
        "assert(it.referenceNode===r&&!it.pointerBeforeReferenceNode,'after: the previous node, here the parent; got '+"
        "it.referenceNode.nodeName+'#'+it.referenceNode.id+' '+it.pointerBeforeReferenceNode);"
        "assert(it.nextNode().id==='b');"
        "it.previousNode();assert(it.referenceNode.id==='b'&&it.pointerBeforeReferenceNode);"
        "document.getElementById('b').remove();"
        "assert(it.referenceNode.id==='c'&&it.pointerBeforeReferenceNode,'before: the next node');"
        "document.getElementById('c').remove();"
        "assert(it.referenceNode===r&&!it.pointerBeforeReferenceNode,'before with none next: the parent, after');"
        "r.innerHTML='<i id=x></i><i id=y></i>';const j=document.createNodeIterator(r);j.nextNode();j.nextNode();"
        "const y=document.getElementById('y');j.nextNode();assert(j.referenceNode===y);"
        "r.removeChild(y);assert(j.referenceNode.id==='x'&&!j.pointerBeforeReferenceNode,'the previous sibling\\'s last');"
        "const k=document.createNodeIterator(r);r.remove();assert(k.referenceNode===r,'the root itself stays');}",
        "walker-removal");
    /* An iterator walked from its own filter; a removal around the root;
     * moves, whose removal libhtml tells once the node is placed. */
    dom_script(&f, WALK_ASSERT "{"
        "const host=document.createElement('div');document.body.appendChild(host);"
        "host.innerHTML='<p id=n1></p><p id=n2></p>';"
        "let inner=[];const it=document.createNodeIterator(host,NodeFilter.SHOW_ELEMENT,n=>{"
        "try{inner.push(String(it.nextNode()))}catch(e){inner.push(e.name)}return NodeFilter.FILTER_SKIP});"
        "assert(it.nextNode()===null&&inner.join()==='InvalidStateError,InvalidStateError,InvalidStateError',inner.join());"
        "assert(it.referenceNode===host&&it.pointerBeforeReferenceNode,'the outer walk left the reference alone');"
        "const ok=document.createNodeIterator(host,NodeFilter.SHOW_ELEMENT,n=>{try{ok.nextNode()}catch(e){}return 1});"
        "assert(ok.nextNode()===host&&ok.nextNode().id==='n1'&&ok.referenceNode.id==='n1','accepted after a nested try');"
        "let armed=false,last;const tail=document.createNodeIterator(host,NodeFilter.SHOW_ELEMENT,n=>{"
        "if(armed)last=tail.nextNode();return 1});"
        "tail.nextNode();tail.nextNode();tail.nextNode();"
        "assert(tail.referenceNode.id==='n2'&&!tail.pointerBeforeReferenceNode,'tail at '+tail.referenceNode.id);"
        "armed=true;"
        "assert(tail.previousNode().id==='n2'&&last===null&&tail.referenceNode.id==='n2'&&"
        "tail.pointerBeforeReferenceNode,'a nested walk with nowhere to go answers null and moves nothing');"
        /* The fixture's page has ids of its own, so these are looked up in host. */
        "const $=id=>host.querySelector('#'+id);"
        "host.innerHTML='<hr id=h><section id=s><div id=r><p></p></div></section><span id=t></span>';"
        "const r=$('r'),around=document.createNodeIterator(r);"
        "$('s').remove();"
        "assert(around.referenceNode===r&&around.pointerBeforeReferenceNode&&around.nextNode()===r,"
        "'a removal around the root leaves it be');"
        "host.innerHTML='<div id=r2><p id=a><b id=b></b></p><i id=x></i></div>';"
        "const m=document.createNodeIterator($('r2'),NodeFilter.SHOW_ELEMENT);"
        "for(let i=0;i<4;i++)m.nextNode();assert(m.referenceNode.id==='x','m at '+m.referenceNode.id);"
        "$('a').appendChild($('x'));"
        "assert(m.referenceNode.id==='b'&&!m.pointerBeforeReferenceNode,'moved into its previous sibling: '+m.referenceNode.id);"
        "assert(m.nextNode().id==='x');"
        "host.innerHTML='<div id=r3><p id=a3><i id=x3></i></p><s id=s3></s></div>';"
        "const r3=$('r3'),m3=document.createNodeIterator(r3,NodeFilter.SHOW_ELEMENT);"
        "m3.nextNode();m3.nextNode();m3.nextNode();m3.previousNode();"
        "assert(m3.referenceNode.id==='x3'&&m3.pointerBeforeReferenceNode,'m3 at '+m3.referenceNode.id);"
        "r3.insertBefore($('x3'),$('s3'));"
        "assert(m3.referenceNode.id==='s3'&&m3.pointerBeforeReferenceNode,'moved out, before: '+m3.referenceNode.id);"
        "assert(m3.nextNode().id==='s3');"
        "host.innerHTML='<div id=r4><u id=u4><em></em></u><a id=a4></a><b id=b4><i id=i4></i></b></div>';"
        "const r4=$('r4'),m4=document.createNodeIterator(r4,NodeFilter.SHOW_ELEMENT);"
        "for(let i=0;i<6;i++)m4.nextNode();assert(m4.referenceNode.id==='i4','m4 at '+m4.referenceNode.id);"
        "r4.replaceChild($('a4'),$('b4'));"
        "assert(m4.referenceNode.nodeName==='EM'&&!m4.pointerBeforeReferenceNode,"
        "'replaced by its previous sibling: '+m4.referenceNode.nodeName);"
        "host.remove();}",
        "walker-reader-cases");
    /* A filter that empties the box the walk stands in: the walk reads on
     * through nodes nothing else holds. */
    dom_script(&f, WALK_ASSERT "{"
        "const box=document.createElement('div');document.body.appendChild(box);"
        "box.insertAdjacentHTML('beforeend','<section><p><b>1</b><b>2</b></p><i></i></section>');"
        "const seen=[];const w=document.createTreeWalker(box,NodeFilter.SHOW_ELEMENT,n=>{seen.push(n.nodeName);"
        "if(n.nodeName==='P')box.textContent='';return NodeFilter.FILTER_SKIP});"
        "assert(w.nextNode()===null&&seen.join()==='SECTION,P,B,B,I',seen.join());"
        "box.insertAdjacentHTML('beforeend','<a><u></u></a><s></s>');"
        "const it=document.createNodeIterator(box,NodeFilter.SHOW_ELEMENT,n=>{if(n.nodeName==='U')box.textContent='';return 1});"
        "assert(it.nextNode()===box&&it.nextNode().nodeName==='A'&&it.nextNode().nodeName==='U'&&it.nextNode()===null);}",
        "walker-mutating-filter");
    fixture_free(&f);
}
#undef WALK_ASSERT
