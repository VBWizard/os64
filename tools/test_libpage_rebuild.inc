// D3 contracts are checked by node identity, independently of model indices.
static void rebuild_expect(const char *name, bool condition)
{
    checks++;
    if (!condition)
        fail(name, "rebuild contract failed");
}

static os64_html_node_t *rebuild_id(os64_html_document_t *doc, const char *id)
{
    for (os64_html_node_t *n = doc->document; n != NULL;) {
        const os64_html_attr_t *candidate = os64_html_attr(n, "id");
        if (candidate != NULL && strcmp(candidate->value, id) == 0)
            return n;
        if (n->first_child != NULL) {
            n = n->first_child;
            continue;
        }
        while (n != NULL && n->next == NULL)
            n = n->parent;
        n = n != NULL ? n->next : NULL;
    }
    return NULL;
}

static void rebuild_value(const char *name, const os64_page_t *page,
                          const os64_html_node_t *node, const char *expected)
{
    const os64_page_control_t *c =
        os64_page_control(page, os64_page_control_for(page, node));
    rebuild_expect(name, c != NULL && c->value != NULL &&
                   c->value_len == strlen(expected) &&
                   memcmp(c->value, expected, c->value_len + 1) == 0);
}

static void rebuild_attribute(const char *name, const os64_html_node_t *node,
                              const char *attr, const char *expected)
{
    const os64_html_attr_t *got = os64_html_attr(node, attr);
    rebuild_expect(name, got != NULL && strcmp(got->value, expected) == 0);
}

// With sixteen slots in the public contract, available pins expose how many
// snapshot constructors and failure paths still own a pin without internals.
static size_t rebuild_available_pins(os64_html_document_t *doc)
{
    os64_html_pin_t pins[16];
    size_t count = 0;
    while (count < sizeof(pins) / sizeof(pins[0])) {
        os64_html_pin_t pin = os64_html_pin(doc);
        if (pin == 0)
            break;
        pins[count++] = pin;
    }
    for (size_t i = 0; i < count; i++)
        os64_html_unpin(doc, pins[i]);
    return count;
}

typedef struct {
    os64_html_document_t *doc;
    os64_page_state_t *state;
    os64_page_t *page;
} RebuildFixture;

static RebuildFixture rebuild_fixture(const char *html, size_t cap)
{
    RebuildFixture f = {0};
    f.doc = parse(html, "utf-8");
    f.state = f.doc != NULL ? os64_page_state_create(f.doc, cap) : NULL;
    f.page = f.state != NULL ? os64_page_build(f.doc, kPage, NULL, f.state) : NULL;
    rebuild_expect("rebuild: fixture constructs", f.page != NULL &&
                   !os64_page_incomplete(f.page));
    return f;
}

static void rebuild_clear(RebuildFixture *f)
{
    os64_page_free(f->page);
    os64_page_state_free(f->state);
    os64_html_document_free(f->doc);
}

static void rebuild_node_value(const char *name, os64_page_state_t *state,
                               os64_html_node_t *node, const char *want)
{
    const char *value = NULL;
    size_t len = 0;
    int64_t status = os64_page_node_value(state, node, &value, &len);
    rebuild_expect(name, status == 0 && value != NULL && len == strlen(want) &&
                   memcmp(value, want, len + 1) == 0);
}

