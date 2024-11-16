#include "driver/system/rtc.h"
#include "io.h"
#include "bcd.h"

struct tm getRTCDate()
{
    struct tm tmbuf;

    outb(0x70, 0x00);
    tmbuf.tm_sec = bcdToDec(inb(0x71));
    outb(0x70, 0x02);
    tmbuf.tm_min = bcdToDec(inb(0x71));
    outb(0x70, 0x04);
    tmbuf.tm_hour = bcdToDec(inb(0x71));
    outb(0x70, 0x07);
    tmbuf.tm_mday = bcdToDec(inb(0x71));
    outb(0x70, 0x08);
    tmbuf.tm_mon = bcdToDec(inb(0x71));
    tmbuf.tm_mon-=1;
    outb(0x70, 0x09);
    tmbuf.tm_year = bcdToDec(inb(0x71));
    tmbuf.tm_isdst = -1;	//Default to DST unknown (0=Not in effect, 1=In effect, -1=Unknown)
    tmbuf.tm_year += 2000;
    tmbuf.tm_year = tmbuf.tm_year - 1900;
    tmbuf.__tm_gmtoff = -5 * 60 * 60;
	return tmbuf;
//printf("System Date = %d/%d/%d %d:%d:%d\n", tmbuf.tm_mon, tmbuf.tm_mday, tmbuf.tm_year, tmbuf.tm_hour, tmbuf.tm_min, tmbuf.tm_sec, tmbuf.tm_hour, tmbuf.tm_min, tmbuf.tm_sec);
}
