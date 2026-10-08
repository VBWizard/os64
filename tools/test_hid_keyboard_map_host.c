#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "driver/system/hid_keyboard_map.h"
#include "hid_keyboard_fixtures.h"

static void array_reports(void)
{
    hid_keyboard_layout_t m,old; memset(&m,0xaa,sizeof(m)); old=m;
    assert(!hid_keyboard_parse_map(keyboard_composite_map,sizeof(keyboard_composite_map),2,&m));
    assert(!memcmp(&m,&old,sizeof(m)));
    assert(!hid_keyboard_parse_map(keyboard_composite_map,sizeof(keyboard_composite_map)-1,7,&m));
    assert(hid_keyboard_parse_map(keyboard_composite_map,sizeof(keyboard_composite_map),7,&m));
    assert(m.bytes==8 && m.report_id==7 && m.array_count==6);
    const uint8_t data[]={2,0,4,5,4,0,0,0}; uint8_t out[8];
    assert(hid_keyboard_decode_map(&m,data,sizeof(data),out));
    const uint8_t expected[]={2,0,4,5,0,0,0,0}; assert(!memcmp(out,expected,8));
    assert(!hid_keyboard_decode_map(&m,data,7,out));
    uint8_t prefixed[]={7,2,0,4,0,0,0,0,0};
    assert(!hid_keyboard_decode_map(&m,prefixed,sizeof(prefixed),out));
    uint8_t bad[]={0,0,0xff,0,0,0,0,0};
    assert(!hid_keyboard_decode_map(&m,bad,8,out));
    uint8_t rollover[]={0,0,1,1,1,1,1,1};
    assert(hid_keyboard_decode_map(&m,rollover,8,out) && !memcmp(out,rollover,8));
}
static void bitmaps(void)
{
    const uint8_t map[]={5,1,9,6,0xa1,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
        0x75,1,0x95,8,0x81,2,0x19,4,0x29,19,0x95,16,0x81,2,0xc0};
    hid_keyboard_layout_t m; assert(hid_keyboard_parse_map(map,sizeof(map),0,&m) && m.bytes==3);
    uint8_t report[]={2,3,0},out[8];
    assert(hid_keyboard_decode_map(&m,report,3,out) && out[0]==2 && out[2]==4 && out[3]==5);
    report[1]=0xff;
    assert(hid_keyboard_decode_map(&m,report,3,out) && out[0]==2);
    for(unsigned i=2;i<8;i++) assert(out[i]==1);
    // Padding precedes an unaligned modifier field, then a single key array.
    const uint8_t packed[]={5,1,9,6,0xa1,1,0x75,3,0x95,1,0x81,1,
        5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
        0x75,5,0x95,1,0x81,1,0x19,0,0x29,0x65,0x25,0x65,0x75,8,0x95,1,0x81,0,0xc0};
    assert(hid_keyboard_parse_map(packed,sizeof(packed),0,&m) && m.bytes==3);
    const uint8_t packed_report[]={0x10,0,4};
    assert(hid_keyboard_decode_map(&m,packed_report,3,out) && out[0]==2 && out[2]==4);
}
static void hostile(void)
{
    uint8_t data[HID_KEYBOARD_MAP_BYTES],report[64]={0},out[8];
    hid_keyboard_layout_t m; unsigned random=12345;
    for(unsigned i=0;i<15000;i++) {
        size_t n=i%2?sizeof(keyboard_composite_map):i%sizeof(data);
        for(size_t j=0;j<n;j++) { random=random*1664525+1013904223; data[j]=random>>24; }
        if(i%2) {
            memcpy(data,keyboard_composite_map,n); data[i%n]^=(uint8_t)(i>>4)|1;
        }
        if(hid_keyboard_parse_map(data,n,7,&m)) {
            assert(m.bytes<=64); (void)hid_keyboard_decode_map(&m,report,m.bytes,out);
        }
    }
    const uint8_t oversized[]={5,1,9,6,0xa1,1,0x75,32,0x96,0xff,0xff,0x81,0,0xc0};
    assert(!hid_keyboard_parse_map(oversized,sizeof(oversized),0,&m));
    const uint8_t pop[]={0xb4}; assert(!hid_keyboard_parse_map(pop,sizeof(pop),0,&m));
}
int main(void)
{ array_reports(); bitmaps(); hostile(); puts("PASS: keyboard Report Map arrays, bitmaps, padding, IDs, rollover and hostile bounds"); }