static void rebuild_lifetime_and_stale(void)
{
    size_t baseline = live;
    RebuildFixture f = rebuild_fixture("<form id=f method=post action=/old>"
        "<input id=t name=t value=base><input id=c type=checkbox name=c>"
        "<select id=s><option>A<option>B</select><button id=b>Send</button></form>"
        "<a id=a href=/old>link</a>", 0);
    os64_html_node_t *t = rebuild_id(f.doc, "t"), *c = rebuild_id(f.doc, "c");
    os64_html_node_t *s = rebuild_id(f.doc, "s"), *form = rebuild_id(f.doc, "f");
    uint64_t html_version = os64_html_version(f.doc);
    uint64_t state_version = os64_page_state_version(f.state);
    rebuild_expect("rebuild: state generation nonzero", state_version != 0);
    rebuild_expect("rebuild: person edit", os64_page_set_text(f.page,
        os64_page_control_for(f.page,t), "typed", 5) == 0);
    rebuild_expect("rebuild: edit changes only state generation",
        os64_page_state_version(f.state) > state_version &&
        os64_html_version(f.doc) == html_version);
    os64_page_t *second = os64_page_rebuild(f.page);
    rebuild_expect("rebuild: second live model", second != NULL);
    rebuild_expect("rebuild: model pins document", rebuild_available_pins(f.doc) == 14);
    rebuild_value("rebuild: new model preserves typing", second, t, "typed");
    state_version = os64_page_state_version(f.state);
    rebuild_expect("rebuild: node edit while both models live",
        os64_page_node_set_value(f.state,t,"replacement",11) == 0);
    rebuild_value("rebuild: old publication updated before old bytes freed", f.page,t,"replacement");
    rebuild_value("rebuild: new publication updated",second,t,"replacement");
    rebuild_expect("rebuild: semantic edit bumps generation",
        os64_page_state_version(f.state) > state_version);
    state_version = os64_page_state_version(f.state);
    rebuild_node_value("rebuild: dirty getter",f.state,t,"replacement");
    rebuild_expect("rebuild: getter does not bump generation",
        os64_page_state_version(f.state) == state_version);
    rebuild_expect("rebuild: equal dirty assignment is semantic no-op",
        os64_page_node_set_value(f.state,t,"replacement",11)==0 &&
        os64_page_state_version(f.state)==state_version);
    rebuild_expect("rebuild: script checked publication",
        os64_page_node_set_checked(f.state,c,true) == 0);
    rebuild_expect("rebuild: checked shared by models",
        os64_page_control(f.page,os64_page_control_for(f.page,c))->checked &&
        os64_page_control(second,os64_page_control_for(second,c))->checked);
    rebuild_expect("rebuild: script selection publication",
        os64_page_node_set_selected_index(f.state,s,1) == 0);
    rebuild_value("rebuild: old select current",f.page,s,"B");
    rebuild_value("rebuild: new select current",second,s,"B");
    rebuild_expect("rebuild: changed action",os64_html_set_attr(f.doc,form,"action","/new",4) == 0);
    os64_page_request_t request;
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_CONTROL,
        os64_page_control_for(f.page,rebuild_id(f.doc,"b")),0,0};
    rebuild_expect("rebuild: stale submit refuses destination",os64_page_activate(f.page,what,&request)
        == OS64_PAGE_REFUSED && request.reason == OS64_PAGE_REASON_STALE);
    os64_page_request_free(&request);
    what.how = OS64_PAGE_ACTIVATE_LINK;
    what.index = os64_page_link_for(f.page,rebuild_id(f.doc,"a"));
    rebuild_expect("rebuild: stale link refuses",os64_page_activate(f.page,what,&request)
        == OS64_PAGE_REFUSED && request.reason == OS64_PAGE_REASON_STALE);
    os64_page_request_free(&request);
    rebuild_expect("rebuild: stale person text",os64_page_set_text(f.page,0,"lost",4)
        == -OS64_PAGE_REASON_STALE);
    rebuild_expect("rebuild: stale person tick",os64_page_set_checked(f.page,1,false)
        == -OS64_PAGE_REASON_STALE);
    rebuild_expect("rebuild: stale person option",os64_page_set_chosen(f.page,2,0,true)
        == -OS64_PAGE_REASON_STALE);
    rebuild_expect("rebuild: stale reset",os64_page_reset(f.page,0) == -OS64_PAGE_REASON_STALE);
    rebuild_node_value("rebuild: stale edits preserve state",f.state,t,"replacement");
    os64_page_t *current = os64_page_rebuild(second);
    rebuild_expect("rebuild: current model built",current != NULL);
    what.how = OS64_PAGE_ACTIVATE_CONTROL;
    what.index = os64_page_control_for(current,rebuild_id(f.doc,"b"));
    rebuild_expect("rebuild: current submission uses changed action",
        os64_page_activate(current,what,&request) == OS64_PAGE_NAVIGATE &&
        request.url != NULL && strstr(request.url,"/new") != NULL &&
        request.body != NULL && strstr(request.body,"t=replacement") != NULL);
    os64_page_request_free(&request);
    os64_page_free(second);
    os64_page_free(current);
    rebuild_expect("rebuild: releases each model pin",rebuild_available_pins(f.doc) == 15);
    rebuild_clear(&f);
    rebuild_expect("rebuild: lifetime heap returns",live == baseline);

    // Private state is shared by rebuilding even when no owner supplied one.
    for (int order = 0; order < 2; order++) {
        os64_html_document_t *doc = parse("<input value=base>","utf-8");
        os64_page_t *old = os64_page_build(doc,kPage,NULL,NULL);
        rebuild_expect("rebuild: private state edit",os64_page_set_text(old,0,"private",7) == 0);
        os64_page_t *fresh = os64_page_rebuild(old);
        rebuild_expect("rebuild: private state rebuild",fresh != NULL);
        os64_page_t *survivor = order == 0 ? fresh : old;
        os64_page_free(order == 0 ? old : fresh);
        rebuild_value("rebuild: private state survives either free order",survivor,
            doc->body->first_child,"private");
        os64_page_free(survivor);
        rebuild_expect("rebuild: private model pins released",rebuild_available_pins(doc) == 16);
        os64_html_document_free(doc);
    }
}

static void rebuild_detach_and_options(void)
{
    RebuildFixture f = rebuild_fixture("<form id=f method=post><input id=t name=t value=base>"
        "<select id=s name=s><option id=a value=A>A<option id=b value=B>B"
        "<option id=c value=C>C</select><button id=send>send</button></form>",0);
    os64_html_node_t *t=rebuild_id(f.doc,"t"), *form=rebuild_id(f.doc,"f");
    os64_html_node_t *s=rebuild_id(f.doc,"s"), *a=rebuild_id(f.doc,"a"), *b=rebuild_id(f.doc,"b");
    rebuild_expect("rebuild: set before detach",os64_page_node_set_value(f.state,t,"kept",4) == 0);
    rebuild_expect("rebuild: select option B",os64_page_node_set_selected_index(f.state,s,1) == 0);
    rebuild_expect("rebuild: detach typed control",os64_html_remove(f.doc,t) == 0);
    rebuild_expect("rebuild: ownership includes detached nodes",os64_html_owns_node(f.doc,t));
    os64_page_t *absent=os64_page_rebuild(f.page);
    rebuild_expect("rebuild: detached control absent from model",os64_page_control_for(absent,t) == -1);
    rebuild_node_value("rebuild: detached control still has value",f.state,t,"kept");
    rebuild_expect("rebuild: reinsert typed control",os64_html_insert(f.doc,form,t,s) == 0);
    rebuild_expect("rebuild: reorder options",os64_html_insert(f.doc,s,b,a) == 0);
    os64_page_t *reordered=os64_page_rebuild(absent);
    rebuild_value("rebuild: reinserted value",reordered,t,"kept");
    const os64_page_control_t *sc=os64_page_control(reordered,os64_page_control_for(reordered,s));
    rebuild_expect("rebuild: selectedness follows option node through reorder",
        sc != NULL && sc->options[0].node == b && sc->options[0].selected && !sc->options[1].selected);
    rebuild_expect("rebuild: remove unselected option",os64_html_remove(f.doc,a) == 0);
    os64_page_t *removed=os64_page_rebuild(reordered);
    rebuild_value("rebuild: deleting earlier index preserves selection",removed,s,"B");
    int64_t status=0;
    os64_html_node_t *new_input=os64_html_create_element(f.doc,OS64_HTML_NS_HTML,"input",&status);
    rebuild_expect("rebuild: create never inserted input",new_input != NULL && status == 0);
    rebuild_expect("rebuild: detached new input value",os64_page_node_set_value(f.state,new_input,"new",3) == 0);
    rebuild_node_value("rebuild: never inserted value reads",f.state,new_input,"new");
    rebuild_expect("rebuild: insert new input",os64_html_insert(f.doc,form,new_input,NULL) == 0);
    os64_page_t *inserted=os64_page_rebuild(removed);
    rebuild_value("rebuild: new control carries preinsertion state",inserted,new_input,"new");
    os64_page_request_t req;
    os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,os64_page_control_for(inserted,rebuild_id(f.doc,"send")),0,0};
    rebuild_expect("rebuild: submission retains detached value and option identity",
        os64_page_activate(inserted,what,&req)==OS64_PAGE_NAVIGATE && req.body!=NULL &&
        strcmp(req.body,"t=kept&s=B")==0);
    os64_page_request_free(&req);
    os64_page_free(inserted);os64_page_free(removed);os64_page_free(reordered);os64_page_free(absent);
    rebuild_clear(&f);
}

