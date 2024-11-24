// time.h
#ifndef TIME_H
#define TIME_H

#include <stdint.h>

typedef long time_t;

struct tm {
    // Seconds [0,60]
	int tm_sec;
    // Minutes [0,59]
	int tm_min;
    // Hours [0,23]
	int tm_hour;
	// Day of the month [1,31]
    int tm_mday; 
	// Months since January [0,11]
	int tm_mon;   
    //Number of years since 1900
	int tm_year;
    //Days since Sunday [0,6]
	int tm_wday;
    // Days since January 1 [0,365]
	int tm_yday;
    // Daylight Saving Time flag
	int tm_isdst;
    // Seconds east of UTC
	long __tm_gmtoff;
};

time_t mktime(struct tm *tmbuf);
time_t mktime_simple(const struct tm *time);
struct tm *gmtime(const time_t *timer, struct tm *tmbuf);
void kwait(uint64_t msToWait);
void wait(int msToWait);
void __attribute__((noinline))waitTicks(int TicksToWait);

#endif // TIME_H