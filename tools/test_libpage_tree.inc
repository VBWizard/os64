// Tree transitions are observed before any rebuild; the native tree verbs
// alone cannot preserve radio and select histories across these operations.
static void tree_state_histories(void)
{
    RebuildFixture f=rebuild_fixture("<div id=dest><input id=a type=radio name=g checked></div>"
        "<div id=source><input id=b type=radio name=g checked form=no></div>"
        "<select id=s><option id=o1>first<option id=o2 selected>second</select>",0);
    os64_html_node_t *a=rebuild_id(f.doc,"a"),*b=rebuild_id(f.doc,"b");
    os64_html_node_t *dest=rebuild_id(f.doc,"dest");
    rebuild_expect("tree: remove explicit owner",attrs_set(&f,b,"form",NULL)==0);
    rebuild_expect("tree: dirty incoming radio",os64_page_node_set_checked(f.state,b,true)==0);
    rebuild_expect("tree: restore destination radio",os64_page_node_set_checked(f.state,a,true)==0);
    // A detached subtree can keep its checked radio without affecting the
    // document group; reinsertion is the next checkedness transition.
    rebuild_expect("tree: detach incoming",os64_page_node_remove(f.state,b)==0);
    rebuild_expect("tree: detached checked",os64_page_node_set_checked(f.state,b,true)==0);
    rebuild_expect("tree: incoming insertion",os64_page_node_insert(f.state,dest,b,NULL)==0);
    bool on=false;
    rebuild_expect("tree: incoming checked wins",os64_page_node_checked(f.state,b,&on)==0&&on);
    rebuild_expect("tree: destination radio clears",os64_page_node_checked(f.state,a,&on)==0&&!on);
    os64_html_node_t *select=rebuild_id(f.doc,"s"),*one=rebuild_id(f.doc,"o1"),*two=rebuild_id(f.doc,"o2");
    rebuild_expect("tree: remove selected option",os64_page_node_remove(f.state,two)==0);
    rebuild_expect("tree: multiple after fallback",attrs_set(&f,select,"multiple","")==0);
    rebuild_expect("tree: reinsert selected option",os64_page_node_insert(f.state,select,two,NULL)==0);
    int32_t index=-1;
    rebuild_expect("tree: fallback selection persists",os64_page_node_selected_index(f.state,select,&index)==0&&index==0);
    rebuild_expect("tree: restore single",attrs_set(&f,select,"multiple",NULL)==0);
    rebuild_expect("tree: last selected wins on single",os64_page_node_selected_index(f.state,select,&index)==0&&index==1);
    (void)one;
    int64_t why=0;
    os64_html_node_t *fragment=os64_html_parse_fragment(f.doc,dest,"<p id=new>replacement</p>",strlen("<p id=new>replacement</p>"),false,&why);
    rebuild_expect("tree: fragment prepared",fragment!=NULL&&why==0);
    rebuild_expect("tree: children replacement",os64_page_node_replace_children(f.state,dest,fragment)==0);
    rebuild_expect("tree: fragment children moved",fragment->first_child==NULL&&dest->first_child!=NULL&&
        dest->first_child->next==NULL&&a->parent==NULL&&b->parent==NULL);
    uint64_t version=os64_html_version(f.doc),revision=os64_page_state_version(f.state);
    rebuild_expect("tree: reject parented replacement",os64_page_node_replace_children(f.state,dest,select)==OS64_HTML_BAD_ARGUMENT);
    rebuild_expect("tree: refusal preserves versions",version==os64_html_version(f.doc)&&revision==os64_page_state_version(f.state));
    rebuild_expect("tree: empty replacement",os64_page_node_replace_children(f.state,dest,NULL)==0&&dest->first_child==NULL);
    rebuild_clear(&f);
}