static void rebuild_value_modes(void)
{
    static const struct {const char *html,*want;bool attribute;} cases[]={
        {"<input id=q disabled value=old>","ab",false},
        {"<input id=q readonly value=old>","ab",false},
        {"<input id=q hidden value=old>","ab",false},
        {"<input id=q type=hidden value=old>","a\r\nb",true},
        {"<input id=q type=checkbox value=old>","a\r\nb",true},
        {"<input id=q type=radio value=old>","a\r\nb",true},
        {"<input id=q type=button value=old>","a\r\nb",true},
        {"<input id=q type=submit value=old>","a\r\nb",true},
        {"<input id=q type=reset value=old>","a\r\nb",true},
        {"<button id=q value=old>caption</button>","a\r\nb",true},
        {"<textarea id=q readonly>old</textarea>","a\nb",false},
        {"<input id=q type=image value=old>","a\r\nb",true}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        RebuildFixture f=rebuild_fixture(cases[i].html,0);
        os64_html_node_t *q=rebuild_id(f.doc,"q");
        if(i==0)rebuild_expect("rebuild: person disabled refusal preserved",
            os64_page_set_text(f.page,0,"a",1)==-OS64_PAGE_REASON_DISABLED);
        if(i==1 || i==10)rebuild_expect("rebuild: person readonly refusal preserved",
            os64_page_set_text(f.page,0,"a",1)==-OS64_PAGE_REASON_READONLY);
        if(cases[i].attribute)rebuild_expect("rebuild: person cannot type attribute-mode values",
            os64_page_set_text(f.page,0,"a",1)==-OS64_PAGE_REASON_WRONG_KIND);
        uint64_t hv=os64_html_version(f.doc), sv=os64_page_state_version(f.state);
        rebuild_expect("rebuild: script accepts value regardless of person access",
            os64_page_node_set_value(f.state,q,"a\r\nb",4)==0);
        rebuild_node_value("rebuild: script value mode",f.state,q,cases[i].want);
        rebuild_expect("rebuild: state generation tracks script value",
            os64_page_state_version(f.state)>sv);
        rebuild_expect("rebuild: only attribute value mode mutates tree",
            cases[i].attribute ? os64_html_version(f.doc)>hv : os64_html_version(f.doc)==hv);
        if(cases[i].attribute) rebuild_attribute("rebuild: attribute mode lands in tree",q,"value","a\r\nb");
        else rebuild_value("rebuild: state mode publishes to model",f.page,q,cases[i].want);
        rebuild_clear(&f);
    }
    RebuildFixture f=rebuild_fixture("<input id=file type=file value=fake><input id=t disabled>"
        "<input id=c type=checkbox disabled><select id=s disabled><option disabled>A<option>B</select>",0);
    os64_html_node_t *file=rebuild_id(f.doc,"file"),*c=rebuild_id(f.doc,"c"),*s=rebuild_id(f.doc,"s");
    uint64_t sv=os64_page_state_version(f.state),hv=os64_html_version(f.doc);
    rebuild_expect("rebuild: file rejects nonempty",os64_page_node_set_value(f.state,file,"secret",6)
        ==-OS64_PAGE_REASON_WRONG_KIND);
    rebuild_expect("rebuild: file refusal moves no generation",os64_page_state_version(f.state)==sv && os64_html_version(f.doc)==hv);
    rebuild_expect("rebuild: file accepts empty",os64_page_node_set_value(f.state,file,"",0)==0);
    rebuild_node_value("rebuild: file remains empty",f.state,file,"");
    rebuild_expect("rebuild: disabled script checkbox",os64_page_node_set_checked(f.state,c,true)==0);
    bool on=false;
    rebuild_expect("rebuild: checked getter",os64_page_node_checked(f.state,c,&on)==0 && on);
    rebuild_expect("rebuild: disabled script option",os64_page_node_set_selected_index(f.state,s,0)==0);
    int32_t index=-2;
    rebuild_expect("rebuild: selectedIndex getter",os64_page_node_selected_index(f.state,s,&index)==0 && index==0);
    rebuild_expect("rebuild: selectedIndex clears selection",os64_page_node_set_selected_index(f.state,s,-1)==0);
    rebuild_expect("rebuild: script clearing may leave size-one list empty",os64_page_node_selected_index(f.state,s,&index)==0 && index==-1);
    rebuild_expect("rebuild: select value chooses matching option",
        os64_page_node_set_value(f.state,s,"B",1)==0 &&
        os64_page_node_selected_index(f.state,s,&index)==0 && index==1);
    rebuild_expect("rebuild: select unmatched value clears selection",
        os64_page_node_set_value(f.state,s,"missing",7)==0 &&
        os64_page_node_selected_index(f.state,s,&index)==0 && index==-1);
    os64_html_document_t *foreign=parse("<input id=x>","utf-8");
    os64_html_node_t *x=rebuild_id(foreign,"x");
    rebuild_expect("rebuild: owns connected node",os64_html_owns_node(f.doc,c));
    rebuild_expect("rebuild: ownership refuses foreign and null arguments",
        !os64_html_owns_node(f.doc,x) && !os64_html_owns_node(NULL,c) &&
        !os64_html_owns_node(f.doc,NULL));
    sv=os64_page_state_version(f.state);hv=os64_html_version(f.doc);
    rebuild_expect("rebuild: foreign value refused",os64_page_node_set_value(f.state,x,"x",1)==-OS64_PAGE_REASON_WRONG_KIND);
    rebuild_expect("rebuild: foreign checked refused",os64_page_node_set_checked(f.state,x,true)==-OS64_PAGE_REASON_WRONG_KIND);
    rebuild_expect("rebuild: foreign select refused",os64_page_node_set_selected_index(f.state,x,0)==-OS64_PAGE_REASON_WRONG_KIND);
    const char *value="unchanged",*sentinel=value;size_t length=777;
    rebuild_expect("rebuild: foreign getter refused with outputs preserved",
        os64_page_node_value(f.state,x,&value,&length)==-OS64_PAGE_REASON_WRONG_KIND && value==sentinel && length==777);
    on=true;index=777;
    rebuild_expect("rebuild: wrong-kind checked getter preserves output",
        os64_page_node_checked(f.state,x,&on)==-OS64_PAGE_REASON_WRONG_KIND && on);
    rebuild_expect("rebuild: wrong-kind selection getter preserves output",
        os64_page_node_selected_index(f.state,x,&index)==-OS64_PAGE_REASON_WRONG_KIND && index==777);
    rebuild_expect("rebuild: noncontrol refused",os64_page_node_set_value(f.state,f.doc->body,"x",1)==-OS64_PAGE_REASON_WRONG_KIND);
    rebuild_expect("rebuild: wrong-kind calls preserve generations",os64_page_state_version(f.state)==sv && os64_html_version(f.doc)==hv);
    os64_html_document_free(foreign);rebuild_clear(&f);
}

