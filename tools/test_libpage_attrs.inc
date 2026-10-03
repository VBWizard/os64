// Attribute histories are checked without a property observation between
// mutations; this distinguishes transitions from final-tree reconciliation.
static int64_t attrs_set(RebuildFixture *f, os64_html_node_t *node,
                          const char *name, const char *value)
{
    return os64_page_node_set_attr(f->state, node, name, value,
                                   value != NULL ? strlen(value) : 0, value == NULL);
}

static void attrs_transitions(void)
{
    RebuildFixture f = rebuild_fixture("<input id=q value=default><input id=r type=range value=8 step=1>"
        "<input id=a type=radio name=a checked><input id=b type=radio name=b checked>"
        "<select id=s multiple><option selected>a<option selected>b</select>", 0);
    os64_html_node_t *q = rebuild_id(f.doc, "q"), *r = rebuild_id(f.doc, "r");
    rebuild_expect("attrs: borrowed document", os64_page_state_document(f.state) == f.doc &&
                    os64_page_state_document(NULL) == NULL);
    rebuild_expect("attrs: dirty assignment", os64_page_node_set_value(f.state,q,"secret",6)==0);
    rebuild_expect("attrs: hidden transition",attrs_set(&f,q,"type","hidden")==0);
    rebuild_attribute("attrs: transfer to content attribute",q,"value","secret");
    rebuild_expect("attrs: text transition",attrs_set(&f,q,"type","text")==0);
    rebuild_node_value("attrs: history without read",f.state,q,"secret");
    rebuild_expect("attrs: restored clean default follows value",attrs_set(&f,q,"value","new")==0);
    rebuild_node_value("attrs: return resets dirty",f.state,q,"new");
    rebuild_expect("attrs: empty dirty assignment",os64_page_node_set_value(f.state,q,"",0)==0);
    rebuild_expect("attrs: empty hidden transition",attrs_set(&f,q,"type","hidden")==0);
    rebuild_expect("attrs: empty text transition",attrs_set(&f,q,"type","text")==0);
    rebuild_node_value("attrs: empty does not overwrite default",f.state,q,"new");
    rebuild_expect("attrs: file dirty assignment",os64_page_node_set_value(f.state,q,"private",7)==0);
    rebuild_expect("attrs: file transition",attrs_set(&f,q,"type","file")==0);
    rebuild_expect("attrs: file return without read",attrs_set(&f,q,"type",NULL)==0);
    rebuild_node_value("attrs: file clears historical dirty",f.state,q,"new");
    rebuild_expect("attrs: numeric dirty assignment",os64_page_node_set_value(f.state,q,"oops",4)==0);
    rebuild_expect("attrs: number transition",attrs_set(&f,q,"type","number")==0);
    rebuild_expect("attrs: number return",attrs_set(&f,q,"type","text")==0);
    rebuild_node_value("attrs: invalid value never resurrects",f.state,q,"");
    rebuild_expect("attrs: range dirty assignment",os64_page_node_set_value(f.state,r,"8",1)==0);
    rebuild_expect("attrs: temporary max",attrs_set(&f,r,"max","3")==0);
    rebuild_expect("attrs: restore max",attrs_set(&f,r,"max",NULL)==0);
    rebuild_node_value("attrs: clamped value never resurrects",f.state,r,"3");
    os64_html_node_t *a=rebuild_id(f.doc,"a"), *b=rebuild_id(f.doc,"b");
    rebuild_expect("attrs: radio regroup",attrs_set(&f,b,"name","a")==0);
    bool on=true;
    rebuild_expect("attrs: radio peer clears immediately",os64_page_node_checked(f.state,a,&on)==0&&!on);
    rebuild_expect("attrs: radio moved node wins",os64_page_node_checked(f.state,b,&on)==0&&on);
    os64_html_node_t *select=rebuild_id(f.doc,"s");
    rebuild_expect("attrs: select becomes single",attrs_set(&f,select,"multiple",NULL)==0);
    rebuild_expect("attrs: select returns multiple",attrs_set(&f,select,"multiple","")==0);
    int32_t index=-1;
    rebuild_expect("attrs: select history keeps last choice",os64_page_node_selected_index(f.state,select,&index)==0&&index==1);
    uint64_t version=os64_html_version(f.doc), revision=os64_page_state_version(f.state);
    size_t bytes=os64_page_state_bytes(f.state), arena=f.doc->arena_bytes, peak=f.doc->peak_arena_bytes;
    rebuild_expect("attrs: same set",attrs_set(&f,q,"type","text")==0);
    rebuild_expect("attrs: absent remove",attrs_set(&f,q,"absent",NULL)==0);
    rebuild_expect("attrs: no op accounting",version==os64_html_version(f.doc)&&revision==os64_page_state_version(f.state)&&
        bytes==os64_page_state_bytes(f.state)&&arena==f.doc->arena_bytes&&peak==f.doc->peak_arena_bytes);
    rebuild_clear(&f);
}

