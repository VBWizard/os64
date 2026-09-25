// Descriptor semantics and packed reports, independent of xHCI/MMIO.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/driver/system/hid_mouse.c"

#include "hid_mouse_fixtures.h"

int main(void)
{
    hid_mouse_layout_t m = {0}; hid_mouse_sample_t s;
    assert(hid_mouse_parse(basic,sizeof(basic),&m));
    assert(m.report_id == 0 && m.bytes == 4 && m.wheel.bit == 24);
    const uint8_t plain[] = {5,0xff,0x7f,0x81};
    assert(hid_mouse_decode(&m,plain,sizeof(plain),&s));
    assert(s.buttons == 5 && s.x == -1 && s.y == 127 && s.wheel == -127);
    assert(!hid_mouse_decode(&m,plain,3,&s));
    assert(hid_mouse_parse(packed,sizeof(packed),&m));
    assert(m.report_id == 2 && m.bytes == 9 && m.wheel.bit == 32);
    const uint8_t report[] = {2,3,0xff,0x7f,0x80,0xff,0,0,0};
    assert(hid_mouse_decode(&m,report,sizeof(report),&s));
    assert(s.buttons == 3 && s.x == -1 && s.y == -2041 && s.wheel == -1);
    for (unsigned n = 0; n < sizeof(report); ++n) assert(!hid_mouse_decode(&m,report,n,&s));
    const uint8_t other[] = {3,0,0,0,0,0,0,0,0};
    s.buttons = 7;
    assert(!hid_mouse_decode(&m,other,sizeof(other),&s) && s.buttons == 7);
    // Every truncated basic descriptor fails and preserves the caller's layout.
    for (unsigned n = 0; n < sizeof(basic); ++n) {
        hid_mouse_layout_t prior = m;
        assert(!hid_mouse_parse(basic,n,&m));
        assert(memcmp(&m,&prior,sizeof(m)) == 0);
    }
    uint8_t mutated[sizeof(basic)];
    memcpy(mutated,basic,sizeof(basic));
    mutated[sizeof(basic)-4] = 0x81; mutated[sizeof(basic)-3] = 2; // absolute axes
    assert(!hid_mouse_parse(mutated,sizeof(mutated),&m));
    memcpy(mutated,basic,sizeof(basic)); mutated[sizeof(basic)-7] = 255; // overwide fields
    assert(!hid_mouse_parse(mutated,sizeof(mutated),&m));
    memcpy(mutated,basic,sizeof(basic));
    mutated[sizeof(basic)-13]=0x32; // replace Wheel usage with Z: no usable wheel
    assert(!hid_mouse_parse(mutated,sizeof(mutated),&m));
    // Global Push/Pop restores page, size/count and range; Feature/Output
    // items do not consume input bits. Insert them inside the application.
    uint8_t extended[sizeof(basic) + 16];
    const uint8_t extra[] = {0xa4,0x05,0x0c,0x75,16,0x95,1,0x91,2,0xb1,2,0xb4};
    memcpy(extended,basic,6); memcpy(extended+6,extra,sizeof(extra));
    memcpy(extended+6+sizeof(extra),basic+6,sizeof(basic)-6);
    assert(hid_mouse_parse(extended,sizeof(basic)+sizeof(extra),&m));
    assert(m.bytes == 4 && m.wheel.bit == 24);
    memcpy(mutated,basic,sizeof(basic));
    mutated[sizeof(basic)-7] = 16; // signed 16-bit axes/wheel, same logical range
    assert(hid_mouse_parse(mutated,sizeof(mutated),&m) && m.bytes == 7);
    const uint8_t wide[] = {0,0,0x80,0xff,0x7f,0,0x80};
    assert(hid_mouse_decode(&m,wide,sizeof(wide),&s));
    assert(s.x == -32768 && s.y == 32767 && s.wheel == -32768);
    // Host sanitizer sweep over valid descriptors
    // with single-byte mutations; accepted layouts must decode within bounds.
    uint32_t rng = 1;
    for (unsigned n = 0; n < 20000; ++n) {
        uint8_t d[sizeof(packed)], r[HID_MOUSE_REPORT_BYTES] = {0};
        memcpy(d,packed,sizeof(d));
        rng = rng * 1664525u + 1013904223u;
        d[rng % sizeof(d)] = (uint8_t)(rng >> 16);
        if (hid_mouse_parse(d,sizeof(d),&m)) {
            r[0] = m.report_id;
            assert(hid_mouse_decode(&m,r,sizeof(r),&s));
        }
    }
    puts("test_hid_mouse_host: descriptor, report ID, packed axes, truncation and mutation checks passed");
}