static RebuildFixture rebuild_radio_fixture(void)
{
    RebuildFixture f=rebuild_fixture("<input id=t value=base><input id=r0 type=radio name=a checked>"
        "<input id=r1 type=radio name=b checked><input id=r2 type=radio name=c checked>"
        "<select id=s><option>A<option>B</select>",0);
    rebuild_expect("rebuild: initialized value for refusal inspection",
        os64_page_node_set_value(f.state,rebuild_id(f.doc,"t"),"kept",4)==0);
    rebuild_expect("rebuild: merging radio groups",os64_html_set_attr(f.doc,
        rebuild_id(f.doc,"r1"),"name","a",1)==0);
    rebuild_expect("rebuild: merging third radio group",os64_html_set_attr(f.doc,
        rebuild_id(f.doc,"r2"),"name","a",1)==0);
    // New controls force state reservations during normalization, after the
    // old model already owns records for its original controls.
    for(int i=0;i<2;i++) {
        int64_t status=0;
        os64_html_node_t *node=os64_html_create_element(f.doc,OS64_HTML_NS_HTML,"input",&status);
        rebuild_expect("rebuild: new normalization reservation node",node!=NULL && status==0 &&
            os64_html_set_attr(f.doc,node,"type","radio",5)==0 &&
            os64_html_set_attr(f.doc,node,"name","a",1)==0 &&
            os64_html_set_attr(f.doc,node,"checked","",0)==0 &&
            os64_html_insert(f.doc,f.doc->body,node,NULL)==0);
    }
    return f;
}

static void rebuild_failures_and_caps(void)
{
    os64_html_document_t *small_doc=parse("<input>","utf-8");
    size_t initial_live=live;
    os64_page_state_t *too_small=os64_page_state_create(small_doc,1);
    rebuild_expect("rebuild: state header obeys cap",too_small==NULL && live==initial_live);
    os64_page_state_free(too_small);
    rebuild_expect("rebuild: state refuses absent document",os64_page_state_create(NULL,0)==NULL);
    os64_html_document_free(small_doc);
    RebuildFixture f=rebuild_radio_fixture();
    size_t before=allocations;
    os64_page_t *fresh=os64_page_rebuild(f.page);
    size_t count=allocations-before;
    rebuild_expect("rebuild: successful normalization candidate",fresh!=NULL);
    bool a=true,b=true,c=true;
    rebuild_expect("rebuild: merged radio group normalizes last member",
        os64_page_node_checked(f.state,rebuild_id(f.doc,"r0"),&a)==0 &&
        os64_page_node_checked(f.state,rebuild_id(f.doc,"r1"),&b)==0 &&
        os64_page_node_checked(f.state,rebuild_id(f.doc,"r2"),&c)==0 && !a && !b && !c &&
        os64_page_control(fresh,6)->checked);
    os64_page_free(fresh);rebuild_clear(&f);
    fail_single=true;
    for(size_t point=1;point<=count;point++) {
        size_t heap=live;
        f=rebuild_radio_fixture();
        uint64_t sv=os64_page_state_version(f.state),hv=os64_html_version(f.doc);
        size_t state_bytes=os64_page_state_bytes(f.state);
        fail_at=allocations+point;
        fresh=os64_page_rebuild(f.page);
        fail_at=0;
        rebuild_expect("rebuild: each allocation refusal rejects candidate",fresh==NULL);
        if(fresh==NULL) {
            rebuild_value("rebuild: failed candidate preserves old value",f.page,rebuild_id(f.doc,"t"),"kept");
            rebuild_expect("rebuild: failed normalization preserves old ticks",
                os64_page_control(f.page,1)->checked && os64_page_control(f.page,2)->checked &&
                os64_page_control(f.page,3)->checked);
            rebuild_expect("rebuild: failed candidate preserves generations",
                os64_page_state_version(f.state)==sv && os64_html_version(f.doc)==hv);
            rebuild_expect("rebuild: failed candidate preserves state charge",
                os64_page_state_bytes(f.state)==state_bytes);
            rebuild_expect("rebuild: failure releases candidate pin",rebuild_available_pins(f.doc)==15);
        }
        os64_page_free(fresh);
        rebuild_clear(&f);
        rebuild_expect("rebuild: allocation sweep returns heap",live==heap);
    }
    fail_single=false;

    f=rebuild_fixture("<input id=t>",4096);
    os64_html_node_t *t=rebuild_id(f.doc,"t");
    rebuild_expect("rebuild: capped state initialized",os64_page_node_set_value(f.state,t,"kept",4)==0);
    char large[8193];memset(large,'x',sizeof(large)-1);large[sizeof(large)-1]=0;
    uint64_t sv=os64_page_state_version(f.state),hv=os64_html_version(f.doc);
    size_t used=os64_page_state_bytes(f.state);
    rebuild_expect("rebuild: state cap is refusal",os64_page_node_set_value(f.state,t,large,sizeof(large)-1)
        ==-OS64_PAGE_REASON_NO_MEMORY);
    rebuild_node_value("rebuild: cap preserves current value",f.state,t,"kept");
    rebuild_expect("rebuild: cap preserves budgets and generations",os64_page_state_bytes(f.state)==used &&
        os64_page_state_version(f.state)==sv && os64_html_version(f.doc)==hv);
    os64_html_pin_t pins[16];size_t npins=0;
    while(npins<16) {os64_html_pin_t pin=os64_html_pin(f.doc);if(pin==0)break;pins[npins++]=pin;}
    rebuild_expect("rebuild: model leaves fifteen pin slots",npins==15);
    before=live;
    fresh=os64_page_rebuild(f.page);
    rebuild_expect("rebuild: pin exhaustion refuses snapshot",fresh==NULL);
    os64_page_free(fresh);
    rebuild_expect("rebuild: pin refusal releases allocations",live==before);
    fresh=os64_page_build(f.doc,kPage,NULL,f.state);
    rebuild_expect("rebuild: pin exhaustion refuses initial constructor",fresh==NULL);
    os64_page_free(fresh);
    for(size_t i=0;i<npins;i++)os64_html_unpin(f.doc,pins[i]);
    rebuild_value("rebuild: pin exhaustion preserves model",f.page,t,"kept");
    rebuild_clear(&f);
}