static void attrs_constraint_and_group_histories(void)
{
    RebuildFixture f=rebuild_fixture("<input id=e type=email><input id=c type=checkbox checked>"
        "<form id=f></form><input id=a type=radio name=g checked>"
        "<input id=b type=radio name=g checked form=f>",0);
    os64_html_node_t *e=rebuild_id(f.doc,"e"), *c=rebuild_id(f.doc,"c");
    rebuild_expect("attrs: email dirty",os64_page_node_set_value(f.state,e," a@b , c@d ",11)==0);
    rebuild_expect("attrs: email multiple",attrs_set(&f,e,"multiple","")==0);
    rebuild_expect("attrs: email restores single",attrs_set(&f,e,"multiple",NULL)==0);
    rebuild_node_value("attrs: per-address strip persists",f.state,e,"a@b,c@d");
    rebuild_expect("attrs: default checked removed",attrs_set(&f,c,"checked",NULL)==0);
    rebuild_expect("attrs: default checked restored",attrs_set(&f,c,"checked","")==0);
    bool on=false;
    rebuild_expect("attrs: checked history follows attribute",os64_page_node_checked(f.state,c,&on)==0&&on);
    rebuild_expect("attrs: dirty unchecked",os64_page_node_set_checked(f.state,c,false)==0);
    rebuild_expect("attrs: dirty checked removed",attrs_set(&f,c,"checked",NULL)==0);
    rebuild_expect("attrs: dirty checked restored",attrs_set(&f,c,"checked","")==0);
    rebuild_expect("attrs: dirty checked survives attributes",os64_page_node_checked(f.state,c,&on)==0&&!on);
    os64_html_node_t *form=rebuild_id(f.doc,"f"), *a=rebuild_id(f.doc,"a"), *b=rebuild_id(f.doc,"b");
    rebuild_expect("attrs: form id changes owner",attrs_set(&f,form,"id","renamed")==0);
    rebuild_expect("attrs: id regroup clears old peer",os64_page_node_checked(f.state,a,&on)==0&&!on);
    rebuild_expect("attrs: id changed owner's radio wins",os64_page_node_checked(f.state,b,&on)==0&&on);
    rebuild_clear(&f);
}

static void attrs_failure_sweep(void)
{
    size_t points=0;
    for(size_t point=0;;point++) {
        RebuildFixture f=rebuild_fixture("<input id=q value=default>",0);
        os64_html_node_t *q=rebuild_id(f.doc,"q");
        rebuild_expect("attrs sweep: dirty setup",os64_page_node_set_value(f.state,q,"private",7)==0);
        const char *borrowed=NULL;size_t len=0;
        rebuild_expect("attrs sweep: borrowed value",os64_page_node_value(f.state,q,&borrowed,&len)==0);
        os64_html_pin_t pin=os64_html_pin(f.doc);
        const os64_html_attr_t *old=q->attrs;
        uint64_t version=os64_html_version(f.doc), revision=os64_page_state_version(f.state);
        size_t bytes=os64_page_state_bytes(f.state), arena=f.doc->arena_bytes, peak=f.doc->peak_arena_bytes, retained=live;
        allocations=0;fail_at=point;fail_single=true;
        int64_t status=attrs_set(&f,q,"type","hidden");
        size_t attempted=allocations;fail_at=0;fail_single=false;
        if(point==0)points=attempted;
        if(point!=0) {
            rebuild_expect("attrs sweep: named refusal",status==OS64_HTML_NO_MEMORY);
            rebuild_expect("attrs sweep: exact rollback",q->attrs==old&&version==os64_html_version(f.doc)&&
                revision==os64_page_state_version(f.state)&&bytes==os64_page_state_bytes(f.state)&&
                arena==f.doc->arena_bytes&&peak==f.doc->peak_arena_bytes&&retained==live);
            rebuild_expect("attrs sweep: retained dirty bytes",len==7&&memcmp(borrowed,"private",8)==0);
            rebuild_node_value("attrs sweep: state remains dirty",f.state,q,"private");
        } else {
            rebuild_expect("attrs sweep: success",status==0);
            rebuild_expect("attrs sweep: pinned attribute bytes",strcmp(old->name,"id")==0);
        }
        os64_html_unpin(f.doc,pin);rebuild_clear(&f);
        if(point==points)break;
    }
}

