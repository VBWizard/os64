/* Public native seams cover template controls and refused foreign sources that
 * the deliberately small script surface cannot construct or inspect. */
static bool clone_value_is(os64_page_state_t *state,const os64_html_node_t *node,const char *expected)
{
    const char *value=NULL;size_t length=0;
    return os64_page_node_value(state,node,&value,&length)==0&&length==strlen(expected)&&!memcmp(value,expected,length);
}
static void dom_clone_native_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,"document.getElementById('template').innerHTML='<input id=template-input value=default><textarea id=template-area>default-area</textarea><input id=template-check type=checkbox>'","clone-template-setup.js");
    os64_html_node_t *source=native_id(f.document->document,"template");
    os64_html_node_t *input=native_id(source->template_contents,"template-input"),*area=native_id(source->template_contents,"template-area"),*tick=native_id(source->template_contents,"template-check");
    check(input&&area&&tick,"native template fixture controls");
    check(os64_page_node_set_value(f.state,input,"template-typed",14)==0&&os64_page_node_set_value(f.state,area,"template-area",13)==0&&os64_page_node_set_checked(f.state,tick,true)==0,"initialize current template control state");
    uint64_t tree=os64_html_version(f.document),version=os64_page_state_version(f.state);int64_t status=123;
    os64_html_node_t *copy=os64_page_node_clone(f.state,source,true,&status);
    check(copy&&status==OS64_HTML_OK&&copy->template_contents,"native deep template clone");
    if(copy&&copy->template_contents){
        bool checked=false;
        check(clone_value_is(f.state,native_id(copy->template_contents,"template-input"),"template-typed"),"deep clone includes template input state");
        check(clone_value_is(f.state,native_id(copy->template_contents,"template-area"),"template-area"),"deep clone includes template textarea state");
        check(os64_page_node_checked(f.state,native_id(copy->template_contents,"template-check"),&checked)==0&&checked,"deep clone includes template checkedness");
    }
    check(tree==os64_html_version(f.document)&&version==os64_page_state_version(f.state),"detached successful clone leaves both revisions unchanged");
    size_t bytes=os64_page_state_bytes(f.state);
    check(!os64_page_node_clone(NULL,source,true,&status)&&status==OS64_HTML_BAD_ARGUMENT,"clone rejects NULL state");
    check(!os64_page_node_clone(f.state,NULL,true,&status)&&status==OS64_HTML_BAD_ARGUMENT,"clone rejects NULL source");
    check(!os64_page_node_clone(f.state,f.document->document,true,&status)&&status==OS64_HTML_BAD_ARGUMENT,"clone rejects document source");
    DomFixture foreign=fixture_base(0,0,0);
    check(!os64_page_node_clone(f.state,foreign.document->body,true,&status)&&status==OS64_HTML_BAD_ARGUMENT,"clone refuses foreign document source");
    check(tree==os64_html_version(f.document)&&version==os64_page_state_version(f.state)&&bytes==os64_page_state_bytes(f.state),"malformed clone refusal preserves state and revisions");
    os64_js_destroy(foreign.runtime);os64_page_state_free(foreign.state);os64_html_document_free(foreign.document);
    check(os64_page_node_clone(f.state,input,false,NULL)!=NULL,"clone accepts optional status output");
    fixture_free(&f);
}
static void dom_clone_state_cap_cases(void)
{
    DomFixture f=fixture_base(0,24576,0);os64_html_node_t *field=native_id(f.document->document,"field");
    char text[5001];memset(text,'x',5000);text[5000]=0;
    check(os64_page_node_set_value(f.state,field,text,5000)==0,"bounded clone source fits state cap");
    int64_t prepare=0;os64_html_node_t *peer=os64_html_create_element(f.document,OS64_HTML_NS_HTML,"textarea",&prepare);
    check(peer&&os64_page_node_insert(f.state,f.document->body,peer,NULL)==OS64_HTML_OK&&os64_page_node_set_value(f.state,peer,text,5000)==0,"second bounded clone source fits state cap individually");
    os64_page_t *model=os64_page_build(f.document,"https://fixture.test/",NULL,f.state);check(model!=NULL,"bounded clone source model");
    const os64_page_control_t *control=model?os64_page_control(model,os64_page_control_for(model,field)):NULL;
    const char *borrowed=NULL;size_t length=0;
    check(os64_page_node_value(f.state,field,&borrowed,&length)==0&&length==5000,"bounded clone source borrowed bytes");
    const char *published=control?control->value:NULL;size_t bytes=os64_page_state_bytes(f.state);
    uint64_t tree=os64_html_version(f.document),version=os64_page_state_version(f.state);int64_t status=0;
    check(!os64_page_node_clone(f.state,f.document->body,true,&status)&&status==OS64_HTML_NO_MEMORY,"deep current-state clone refuses state cap");
    const char *after=NULL;size_t after_length=0;
    check(os64_page_node_value(f.state,field,&after,&after_length)==0&&after==borrowed&&after_length==length&&!memcmp(after,text,length),"state-cap clone refusal retains borrowed source value");
    check(control&&control->value==published&&control->value_len==length&&!memcmp(control->value,text,length),"state-cap clone refusal preserves model publication");
    check(bytes==os64_page_state_bytes(f.state)&&tree==os64_html_version(f.document)&&version==os64_page_state_version(f.state),"state-cap clone refusal preserves accounting and revisions");
    os64_page_free(model);fixture_free(&f);
}
static DomFixture clone_sweep_fixture(os64_page_t **model)
{
    DomFixture f=fixture_new(NULL);if(!f.binding)return f;
    os64_html_node_t *field=native_id(f.document->document,"field"),*tick=native_id(f.document->document,"check");
    check(os64_page_node_set_value(f.state,field,"clone-source",12)==0&&os64_page_node_set_checked(f.state,tick,true)==0,"clone allocation fixture dirty values");
    int64_t status=0;
    os64_html_node_t *area=os64_html_create_element(f.document,OS64_HTML_NS_HTML,"textarea",&status);
    os64_html_node_t *text=os64_html_create_text(f.document,"uncached-area",13,&status);
    check(area&&text&&os64_page_node_insert(f.state,area,text,NULL)==OS64_HTML_OK&&os64_page_node_insert(f.state,f.document->body,area,NULL)==OS64_HTML_OK,"clone sweep clean textarea before first getter");
    *model=os64_page_build(f.document,"https://fixture.test/",NULL,f.state);check(*model!=NULL,"clone allocation fixture live model");
    return f;
}
static void dom_clone_allocation_cases(void)
{
    os64_page_t *model=NULL;DomFixture baseline=clone_sweep_fixture(&model);
    size_t start=attempts;int64_t status=0;
    os64_html_node_t *copy=os64_page_node_clone(baseline.state,baseline.document->body,true,&status);
    size_t count=attempts-start;check(copy&&status==OS64_HTML_OK&&count>0,"measure native clone allocation points");
    printf("Native clone allocation points: %zu\n",count);os64_page_free(model);fixture_free(&baseline);
    for(size_t cut=1;cut<=count;cut++){
        model=NULL;DomFixture f=clone_sweep_fixture(&model);if(!fixture_ready(&f))break;
        os64_html_node_t *field=native_id(f.document->document,"field"),*tick=native_id(f.document->document,"check");
        const char *borrowed=NULL;size_t length=0;
        check(os64_page_node_value(f.state,field,&borrowed,&length)==0&&length==12,"clone sweep source bytes");
        const os64_page_control_t *control=model?os64_page_control(model,os64_page_control_for(model,field)):NULL;
        const char *published=control?control->value:NULL;size_t bytes=os64_page_state_bytes(f.state);
        char *before=native_serialized(f.document->document);
        uint64_t tree=os64_html_version(f.document),version=os64_page_state_version(f.state);
        size_t target=attempts+cut;fail_at=target;copy=os64_page_node_clone(f.state,f.document->body,true,&status);fail_at=0;dom_sweeps++;
        check(attempts>=target,"native clone injection point reached");
        check(!copy&&status==OS64_HTML_NO_MEMORY,"each native clone allocation failure refuses");
        const char *after=NULL;size_t after_length=0;bool checked=false;
        check(os64_page_node_value(f.state,field,&after,&after_length)==0&&after==borrowed&&after_length==length&&!memcmp(after,"clone-source",length)&&os64_page_node_checked(f.state,tick,&checked)==0&&checked,"clone allocation failure preserves source control state");
        check(control&&control->value==published&&control->value_len==length&&!memcmp(control->value,"clone-source",length),"clone allocation failure preserves publication and borrowed pointer");
        char *after_tree=native_serialized(f.document->document);
        check(!strcmp(before,after_tree)&&bytes==os64_page_state_bytes(f.state)&&tree==os64_html_version(f.document)&&version==os64_page_state_version(f.state),"clone allocation failure preserves source tree/state accounting/revisions");
        free(before);free(after_tree);os64_page_free(model);fixture_free(&f);
    }
}
static DomFixture clone_character_fixture(os64_page_t **model,os64_html_node_t **copy)
{
    DomFixture f=fixture_base(0,65536,0);int64_t status=0;
    os64_html_node_t *area=os64_html_create_element(f.document,OS64_HTML_NS_HTML,"textarea",&status);
    os64_html_node_t *text=os64_html_create_text(f.document,"clean-area",10,&status);
    check(area&&text&&os64_page_node_insert(f.state,area,text,NULL)==OS64_HTML_OK,"native clean textarea CharacterData fixture");
    *copy=os64_page_node_clone(f.state,area,true,&status);
    check(*copy&&status==OS64_HTML_OK&&os64_page_node_insert(f.state,f.document->body,*copy,NULL)==OS64_HTML_OK,"attach clean cloned textarea for model publication");
    *model=os64_page_build(f.document,"https://fixture.test/",NULL,f.state);check(*model!=NULL,"clean clone CharacterData model");
    return f;
}
static void dom_clone_character_data_cases(void)
{
    os64_page_t *model=NULL;os64_html_node_t *copy=NULL;
    DomFixture f=clone_character_fixture(&model,&copy);uint64_t tree=os64_html_version(f.document),version=os64_page_state_version(f.state);
    size_t before=attempts;
    check(os64_page_node_set_text(f.state,copy->first_child,"clean-area",10)==OS64_HTML_OK&&tree==os64_html_version(f.document)&&version==os64_page_state_version(f.state)&&attempts==before,"equal CharacterData no-op preserves clean clone without allocation");
    size_t start=attempts;
    check(os64_page_node_set_text(f.state,copy->first_child,"changed-child",13)==OS64_HTML_OK,"CharacterData mutation succeeds");
    size_t count=attempts-start;
    check(clone_value_is(f.state,copy,"changed-child"),"CharacterData success resets clean current value");printf("Clean clone CharacterData allocation points: %zu\n",count);os64_page_free(model);fixture_free(&f);
    for(size_t cut=1;cut<=count;cut++){
        model=NULL;copy=NULL;f=clone_character_fixture(&model,&copy);
        const os64_page_control_t *control=model?os64_page_control(model,os64_page_control_for(model,copy)):NULL;
        const char *borrowed=NULL;size_t length=0;
        check(os64_page_node_value(f.state,copy,&borrowed,&length)==0&&length==10,"clean clone CharacterData borrowed value");
        const char *published=control?control->value:NULL;size_t bytes=os64_page_state_bytes(f.state);
        tree=os64_html_version(f.document);version=os64_page_state_version(f.state);
        size_t target=attempts+cut;fail_at=target;int64_t status=os64_page_node_set_text(f.state,copy->first_child,"changed-child",13);fail_at=0;dom_sweeps++;
        check(attempts>=target&&status==OS64_HTML_NO_MEMORY,"CharacterData reached state/HTML allocation failure refuses");
        const char *after=NULL;size_t after_length=0;
        check(os64_page_node_value(f.state,copy,&after,&after_length)==0&&after==borrowed&&after_length==length&&!memcmp(after,"clean-area",length),"CharacterData refusal retains borrowed clean clone bytes");
        check(control&&control->value==published&&control->value_len==length&&!memcmp(control->value,"clean-area",length),"CharacterData refusal preserves model publication");
        check(!strcmp(copy->first_child->text,"clean-area")&&bytes==os64_page_state_bytes(f.state)&&tree==os64_html_version(f.document)&&version==os64_page_state_version(f.state),"CharacterData refusal preserves native text/accounting/revisions");
        os64_page_free(model);fixture_free(&f);
    }
    f=fixture_new(NULL);if(!fixture_ready(&f))return;int64_t status=0;
    os64_html_node_t *area=os64_html_create_element(f.document,OS64_HTML_NS_HTML,"textarea",&status);
    os64_html_node_t *text=os64_html_create_text(f.document,"clean-area",10,&status);
    check(area&&text&&os64_page_node_insert(f.state,area,text,NULL)==OS64_HTML_OK,"direct Comment notification source");
    copy=os64_page_node_clone(f.state,area,false,&status);
    os64_html_node_t *comment=os64_html_create_comment(f.document,"before",6,&status);
    /* Raw HTML changes cannot reconstruct control history. The following
     * state-aware CharacterData call must reconcile its direct parent. */
    check(copy&&comment&&os64_html_insert(f.document,copy,comment,NULL)==OS64_HTML_OK,"raw native Comment setup");
    check(os64_page_node_set_text(f.state,comment,"after",5)==OS64_HTML_OK&&clone_value_is(f.state,copy,""),"direct Comment CharacterData resets clean override to direct Text default");
    copy=os64_page_node_clone(f.state,area,false,&status);
    comment=os64_html_create_comment(f.document,"before",6,&status);
    check(copy&&comment&&os64_html_insert(f.document,copy,comment,NULL)==OS64_HTML_OK&&os64_html_set_attr(f.document,copy,"id","comment-clone",13)==OS64_HTML_OK&&os64_html_insert(f.document,f.document->body,copy,NULL)==OS64_HTML_OK,"binding Comment history fixture");
    dom_script(&f,"const c=document.getElementById('comment-clone');c.firstChild.data='after';if(c.value!=='')throw Error('Comment mutation must notify direct textarea parent');","clone-comment-binding-history.js");
    fixture_free(&f);
}