static void tree_native_refusals_and_templates(void)
{
    RebuildFixture f=rebuild_fixture("<div id=p><div id=c></div></div>"
        "<template id=t><input id=a type=radio name=g checked></template>"
        "<input id=b type=radio name=g checked>",0);
    os64_html_node_t *parent=rebuild_id(f.doc,"p"),*child=rebuild_id(f.doc,"c");
    uint64_t version=os64_html_version(f.doc),revision=os64_page_state_version(f.state);
    size_t bytes=os64_page_state_bytes(f.state),arena=f.doc->arena_bytes,peak=f.doc->peak_arena_bytes,heap=live;
    rebuild_expect("tree: native cycle refusal",os64_page_node_insert(f.state,child,parent,NULL)==OS64_HTML_HIERARCHY);
    rebuild_expect("tree: native refusal exact rollback",version==os64_html_version(f.doc)&&
        revision==os64_page_state_version(f.state)&&bytes==os64_page_state_bytes(f.state)&&
        arena==f.doc->arena_bytes&&peak==f.doc->peak_arena_bytes&&heap==live&&child->parent==parent);
    os64_html_node_t *body=f.doc->body;
    rebuild_expect("tree: wrong reference",os64_page_node_insert(f.state,parent,child,body)==OS64_HTML_NOT_FOUND);
    rebuild_expect("tree: wrong reference exact rollback",version==os64_html_version(f.doc)&&
        revision==os64_page_state_version(f.state)&&bytes==os64_page_state_bytes(f.state)&&heap==live);
    rebuild_expect("tree: same location",os64_page_node_insert(f.state,parent,child,NULL)==0);
    rebuild_expect("tree: same location no op",version==os64_html_version(f.doc)&&revision==os64_page_state_version(f.state)&&
        bytes==os64_page_state_bytes(f.state)&&heap==live);
    os64_html_node_t *t=rebuild_id(f.doc,"t"),*b=rebuild_id(f.doc,"b");
    os64_html_node_t *a=t->template_contents->first_child;
    rebuild_expect("tree: move template as a whole",os64_page_node_insert(f.state,parent,t,NULL)==0);
    bool on=false;
    rebuild_expect("tree: contents group separate",os64_page_node_checked(f.state,a,&on)==0&&on);
    rebuild_expect("tree: document group unaffected",os64_page_node_checked(f.state,b,&on)==0&&on);
    rebuild_expect("tree: move checked from contents",os64_page_node_insert(f.state,parent,a,NULL)==0);
    rebuild_expect("tree: contents radio incoming priority",os64_page_node_checked(f.state,b,&on)==0&&!on);
    int64_t why=0;
    os64_html_node_t *text=os64_html_create_text(f.doc,"fresh",5,&why);
    rebuild_expect("tree: replacement prepared",text!=NULL&&why==0);
    rebuild_expect("tree: replace public verb",os64_page_node_replace(f.state,parent,text,child)==0&&child->parent==NULL&&text->parent==parent);
    rebuild_expect("tree: detached remove no op",os64_page_node_remove(f.state,child)==0);
    rebuild_clear(&f);
}

static void tree_unmodeled_select(void)
{
    os64_html_document_t *doc=parse("<select id=s><option>A<option>B</select><div id=d></div>","utf-8");
    os64_page_state_t *state=os64_page_state_create(doc,0);
    os64_html_node_t *select=rebuild_id(doc,"s"),*div=rebuild_id(doc,"d");
    int64_t why=0;
    os64_html_node_t *fresh=os64_html_create_element(doc,OS64_HTML_NS_HTML,"div",&why);
    rebuild_expect("tree unmodeled: unrelated insert",os64_page_node_insert(state,div,fresh,NULL)==0);
    rebuild_expect("tree unmodeled: change multiple",os64_page_node_set_attr(state,select,"multiple","",0,false)==0);
    int32_t index=-1;
    rebuild_expect("tree unmodeled: fallback not locked false",os64_page_node_selected_index(state,select,&index)==0&&index==0);
    os64_page_state_free(state);os64_html_document_free(doc);
}

