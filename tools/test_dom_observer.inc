/* MUTATIONOBSERVER (libdom/observer.c; CUSTOM_ELEMENTS.md, the third cut):
 * observe()'s options and their errors, each kind of record and what it
 * says, delivery at the checkpoint after the script (the end of
 * dom_script's run, so a callback is looked for in the next script),
 * takeRecords and disconnect, transient registrations, delivery order and a
 * callback that throws, records the parser's changes leave owed between
 * tasks, and teardown with observers still observing. `L` is the log. */

#define MO_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(f,name){try{f()}catch(e){return e.name===name}return false}"

static void observer_script(DomFixture *f, const char *source, const char *name)
{
    dom_script(f, source, name);
    os64_js_outcome_t outcome;
    os64_dom_script_ended(f->binding, name, &outcome);
}

static void dom_observer_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;

    observer_script(&f, MO_ASSERT
        "assert(throws(function(){new MutationObserver()},'TypeError'),'a callback is required');"
        "assert(throws(function(){new MutationObserver(1)},'TypeError'));"
        "assert(throws(function(){new MutationRecord()},'TypeError'),'MutationRecord constructs nothing');"
        "var o=new MutationObserver(function(){});assert(o instanceof MutationObserver);"
        "var box=document.getElementById('box');"
        "assert(throws(function(){o.observe(box)},'TypeError'),'nothing to observe');"
        "assert(throws(function(){o.observe(box,{})},'TypeError'));"
        "assert(throws(function(){o.observe(box,{attributes:false,attributeOldValue:true})},'TypeError'));"
        "assert(throws(function(){o.observe(box,{attributes:false,attributeFilter:['a']})},'TypeError'));"
        "assert(throws(function(){o.observe(box,{characterData:false,characterDataOldValue:true})},'TypeError'));"
        "assert(throws(function(){o.observe(1,{childList:true})},'TypeError'),'a node is required');"
        "o.observe(box,{attributeOldValue:true});o.observe(box,{attributeFilter:['x']});"
        "o.observe(box,{characterDataOldValue:true});o.disconnect();",
        "observer-options");

    /* Each kind, delivered after the script, with `this` the observer. */
    observer_script(&f, MO_ASSERT
        "window.L=[];var box=document.getElementById('box');"
        "window.mo=new MutationObserver(function(records,observer){"
        "  assert(this===mo&&observer===mo,'the callback\\'s this and second argument');"
        "  records.forEach(function(r){assert(r instanceof MutationRecord);L.push(r)})});"
        "mo.observe(box,{childList:true,attributes:true,attributeOldValue:true,characterData:true,"
        "characterDataOldValue:true,subtree:true});"
        "var p=document.createElement('p');box.appendChild(p);box.setAttribute('class','new');"
        "document.getElementById('one').firstChild.data='uno';"
        "assert(L.length===0,'records are delivered at the checkpoint');",
        "observer-kinds");
    observer_script(&f, MO_ASSERT
        "assert(L.length===3,'three records: '+L.length);"
        "var c=L[0],a=L[1],t=L[2],box=document.getElementById('box');"
        "assert(c.type==='childList'&&c.target===box&&c.addedNodes.length===1&&c.addedNodes[0].localName==='p'&&"
        "c.removedNodes.length===0&&c.previousSibling===document.getElementById('two')&&c.nextSibling===null,"
        "'a childList record');"
        "assert(c.addedNodes===c.addedNodes,'addedNodes is [SameObject]');"
        "assert(a.type==='attributes'&&a.target===box&&a.attributeName==='class'&&a.attributeNamespace===null&&"
        "a.oldValue==='old','an attributes record and its old value');"
        "assert(t.type==='characterData'&&t.oldValue==='one'&&t.target.data==='uno','a characterData record');"
        "mo.disconnect();",
        "observer-records");

    /* Filters, old values not asked for, subtree, takeRecords, disconnect. */
    observer_script(&f, MO_ASSERT
        "window.L=[];var box=document.getElementById('box');"
        "var o=new MutationObserver(function(r){L.push('called:'+r.length)});"
        "o.observe(box,{attributes:true,attributeFilter:['title']});"
        "box.setAttribute('lang','en');box.setAttribute('title','t');box.setAttribute('title','t');"
        "var r=o.takeRecords();"
        "assert(r.length===2&&r[0].attributeName==='title'&&r[0].oldValue===null&&r[1].oldValue===null,"
        "'the filter, and no old value unless asked; a set to its own value is a record');"
        "assert(o.takeRecords().length===0,'takeRecords empties the queue');"
        "box.setAttribute('title','u');o.disconnect();box.setAttribute('title','v');"
        "assert(o.takeRecords().length===0,'disconnect empties the queue and stops');"
        "window.shallow=new MutationObserver(function(){L.push('shallow')});shallow.observe(box,{childList:true});"
        "document.getElementById('one').appendChild(document.createElement('i'));",
        "observer-filters");
    observer_script(&f, MO_ASSERT "assert(L.length===0,'no subtree, no record of a descendant: '+L);"
                    "shallow.disconnect();", "observer-shallow");

    /* A node removed from an observed subtree keeps its observers until
     * they are next notified: a change in it at once is still seen. */
    observer_script(&f, MO_ASSERT
        "window.L=[];var box=document.getElementById('box'),one=document.getElementById('one');"
        "window.tr=new MutationObserver(function(r){r.forEach(function(x){L.push(x.type+':'+x.target.id)})});"
        "tr.observe(box,{childList:true,attributes:true,subtree:true});"
        "box.removeChild(one);one.setAttribute('id','gone');window.gone=one;",
        "observer-transient");
    observer_script(&f, MO_ASSERT
        "assert(L.join()==='childList:box,attributes:gone','the removed node is still observed: '+L);"
        "gone.setAttribute('id','forgotten');"
        "assert(tr.takeRecords().length===0,'once notified, the removed node is not');"
        "tr.disconnect();",
        "observer-transient-seen");

    /* Removing a node queues the notify though no record was queued (the
     * observer watches attributes only), so it runs before a promise the
     * script queues after the removal; and a subtree removed from a
     * removed subtree goes when the observer observes its target again
     * (WPT's transient-registered-observers). */
    observer_script(&f, MO_ASSERT
        "window.L=[];var box=document.getElementById('box');"
        "box.innerHTML='<div id=c><i id=g></i></div>';var c=document.getElementById('c'),g=document.getElementById('g');"
        "var o=new MutationObserver(function(){L.push('observer')});o.observe(box,{attributes:true,subtree:true});"
        "c.remove();Promise.resolve().then(function(){L.push('promise')});c.setAttribute('data-t','');"
        "window.again=new MutationObserver(function(){});again.observe(box,{childList:true,subtree:true});"
        "box.appendChild(c);c.remove();g.remove();again.observe(box,{childList:true,subtree:true});"
        "c.appendChild(document.createElement('b'));g.appendChild(document.createElement('u'));"
        "var r=again.takeRecords().map(function(x){return x.target.id+(x.addedNodes.length?'+':'-')});"
        "assert(r.join()==='box+,box-,c-','re-observing drops the whole chain: '+r);"
        "again.disconnect();window.o1=o;",
        "observer-transient-chain");
    observer_script(&f, MO_ASSERT
        "assert(L.join()==='observer,promise','the removal queued the notify: '+L);o1.disconnect();",
        "observer-transient-microtask");

    /* Two observers: notified in the order they were made, whichever
     * registered or was queued a record first, and one that throws is
     * reported while the next still runs. */
    observer_script(&f, MO_ASSERT
        "window.L=[];window.errors=[];window.onerror=function(m){errors.push(m)};"
        "var box=document.getElementById('box');"
        "var first=new MutationObserver(function(){L.push('first');throw Error('observer failed')}),"
        "second=new MutationObserver(function(){L.push('second')});"
        "second.observe(box,{attributes:true});first.observe(box,{attributes:true});"
        "box.setAttribute('data-o','1');window.pair=[first,second];",
        "observer-order");
    observer_script(&f, MO_ASSERT
        "assert(L.join()==='first,second','in the order they were made: '+L);"
        "assert(errors.length===1&&/observer failed/.test(errors[0]),'the throw is reported: '+errors);"
        "pair.forEach(function(o){o.disconnect()});",
        "observer-ordered");

    /* Between tasks: what the parser inserts into an observed document is
     * owed, and the host's run delivers it. */
    observer_script(&f, MO_ASSERT
        "window.L=[];window.doc=new MutationObserver(function(r){r.forEach(function(x){"
        "x.addedNodes.forEach(function(n){L.push(n.id)})})});doc.observe(document,{childList:true,subtree:true});",
        "observer-document");
    int64_t status;
    os64_html_node_t *arrived = os64_html_create_element(f.document, OS64_HTML_NS_HTML, "p", &status);
    check(arrived != NULL && os64_html_set_attr(f.document, arrived, "id", "arrived", 7) == OS64_HTML_OK &&
              os64_html_insert(f.document, f.document->body, arrived, NULL) == OS64_HTML_OK,
          "observer: a parser's insertion between tasks");
    os64_js_outcome_t outcome;
    check(os64_dom_reactions_owed(f.binding) && os64_dom_reactions_run(f.binding, &outcome) &&
              outcome.status == OS64_JS_OK && !os64_dom_reactions_owed(f.binding),
          "observer: the records it left are owed, and the host's run delivers them");
    observer_script(&f, MO_ASSERT "assert(L.join()==='arrived','the observer saw it arrive: '+L);",
                    "observer-arrived");

    /* Left observing, with records queued: teardown lets go of all of it. */
    observer_script(&f, "document.body.appendChild(document.createElement('b'));", "observer-left");
    fixture_free(&f);

    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    observer_script(&f, MO_ASSERT "assert(typeof MutationObserver==='undefined');", "observer-off");
    fixture_free(&f);
}
#undef MO_ASSERT