static void rebuild_detached_radios(void)
{
    RebuildFixture f=rebuild_fixture("<input id=a type=radio name=r checked>"
        "<input id=b type=radio name=r checked>",0);
    os64_html_node_t *a=rebuild_id(f.doc,"a"),*b=rebuild_id(f.doc,"b");
    bool a_on=true,b_on=false;
    rebuild_expect("rebuild: node checked defaults agree normalized model",
        os64_page_node_checked(f.state,a,&a_on)==0 && os64_page_node_checked(f.state,b,&b_on)==0 && !a_on && b_on);
    rebuild_expect("rebuild: detach radio",os64_html_remove(f.doc,a)==0);
    rebuild_expect("rebuild: detached radio can be checked",os64_page_node_set_checked(f.state,a,true)==0);
    rebuild_expect("rebuild: separate trees do not untick each other",
        os64_page_node_checked(f.state,a,&a_on)==0 && os64_page_node_checked(f.state,b,&b_on)==0 && a_on && b_on);
    rebuild_expect("rebuild: reinsert radio before group winner",os64_html_insert(f.doc,f.doc->body,a,b)==0);
    os64_page_t *fresh=os64_page_rebuild(f.page);
    rebuild_expect("rebuild: rejoined radio group normalizes last checked",
        fresh!=NULL && os64_page_node_checked(f.state,a,&a_on)==0 &&
        os64_page_node_checked(f.state,b,&b_on)==0 && !a_on && b_on);
    rebuild_expect("rebuild: connected radio setter unticks peer",os64_page_node_set_checked(f.state,a,true)==0 &&
        os64_page_node_checked(f.state,a,&a_on)==0 && os64_page_node_checked(f.state,b,&b_on)==0 && a_on && !b_on);
    os64_page_free(fresh);rebuild_clear(&f);
}

static RebuildFixture rebuild_current_mode_fixture(void)
{
    RebuildFixture f=rebuild_fixture("<input id=r type=range min=1 max=20>"
        "<input id=t value=initial><input id=u value=first>",0);
    rebuild_expect("rebuild: dirty range fixture",os64_page_node_set_value(f.state,rebuild_id(f.doc,"r"),"15",2)==0);
    rebuild_expect("rebuild: dirty text fixture",os64_page_node_set_value(f.state,rebuild_id(f.doc,"t"),"abc",3)==0);
    rebuild_node_value("rebuild: default cache fixture",f.state,rebuild_id(f.doc,"u"),"first");
    rebuild_expect("rebuild: range maximum mutation",os64_html_set_attr(f.doc,rebuild_id(f.doc,"r"),"max","3",1)==0);
    rebuild_expect("rebuild: current text type mutation",os64_html_set_attr(f.doc,rebuild_id(f.doc,"t"),"type","number",6)==0);
    rebuild_expect("rebuild: cached default mutation",os64_html_set_attr(f.doc,rebuild_id(f.doc,"u"),"value","newer",5)==0);
    return f;
}

static void rebuild_current_modes(void)
{
    for(int route=0;route<2;route++) {
        RebuildFixture f=rebuild_current_mode_fixture();
        os64_html_node_t *r=rebuild_id(f.doc,"r"),*t=rebuild_id(f.doc,"t"),*u=rebuild_id(f.doc,"u");
        if(route==0) {
            rebuild_node_value("rebuild: current max clamps dirty getter",f.state,r,"3");
            rebuild_node_value("rebuild: current number sanitizes dirty getter",f.state,t,"");
            rebuild_node_value("rebuild: default cache follows changed attribute",f.state,u,"newer");
        }
        os64_page_t *fresh=os64_page_rebuild(f.page);
        rebuild_expect("rebuild: current modes snapshot",fresh!=NULL);
        rebuild_value("rebuild: rebuilt dirty range clamped",fresh,r,"3");
        rebuild_value("rebuild: rebuilt current number empty",fresh,t,"");
        rebuild_value("rebuild: rebuilt default uses new attribute",fresh,u,"newer");
        rebuild_node_value("rebuild: range getter agrees rebuilt model",f.state,r,"3");
        rebuild_node_value("rebuild: number getter agrees rebuilt model",f.state,t,"");
        rebuild_node_value("rebuild: default getter agrees rebuilt model",f.state,u,"newer");
        rebuild_value("rebuild: old range publication safe after resanitizing",f.page,r,"3");
        rebuild_value("rebuild: old number publication safe after resanitizing",f.page,t,"");
        rebuild_value("rebuild: old default publication safe after refreshing",f.page,u,"newer");
        rebuild_expect("rebuild: dirty current default before reset",os64_page_node_set_value(f.state,u,"dirty",5)==0);
        rebuild_expect("rebuild: reset uses rebuilt default",os64_page_reset(fresh,-1)==0);
        rebuild_value("rebuild: fresh reset default",fresh,u,"newer");
        rebuild_node_value("rebuild: node reset default",f.state,u,"newer");
        rebuild_value("rebuild: old model reset publication safe",f.page,u,"newer");
        os64_page_free(fresh);rebuild_clear(&f);
    }
    RebuildFixture f=rebuild_fixture("<input id=t value=default>",0);
    os64_html_node_t *t=rebuild_id(f.doc,"t");
    rebuild_expect("rebuild: text before file",os64_page_node_set_value(f.state,t,"secret",6)==0);
    rebuild_expect("rebuild: change current type to file",os64_html_set_attr(f.doc,t,"type","file",4)==0);
    rebuild_node_value("rebuild: observed file cannot expose old dirty text",f.state,t,"");
    os64_page_t *file=os64_page_rebuild(f.page);
    rebuild_value("rebuild: file model empty",file,t,"");
    rebuild_expect("rebuild: return current type to text",os64_html_set_attr(f.doc,t,"type","text",4)==0);
    const char *value=NULL;size_t length=0;
    rebuild_expect("rebuild: observed file clears old dirty text permanently",
        os64_page_node_value(f.state,t,&value,&length)==0 && value!=NULL && strcmp(value,"secret")!=0);
    os64_page_t *text=os64_page_rebuild(file);
    const os64_page_control_t *c=os64_page_control(text,os64_page_control_for(text,t));
    rebuild_expect("rebuild: rebuilt text cannot resurrect prior file contents",c!=NULL && strcmp(c->value,"secret")!=0);
    os64_page_free(text);os64_page_free(file);rebuild_clear(&f);
}

