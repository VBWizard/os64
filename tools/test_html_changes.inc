/* Retained invalidations share mutation records with the listener, but loss
 * is recoverable for these readers and cannot discard listener delivery. */
static unsigned log_notifications;
static void log_notified(void *opaque, const os64_html_change_t *change)
{
    (void)opaque; (void)change; ++log_notifications;
}
static void t_change_log(void)
{
    size_t start_live = live;
    os64_html_document_t *d = parse("<body><div id=a><span id=b>old</span></div><p id=c></p>");
    HNode *a=id_in(d,"a"), *b=id_in(d,"b"), *c=id_in(d,"c");
    uint64_t version=os64_html_version(d);
    log_notifications=0;os64_html_observe(d,log_notified,NULL);
    check(os64_html_changes_bytes(d)==0,"log: no storage without readers");
    os64_html_reader_t *one=os64_html_reader_new(d,version), *two=os64_html_reader_new(d,version);
    check(one && two && !os64_html_reader_new(d,version+1),"log: independent readers, future refused");
    const os64_html_change_entry_t *entries=NULL;
    check(os64_html_changes(one,&entries)==0 && !entries,"log: empty current read");
    char name[]="data-x";
    check(os64_html_set_attr(d,a,name,"v",1)==0,"log: attribute change");
    name[5]='y';
    check(os64_html_set_attr(d,a,"data-x","v",1)==0 &&
          os64_html_set_text(d,b->first_child,"old",3)==0 &&
          os64_html_insert(d,a,b,NULL)==0 && os64_html_replace(d,a,b,b)==0,
          "log: same-value and same-place mutations succeed");
    check(log_notifications==7,"log: listener still receives no-op mutation records");
    check(os64_html_changes(one,&entries)==1 && entries && entries->target==a &&
          entries->kind==OS64_HTML_CHANGE_ATTR && !strcmp(entries->attr_name,"data-x") &&
          !strcmp(entries->attr_local,"data-x"),"log: copied attribute name, no no-op entries");
    check(os64_html_changes(two,&entries)==1,"log: second reader sees the same change");
    check(os64_html_changes(one,&entries)==0 && os64_html_changes_bytes(d)==0,
          "log: storage pruned after readers pass it");
    check(os64_html_set_ce(d,a,OS64_HTML_CE_CUSTOM)==0,"log: custom state change");
    check(os64_html_changes(one,&entries)==1 && entries->kind==OS64_HTML_CHANGE_STATE,
          "log: selector-visible custom state invalidates");
    (void)os64_html_changes(two,&entries);
    int64_t why=0;
    HNode *fragment=os64_html_create_fragment(d,&why);
    HNode *x=os64_html_create_element(d,OS64_HTML_NS_HTML,"i",&why), *y=os64_html_create_element(d,OS64_HTML_NS_HTML,"u",&why);
    check(fragment && x && y,"log: fragment fixture");
    check(!os64_html_insert(d,fragment,x,NULL) && !os64_html_insert(d,fragment,y,NULL),"log: fill fragment");
    (void)os64_html_changes(one,&entries); (void)os64_html_changes(two,&entries);
    check(!os64_html_insert(d,a,fragment,NULL) && !os64_html_insert(d,c,y,NULL),"log: drain then move a member");
    check(os64_html_changes(one,&entries)==4 && entries->target==fragment && entries->nremoved==2 &&
          entries->removed[0]==x && entries->removed[1]==y && entries->next->nadded==2 &&
          entries->next->added[0]==x && entries->next->added[1]==y,
          "log: fragment arrays preserve membership after sibling links change");
    (void)os64_html_changes(two,&entries);
    check(!os64_html_remove(d,b),"log: remove unheld subtree");
    check(os64_html_changes(one,&entries)==1 && entries->nremoved==1 &&
          entries->removed[0]==b && !strcmp(b->name,"span") && !strcmp(b->first_child->text,"old"),
          "log: removed subtree survives until readers pass");
    (void)os64_html_changes(two,&entries); (void)os64_html_changes(one,&entries);
    os64_html_reader_free(one); os64_html_reader_free(two);
    check(os64_html_changes_bytes(d)==0,"log: last reader releases storage");
    os64_html_document_free(d);
    check(live==start_live,"log: identities and payloads released");
}

