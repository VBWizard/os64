#include "os64/os64.h"
#include "os64/pty.h"
#include "os64/proc.h"
#include "os64/procfs.h"
#include "os64/io.h"
#include "os64/fmt.h"
#include "os64/str.h"
#include "os64/mem.h"

static unsigned failures,checks;
static void check(bool ok,const char *name)
{
    ++checks;if(!ok)++failures;
    os64_printf("ptyhistorytest: %s %s\n",ok?"PASS":"FAIL",name);
}
static bool wait_line(int64_t fd,os64_pty_viewport_t *v,uint64_t line)
{
    for(unsigned i=0;i<100;++i){
        if(os64_pty_viewport(fd,v,NULL,0,OS64_PTY_VIEW_LIVE,0)<0)return false;
        if(v->live_line>=line)return true;
        os64_sleep(50);
    }
    return false;
}
typedef struct {int64_t fd;volatile bool done;unsigned failures;} race_grid_t;
static int64_t change_grid(void *user)
{
    race_grid_t *r=user;
    for(unsigned i=0;i<80;++i){
        if(os64_pty_resize(r->fd,i&1?12:16,i&1?4:6)<0)++r->failures;
        if(os64_pty_history(r->fd,i&1?40:0)<0)++r->failures;
        os64_sleep(1);
    }
    __atomic_store_n(&r->done,true,__ATOMIC_RELEASE);return 0;
}
static bool race_snapshots(int64_t fd)
{
    race_grid_t race={.fd=fd};
    struct {os64_pty_cell_t cells[128];uint64_t guard;} storage={.guard=0x12345678abcdefUL};
    int64_t thread=os64_thread(change_grid,&race),status;
    if(thread<0)return false;
    bool ok=true;unsigned reads=0;
    do {
        os64_pty_viewport_t v={0};
        int64_t n=os64_pty_viewport(fd,&v,storage.cells,128,0,0);
        ok &= n>=0 && n==(int64_t)v.screen.cols*v.screen.rows && n<=128 &&
            v.oldest_line<=v.first_line && v.first_line<=v.live_line &&
            v.live_line-v.oldest_line==v.history_lines && v.history_lines<=v.history_limit;
        ++reads;os64_yield();
    } while(!__atomic_load_n(&race.done,__ATOMIC_ACQUIRE));
    return os64_thread_join((int32_t)thread,&status)==0 && !status && !race.failures &&
        ok && reads>1 && storage.guard==0x12345678abcdefUL;
}