static void rebuild_current_mode_failures(void)
{
    for(int operation=0;operation<3;operation++) {
        RebuildFixture f=rebuild_current_mode_fixture();
        size_t before=allocations;
        os64_page_t *fresh=NULL;
        if(operation==0) {
            fresh=os64_page_rebuild(f.page);
            rebuild_expect("rebuild: resanitizing candidate counts",fresh!=NULL);
        } else rebuild_node_value("rebuild: resanitizing getter counts",f.state,
            rebuild_id(f.doc,operation==1 ? "r" : "t"),operation==1 ? "3" : "");
        size_t count=allocations-before;
        os64_page_free(fresh);rebuild_clear(&f);
        fail_single=true;
        for(size_t point=1;point<=count;point++) {
            size_t heap=live;
            f=rebuild_current_mode_fixture();
            uint64_t sv=os64_page_state_version(f.state),hv=os64_html_version(f.doc);
            size_t charged=os64_page_state_bytes(f.state);
            fail_at=allocations+point;
            if(operation==0) {
                fresh=os64_page_rebuild(f.page);
                rebuild_expect("rebuild: resanitization allocation rejects candidate",fresh==NULL);
            } else {
                const char *value="unchanged",*sentinel=value;size_t length=777;
                int64_t status=os64_page_node_value(f.state,rebuild_id(f.doc,operation==1 ? "r" : "t"),&value,&length);
                rebuild_expect("rebuild: resanitization getter allocation refusal",status==-OS64_PAGE_REASON_NO_MEMORY &&
                    value==sentinel && length==777);
            }
            fail_at=0;
            rebuild_value("rebuild: failed resanitization leaves old dirty range",f.page,rebuild_id(f.doc,"r"),"15");
            rebuild_value("rebuild: failed resanitization leaves old dirty text",f.page,rebuild_id(f.doc,"t"),"abc");
            rebuild_value("rebuild: failed resanitization leaves old default",f.page,rebuild_id(f.doc,"u"),"first");
            rebuild_expect("rebuild: failed resanitization changes no revision or charge",
                os64_page_state_version(f.state)==sv && os64_html_version(f.doc)==hv &&
                os64_page_state_bytes(f.state)==charged);
            rebuild_expect("rebuild: failed resanitization releases candidate pin",rebuild_available_pins(f.doc)==15);
            os64_page_free(fresh);rebuild_clear(&f);
            rebuild_expect("rebuild: resanitization sweep returns heap",live==heap);
        }
        fail_single=false;
    }
}

static void rebuild_length_verdict(RebuildFixture *f,bool valid,const char *expected_body)
{
    os64_page_request_t request;
    os64_page_what_t what={OS64_PAGE_ACTIVATE_CONTROL,
        os64_page_control_for(f->page,rebuild_id(f->doc,"send")),0,0};
    os64_page_verdict_t verdict=os64_page_activate(f->page,what,&request);
    if(valid)rebuild_expect("rebuild: script value is not user length validation",
        verdict==OS64_PAGE_NAVIGATE && request.body!=NULL && strcmp(request.body,expected_body)==0);
    else rebuild_expect("rebuild: person value retains length validation",
        verdict==OS64_PAGE_NOTHING && request.reason==OS64_PAGE_REASON_INVALID &&
        request.control==os64_page_control_for(f->page,rebuild_id(f->doc,"t")));
    os64_page_request_free(&request);
}

static void rebuild_length_origins(void)
{
    static const struct {const char *html,*value,*body;} cases[]={
        {"<form method=post><input id=t name=t maxlength=3><button id=send>send</button></form>","abcdef","t=abcdef"},
        {"<form method=post><input id=t name=t minlength=3><button id=send>send</button></form>","xy","t=xy"}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        RebuildFixture f=rebuild_fixture(cases[i].html,0);
        os64_html_node_t *t=rebuild_id(f.doc,"t");size_t length=strlen(cases[i].value);
        rebuild_expect("rebuild: script sets value beyond length constraint",
            os64_page_node_set_value(f.state,t,cases[i].value,length)==0);
        rebuild_length_verdict(&f,true,cases[i].body);
        rebuild_expect("rebuild: person supplies same out-of-bound value",
            os64_page_set_text(f.page,os64_page_control_for(f.page,t),cases[i].value,length)==0);
        rebuild_length_verdict(&f,false,cases[i].body);
        rebuild_expect("rebuild: same script value releases user length constraint",
            os64_page_node_set_value(f.state,t,cases[i].value,length)==0);
        rebuild_length_verdict(&f,true,cases[i].body);
        os64_page_t *fresh=os64_page_rebuild(f.page);
        rebuild_expect("rebuild: script length origin survives snapshot",fresh!=NULL);
        os64_page_free(f.page);f.page=fresh;
        rebuild_length_verdict(&f,true,cases[i].body);
        rebuild_clear(&f);
    }
    RebuildFixture f=rebuild_fixture(cases[0].html,0);
    os64_html_node_t *t=rebuild_id(f.doc,"t");
    rebuild_expect("rebuild: user origin allocation fixture",os64_page_set_text(f.page,0,"abcdef",6)==0);
    size_t before=allocations;
    rebuild_expect("rebuild: script origin success allocation count",os64_page_node_set_value(f.state,t,"abcdef",6)==0);
    size_t count=allocations-before;
    rebuild_clear(&f);
    fail_single=true;
    for(size_t point=1;point<=count;point++) {
        size_t heap=live;
        f=rebuild_fixture(cases[0].html,0);t=rebuild_id(f.doc,"t");
        rebuild_expect("rebuild: user origin before refusal",os64_page_set_text(f.page,0,"abcdef",6)==0);
        uint64_t revision=os64_page_state_version(f.state);size_t charged=os64_page_state_bytes(f.state);
        fail_at=allocations+point;
        int64_t status=os64_page_node_set_value(f.state,t,"abcdef",6);
        fail_at=0;
        rebuild_expect("rebuild: failed script setter keeps user origin",status==-OS64_PAGE_REASON_NO_MEMORY);
        rebuild_length_verdict(&f,false,cases[0].body);
        rebuild_expect("rebuild: origin refusal changes no state revision or charge",
            os64_page_state_version(f.state)==revision && os64_page_state_bytes(f.state)==charged);
        rebuild_clear(&f);
        rebuild_expect("rebuild: origin refusal returns heap",live==heap);
    }
    fail_single=false;
}

