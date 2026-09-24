/* Time a non-GUI consumer through spawn, exit and reap. The image under test
 * determines libos64's dependency graph; this program does not import FreeType. */
#include "os64/os64.h"
#include "os64/proc.h"
int main(void)
{
    char *args[]={"true",NULL};
    for (unsigned batch=0;batch<6;batch++) {
        os64_ticks_t start,end;
        if (os64_ticks(&start)!=0 || !start.per_second) return 2;
        for (unsigned n=0;n<50;n++) {
            int64_t pid=os64_spawn("/bin/true",args);int32_t status=-1;
            if (pid<=0 || os64_wait(pid,&status)!=pid || status!=0) return 3;
        }
        if (os64_ticks(&end)!=0 || end.per_second!=start.per_second) return 4;
        os64_printf("textspawn: batch %u count 50 ticks %lu hz %u%s\n",batch,
            (unsigned long)(end.ticks-start.ticks),start.per_second,batch==0?" warmup":"");
    }
    os64_printf("textspawn: PASS\n");return 0;
}