static void tree_depth_and_state_cap(void)
{
    RebuildFixture f=rebuild_fixture("<input id=q value=old><div id=p></div>",0);
    os64_html_node_t *q=rebuild_id(f.doc,"q"),*deep=rebuild_id(f.doc,"p");
    rebuild_expect("tree depth: borrowed dirty value",os64_page_node_set_value(f.state,q,"borrowed",8)==0);
    const char *value=NULL;size_t value_len=0;
    rebuild_expect("tree depth: value read",os64_page_node_value(f.state,q,&value,&value_len)==0);
    int64_t why=0;
    os64_html_node_t *fresh=NULL;
    for(size_t i=0;i<2000;i++) {
        fresh=os64_html_create_element(f.doc,OS64_HTML_NS_HTML,"div",&why);
        rebuild_expect("tree depth: detached preparation",fresh!=NULL&&why==0);
        int64_t status=os64_html_insert(f.doc,deep,fresh,NULL);
        if(status==OS64_HTML_TOO_DEEP)break;
        rebuild_expect("tree depth: native link",status==0);
        deep=fresh;fresh=NULL;
    }
    rebuild_expect("tree depth: reached native limit",fresh!=NULL);
    uint64_t version=os64_html_version(f.doc),revision=os64_page_state_version(f.state);
    size_t bytes=os64_page_state_bytes(f.state),heap=live,pins=rebuild_available_pins(f.doc);
    rebuild_expect("tree depth: staged refusal",os64_page_node_insert(f.state,deep,fresh,NULL)==OS64_HTML_TOO_DEEP);
    rebuild_expect("tree depth: preserves readers",version==os64_html_version(f.doc)&&revision==os64_page_state_version(f.state)&&
        bytes==os64_page_state_bytes(f.state)&&heap==live&&pins==rebuild_available_pins(f.doc)&&
        value_len==8&&memcmp(value,"borrowed",9)==0&&fresh->parent==NULL);
    rebuild_clear(&f);
    os64_html_document_t *doc=parse("<div id=p></div>","utf-8");
    os64_page_state_t *probe=os64_page_state_create(doc,0);
    size_t cap=os64_page_state_bytes(probe);os64_page_state_free(probe);
    os64_page_state_t *state=os64_page_state_create(doc,cap);
    os64_html_node_t *parent=rebuild_id(doc,"p");
    fresh=os64_html_create_element(doc,OS64_HTML_NS_HTML,"span",&why);
    version=os64_html_version(doc);revision=os64_page_state_version(state);heap=live;
    rebuild_expect("tree cap: named refusal",os64_page_node_insert(state,parent,fresh,NULL)==OS64_HTML_NO_MEMORY);
    rebuild_expect("tree cap: exact rollback",version==os64_html_version(doc)&&revision==os64_page_state_version(state)&&
        cap==os64_page_state_bytes(state)&&heap==live&&fresh->parent==NULL);
    os64_page_state_free(state);os64_html_document_free(doc);
}