static int64_t rebuild_failure_operation(RebuildFixture *f,int op)
{
    if(op==0)return os64_page_node_set_value(f->state,rebuild_id(f->doc,"t"),"changed",7);
    if(op==1)return os64_page_node_set_checked(f->state,rebuild_id(f->doc,"r0"),true);
    if(op==2)return os64_page_node_set_selected_index(f->state,rebuild_id(f->doc,"s"),1);
    if(op==3)return os64_page_node_set_value(f->state,rebuild_id(f->doc,"h"),"changed",7);
    const char *v="unchanged",*sentinel=v;size_t n=777;
    int64_t status=os64_page_node_value(f->state,rebuild_id(f->doc,"new"),&v,&n);
    if(status<0)rebuild_expect("rebuild: default getter refusal preserves outputs",v==sentinel && n==777);
    else rebuild_expect("rebuild: default getter sanitizer",v!=NULL && n==12 && strcmp(v,"default\ntext")==0);
    return status;
}

static RebuildFixture rebuild_setter_fixture(void)
{
    RebuildFixture f=rebuild_fixture("<input id=t value=base><input id=r0 type=radio name=r>"
        "<input id=r1 type=radio name=r checked><select id=s><option>A<option>B</select>"
        "<input id=h type=hidden value=old>",0);
    rebuild_expect("rebuild: value initialized for setter sweep",
        os64_page_node_set_value(f.state,rebuild_id(f.doc,"t"),"kept",4)==0);
    int64_t status=0;
    os64_html_node_t *node=os64_html_create_element(f.doc,OS64_HTML_NS_HTML,"textarea",&status);
    rebuild_expect("rebuild: detached default getter fixture",node!=NULL && status==0);
    // The node is connected only for a convenient lookup; the old model has
    // never seen it, so reading its default must establish new state.
    rebuild_expect("rebuild: default getter node id",os64_html_set_attr(f.doc,node,"id","new",3)==0);
    os64_html_node_t *text=os64_html_create_text(f.doc,"default\r\ntext",13,&status);
    rebuild_expect("rebuild: default getter text",text!=NULL && status==0 &&
        os64_html_insert(f.doc,node,text,NULL)==0 && os64_html_insert(f.doc,f.doc->body,node,NULL)==0);
    return f;
}

static void rebuild_setter_failure_sweeps(void)
{
    for(int op=0;op<5;op++) {
        RebuildFixture f=rebuild_setter_fixture();
        uint64_t initial_generation=os64_page_state_version(f.state);
        size_t before=allocations;
        rebuild_expect("rebuild: setter sweep successful operation",rebuild_failure_operation(&f,op)==0);
        size_t count=allocations-before;
        if(op==4)rebuild_expect("rebuild: default cache does not change state generation",
            os64_page_state_version(f.state)==initial_generation);
        rebuild_clear(&f);
        fail_single=true;
        for(size_t point=1;point<=count;point++) {
            size_t heap=live;
            f=rebuild_setter_fixture();
            uint64_t sv=os64_page_state_version(f.state),hv=os64_html_version(f.doc);
            size_t state_bytes=os64_page_state_bytes(f.state);
            fail_at=allocations+point;
            int64_t got=rebuild_failure_operation(&f,op);
            fail_at=0;
            rebuild_expect("rebuild: setter allocation refusal named",got==-OS64_PAGE_REASON_NO_MEMORY);
            rebuild_node_value("rebuild: failed setter preserves text",f.state,rebuild_id(f.doc,"t"),"kept");
            rebuild_value("rebuild: failed setter preserves old publication",f.page,rebuild_id(f.doc,"t"),"kept");
            rebuild_expect("rebuild: failed setter preserves radios",!os64_page_control(f.page,1)->checked &&
                os64_page_control(f.page,2)->checked);
            rebuild_value("rebuild: failed setter preserves select",f.page,rebuild_id(f.doc,"s"),"A");
            rebuild_attribute("rebuild: failed setter preserves attribute",rebuild_id(f.doc,"h"),"value","old");
            rebuild_expect("rebuild: failed setter preserves generations",os64_page_state_version(f.state)==sv &&
                os64_html_version(f.doc)==hv);
            rebuild_expect("rebuild: failed setter returns state allocations",
                os64_page_state_bytes(f.state)==state_bytes);
            rebuild_clear(&f);
            rebuild_expect("rebuild: setter refusal returns heap",live==heap);
        }
        fail_single=false;
    }
}

static uint32_t rebuild_random(uint32_t *seed)
{
    *seed=*seed*1664525u+1013904223u;
    return *seed>>16;
}