static void attrs_unmodeled_radio(void)
{
    os64_html_document_t *doc=parse("<input id=a type=radio name=g checked><input id=b type=radio name=g checked>","utf-8");
    os64_page_state_t *state=os64_page_state_create(doc,0);
    os64_html_node_t *a=rebuild_id(doc,"a"),*b=rebuild_id(doc,"b");
    bool on=true;
    rebuild_expect("attrs unmodeled: earlier radio false",os64_page_node_checked(state,a,&on)==0&&!on);
    rebuild_expect("attrs unmodeled: last radio true",os64_page_node_checked(state,b,&on)==0&&on);
    rebuild_expect("attrs unmodeled: move unchecked radio group",os64_page_node_set_attr(state,a,"name","other",5,false)==0);
    rebuild_expect("attrs unmodeled: name does not resurrect checked",os64_page_node_checked(state,a,&on)==0&&!on);
    rebuild_expect("attrs unmodeled: return unchanged radio",os64_page_node_set_attr(state,a,"name","g",1,false)==0);
    rebuild_expect("attrs unmodeled: true radio stays true",os64_page_node_checked(state,b,&on)==0&&on);
    rebuild_expect("attrs unmodeled: checked winner leaves group",os64_page_node_set_attr(state,b,"name","separate",8,false)==0);
    rebuild_expect("attrs unmodeled: earlier raw attribute stays false",os64_page_node_checked(state,a,&on)==0&&!on);
    os64_page_state_free(state);os64_html_document_free(doc);
}

static void attrs_unmodeled_select(void)
{
    for(int observe=0;observe<2;observe++) {
        os64_html_document_t *doc=parse("<select id=s><option>A<option>B</select><div id=d></div>","utf-8");
        os64_page_state_t *state=os64_page_state_create(doc,0);
        os64_html_node_t *select=rebuild_id(doc,"s");
        int32_t index=-1;
        if(observe)
            rebuild_expect("attrs unmodeled: initial fallback",os64_page_node_selected_index(state,select,&index)==0&&index==0);
        rebuild_expect("attrs unmodeled: multiple",os64_page_node_set_attr(state,select,"multiple","",0,false)==0);
        rebuild_expect("attrs unmodeled: selected fallback retained",os64_page_node_selected_index(state,select,&index)==0&&index==0);
        os64_page_state_free(state);os64_html_document_free(doc);
    }
}

static void attrs_batch_contract(void)
{
    RebuildFixture f=rebuild_fixture("<input id=q a=one b=two>",0);
    os64_html_node_t *q=rebuild_id(f.doc,"q");
    rebuild_expect("batch: make records private",os64_html_set_attr(f.doc,q,"a","old",3)==0);
    const os64_html_attr_t *old=q->attrs;
    os64_html_pin_t pin=os64_html_pin(f.doc);
    uint64_t version=os64_html_version(f.doc);
    os64_html_attr_change_t changes[]={{"a","new",3,false},{"b",NULL,0,true},{"c","last",4,false}};
    rebuild_expect("batch: complete transaction",os64_html_set_attrs(f.doc,q,changes,3)==0);
    rebuild_expect("batch: one publication",os64_html_version(f.doc)==version+1);
    rebuild_expect("batch: pin keeps old records",strcmp(old->next->value,"old")==0&&
        strcmp(old->next->next->value,"two")==0);
    rebuild_expect("batch: ordered result",strcmp(q->attrs->name,"id")==0&&
        strcmp(q->attrs->next->name,"a")==0&&strcmp(q->attrs->next->next->name,"c")==0);
    size_t arena=f.doc->arena_bytes,peak=f.doc->peak_arena_bytes;
    version=os64_html_version(f.doc);
    os64_html_attr_change_t cancel[]={{"a","temporary",9,false},{"a","new",3,false}};
    const os64_html_attr_t *current=q->attrs;
    rebuild_expect("batch: cancel changes",os64_html_set_attrs(f.doc,q,cancel,2)==0);
    rebuild_expect("batch: final no op",q->attrs==current&&version==os64_html_version(f.doc)&&
        arena==f.doc->arena_bytes&&peak==f.doc->peak_arena_bytes);
    os64_html_attr_change_t invalid[]={{"a","changed",7,false},{"x","bad\0value",9,false}};
    rebuild_expect("batch: validates whole input",os64_html_set_attrs(f.doc,q,invalid,2)==OS64_HTML_BAD_TEXT);
    rebuild_expect("batch: validation changes nothing",q->attrs==current&&version==os64_html_version(f.doc)&&
        arena==f.doc->arena_bytes&&peak==f.doc->peak_arena_bytes);
    os64_html_unpin(f.doc,pin);rebuild_clear(&f);
}

static void test_libpage_attrs(void)
{
    attrs_unmodeled_radio();
    attrs_unmodeled_select();
    attrs_batch_contract();
    attrs_transitions();
    attrs_constraint_and_group_histories();
    attrs_failure_sweep();
}
