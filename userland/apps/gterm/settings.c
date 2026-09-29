#include "settings.h"
#include "os64/os64.h"
#include "os64/pty.h"
#include "os64/conf.h"
#include "os64/fmt.h"
#include "os64/io.h"

bool gterm_history_parse(const char *text,uint32_t *out)
{
    if(!text || !*text)return false;
    uint32_t n=0;
    for(;*text;++text){
        if(*text<'0' || *text>'9')return false;
        uint32_t digit=(uint32_t)(*text-'0');
        if(n>(OS64_PTY_HISTORY_MAX-digit)/10)return false;
        n=n*10+digit;
    }
    *out=n;return true;
}
uint32_t gterm_history_load(void)
{
    char value[32];uint32_t lines=GTERM_HISTORY_DEFAULT;
    int64_t rc=os64_conf_get("gterm.conf","ScrollbackLines",value,sizeof(value));
    if(rc!=OS64_CONF_NO_FILE && rc!=OS64_CONF_NO_KEY &&
        (rc || !gterm_history_parse(value,&lines)))
        os64_debug_log("gterm: invalid/unreadable ScrollbackLines; using 2000");
    return lines;
}
int gterm_history_save(uint32_t lines)
{
    if(lines>OS64_PTY_HISTORY_MAX)return OS64_CONF_BAD_SETTING;
    char value[16];os64_snprintf(value,sizeof(value),"%u",lines);
    return (int)os64_conf_set("gterm.conf","ScrollbackLines",value);
}

const char *gterm_history_error(int64_t result)
{
    switch(result){
    case OS64_PTY_ERR_HISTORY_BUDGET: return "History budget full; try fewer lines.";
    case OS64_PTY_ERR_NO_MEMORY: return "Not enough memory; try fewer lines.";
    case OS64_PTY_ERR_BUSY: return "Terminal changed during Apply; try again.";
    case -1: return "Invalid scrollback request; limit unchanged.";
    default: return "Could not change the scrollback limit.";
    }
}