static void rebuild_random_oracle(void)
{
    for(uint32_t seed=1;seed<=4;seed++) {
        uint32_t random=seed;
        RebuildFixture f=rebuild_fixture("<div id=parent><input id=t0 value=d0><input id=t1 value=d1>"
            "<input id=t2 value=d2><input id=t3 value=d3><select id=s><option id=a>A"
            "<option id=b>B<option id=c>C</select></div>",0);
        os64_html_node_t *parent=rebuild_id(f.doc,"parent"),*inputs[4],*options[3];
        os64_html_node_t *select=rebuild_id(f.doc,"s");
        bool connected[4]={true,true,true,true};char values[4][32]={"d0","d1","d2","d3"};
        int order[3]={0,1,2},selected=0;
        unsigned operations[4]={0},edits[4]={0},toggles[4]={0};
        for(int i=0;i<4;i++){char id[3]={'t',(char)('0'+i),0};inputs[i]=rebuild_id(f.doc,id);}
        options[0]=rebuild_id(f.doc,"a");options[1]=rebuild_id(f.doc,"b");options[2]=rebuild_id(f.doc,"c");
        for(int step=0;step<320;step++) {
            unsigned op=rebuild_random(&random)%4,which=rebuild_random(&random)%4;
            operations[op]++;
            if(op==0) {
                edits[which]++;
                snprintf(values[which],sizeof(values[which]),"v%u_%d",seed,step);
                rebuild_expect("rebuild oracle: assign node",os64_page_node_set_value(f.state,inputs[which],
                    values[which],strlen(values[which]))==0);
            } else if(op==1) {
                toggles[which]++;
                int64_t result=connected[which] ? os64_html_remove(f.doc,inputs[which]) :
                    os64_html_insert(f.doc,parent,inputs[which],NULL);
                rebuild_expect("rebuild oracle: toggle connection",result==0);connected[which]=!connected[which];
            } else if(op==2) {
                int index=(int)(rebuild_random(&random)%3);
                selected=order[index];
                rebuild_expect("rebuild oracle: choose by current position",
                    os64_page_node_set_selected_index(f.state,select,index)==0);
            } else {
                int moving=order[2];
                rebuild_expect("rebuild oracle: rotate option nodes",
                    os64_html_insert(f.doc,select,options[moving],options[order[0]])==0);
                order[2]=order[1];order[1]=order[0];order[0]=moving;
            }
            os64_page_t *fresh=os64_page_rebuild(f.page);
            rebuild_expect("rebuild oracle: snapshot",fresh!=NULL);
            for(int i=0;i<4;i++) {
                if(connected[i]) rebuild_value("rebuild oracle: connected node value",fresh,inputs[i],values[i]);
                else rebuild_expect("rebuild oracle: disconnected node excluded",os64_page_control_for(fresh,inputs[i])==-1);
                rebuild_node_value("rebuild oracle: state follows node through detach",f.state,inputs[i],values[i]);
            }
            const os64_page_control_t *sc=os64_page_control(fresh,os64_page_control_for(fresh,select));
            for(int i=0;i<3;i++)rebuild_expect("rebuild oracle: selectedness follows option identity",
                sc!=NULL && sc->options[i].node==options[order[i]] &&
                sc->options[i].selected==(order[i]==selected));
            os64_page_free(f.page);f.page=fresh;
        }
        for(int i=0;i<4;i++) {
            rebuild_expect("rebuild oracle: each operation exercised",operations[i]!=0);
            rebuild_expect("rebuild oracle: each control assigned",edits[i]!=0);
            rebuild_expect("rebuild oracle: each control detached and reinserted",toggles[i]>=2);
        }
        rebuild_clear(&f);
    }
}

#include "test_libpage_d3_followup.inc"

static void rebuild_reclamation_holders(void)
{
    RebuildFixture f=rebuild_fixture("<p id=paragraph>kept by the model</p>",0);
    os64_html_node_t *paragraph=rebuild_id(f.doc,"paragraph");
    size_t count=f.doc->node_count;
    rebuild_expect("reclaim: model node reference survives detach",
        os64_html_remove(f.doc,paragraph)==OS64_HTML_OK && f.doc->node_count==count);
    rebuild_expect("reclaim: stale model anchor still names its held identity",
        os64_page_anchor(f.page,"paragraph")==paragraph && paragraph->text==NULL &&
        paragraph->first_child!=NULL);
    os64_page_free(f.page);f.page=NULL;
    rebuild_expect("reclaim: model teardown releases node and string protection",
        f.doc->node_count==count-2);
    rebuild_clear(&f);

    f=rebuild_fixture("<select id=select><option id=option selected>first</select>",0);
    os64_html_node_t *select=rebuild_id(f.doc,"select"), *option=rebuild_id(f.doc,"option");
    rebuild_expect("reclaim: create persistent option selectedness key",
        os64_page_node_set_selected_index(f.state,select,-1)==0 &&
        os64_page_node_set_selected_index(f.state,select,0)==0);
    rebuild_node_value("reclaim: create select default cache and option state",f.state,select,"first");
    count=f.doc->node_count;
    rebuild_expect("reclaim: state-key subtree detaches",os64_html_remove(f.doc,select)==OS64_HTML_OK);
    os64_page_free(f.page);f.page=NULL;
    rebuild_expect("reclaim: state holds outlive model pin",f.doc->node_count==count);
    rebuild_node_value("reclaim: detached default cache remains readable",f.state,select,"first");
    rebuild_expect("reclaim: option key remains held independently of its select",
        os64_html_remove(f.doc,option)==OS64_HTML_OK && f.doc->node_count==count &&
        option->first_child!=NULL && strcmp(option->first_child->text,"first")==0);
    os64_page_state_free(f.state);f.state=NULL;
    rebuild_expect("reclaim: state teardown releases select and option holds",f.doc->node_count==count-3);
    rebuild_clear(&f);

    // No model or snapshot pin can mask a reservation hold left on abort.
    f=(RebuildFixture){.doc=parse("<select id=select><option id=option>first</select>","utf-8")};
    f.state=os64_page_state_create(f.doc,0);
    select=rebuild_id(f.doc,"select"); option=rebuild_id(f.doc,"option");
    count=f.doc->node_count;
    size_t bytes=os64_page_state_bytes(f.state), start=allocations;
    uint64_t version=os64_html_version(f.doc);
    int64_t status=os64_page_node_insert(f.state,f.doc->body,select,option);
    rebuild_expect("reclaim: invalid placement reaches state reservations then refuses",
        status!=OS64_HTML_OK && allocations>start &&
        os64_page_state_bytes(f.state)==bytes && os64_html_version(f.doc)==version);
    rebuild_expect("reclaim: aborted reservations release their select and option holds",
        os64_html_remove(f.doc,select)==OS64_HTML_OK && f.doc->node_count==count-3);
    rebuild_clear(&f);
}

static void rebuild_contract(void)
{
    rebuild_lifetime_and_stale();
    rebuild_detach_and_options();
    rebuild_value_modes();
    rebuild_failures_and_caps();
    rebuild_detached_radios();
    rebuild_current_modes();
    rebuild_current_mode_failures();
    rebuild_length_origins();
    rebuild_setter_failure_sweeps();
    rebuild_random_oracle();
    rebuild_review_followup();
    rebuild_reclamation_holders();
}

static void rebuild_live_state_free(void)
{
    RebuildFixture f=rebuild_fixture("<input>",0);
    os64_page_state_free(f.state);
    fail("rebuild: freeing borrowed state must terminate", "state_free returned");
}
