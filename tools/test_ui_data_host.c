/* Public model contracts, independent of fonts and windows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include "os64/ui_data.h"
#ifndef E3_CORE_EMBED
static unsigned long checks, allocations;
static int failures;
static const char *current;
static long deny_countdown=-1;
static size_t reserve_next;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %s:%d [%s] %s\n",__FILE__,__LINE__,current,#x); } } while (0)
void *os64_malloc(size_t n) {
    ++allocations;
    if (deny_countdown==0) { deny_countdown=-1; return NULL; }
    if (deny_countdown>0) --deny_countdown;
    size_t reserve=reserve_next; reserve_next=0;
    return malloc((n?n:1)+reserve);
}
void os64_free(void *p) { free(p); }
size_t os64_malloc_size(const void *p) { return p?malloc_usable_size((void *)p):0; }
#endif
#define TXT(s) (os64_ui_data_text_t){s,sizeof(s)-1}
static const uint64_t fixture_ids[]={101,307,19,811,53};
static os64_ui_data_column_t core_columns[]={
    {.id=1,.title={"Name",4},.type=OS64_UI_DATA_TEXT_KEY,.width=260,.required=true,.search=true,.sortable=true},
    {.id=2,.title={"Size",4},.type=OS64_UI_DATA_UNSIGNED,.width=100,.right=true,.sortable=true},
    {.id=3,.title={"Modified",8},.type=OS64_UI_DATA_SIGNED,.width=180,.sortable=true}
};
static os64_ui_data_t *core_fixture(uint64_t domain)
{
    char names[5][8]={"Alpha","Bravo","Charlie","Delta","Echo"};
    uint64_t sizes[]={UINT64_MAX,10,2,0,2};
    int64_t dates[]={INT64_MIN,INT64_MAX,-1,0,0};
    os64_ui_data_cell_t cells[5][3]={0}; os64_ui_data_row_t rows[5]={0};
    for (size_t i=0;i<5;++i) {
        os64_ui_data_text_t name={names[i],strlen(names[i])};
        cells[i][0]=(os64_ui_data_cell_t){name,{.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text=name}};
        cells[i][1]=(os64_ui_data_cell_t){TXT("formatted"),{.present=true,.type=OS64_UI_DATA_UNSIGNED,.value.u=sizes[i]}};
        cells[i][2]=(os64_ui_data_cell_t){TXT("date"),{.present=i!=4,.type=OS64_UI_DATA_SIGNED,.value.i=dates[i]}};
        rows[i]=(os64_ui_data_row_t){fixture_ids[i],i==3?0:1,name,TXT("folder"),cells[i]};
    }
    os64_ui_data_t *d=NULL; CHECK(os64_ui_data_create(domain,core_columns,3,rows,5,&d)==OS64_UI_OK);
    memset(names,'X',sizeof(names));
    CHECK(d && !strcmp(os64_ui_data_row(d,101)->search.bytes,"Alpha"));
    return d;
}
static void tuple(os64_ui_selection_t *s,const uint64_t *ids,size_t count,uint64_t focus,uint64_t anchor)
{
    CHECK(os64_ui_selection_count(s)==count);
    CHECK(os64_ui_selection_focus(s)==focus); CHECK(os64_ui_selection_anchor(s)==anchor);
    uint64_t revision=os64_ui_selection_revision(s); size_t cursor=0;
    for (size_t i=0;i<count;++i) { uint64_t id=0; CHECK(os64_ui_selection_next(s,revision,&cursor,&id)==OS64_UI_OK && id==ids[i]); }
    uint64_t id=77; CHECK(os64_ui_selection_next(s,revision,&cursor,&id)==OS64_UI_EMPTY && !id);
}
static void core_selection(void)
{
    current="selection tuples";
    os64_ui_data_t *d=core_fixture(7); os64_ui_data_projection_t *p=NULL;
    CHECK(os64_ui_data_projection_identity(d,&p)==OS64_UI_OK);
    os64_ui_selection_t s={0}, independent={0};
    CHECK(os64_ui_selection_bind(&s,p,OS64_UI_DATA_RESET)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&independent,p,OS64_UI_DATA_RESET)==OS64_UI_OK);
#define CMD(c,id) CHECK(os64_ui_selection_command(&s,OS64_UI_SELECTION_##c,id)==OS64_UI_OK)
    CMD(CHOOSE,307); CMD(TOGGLE,811); tuple(&s,(uint64_t[]){307,811},2,811,811);
    CMD(RANGE,307); tuple(&s,(uint64_t[]){307,19,811},3,307,811);
    CMD(RANGE,19); tuple(&s,(uint64_t[]){19,811},2,19,811);
    CMD(CHOOSE,307); CMD(FOCUS,19); tuple(&s,(uint64_t[]){307},1,19,307);
    CMD(TOGGLE,19); tuple(&s,(uint64_t[]){307,19},2,19,19);
    CMD(TOGGLE,19); tuple(&s,(uint64_t[]){307},1,19,19);
    CMD(CHOOSE,307); CMD(UNION,811); CMD(UNION,19); tuple(&s,(uint64_t[]){307,19,811},3,19,307);
    CMD(CHOOSE,307); CMD(TOGGLE,811);
    os64_ui_data_projection_t *reversed=NULL,*filtered=NULL;
    CHECK(os64_ui_data_projection_create(d,(uint64_t[]){53,811,19,307,101},5,&reversed)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&s,reversed,OS64_UI_DATA_PRESERVE)==OS64_UI_OK);
    tuple(&s,(uint64_t[]){811,307},2,811,811);
    CHECK(os64_ui_data_projection_create(d,(uint64_t[]){53,19,307,101},4,&filtered)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&s,filtered,OS64_UI_DATA_PRESERVE)==OS64_UI_OK);
    tuple(&s,(uint64_t[]){307},1,19,19);
    CHECK(os64_ui_selection_bind(&s,reversed,OS64_UI_DATA_PRESERVE)==OS64_UI_OK);
    tuple(&s,(uint64_t[]){307},1,19,19);
    CHECK(os64_ui_selection_bind(&s,p,OS64_UI_DATA_RESET)==OS64_UI_OK);
    CMD(CHOOSE,307); CMD(FOCUS,811);
    os64_ui_data_projection_t *removed=NULL;
    CHECK(os64_ui_data_projection_create(d,(uint64_t[]){101,19,53},3,&removed)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&s,removed,OS64_UI_DATA_PRESERVE)==OS64_UI_OK);
    tuple(&s,NULL,0,53,53);
    os64_ui_data_t *newdomain=core_fixture(8); os64_ui_data_projection_t *newp=NULL,*empty=NULL;
    CHECK(os64_ui_data_projection_identity(newdomain,&newp)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&s,newp,OS64_UI_DATA_PRESERVE)==OS64_UI_OK); tuple(&s,NULL,0,0,0);
    CMD(CHOOSE,307);
    CHECK(os64_ui_data_projection_create(newdomain,NULL,0,&empty)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&s,empty,OS64_UI_DATA_PRESERVE)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&s,newp,OS64_UI_DATA_PRESERVE)==OS64_UI_OK); tuple(&s,NULL,0,0,0);
    tuple(&independent,NULL,0,0,0);
    size_t cursor=0; uint64_t id=3;
    CHECK(os64_ui_selection_next(&s,os64_ui_selection_revision(&s)-1,&cursor,&id)==OS64_UI_STALE && !id);
    uint64_t rev=os64_ui_selection_revision(&s);
    deny_countdown=0;
    CHECK(os64_ui_selection_bind(&s,p,OS64_UI_DATA_RESET)==OS64_UI_NO_MEMORY);
    CHECK(os64_ui_selection_revision(&s)==rev); tuple(&s,NULL,0,0,0);
    os64_ui_selection_dispose(&s); os64_ui_selection_dispose(&independent);
    os64_ui_data_projection_release(p); os64_ui_data_projection_release(reversed); os64_ui_data_projection_release(filtered);
    os64_ui_data_projection_release(removed); os64_ui_data_projection_release(empty); os64_ui_data_projection_release(newp);
    os64_ui_data_release(d); os64_ui_data_release(newdomain);
#undef CMD
}
static void core_sorts(void)
{
    current="typed group/missing/tie sorts";
    os64_ui_data_t *d=core_fixture(1); os64_ui_data_projection_t *p=NULL,*q=NULL;
    CHECK(os64_ui_data_projection_create(d,(uint64_t[]){53,811,19,307,101},5,&p)==OS64_UI_OK);
    CHECK(os64_ui_data_projection_sort(p,(os64_ui_data_sort_t){2,false},&q)==OS64_UI_OK);
    uint64_t expected[]={811,19,53,307,101};
    for (size_t i=0;i<5;++i) CHECK(os64_ui_data_projection_id(q,i)==expected[i]);
    os64_ui_data_projection_release(q);
    CHECK(os64_ui_data_projection_sort(p,(os64_ui_data_sort_t){2,true},&q)==OS64_UI_OK);
    uint64_t desc[]={811,101,307,19,53};
    for (size_t i=0;i<5;++i) CHECK(os64_ui_data_projection_id(q,i)==desc[i]);
    os64_ui_data_projection_release(q);
    for (unsigned reverse=0;reverse<2;++reverse) {
        CHECK(os64_ui_data_projection_sort(p,(os64_ui_data_sort_t){3,reverse},&q)==OS64_UI_OK);
        CHECK(os64_ui_data_projection_id(q,0)==811 && os64_ui_data_projection_id(q,4)==53);
        CHECK(os64_ui_data_projection_id(q,1)==(reverse?307:101));
        os64_ui_data_projection_release(q);
    }
    for (long fail=0;fail<2;++fail) {
        deny_countdown=fail; q=(void *)1;
        CHECK(os64_ui_data_projection_sort(p,(os64_ui_data_sort_t){1,false},&q)==OS64_UI_NO_MEMORY && !q);
    }
    deny_countdown=-1;
    /* Equal folded keys must use snapshot order, even from reverse input. */
    os64_ui_data_cell_t cells[3]={{TXT("lower"),{.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text={"A",1}}},
        {TXT("upper"),{.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text={"a",1}}},
        {TXT("utf8"),{.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text={"\xc3\xa9",2}}}};
    os64_ui_data_row_t rows[3]={{7,0,TXT("A"),TXT(""),&cells[0]},{9,0,TXT("a"),TXT(""),&cells[1]},{11,0,TXT("\xc3\xa9"),TXT(""),&cells[2]}};
    os64_ui_data_t *tie=NULL; os64_ui_data_projection_t *tp=NULL;
    CHECK(os64_ui_data_create(2,core_columns,1,rows,3,&tie)==OS64_UI_OK);
    CHECK(os64_ui_data_projection_create(tie,(uint64_t[]){11,9,7},3,&tp)==OS64_UI_OK);
    CHECK(os64_ui_data_projection_sort(tp,(os64_ui_data_sort_t){1,true},&q)==OS64_UI_OK);
    CHECK(os64_ui_data_projection_id(q,0)==11 && os64_ui_data_projection_id(q,1)==7 && os64_ui_data_projection_id(q,2)==9);
    os64_ui_data_projection_release(q); os64_ui_data_projection_release(tp); os64_ui_data_release(tie);
    os64_ui_data_projection_release(p); os64_ui_data_release(d);
}
static void core_refusals(void)
{
    current="invalid and limit inputs / allocation sweep";
    os64_ui_data_cell_t cell={TXT("display"),{.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text={"key",3}}};
    os64_ui_data_row_t rows[2]={{7,0,TXT("search"),TXT("file"),&cell},{7,0,TXT("search"),TXT("file"),&cell}};
    os64_ui_data_t *d=(void *)1;
    CHECK(os64_ui_data_create(1,core_columns,1,rows,100001,&d)==OS64_UI_LIMIT && !d);
    CHECK(os64_ui_data_create(0,core_columns,1,rows,1,&d)==OS64_UI_INVALID && !d);
    CHECK(os64_ui_data_create(1,core_columns,1,rows,2,&d)==OS64_UI_DUPLICATE && !d);
    rows[0].search=(os64_ui_data_text_t){"a\0b",3};
    CHECK(os64_ui_data_create(1,core_columns,1,rows,1,&d)==OS64_UI_INVALID && !d);
    rows[0].search=TXT("line\n"); CHECK(os64_ui_data_create(1,core_columns,1,rows,1,&d)==OS64_UI_INVALID && !d);
    rows[0].search=(os64_ui_data_text_t){"x",SIZE_MAX}; CHECK(os64_ui_data_create(1,core_columns,1,rows,1,&d)==OS64_UI_LIMIT && !d);
    rows[0].search=TXT("search"); cell.key.type=OS64_UI_DATA_UNSIGNED;
    CHECK(os64_ui_data_create(1,core_columns,1,rows,1,&d)==OS64_UI_INVALID && !d); cell.key.type=OS64_UI_DATA_TEXT_KEY;
    deny_countdown=0; CHECK(os64_ui_data_create(1,core_columns,1,rows,1,&d)==OS64_UI_NO_MEMORY && !d);
    CHECK(os64_ui_data_create(1,core_columns,1,rows,1,&d)==OS64_UI_OK);
    os64_ui_data_projection_t *p=(void *)1;
    CHECK(os64_ui_data_projection_create(d,(uint64_t[]){7},100001,&p)==OS64_UI_LIMIT && !p);
    deny_countdown=0; CHECK(os64_ui_data_projection_identity(d,&p)==OS64_UI_NO_MEMORY && !p);
    CHECK(os64_ui_data_projection_create(d,(uint64_t[]){8},1,&p)==OS64_UI_INVALID && !p);
    os64_ui_data_release(d);
    d=core_fixture(1);
    CHECK(os64_ui_data_projection_create(d,(uint64_t[]){101,101},2,&p)==OS64_UI_DUPLICATE && !p);
    os64_ui_data_release(d);
}
static void core_limits(void)
{
    current="100000 rows and snapshot cap";
    size_t n=100000;
    os64_ui_data_cell_t cell={TXT("2"),{.present=true,.type=OS64_UI_DATA_TEXT_KEY,.value.text={"2",1}}};
    os64_ui_data_row_t *rows=malloc(n*sizeof(*rows)); CHECK(rows!=NULL); if (!rows) return;
    for (size_t i=0;i<n;++i) rows[i]=(os64_ui_data_row_t){UINT64_C(9000000000)+i*17,1,TXT("item"),TXT("file"),&cell};
    os64_ui_data_t *d=NULL; os64_ui_data_projection_t *p=NULL,*q=NULL;
    CHECK(os64_ui_data_create(10,core_columns,1,rows,n,&d)==OS64_UI_OK);
    CHECK(os64_ui_data_projection_identity(d,&p)==OS64_UI_OK);
    CHECK(os64_ui_data_projection_sort(p,(os64_ui_data_sort_t){1,true},&q)==OS64_UI_OK);
    for (size_t i=0;i<n;i+=997) { size_t at=SIZE_MAX; CHECK(os64_ui_data_projection_position(q,rows[i].id,&at) && at==i); }
    printf("core maximum: rows=%zu snapshot=%zu projection=%zu\n",n,os64_ui_data_bytes(d),os64_ui_data_projection_bytes(q));
    os64_ui_selection_t s={0}; CHECK(os64_ui_selection_bind(&s,q,OS64_UI_DATA_RESET)==OS64_UI_OK);
    CHECK(os64_ui_selection_command(&s,OS64_UI_SELECTION_ALL,0)==OS64_UI_OK);
    CHECK(os64_ui_selection_count(&s)==n);
    os64_ui_selection_dispose(&s); os64_ui_data_projection_release(q); os64_ui_data_projection_release(p); os64_ui_data_release(d);
    char *longtext=malloc(4096); memset(longtext,'x',4096);
    cell.display=(os64_ui_data_text_t){longtext,4096};
    CHECK(os64_ui_data_create(10,core_columns,1,rows,n,&d)==OS64_UI_LIMIT && !d);
    free(longtext); free(rows);
}
#ifndef E3_CORE_EMBED
static void reused_capacity(void)
{
    current="large recycled projection capacity is charged once";
    os64_ui_data_t *d=core_fixture(1); os64_ui_data_projection_t *p=NULL,*q=NULL;
    reserve_next=12u*1024u*1024u;
    CHECK(os64_ui_data_projection_identity(d,&p)==OS64_UI_OK);
    CHECK(os64_ui_data_projection_bytes(p)>12u*1024u*1024u);
    CHECK(os64_ui_data_projection_sort(p,(os64_ui_data_sort_t){2,false},&q)==OS64_UI_OK);
    os64_ui_selection_t s={0}; CHECK(os64_ui_selection_bind(&s,p,OS64_UI_DATA_RESET)==OS64_UI_OK);
    CHECK(os64_ui_selection_bind(&s,p,OS64_UI_DATA_PRESERVE)==OS64_UI_OK);
    os64_ui_selection_dispose(&s); os64_ui_data_projection_release(p); os64_ui_data_projection_release(q);
    reserve_next=OS64_UI_DATA_VIEW_BYTES;
    CHECK(os64_ui_data_projection_identity(d,&p)==OS64_UI_LIMIT && !p);
    os64_ui_data_release(d);
}
#endif
static void core_suite(void) { core_refusals(); core_sorts(); core_selection(); core_limits(); }

#ifndef E3_CORE_EMBED
int main(void) { core_suite(); reused_capacity(); printf("data core: %lu checks, %d failures\n",checks,failures); return failures?1:0; }
#endif