static void t_change_log_loss(void)
{
    size_t start_live=live;
    os64_html_document_t *d=parse("<body><div id=a><span id=b></span></div>");
    HNode *a=id_in(d,"a"), *b=id_in(d,"b");
    os64_html_reader_t *fast=os64_html_reader_new(d,os64_html_version(d));
    os64_html_reader_t *slow=os64_html_reader_new(d,os64_html_version(d));
    const os64_html_change_entry_t *entries=NULL;
    check(!os64_html_set_attr(d,a,"x","a",1),"log loss: first change");
    size_t single=os64_html_changes_bytes(d);
    os64_html_changes_limit(d,single);
    check(os64_html_changes(fast,&entries)==1,"log loss: fast reader current");
    check(!os64_html_set_attr(d,a,"x","b",1) && os64_html_changes_bytes(d)<=single,
          "log loss: cap includes allocation overhead");
    check(os64_html_changes(fast,&entries)==1 && os64_html_changes(slow,&entries)==-1 && !entries,
          "log loss: only reader behind eviction reports gap");
    check(os64_html_changes(slow,&entries)==0,"log loss: gap acknowledges current version");
    os64_html_changes_limit(d,0);
    check(!os64_html_set_attr(d,a,"x","c",1) && os64_html_changes_bytes(d)==0 &&
          os64_html_changes(fast,&entries)==-1,"log loss: disabled retention is explicit loss");
    (void)os64_html_changes(slow,&entries);
    os64_html_changes_limit(d,65536);
    /* Remove allocates nothing for the tree: fail the log's allocation. */
    log_notifications=0;os64_html_observe(d,log_notified,NULL);
    fail_at=allocations+1;
    int64_t removed=os64_html_remove(d,b);
    fail_at=0;
    check(removed==OS64_HTML_OK && !d->refusal && log_notifications==1 &&
          os64_html_changes(fast,&entries)==-1,
          "log loss: OOM cannot undo or refuse the mutation");
    (void)os64_html_changes(slow,&entries);
    check(!os64_html_set_attr(d,a,"x","d",1) && os64_html_changes(fast,&entries)==1,
          "log loss: recover after OOM");
    (void)os64_html_changes(slow,&entries);
    os64_html_changes_limit(d,single);
    os64_html_attr_change_t pair[]={{.name="x",.value="e",.value_len=1},
                                    {.name="y",.value="f",.value_len=1}};
    uint64_t before=os64_html_version(d);
    check(!os64_html_set_attrs(d,a,pair,2) && os64_html_version(d)==before+1,
          "log loss: one atomic verb produces multiple records at one version");
    check(os64_html_changes(fast,&entries)==-1 && !entries &&
          os64_html_changes(slow,&entries)==-1 && !entries,
          "log loss: partial eviction of one version cannot look like a complete diff");
    os64_html_reader_free(fast); os64_html_reader_free(slow);
    os64_html_reader_t *late=os64_html_reader_new(d,1);
    check(late && os64_html_changes(late,&entries)==-1,"log loss: unrecorded history is not an empty diff");
    os64_html_reader_free(late); os64_html_document_free(d);
    check(live==start_live,"log loss: no retained allocation");
}

static void t_change_log_parser(void)
{
    size_t start_live=live;
    os64_html_options_t opt=os64_html_options_default();opt.charset="utf-8";
    os64_html_parser_t *p=os64_html_parser_new(&opt);
    feed_past_window(p);
    const char *start="<!doctype html><body><p>";
    check(!os64_html_parser_feed(p,start,strlen(start)),"log parser: initial chunk");
    os64_html_document_t *d=(os64_html_document_t *)os64_html_parser_document(p);
    os64_html_reader_t *r=os64_html_reader_new(d,os64_html_version(d));
    const char *next="hello</p><div class=x>world</div>";
    check(!os64_html_parser_feed(p,next,strlen(next)),"log parser: next chunk");
    const os64_html_change_entry_t *e=NULL;
    int64_t count=os64_html_changes(r,&e);bool text=false,children=false;
    for (const os64_html_change_entry_t *at=e;at;at=at->next) {
        text |= at->kind==OS64_HTML_CHANGE_TEXT;
        children |= at->kind==OS64_HTML_CHANGE_CHILDREN;
    }
    check(count>2 && text && children,"log parser: multiple entries within one feed version");
    os64_html_reader_free(r);
    d=os64_html_parser_finish(p);os64_html_document_free(d);
    check(live==start_live,"log parser: no retained allocation");
}