int main(int argc,char **argv)
{
    if(argc>1){
        (void)os64_tty_set_raw(true);
        bool demo=os64_streq(argv[1],"--demo");
        for(unsigned i=0;i<500;++i){os64_printf("row %03u\n",i);if(demo)os64_sleep(5);}
        char c; (void)os64_read(0,&c,1);
        for(unsigned i=500;i<510;++i)os64_printf("row %03u\n",i);
        (void)os64_read(0,&c,1);return 0;
    }
    int64_t fd=os64_pty_create(12,4);check(fd>=0,"create");if(fd<0)return 1;
    check(os64_pty_history(fd,40)==0,"set independent capacity");
    char *args[]={"/tests/ptyhistorytest","--child",NULL};
    check(os64_spawn_seated(args[0],args,fd)>0,"seat producer");
    os64_pty_viewport_t v;os64_pty_cell_t cells[128];
    check(wait_line(fd,&v,497),"burst retained without intermediate snapshots");
    check(v.history_limit==40 && v.history_lines==40,"bounded retained history");
    uint64_t epoch=v.epoch;
    int64_t n=os64_pty_viewport(fd,&v,cells,128,470,epoch);
    check(n==48 && v.first_line==470 && cells[4].ch=='4' && cells[5].ch=='7' && cells[6].ch=='0',"read old physical row");
    os64_write((int32_t)fd,"x",1);
    check(wait_line(fd,&v,507),"continued output");
    n=os64_pty_viewport(fd,&v,cells,128,470,epoch);
    check(n==48 && v.first_line==470 && cells[5].ch=='7',"anchor survives new output");
    n=os64_pty_viewport(fd,&v,cells,1,0,epoch);
    check(n==1 && v.first_line==v.oldest_line && cells[0].ch=='r',"clamp evicted anchor and bounded copy");
    check(os64_pty_history(fd,10001)==-1,"refuse oversized capacity");
    check((int64_t)os64_syscall2(SYSCALL_PTY_HISTORY,fd,1UL<<32)<0,"reject wide capacity before narrowing");
    check(os64_pty_viewport(fd,&v,NULL,1,0,epoch)<0,"refuse missing cells");
    check(os64_pty_viewport(fd,&v,cells,131073,0,epoch)<0,"refuse oversized snapshot");
    check(os64_pty_resize(fd,16,6)==0,"resize");
    n=os64_pty_viewport(fd,&v,cells,128,470,epoch);
    check(n==96 && v.epoch!=epoch && v.first_line==v.live_line && v.history_limit==40,"geometry epoch and capacity survive resize");
    os64_pty_header_t legacy;
    check(os64_pty_snapshot(fd,&legacy,cells,128)==96 && legacy.cols==16,"legacy live snapshot ABI");
    check(os64_pty_history(fd,0)==0,"disable history");
    n=os64_pty_viewport(fd,&v,cells,128,OS64_PTY_VIEW_LIVE,v.epoch);
    check(v.history_lines==0 && v.oldest_line==v.live_line,"zero means no retained rows");
    os64_pty_cell_t grown[128];uint64_t live=v.live_line;
    check(n==96 && os64_pty_history(fd,40)==0 &&
        os64_pty_viewport(fd,&v,grown,128,OS64_PTY_VIEW_LIVE,v.epoch)==n &&
        v.live_line==live && !v.history_lines &&
        !os64_memcmp(cells,grown,(size_t)n*sizeof(*cells)),"growing from zero preserves the live cells");
    check(race_snapshots(fd),"coherent bounded snapshots during concurrent resize and capacity changes");
    check((int64_t)os64_syscall3(SYSCALL_PTY_RESIZE,fd,(1UL<<32)|12,4)==-1,
          "reject wide geometry before narrowing");
    int64_t many[4]={-1,-1,-1,-1}; bool refused=false, preserved=true;
    for(unsigned i=0;i<4;++i){
        many[i]=os64_pty_create(512,2);
        if(many[i]<0){preserved=false;break;}
        os64_pty_viewport_t before,after;
        preserved &= os64_pty_viewport(many[i],&before,NULL,0,0,0)==0;
        int64_t result=os64_pty_history(many[i],10000);
        if(result<0){
            refused=result==OS64_PTY_ERR_HISTORY_BUDGET;
            preserved &= os64_pty_viewport(many[i],&after,NULL,0,0,0)==0 &&
                after.history_limit==before.history_limit &&
                after.screen.generation==before.screen.generation;
            break;
        }
    }
    check(refused && preserved,"quota has distinct error and preserves refused grid");
    // Discover remaining quota rather than assuming no other terminal owns
    // history. Drop each probe before the next so growth's overlap charge
    // cannot turn this into a search for only half the remaining capacity.
    bool filled=many[3]>=0 && os64_pty_history(many[3],0)==0;
    uint32_t low=0,high=OS64_PTY_HISTORY_MAX;
    while(filled && low<high){
        uint32_t probe=low+(high-low+1)/2;
        int64_t result=os64_pty_history(many[3],probe);
        if(result==0)low=probe;
        else if(result==OS64_PTY_ERR_HISTORY_BUDGET)high=probe-1;
        else filled=false;
        filled &= os64_pty_history(many[3],0)==0;
    }
    filled &= many[3]>=0 && os64_pty_history(many[3],low)==0;
    check(filled,"fill remaining quota to less than one 512-column history row");
    int64_t fresh=os64_pty_create(512,256);
    os64_pty_viewport_t born={0};
    check(filled && fresh>=0 && os64_pty_viewport(fresh,&born,NULL,0,0,0)==0 &&
        born.history_limit==0,"new terminal actually uses live-only fallback");
    if(fresh>=0){
        check(os64_pty_resize(fresh,500,250)==0 &&
            os64_pty_viewport(fresh,&born,NULL,0,0,0)==0 &&
            born.history_limit==0 && born.screen.cols==500 && born.screen.rows==250,
            "live-only fallback resizes without enabling history");
        os64_close((int32_t)fresh);
    }
    os64_pty_cell_t oldcell,newcell;os64_pty_viewport_t shrink_before,shrink_after;
    bool snapshot=many[0]>=0 && os64_pty_viewport(many[0],&shrink_before,&oldcell,1,
        OS64_PTY_VIEW_LIVE,0)==1;
    check(snapshot && os64_pty_history(many[0],9999)==0 &&
        os64_pty_viewport(many[0],&shrink_after,&newcell,1,OS64_PTY_VIEW_LIVE,0)==1 &&
        shrink_after.history_limit==9999 && !os64_memcmp(&oldcell,&newcell,sizeof(oldcell)),
        "shrink succeeds at the quota and preserves live cells");
    for(unsigned i=0;i<4;++i)if(many[i]>=0)os64_close((int32_t)many[i]);
    os64_close((int32_t)fd);
    fd=os64_pty_create_stream(12,4);
    check(fd>=0 && os64_pty_history(fd,5)<0 && os64_pty_viewport(fd,&v,NULL,0,0,0)<0,"STREAM refuses grid operations");
    if(fd>=0)os64_close((int32_t)fd);
    os64_printf("ptyhistorytest: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