static void tree_state_failure_sweep(void)
{
    size_t points=0;
    for(size_t point=0;;point++) {
        RebuildFixture f=rebuild_fixture("<div id=dest><input id=a type=radio name=g checked><p>old</p></div>",0);
        os64_html_node_t *dest=rebuild_id(f.doc,"dest"),*old=dest->first_child;
        int64_t why=0;
        const char *markup="<input id=b type=radio name=g checked><p>new</p>";
        os64_html_node_t *fragment=os64_html_parse_fragment(f.doc,dest,markup,strlen(markup),false,&why);
        rebuild_expect("tree sweep: detached preparation",fragment!=NULL&&why==0);
        os64_html_node_t *incoming=fragment->first_child;
        uint64_t version=os64_html_version(f.doc),revision=os64_page_state_version(f.state);
        size_t bytes=os64_page_state_bytes(f.state),arena=f.doc->arena_bytes,peak=f.doc->peak_arena_bytes,heap=live;
        allocations=0;fail_at=point;fail_single=true;
        int64_t status=os64_page_node_replace_children(f.state,dest,fragment);
        size_t attempted=allocations;fail_at=0;fail_single=false;
        if(point==0)points=attempted;
        if(point!=0) {
            rebuild_expect("tree sweep: allocation refusal",status==OS64_HTML_NO_MEMORY);
            rebuild_expect("tree sweep: unchanged tree and state",dest->first_child==old&&old->parent==dest&&
                fragment->first_child==incoming&&incoming->parent==fragment&&version==os64_html_version(f.doc)&&
                revision==os64_page_state_version(f.state)&&bytes==os64_page_state_bytes(f.state)&&
                arena==f.doc->arena_bytes&&peak==f.doc->peak_arena_bytes&&heap==live);
        } else
            rebuild_expect("tree sweep: success",status==0&&fragment->first_child==NULL&&old->parent==NULL);
        rebuild_clear(&f);
        if(point==points)break;
    }
}

static void tree_explicit_empty_selection(void)
{
    RebuildFixture f=rebuild_fixture("<select id=s><option id=a>A<option>B</select>",0);
    os64_html_node_t *select=rebuild_id(f.doc,"s"),*a=rebuild_id(f.doc,"a");
    os64_html_node_t *option=followup_element(f.doc,"option",NULL);
    int32_t index=-99;
    rebuild_expect("empty select: clear before insertion",os64_page_node_set_selected_index(f.state,select,-1)==0);
    rebuild_expect("empty select: insert option",os64_page_node_insert(f.state,select,option,NULL)==0);
    rebuild_expect("empty select: insertion restores first",os64_page_node_selected_index(f.state,select,&index)==0&&index==0);
    rebuild_expect("empty select: clear before removal",os64_page_node_set_selected_index(f.state,select,-1)==0);
    rebuild_expect("empty select: remove option",os64_page_node_remove(f.state,a)==0);
    rebuild_expect("empty select: removal restores first",os64_page_node_selected_index(f.state,select,&index)==0&&index==0);
    rebuild_expect("empty select: clear before size",os64_page_node_set_selected_index(f.state,select,-1)==0);
    rebuild_expect("empty select: size roundtrip",attrs_set(&f,select,"size","2")==0&&attrs_set(&f,select,"size","1")==0);
    rebuild_expect("empty select: size restores first",os64_page_node_selected_index(f.state,select,&index)==0&&index==0);
    rebuild_expect("empty select: clear before multiple",os64_page_node_set_selected_index(f.state,select,-1)==0);
    rebuild_expect("empty select: multiple roundtrip",attrs_set(&f,select,"multiple","")==0&&attrs_set(&f,select,"multiple",NULL)==0);
    rebuild_expect("empty select: multiple restores first",os64_page_node_selected_index(f.state,select,&index)==0&&index==0);
    rebuild_expect("empty select: unmatched value",os64_page_node_set_value(f.state,select,"missing",7)==0);
    rebuild_expect("empty select: property clear stays empty",os64_page_node_selected_index(f.state,select,&index)==0&&index==-1);
    os64_html_node_t *nested=followup_option(f.doc,select->first_child,"nested");
    rebuild_expect("empty select: excluded option attribute",attrs_set(&f,nested,"selected","")==0);
    rebuild_expect("empty select: excluded option leaves outer selection empty",
        os64_page_node_selected_index(f.state,select,&index)==0&&index==-1);
    rebuild_clear(&f);
}

static void test_libpage_tree(void)
{
    tree_depth_and_state_cap();
    tree_unmodeled_select();
    tree_state_histories();
    tree_native_refusals_and_templates();
    tree_state_failure_sweep();
    tree_explicit_empty_selection();
}
