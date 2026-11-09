#define main upstream_test_main
#define os64_malloc test_original_malloc
#define os64_free test_original_free
#include F4_C1_HOST_TEST
#undef main
#undef os64_malloc
#undef os64_free

static size_t allocation_calls, deny_at;
static bool deny_all;
void *os64_malloc(size_t size)
{
    allocation_calls++;
    if(deny_all || allocation_calls==deny_at)return NULL;
    return malloc(size?size:1);
}
void os64_free(void *p) {free(p);}
static int registered_plans, actual_width;
static os64_font_status_t plan_measure_status=OS64_FONT_OK;
static os64_font_status_t measure_plan(os64_ui_t *ui,void *user,void **out)
{
    (void)user;*out=NULL;registered_plans++;
    /* A width the planner cannot measure is no longer an ordinary integer:
     * the status is what travels, and the coordinator sees it. */
    plan_measure_status=os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,"WWWW",4,&actual_width);
    return plan_measure_status;
}
static os64_font_status_t fail_measure_plan(os64_ui_t *ui,void *user,void **out)
{
    deny_all=true;
    os64_font_status_t status=measure_plan(ui,user,out);
    deny_all=false;
    return status;
}
static void nop_commit(os64_ui_t *ui,void *user,void *plan)
{(void)ui;(void)user;(void)plan;}
static int32_t width_of(os64_ui_t *ui,const char *s,size_t n)
{int32_t w=-1;os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,s,n,&w);return w;}
int main(int argc,char **argv)
{
    if(argc<3)return 2;
    const char *dir=argv[2];
    os64_ui_t ui={0};
    if(!strcmp(argv[1],"register")) {
        deny_at=allocation_calls+1;
        os64_font_status_t registration=
            os64_ui_font_planner(&ui,measure_plan,nop_commit,nop_commit,NULL);
        deny_at=0;
        printf("registration status=%d (0 would be a silent success)\n",registration);
        os64_text_context_t *context=os64_ui_font_context(&ui);
        os64_font_set_t *set=outline_set(context,dir,"DejaVuSans.ttf",24);
        os64_font_consumer_t consumer;os64_ui_font_consumer(&ui,&consumer);
        int status=os64_font_adopt(set,&consumer,1,NULL);
        printf("registration allocation denied; adoption=%d planner_calls=%d (expected refusal or registered planner)\n",status,registered_plans);
        os64_font_set_release(set);os64_ui_font_release(&ui);return 0;
    }
    os64_text_context_t *context=os64_ui_font_context(&ui);
    os64_font_set_t *set=outline_set(context,dir,"DejaVuSans.ttf",!strcmp(argv[1],"clip")?8:24);
    if(!set)return 2;
    if(!strcmp(argv[1],"release")) {
        os64_ui_font_bind(&ui,set);
        os64_font_status_t first=os64_ui_font_release(&ui);
        printf("release while the caller still holds the candidate: status=%d (7=BUSY, owner kept)\n",first);
        os64_font_set_release(set);
        printf("after releasing the candidate: status=%d\n",os64_ui_font_release(&ui));
        return 0;
    }
    if(!strcmp(argv[1],"measure")) {
        os64_font_set_t *old=NULL;os64_font_set_prepare(context,NULL,&old);
        os64_ui_font_bind(&ui,old);os64_font_set_release(old);
        os64_ui_font_planner(&ui,fail_measure_plan,nop_commit,nop_commit,NULL);
        os64_font_consumer_t consumer;os64_ui_font_consumer(&ui,&consumer);
        int status=os64_font_adopt(set,&consumer,1,NULL);
        int expected=width_of(&ui,"WWWW",4);
        printf("planner measurement allocation denied; adoption=%d measure_status=%d staged_width=%d actual_width=%d\n",status,plan_measure_status,actual_width,expected);
    } else if(!strcmp(argv[1],"paint")) {
        os64_ui_font_bind(&ui,set);canvas_t c;canvas_init(&c,0xff123456);
        int expected=width_of(&ui,"WWWW",4);
        deny_all=true;
        int pen=os64_ui_draw_text(&ui,NULL,OS64_FONT_ROLE_UI,&c.s,(os64_gui_rect_t){0,0,SURF_W,SURF_H},0,20,"WWWW",4,0xffffffff,0xff123456);
        deny_all=false;
        printf("installed face width=%d; paint allocation denied, rendered pen=%d\n",expected,pen);
    } else if(!strcmp(argv[1],"clip")) {
        os64_ui_font_bind(&ui,set);canvas_t c;canvas_init(&c,0xff123456);
        int row=os64_ui_font_row_height(&ui,OS64_FONT_ROLE_UI);
        os64_ui_draw_text(&ui,NULL,OS64_FONT_ROLE_UI,&c.s,(os64_gui_rect_t){0,0,SURF_W,SURF_H},20,20,"\xf0\x9f\x98\x80",4,0xffffffff,0xff123456);
        int outside=0,miny=100,maxy=-1;
        for(int y=0;y<SURF_H;y++)for(int x=0;x<SURF_W;x++) if(c.px[y*SURF_W+x]!=0xff123456) {
            if(y<20||y>=20+row)outside++;
            if(y<miny)miny=y;if(y>maxy)maxy=y;
        }
        printf("primary row=[20,%d), painted y=[%d,%d], pixels outside row=%d\n",20+row,miny,maxy,outside);
    } else if(!strcmp(argv[1],"cross")) {
        os64_ui_t other={0};os64_ui_font_context(&other);
        int status=os64_ui_font_bind(&other,set);
        printf("foreign-context bind=%d widths=%d (a refusal is the correct answer)\n",status,width_of(&other,"WWWW",4));
        os64_ui_font_release(&other);
    }
    os64_font_set_release(set);os64_ui_font_release(&ui);return 0;
}
