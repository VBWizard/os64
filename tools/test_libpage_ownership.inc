/* Cross-document state is tested with old source models still borrowing its
 * value bytes, plus refusal at each allocation before the native commit. */
typedef struct {
    os64_html_document_t *a, *b;
    os64_html_group_t *html;
    os64_page_state_t *sa, *sb;
    os64_page_state_group_t *states;
} OwnershipFixture;

static OwnershipFixture ownership_fixture(size_t limit)
{
    OwnershipFixture f = {0};
    f.a = parse("<section id=move><input id=i value=markup><input id=r type=radio name=g checked>"
                "<textarea id=t>initial</textarea><select id=s><option id=o1>one<option id=o2>two</select></section>", "utf-8");
    f.b = parse("<input id=peer type=radio name=g checked><div id=destination></div>", "utf-8");
    f.html = os64_html_group_create(4u * 1024u * 1024u);
    rebuild_expect("ownership: native admission", f.html && f.a && f.b &&
        !os64_html_group_add(f.html, f.a, 1) && !os64_html_group_add(f.html, f.b, 2));
    f.sa = os64_page_state_create(f.a, 0);
    f.sb = os64_page_state_create(f.b, 0);
    f.states = os64_page_state_group_create(limit);
    rebuild_expect("ownership: state admission", f.sa && f.sb && f.states &&
        os64_page_state_group_add(f.states, f.sa) && os64_page_state_group_add(f.states, f.sb));
    rebuild_expect("ownership: dirty source input", !os64_page_node_set_value(f.sa, rebuild_id(f.a,"i"), "typed", 5));
    rebuild_expect("ownership: dirty source textarea", !os64_page_node_set_value(f.sa, rebuild_id(f.a,"t"), "edited", 6));
    rebuild_expect("ownership: source selection", !os64_page_node_set_selection(f.sa, rebuild_id(f.a,"i"), 1, 4, OS64_PAGE_DIRECTION_BACKWARD));
    rebuild_expect("ownership: source validity", !os64_page_node_set_custom_validity(f.sa, rebuild_id(f.a,"i"), "explain", 7));
    return f;
}
static void ownership_free(OwnershipFixture *f)
{
    os64_page_state_group_free(f->states);
    os64_html_group_free(f->html);
    rebuild_expect("ownership: group teardown balances allocations", live == 0);
}
static void ownership_state_move(void)
{
    OwnershipFixture f = ownership_fixture(2u * 1024u * 1024u);
    os64_html_node_t *root = rebuild_id(f.a,"move"), *input = rebuild_id(f.a,"i"),
        *textarea = rebuild_id(f.a,"t"), *radio = rebuild_id(f.a,"r"), *peer = rebuild_id(f.b,"peer");
    os64_page_t *old = os64_page_build(f.a, kPage, NULL, f.sa);
    rebuild_expect("ownership: retained source model", old && !os64_page_incomplete(old));
    const char *borrowed = NULL; size_t len = 0;
    rebuild_expect("ownership: borrowed source value", !os64_page_node_value(f.sa,input,&borrowed,&len) && len == 5);
    rebuild_expect("ownership: implicit state migration", !os64_page_node_insert(f.sb,f.b->body,root,NULL));
    rebuild_node_value("ownership: typed value follows", f.sb,input,"typed");
    rebuild_node_value("ownership: textarea value follows", f.sb,textarea,"edited");
    uint32_t start = 0, end = 0; os64_page_direction_t direction;
    rebuild_expect("ownership: selection follows", !os64_page_node_selection(f.sb,input,&start,&end,&direction) &&
        start == 1 && end == 4 && direction == OS64_PAGE_DIRECTION_BACKWARD);
    const char *custom = NULL;
    rebuild_expect("ownership: custom validity follows", !os64_page_node_custom_validity(f.sb,input,&custom,&len) &&
        len == 7 && !memcmp(custom,"explain",7));
    bool on = false;
    rebuild_expect("ownership: incoming radio wins", !os64_page_node_checked(f.sb,radio,&on) && on);
    rebuild_expect("ownership: destination radio cleared", !os64_page_node_checked(f.sb,peer,&on) && !on);
    rebuild_expect("ownership: destination changes moved value", !os64_page_node_set_value(f.sb,input,"replacement",11));
    rebuild_expect("ownership: source snapshot bytes survive replacement", !memcmp(borrowed,"typed",6));
    size_t retained = os64_page_state_group_bytes(f.states);
    os64_page_free(old);
    rebuild_expect("ownership: source model release reclaims shared value", os64_page_state_group_bytes(f.states) < retained);
    int64_t why = 0;
    os64_html_node_t *copy = os64_page_node_import(f.sa,f.sb,input,true,&why);
    rebuild_expect("ownership: import creates destination identity", copy && !why && copy != input &&
        os64_html_node_creation_realm(copy) == 1 && os64_html_node_document(copy) == f.a->document);
    rebuild_node_value("ownership: import copies dirty value", f.sa,copy,"replacement");
    rebuild_expect("ownership: import does not copy custom validity", !os64_page_node_custom_validity(f.sa,copy,&custom,&len) && !len);
    rebuild_expect("ownership: explicit readoption", !os64_page_node_adopt(f.sa,f.a->document,root) && !root->parent);
    rebuild_node_value("ownership: readopted value", f.sa,input,"replacement");
    size_t stable = 0;
    for (size_t round = 0; round < 25; round++) {
        rebuild_expect("ownership: repeated state exchange", !os64_page_node_adopt(f.sb,f.b->document,root) &&
            !os64_page_node_adopt(f.sa,f.a->document,root));
        if (!round) stable = os64_page_state_group_bytes(f.states);
        rebuild_expect("ownership: repeated moves keep charges stable", stable == os64_page_state_group_bytes(f.states));
    }
    ownership_free(&f);
}
static void ownership_failure_sweep(void)
{
    size_t successful_allocations = 0;
    OwnershipFixture probe = ownership_fixture(2u * 1024u * 1024u);
    os64_html_node_t *root = rebuild_id(probe.a,"move");
    size_t before = allocations;
    rebuild_expect("ownership: allocation probe", !os64_page_node_insert(probe.sb,probe.b->body,root,NULL));
    successful_allocations = allocations - before;
    ownership_free(&probe);
    rebuild_expect("ownership: migration has fallible preparation", successful_allocations > 0);
    for (size_t cut = 1; cut <= successful_allocations; cut++) {
        OwnershipFixture f = ownership_fixture(2u * 1024u * 1024u);
        root = rebuild_id(f.a,"move");
        os64_html_node_t *input = rebuild_id(f.a,"i");
        size_t bytes = os64_page_state_group_bytes(f.states), html = os64_html_group_bytes(f.html), heap = live;
        uint64_t av = os64_html_version(f.a), bv = os64_html_version(f.b),
            as = os64_page_state_version(f.sa), bs = os64_page_state_version(f.sb);
        fail_at = allocations + cut;
        int64_t status = os64_page_node_insert(f.sb,f.b->body,root,NULL);
        fail_at = 0;
        rebuild_expect("ownership: allocation refusal before commit", status == OS64_HTML_NO_MEMORY);
        rebuild_expect("ownership: both trees and revisions unchanged", root->parent == f.a->body &&
            os64_html_node_document(input) == f.a->document && av == os64_html_version(f.a) &&
            bv == os64_html_version(f.b) && as == os64_page_state_version(f.sa) && bs == os64_page_state_version(f.sb));
        rebuild_expect("ownership: refusal restores both ledgers and holds", bytes == os64_page_state_group_bytes(f.states) &&
            html == os64_html_group_bytes(f.html) && heap == live);
        rebuild_node_value("ownership: refusal preserves dirty input",f.sa,input,"typed");
        rebuild_expect("ownership: refusal retry succeeds", !os64_page_node_insert(f.sb,f.b->body,root,NULL));
        ownership_free(&f);
    }
    printf("ownership migration: %zu allocation refusal cuts\n", successful_allocations);
}
static void ownership_aggregate_cap(void)
{
    OwnershipFixture f = ownership_fixture(64u * 1024u);
    os64_html_node_t *root = rebuild_id(f.a,"move"), *input = rebuild_id(f.a,"i");
    os64_page_t *old = os64_page_build(f.a,kPage,NULL,f.sa);
    rebuild_expect("ownership cap: model and adoption", old && !os64_page_node_insert(f.sb,f.b->body,root,NULL));
    size_t start = os64_page_state_group_bytes(f.states), turns = 0;
    char value[4096]; memset(value,'x',sizeof(value));
    int64_t status = 0;
    for (; turns < 1000; turns++) {
        value[0] = (char)('a' + turns % 26);
        status = os64_page_node_set_value(f.sb,input,value,sizeof(value));
        if (status) break;
    }
    size_t stalled = os64_page_state_group_bytes(f.states);
    rebuild_expect("ownership cap: bounded shared retention refuses", turns > 0 && turns < 1000 && status != 0 &&
        stalled > start && os64_page_state_group_peak(f.states) <= 64u * 1024u);
    os64_page_free(old);
    size_t recovered = os64_page_state_group_bytes(f.states);
    rebuild_expect("ownership cap: release reclaims and retry succeeds", recovered < stalled &&
        !os64_page_node_set_value(f.sb,input,"recovered",9));
    printf("ownership state cap: %zu turns, %zu -> %zu -> %zu bytes; peak %zu\n",
        turns,start,stalled,recovered,os64_page_state_group_peak(f.states));
    ownership_free(&f);
}
static void ownership_source_form_repair(void)
{
    OwnershipFixture f = ownership_fixture(2u * 1024u * 1024u);
    os64_html_node_t *radio = rebuild_id(f.a,"r");
    os64_html_node_t *form = os64_html_create_element(f.a,OS64_HTML_NS_HTML,"form",NULL);
    os64_html_node_t *peer = os64_html_create_element(f.a,OS64_HTML_NS_HTML,"input",NULL);
    rebuild_expect("ownership source form: fixture", form && peer &&
        !os64_page_node_set_attr(f.sa,form,"id","external",8,false) &&
        !os64_page_node_insert(f.sa,f.a->body,form,NULL) &&
        !os64_page_node_set_attr(f.sa,radio,"form","external",8,false) &&
        !os64_page_node_set_attr(f.sa,peer,"type","radio",5,false) &&
        !os64_page_node_set_attr(f.sa,peer,"name","g",1,false) &&
        !os64_page_node_insert(f.sa,f.a->body,peer,NULL) &&
        !os64_page_node_set_checked(f.sa,peer,true));
    bool on = false;
    rebuild_expect("ownership source form: distinct groups start checked", !os64_page_node_checked(f.sa,radio,&on) && on);
    rebuild_expect("ownership source form: explicit adoption", !os64_page_node_adopt(f.sb,f.b->document,form));
    rebuild_expect("ownership source form: retargeted checked radio wins", !os64_page_node_checked(f.sa,radio,&on) && on);
    rebuild_expect("ownership source form: old source peer repaired", !os64_page_node_checked(f.sa,peer,&on) && !on);
    ownership_free(&f);
}
static void test_libpage_ownership(void)
{
    ownership_state_move();
    ownership_failure_sweep();
    ownership_aggregate_cap();
    ownership_source_form_repair();
}
