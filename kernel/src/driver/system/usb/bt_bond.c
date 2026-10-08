#include "driver/system/usb/bt_manager.h"
#include "crypto/wipe.h"

static void bond_copy(uint8_t *d,const uint8_t *s,size_t n)
{ while(n--) *d++=*s++; }
static uint32_t bond_crc(const uint8_t *p,size_t n)
{
    uint32_t crc=0xffffffff;
    while(n--) {
        crc^=*p++;
        for(unsigned i=0;i<8;i++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1)));
    }
    return ~crc;
}
static bool bond_identity(uint8_t type,const uint8_t *p)
{
    if(!type) return true;
    if(type!=1 || (p[5]&0xc0)!=0xc0) return false;
    uint8_t any=p[5]&0x3f,not_all=(~p[5])&0x3f;
    for(unsigned i=0;i<5;i++) { any|=p[i]; not_all|=(uint8_t)~p[i]; }
    return any && not_all;
}
void bt_bond_encode(uint8_t out[BT_BOND_RECORD_BYTES],const bt_le_bond_t *b,bool automatic)
{
    crypto_wipe(out,BT_BOND_RECORD_BYTES);
    bond_copy(out,(const uint8_t *)"OS64BT01",8);
    if(b->valid) {
        out[8]=1 | (automatic?2:0) | (b->authenticated?4:0) | (b->has_irk?8:0);
        out[9]=b->address_type; out[10]=b->last_address_type;
        bond_copy(out+12,b->local,6); bond_copy(out+18,b->peer,6);
        bond_copy(out+24,b->last_peer,6); bond_copy(out+30,b->ltk,16);
        bond_copy(out+46,b->rand,8); out[54]=b->ediv; out[55]=b->ediv>>8;
        if(b->has_irk) bond_copy(out+56,b->irk,16);
    }
    uint32_t crc=bond_crc(out,76);
    for(unsigned i=0;i<4;i++) out[76+i]=crc>>(8*i);
}
bool bt_bond_decode(bt_le_bond_t *b,bool *automatic,const uint8_t *p,size_t n)
{
    crypto_wipe(b,sizeof(*b)); *automatic=false;
    if(n!=BT_BOND_RECORD_BYTES) return false;
    const char *magic="OS64BT01";
    for(unsigned i=0;i<8;i++) if(p[i]!=(uint8_t)magic[i]) return false;
    uint32_t crc=bond_crc(p,76);
    for(unsigned i=0;i<4;i++) if(p[76+i]!=(uint8_t)(crc>>(8*i))) return false;
    if((p[8]&~15) || p[11] || p[72] || p[73] || p[74] || p[75]) return false;
    if(!(p[8]&1)) {
        for(unsigned i=8;i<76;i++) if(p[i]) return false;
        return true;
    }
    if(!bond_identity(p[9],p+18) || p[10]>1) return false;
    if(!(p[8]&8)) for(unsigned i=56;i<72;i++) if(p[i]) return false;
    b->valid=true; b->authenticated=(p[8]&4)!=0; b->has_irk=(p[8]&8)!=0;
    *automatic=(p[8]&2)!=0; b->address_type=p[9]; b->last_address_type=p[10];
    bond_copy(b->local,p+12,6); bond_copy(b->peer,p+18,6); bond_copy(b->last_peer,p+24,6);
    bond_copy(b->ltk,p+30,16); bond_copy(b->rand,p+46,8); b->ediv=p[54]|(uint16_t)p[55]<<8;
    bond_copy(b->irk,p+56,16);
    return true;
}
