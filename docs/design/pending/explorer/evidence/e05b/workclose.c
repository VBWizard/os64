#include "os64/gui.h"
#include "os64/work.h"
#include "os64/proc.h"
#include "os64/io.h"
#include "os64/fmt.h"
int main(void)
{
    int64_t win=os64_gui_window_create_content("Worker close timing",40,40,240,80,OS64_GUI_CREATE_FIT_SCREEN);
    if(win<=0)return 1;
    const unsigned delays[]={30,200,500,800};
    for(unsigned workers=1;workers<=2;++workers){
        for(unsigned i=0;i<4;++i){
            os64_work_pool_t *p=os64_work_pool_create(workers,4096,win,4);
            if(!p)return 2;
            os64_sleep(delays[i]);
            int64_t start=os64_micros();
            os64_work_pool_destroy(p);
            int64_t elapsed=os64_micros()-start;
            char line[192];
            os64_snprintf(line,sizeof(line),"workclose: workers=%u idle_ms=%u destroy_us=%ld",workers,delays[i],(long)elapsed);
            os64_debug_log(line);
        }
    }
    os64_gui_window_destroy(win);
    return 0;
}
