/* Review probes: real libui, FreeType, Scribe planner and Scribe buffer.
 * GUI main is not invoked. File syscalls use a bounded in-memory fixture. */
#define main upstream_test_main
#include F4_C2_HOST_TEST
#undef main
#include F4_C2_SCRIBE
#include <stdarg.h>

static const unsigned char *c2_input;
static size_t c2_input_n,c2_pos,c2_output_n;
static unsigned char c2_output[4096];
void *os64_realloc(void *p,size_t n) { return realloc(p,n?n:1); }
int64_t os64_memory(os64_memory_t *m) { (void)m;return -1; }
int32_t os64_snprintf(char *s,size_t n,const char *f,...)
{ va_list a;va_start(a,f);int r=vsnprintf(s,n,f,a);va_end(a);return r; }
int64_t os64_stat(const char *path,os64_dirent_t *e)
{ (void)path;memset(e,0,sizeof(*e));e->size=c2_input_n;return 0; }
int64_t os64_open(const char *path,const char *mode)
{ (void)path;if (*mode=='r') {c2_pos=0;return 10;}c2_output_n=0;return 11; }
int64_t os64_read(int32_t fd,void *p,size_t n)
{ (void)fd;if(n>c2_input_n-c2_pos)n=c2_input_n-c2_pos;memcpy(p,c2_input+c2_pos,n);c2_pos+=n;return n; }
int64_t os64_write(int32_t fd,const void *p,size_t n)
{ (void)fd;if(n>sizeof(c2_output)-c2_output_n)return -1;memcpy(c2_output+c2_output_n,p,n);c2_output_n+=n;return n; }
int64_t os64_close(int32_t fd) { (void)fd;return 0; }
int64_t os64_sync(int32_t fd) { (void)fd;return 0; }
static void c2_hex(const char *s,size_t n)
{ for(size_t i=0;i<n;++i)printf("%02x",(unsigned char)s[i]); }
static void c2_key(os64_ui_widget_t *w,os64_ui_t *ui,char a)
{ os64_gui_event_t e={0};e.type=OS64_GUI_EVENT_KEY_DOWN;e.key.ascii=a;e.key.scancode=0x4d;w->cls->event(w,ui,&e); }
static void c2_delete(os64_ui_widget_t *w,os64_ui_t *ui)
{ c2_key(w,ui,27);c2_key(w,ui,'[');c2_key(w,ui,'3');c2_key(w,ui,'~'); }
static void c2_bind(os64_ui_t *ui,const char *dir,unsigned size)
{
    os64_font_set_t *set=outline_set(os64_ui_font_context(ui),dir,"DejaVuSans.ttf",size);
    if(!set||os64_ui_font_bind(ui,set)!=OS64_FONT_OK)exit(2);
    os64_font_set_release(set);
}
static void c2_load(sbuf_t *b,const char *s)
{
    char err[128];c2_input=(const unsigned char *)s;c2_input_n=strlen(s);
    if(!sbuf_init(b)||sbuf_load(b,"input",err,sizeof(err))!=0)exit(2);
}
static unsigned long c2_barrier_allocs;
static bool c2_deny_commit;
static os64_font_status_t c2_commit_barrier(void *user,void *plan)
{
    (void)user;(void)plan;c2_barrier_allocs=allocations;
    if(c2_deny_commit)deny_countdown=0;
    return OS64_FONT_OK;
}
static void c2_scribe_init(const char *dir)
{
    memset(&g,0,sizeof(g));ui_test_view_theme(&g.ui.theme);g.ui.theme.scroll_w=12;
    c2_load(&g.buf,"WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW\n");
    g.textbuf=sbuf_textbuf_template;g.textbuf.user=&g.buf;
    os64_ui_panel(&g.root);g.root.bounds=(os64_gui_rect_t){0,0,800,400};
    os64_ui_textview(&g.view,&g.textbuf,NULL,NULL,NULL);
    g.view.w.bounds=(os64_gui_rect_t){6,40,770,320};
    os64_ui_add_child(&g.root,&g.view.w);os64_ui_set_root(&g.ui,&g.root);
    c2_bind(&g.ui,dir,16);measure_document();
    os64_ui_font_planner(&g.ui,plan_font,commit_font,discard_font,NULL);
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    const char *mode=argv[1],*dir=argv[2];
    if(!strcmp(mode,"delete")) {
        for(int del=0;del<2;++del) {
            sbuf_t b;c2_load(&b,"caf\xc3\xa9\n");
            os64_ui_textbuf_t model=sbuf_textbuf_template;model.user=&b;
            os64_ui_t ui={0};ui_test_view_theme(&ui.theme);
            os64_ui_textview_t tv;os64_ui_textview(&tv,&model,NULL,NULL,NULL);
            tv.w.bounds=(os64_gui_rect_t){0,0,200,60};os64_ui_set_root(&ui,&tv.w);c2_bind(&ui,dir,16);
            tv.cur_col=del?3:5;
            if(del)c2_delete(&tv.w,&ui);else c2_key(&tv.w,&ui,'\b');
            printf("textview %s: bytes=",del?"Delete before e-acute":"Backspace after e-acute");c2_hex(b.lines[0].bytes,b.lines[0].len);
            printf(" cursor=%zu; expected bytes=636166 cursor=3\n",tv.cur_col);
            os64_ui_font_release(&ui);sbuf_free(&b);
        }
    } else if(!strcmp(mode,"view-oom")) {
        sbuf_t b;c2_load(&b,"caf\xc3\xa9\n");
        os64_ui_textbuf_t model=sbuf_textbuf_template;model.user=&b;
        os64_ui_t ui={0};ui_test_view_theme(&ui.theme);
        os64_ui_textview_t tv;os64_ui_textview(&tv,&model,NULL,NULL,NULL);
        tv.w.bounds=(os64_gui_rect_t){0,0,200,60};os64_ui_set_root(&ui,&tv.w);c2_bind(&ui,dir,16);
        tv.cur_col=3;deny_countdown=0;
        c2_key(&tv.w,&ui,27);c2_key(&tv.w,&ui,'[');c2_key(&tv.w,&ui,'C');deny_countdown=-1;
        printf("textview Right before e-acute after refused layout: cursor=%zu; legal neighboring offsets=3,5\n",tv.cur_col);
        os64_ui_font_release(&ui);sbuf_free(&b);
    } else if(!strcmp(mode,"field-oom")||!strcmp(mode,"field-adopt")) {
        os64_ui_t ui={0};ui_test_view_theme(&ui.theme);
        os64_ui_textfield_t tf;char buf[64];os64_ui_textfield(&tf,buf,sizeof(buf),NULL,NULL,NULL);
        tf.w.bounds=(os64_gui_rect_t){0,0,100,40};os64_ui_set_root(&ui,&tf.w);
        c2_bind(&ui,dir,!strcmp(mode,"field-oom")?16:8);
        if(!strcmp(mode,"field-oom")) {
            deny_countdown=0;os64_ui_textfield_set(&ui,&tf,"caf\xc3\xa9");deny_countdown=-1;
            deny_countdown=0;c2_key(&tf.w,&ui,'\b');deny_countdown=-1;
            printf("textfield Backspace after refused layout: bytes=");c2_hex(buf,tf.len);printf(" cursor=%zu; expected intact input or whole-cluster deletion\n",tf.cursor);
        } else {
            os64_ui_textfield_set(&ui,&tf,"WWWWWWWW");
            int32_t before=0;os64_ui_run_caret(tf.w.run,tf.cursor,false,&before);
            int left_before=tf.left_px;
            os64_font_set_t *set=outline_set(os64_ui_font_context(&ui),dir,"DejaVuSans.ttf",24);
            os64_font_consumer_t c;os64_ui_font_consumer(&ui,&c);
            int st=os64_font_adopt(set,&c,1,NULL);os64_font_set_release(set);
            int32_t after=0;os64_ui_run_caret(tf.w.run,tf.cursor,false,&after);
            printf("textfield adoption=%d caret before=%d left before=%d; caret after=%d left after=%d inner width=92; visible=%d\n",st,before,left_before,after,tf.left_px,after-tf.left_px<=90);
        }
        printf("teardown=%d\n",os64_ui_font_release(&ui));
    } else if(!strcmp(mode,"commit")||!strcmp(mode,"commit-oom")) {
        c2_scribe_init(dir);int64_t before=g.max_width;
        os64_font_set_t *set=outline_set(os64_ui_font_context(&g.ui),dir,"DejaVuSans.ttf",24);
        os64_font_consumer_t c;os64_ui_font_consumer(&g.ui,&c);c.barrier=c2_commit_barrier;
        c2_deny_commit=!strcmp(mode,"commit-oom");
        int status=os64_font_adopt(set,&c,1,NULL);deny_countdown=-1;
        unsigned long attempts=allocations-c2_barrier_allocs;
        int64_t expected=0;bool whole=true;os64_ui_textview_line_width(&g.ui,&g.view,g.buf.lines[0].bytes,g.buf.lines[0].len,&expected,&whole);
        printf("real Scribe planner: adoption=%d commit allocation attempts=%lu; extent before=%lld after=%lld expected=%lld hscroll.total=%lld\n",status,attempts,(long long)before,(long long)g.max_width,(long long)expected,(long long)g.hscroll.total);
        os64_font_set_release(set);printf("teardown=%d\n",os64_ui_font_release(&g.ui));sbuf_free(&g.buf);
    } else if(!strcmp(mode,"save")) {
        const char *cases[]={"abc","","abc\n"};
        for(size_t i=0;i<3;++i) {
            sbuf_t b;c2_load(&b,cases[i]);char err[128];int rc=sbuf_save(&b,"output",err,sizeof(err));
            printf("unchanged save input=");c2_hex(cases[i],strlen(cases[i]));printf(" output=");c2_hex((char *)c2_output,c2_output_n);
            printf(" status=%d byte-identical=%d\n",rc,c2_output_n==strlen(cases[i])&&!memcmp(cases[i],c2_output,c2_output_n));sbuf_free(&b);
        }
    } else return 2;
    return 0;
}
