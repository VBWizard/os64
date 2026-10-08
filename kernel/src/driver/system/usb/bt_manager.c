#include "driver/system/usb/bt_manager.h"
#include "strings/sprintf.h"

static bool manager_word(const char *p,size_t n,const char *word)
{
    size_t i=0; while(word[i]) { if(i==n || p[i]!=word[i]) return false; i++; }
    return i==n;
}
static bool manager_space(char c) { return c==' ' || c=='\n' || c=='\r' || c=='\t'; }
static void manager_retry(bt_manager_t *m,uint64_t now)
{
    if(!m->backoff_ms) m->backoff_ms=2000;
    m->next_attempt=now+m->backoff_ms;
    if(m->backoff_ms<30000) {
        m->backoff_ms*=2; if(m->backoff_ms>30000) m->backoff_ms=30000;
    }
}
void bt_manager_observe(bt_manager_t *m,const bt_le_t *s,uint64_t now)
{
    if(m->revision_seen==s->bond_revision) return;
    m->revision_seen=s->bond_revision; m->generation++; m->dirty=true;
    m->automatic=s->bond.valid; m->suppressed=false; m->blocked=false;
    m->next_attempt=now; m->next_save=now; m->backoff_ms=2000;
}
static bool manager_candidate(const bt_le_t *s,const bt_scan_t *scan)
{
    for(unsigned i=0;i<scan->count && i<BT_SCAN_DEVICES;i++) {
        const bt_scan_device_t *d=&scan->devices[i];
        if(!d->le) continue;
        if(s->bond.has_irk && d->address_type==1 && (d->address[5]&0xc0)==0x40) return true;
        if(d->address_type!=s->bond.address_type) continue;
        unsigned j=0; while(j<6 && d->address[j]==s->bond.peer[j]) j++;
        if(j==6) return true;
    }
    return false;
}
void bt_manager_step(bt_manager_t *m,bt_le_t *s,bt_scan_t *scan,uint64_t now)
{
    bt_manager_observe(m,s,now);
    if(!m->loaded || !m->automatic || m->suppressed || !s->bond.valid || m->blocked) return;
    if(s->phase==BT_LE_FAILED || scan->phase==BT_SCAN_FAILED) { m->blocked=true; return; }
    if(bt_le_input_active(s)) { m->backoff_ms=2000; return; }
    if(!bt_le_quiescent(s)) return;
    if(s->phase==BT_LE_STOPPED && !s->retryable) { m->blocked=true; return; }
    if(m->scan_owned) {
        if(scan->phase!=BT_SCAN_DONE) return;
        m->scan_owned=false;
        if(manager_candidate(s,scan) && bt_le_request(s,"reconnect",9,now))
            bt_le_reconnect_scan(s,scan);
        manager_retry(m,now); return;
    }
    if(now<m->next_attempt) return;
    if(bt_scan_start_le(scan,now)) m->scan_owned=true;
}
bool bt_manager_command(bt_manager_t *m,bt_le_t *s,bt_scan_t *scan,
                        const char *p,size_t n,uint64_t now)
{
    while(n && manager_space(*p)) { p++; n--; }
    while(n && manager_space(p[n-1])) n--;
    if(!m->loaded) return false;
    bt_manager_observe(m,s,now);
    if(manager_word(p,n,"auto on") || manager_word(p,n,"auto off")) {
        bool enabled=manager_word(p,n,"auto on");
        if(enabled && !s->bond.valid) return false;
        m->automatic=enabled; m->suppressed=false; m->blocked=false;
        m->generation++; m->dirty=true; m->next_save=now; m->next_attempt=now;
        return true;
    }
    if(manager_word(p,n,"save")) {
        m->generation++; m->dirty=true; m->next_save=now; return true;
    }
    bool disconnect=manager_word(p,n,"disconnect"),forget=manager_word(p,n,"forget");
    if(forget) {
        if(!bt_le_request(s,p,n,now)) return false;
        bt_manager_observe(m,s,now); return true;
    }
    if(disconnect && bt_le_quiescent(s)) {
        if(s->phase==BT_LE_STOPPED) (void)bt_le_request(s,p,n,now);
        m->suppressed=true; return true;
    }
    if(scan->phase!=BT_SCAN_IDLE && scan->phase!=BT_SCAN_DONE) return false;
    if(!bt_le_request(s,p,n,now)) return false;
    bt_manager_observe(m,s,now);
    if(disconnect) m->suppressed=true;
    else if(!manager_word(p,n,"inspect")) {
        m->suppressed=false; m->blocked=false; m->scan_owned=false;
        m->next_attempt=now;
        bt_le_reconnect_scan(s,scan);
    }
    return true;
}
size_t bt_manager_status(const bt_manager_t *m,char *out,size_t cap)
{
    const char *storage=!m->loaded?"loading":m->dirty?"unsaved":m->storage_error?"load failed":"saved";
    int n=snprintf(out,cap,"bond storage: %s; %s\nautomatic connection: %s%s%s\n"
        "background discovery: %s; next attempt ms: %lu\n"
        "write auto on, auto off, or save to control background connection and persistence\n",
        storage,m->storage_error?m->storage_error:"no storage error",
        m->automatic?"enabled":"disabled",m->suppressed?"; disconnected by user":"",
        m->blocked?"; user action required":"",
        m->scan_owned?"round pending/completing":"idle",m->next_attempt);
    return n<0 || !cap?0:(size_t)n<cap?(size_t)n:cap-1;
}
