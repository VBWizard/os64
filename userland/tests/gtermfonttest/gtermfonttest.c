/* Run the production terminal/event loop. Only this fixture intercepts digit
 * keys to inject resolved sets; /bin/gterm has no private font selector. */
#include "os64/gui.h"
static int64_t fixture_poll(int64_t handle, os64_gui_event_t *event);
#define os64_gui_event_poll fixture_poll
#define main gterm_app_main
#include "../../apps/gterm/gterm.c"
#undef main
#undef os64_gui_event_poll
#include "os64/slurp.h"
#include "os64/procfs.h"
#include "os64/str.h"
#include "os64/signal.h"

static unsigned current_size;
static bool failed;
static void report(bool ok, const char *what)
{
    char line[256];
    os64_snprintf(line,sizeof(line),"F3 %s: %s\n",ok?"PASS":"FAIL",what);
    os64_printf("%s",line);os64_serial_log(line);
    if(!ok) failed=true;
}
static os64_font_status_t fixture_set(const char *name,unsigned size,os64_font_set_t **out)
{
    uint8_t *bytes=NULL;size_t length=0;
    if(os64_slurp(name,2u*1024u*1024u,&bytes,&length)!=OS64_SLURP_OK)
        return OS64_FONT_MALFORMED;
    os64_font_role_spec_t specs[3]={0};specs[1].pixel_height=size;
    specs[1].primary=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,bytes,length};
    os64_font_status_t result=os64_font_set_prepare(gText,specs,out);
    os64_free(bytes);return result;
}
static int64_t refused_resize(void *user,uint32_t cols,uint32_t rows)
{
    (void)user;(void)cols;
    /* Exercise the real syscall's unchanged-on-refusal contract. */
    return os64_pty_resize(gMaster,1,rows);
}
static int64_t fixture_poll(int64_t handle,os64_gui_event_t *event)
{
    static uint32_t last_cols,last_rows;
    if(gGrid.cols!=last_cols || gGrid.rows!=last_rows) {
        char line[160];const os64_font_role_view_t *f=gterm_grid_font(&gGrid);
        os64_snprintf(line,sizeof(line),"F3 grid %ux%u cell %dx%d\n",gGrid.cols,gGrid.rows,f->cell_width_px,f->row_height_px);
        os64_printf("%s",line);os64_serial_log(line);
        last_cols=gGrid.cols;last_rows=gGrid.rows;
    }
    int64_t result=os64_gui_event_poll(handle,event);
    if(result!=1 || event->type!=OS64_GUI_EVENT_KEY_DOWN) return result;
    char key=event->key.ascii;
    if(key<'1' || key>'6') return result;
    if(key=='6') {
        char bytes[64]={0};int64_t fd=os64_open(OS64_CLIPBOARD_PATH,"r");
        int64_t n=fd>=0?os64_read((int32_t)fd,bytes,sizeof(bytes)):-1;
        if(fd>=0)os64_close((int32_t)fd);
        report(n==7 && os64_streq(bytes,"COPY ME"),"pointer selection copied exact bytes");
        return 0;
    }
    os64_font_set_t *set=NULL;
    unsigned size=key=='1'?12:key=='2'?28:current_size==28?12:28;
    os64_font_status_t status=key=='5'?os64_font_set_prepare(gText,NULL,&set):
        fixture_set(key=='4'?"/tests/fonts/DejaVuSans.ttf":"/tests/fonts/NoBoxes.ttf",size,&set);
    if(key=='4') {
        report(status==OS64_FONT_UNSUPPORTED && set==NULL,"proportional terminal rejected");
        os64_font_set_release(set);return 0;
    }
    if(status!=OS64_FONT_OK) {report(false,"prepare fixture font");return 0;}
    uint64_t identity=gterm_grid_font(&gGrid)->identity;
    uint32_t cols=gGrid.cols,rows=gGrid.rows;
    bool selected=gSelLive,dragging=gDragging,valid=gSnapshotValid;
    if(key=='3') gGrid.resize=refused_resize;
    status=replace_fonts(set);gGrid.resize=resize_pty;
    if(key=='3') {
        os64_pty_header_t header={0};int64_t snap=os64_pty_snapshot(gMaster,&header,NULL,0);
        report(status==OS64_FONT_ENGINE_ERROR && gterm_grid_font(&gGrid)->identity==identity &&
            gGrid.cols==cols && gGrid.rows==rows && gSelLive==selected &&
            gDragging==dragging && gSnapshotValid==valid && snap==0 &&
            header.cols==cols && header.rows==rows,"real PTY refusal preserved font/grid/selection");
    } else {
        report(status==OS64_FONT_OK && !gSelLive && !gDragging && !gSnapshotValid,"font adoption committed and invalidated old snapshot");
        if(status==OS64_FONT_OK)current_size=key=='5'?0:size;
    }
    os64_font_set_release(set);return 0;
}

static volatile bool resized;
static void on_winch(int signal) {(void)signal;resized=true;}
static void specimen(const char *reason)
{
    os64_tty_info_t tty;
    if(os64_tty_read(&tty)!=0)os64_exit(2);
    char line[128];os64_snprintf(line,sizeof(line),"F3 child %s %ux%u\n",reason,tty.cols,tty.rows);
    os64_serial_log(line);
    os64_printf("\033[2J\033[HCOPY ME\n%s",line);
    os64_printf("Latin-1: caf\351  na\357ve  \243\n");
    os64_printf("\033[31mRED \033[32mGREEN \033[34mBLUE\033[0m\n");
    os64_printf("\033[7mREVERSE\033[0m \033[1;33;44mBOLD ON BLUE\033[0m\n");
    os64_printf("\033(U\332\304\304\302\304\304\277  \311\315\315\313\315\315\273\n");
    os64_printf("\263  \263  \263  \272  \272  \272\n");
    os64_printf("\303\304\304\305\304\304\264  \314\315\315\316\315\315\271\n");
    os64_printf("\300\304\304\301\304\304\331  \310\315\315\312\315\315\274\n");
    os64_printf("\260\261\262\333\334\335\336\337\033(B\n");
    os64_printf("Type text; q then Enter closes.\n");
}
static int child(void)
{
    if(os64_signal_set_handler(OS64_SIGWINCH,on_winch)<0)return 2;
    specimen("startup");
    for(;;) {
        if(resized) {resized=false;specimen("SIGWINCH");}
        char c;int64_t n=os64_read(0,&c,1);
        if(n==1) {if(c=='q')break;os64_printf("%c",c);}
        else if(n==0)break;
        else if(!resized)os64_sleep(20);
    }
    os64_serial_log("F3 child input usable, exiting\n");return 0;
}
int main(int argc,char **argv)
{
    if(argc>1 && os64_streq(argv[1],"--child"))return child();
    int result=gterm_app_main(3,(char *[]){"gtermfonttest","/tests/gtermfonttest","--child",NULL});
    report(result==0 && !failed,"fixture finished");return result || failed;
}
