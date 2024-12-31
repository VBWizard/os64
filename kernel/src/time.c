#include "time.h"
#include <stdint.h>
#include "kernel.h"
#include "serial_logging.h"

extern int kTimeZone;
extern volatile uint64_t kTicksSinceStart;
extern uint64_t kTicksPerSecond;

const int _ytab[2][12] = {
  {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
  {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};
long _dstbias = 0;                  // Offset for Daylight Saving Time

#define YEAR0                   1900
#define EPOCH_YR                1970
#define SECS_DAY                (24L * 60L * 60L)
#define LEAPYEAR(year)          (!((year) % 4) && (((year) % 100) || !((year) % 400)))
#define YEARSIZE(year)          (LEAPYEAR(year) ? 366 : 365)
#define FIRSTSUNDAY(timp)       (((timp)->tm_yday - (timp)->tm_wday + 420) % 7)
#define FIRSTDAYOF(timp)        (((timp)->tm_wday - (timp)->tm_yday + 420) % 7)

#define TIME_MAX                2147483647L


//credit: myos
time_t mktime(struct tm *tmbuf) {
  long day, year;
  int tm_year;
  int yday, month;
  /*unsigned*/ long seconds;
  int overflow;
  long dst;

  tmbuf->tm_min += tmbuf->tm_sec / 60;
  tmbuf->tm_sec %= 60;
  if (tmbuf->tm_sec < 0) {
    tmbuf->tm_sec += 60;
    tmbuf->tm_min--;
  }
  tmbuf->tm_hour += tmbuf->tm_min / 60;
  tmbuf->tm_min = tmbuf->tm_min % 60;
  if (tmbuf->tm_min < 0) {
    tmbuf->tm_min += 60;
    tmbuf->tm_hour--;
  }
  day = tmbuf->tm_hour / 24;
  tmbuf->tm_hour= tmbuf->tm_hour % 24;
  if (tmbuf->tm_hour < 0) {
    tmbuf->tm_hour += 24;
    day--;
  }
  tmbuf->tm_year += tmbuf->tm_mon / 12;
  tmbuf->tm_mon %= 12;
  if (tmbuf->tm_mon < 0) {
    tmbuf->tm_mon += 12;
    tmbuf->tm_year--;
  }
  day += (tmbuf->tm_mday - 1);
  while (day < 0) {
    if(--tmbuf->tm_mon < 0) {
      tmbuf->tm_year--;
      tmbuf->tm_mon = 11;
    }
    day += _ytab[LEAPYEAR(YEAR0 + tmbuf->tm_year)][tmbuf->tm_mon];
  }
  while (day >= _ytab[LEAPYEAR(YEAR0 + tmbuf->tm_year)][tmbuf->tm_mon]) {
    day -= _ytab[LEAPYEAR(YEAR0 + tmbuf->tm_year)][tmbuf->tm_mon];
    if (++(tmbuf->tm_mon) == 12) {
      tmbuf->tm_mon = 0;
      tmbuf->tm_year++;
    }
  }
  tmbuf->tm_mday = day + 1;
  year = EPOCH_YR;
  if (tmbuf->tm_year < year - YEAR0) return (time_t) -999;
  seconds = 0;
  day = 0;                      // Means days since day 0 now
  overflow = 0;

  // Assume that when day becomes negative, there will certainly
  // be overflow on seconds.
  // The check for overflow needs not to be done for leapyears
  // divisible by 400.
  // The code only works when year (1970) is not a leapyear.
  tm_year = tmbuf->tm_year + YEAR0;

  if (TIME_MAX / 365 < tm_year - year) overflow=1;
  day = (tm_year - year) * 365;
  if (TIME_MAX - day < (tm_year - year) / 4 + 1) overflow|=2;
  day += (tm_year - year) / 4 + ((tm_year % 4) && tm_year % 4 < year % 4);
  day -= (tm_year - year) / 100 + ((tm_year % 100) && tm_year % 100 < year % 100);
  day += (tm_year - year) / 400 + ((tm_year % 400) && tm_year % 400 < year % 400);

  yday = month = 0;
  while (month < tmbuf->tm_mon) {
    yday += _ytab[LEAPYEAR(tm_year)][month];
    month++;
  }
  yday += (tmbuf->tm_mday - 1);
  if (day + yday < 0) overflow|=4;
  day += yday;

  tmbuf->tm_yday = yday;
  tmbuf->tm_wday = (day + 4) % 7;               // Day 0 was thursday (4)

  seconds = ((tmbuf->tm_hour * 60L) + tmbuf->tm_min) * 60L + tmbuf->tm_sec;

  if ((TIME_MAX - seconds) / SECS_DAY < day) overflow|=8;
  seconds += day * SECS_DAY;

  // Now adjust according to timezone and daylight saving time
  if (((kTimeZone > 0) && (TIME_MAX - kTimeZone < seconds)) || 
      ((kTimeZone < 0) && (seconds < -kTimeZone))) {
          overflow|=16;
  }
  seconds += kTimeZone;

  if (tmbuf->tm_isdst) {
    dst = _dstbias;
  } else {
    dst = 0;
  }

  if (dst > seconds) overflow|=32;        // dst is always non-negative
  seconds -= dst;

  if (overflow) return (time_t) overflow-2;

  if ((time_t) seconds != seconds) return (time_t) -1;
  return (time_t) seconds;
}

//credit: chatgpt
time_t mktime_simple(const struct tm *time) {
    const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    time_t epoch = 0;

    // Years since 1970
    for (int year = EPOCH_YR; year < time->tm_year + 1900; year++) {
        epoch += 365 * 24 * 3600; // Add days for each year
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) { // Leap year
            epoch += 24 * 3600; // Add a day for leap year
        }
    }

    // Months of the current year
    for (int month = 0; month < time->tm_mon; month++) {
        epoch += days_per_month[month] * 24 * 3600;
        if (month == 1 && (((time->tm_year + 1900) % 4 == 0 && (time->tm_year + 1900) % 100 != 0) || (time->tm_year + 1900) % 400 == 0)) {
            epoch += 24 * 3600; // Add a day for February in leap year
        }
    }

    // Days, hours, minutes, and seconds
    epoch += (time->tm_mday - 1) * 24 * 3600;
    epoch += time->tm_hour * 3600;
    epoch += time->tm_min * 60;
    epoch += time->tm_sec;

    return epoch;
}

struct tm *gmtime(const time_t *timer, struct tm *tmbuf) {
  time_t time = *timer;
  unsigned long dayclock, dayno;
  int year = EPOCH_YR;

  dayclock = (unsigned long) time % SECS_DAY;
  dayno = (unsigned long) time / SECS_DAY;

  tmbuf->tm_sec = dayclock % 60;
  tmbuf->tm_min = (dayclock % 3600) / 60;
  tmbuf->tm_hour = dayclock / 3600;
  tmbuf->tm_wday = (dayno + 4) % 7; // Day 0 was a thursday
  while (dayno >= (unsigned long) YEARSIZE(year)) {
    dayno -= YEARSIZE(year);
    year++;
  }
  tmbuf->tm_year = year - YEAR0;
  tmbuf->tm_yday = dayno;
  tmbuf->tm_mon = 0;
  while (dayno >= (unsigned long) _ytab[LEAPYEAR(year)][tmbuf->tm_mon]) {
    dayno -= _ytab[LEAPYEAR(year)][tmbuf->tm_mon];
    tmbuf->tm_mon++;
  }
  tmbuf->tm_mday = dayno + 1;
  tmbuf->tm_isdst = 0;
  return tmbuf;
}

struct tm *localtime(const time_t *timer) {
  time_t t;
  struct tm tmbuf;
  
  t = *timer - kTimeZone;
  return gmtime(&t, &tmbuf);
}

struct tm *localtime_r(const time_t *timer, struct tm *tmbuf) {
  time_t t;

  t = *timer - kTimeZone;
  return gmtime(&t, tmbuf);
}


void kwait(uint64_t msToWait)
{
    uint64_t endTicks = kTicksSinceStart + (msToWait * kTicksPerSecond) / 1000;
    while (kTicksSinceStart < endTicks)
    {
        __asm__ volatile ("pause\n");
    }
}

void __attribute__((noinline))waitTicks(int TicksToWait)
{
    //printf("ttw=%u",ttw);
	__asm__ __volatile__ ("sti\n");
    if (TicksToWait<=0)
        return;
    if (TicksToWait>5000)
        printd(DEBUG_EXCEPTIONS,"waitTicks: Excessive ticks value %u\n",TicksToWait);
    do
    {
        __asm("sti\nhlt\n");
        TicksToWait--;
    } while (TicksToWait>0);
    return;
}

void wait(uint64_t msToWait)
{
	__asm__("sti\n");
	uint64_t ticksToWait = msToWait/MS_PER_TICK; 
	//CLR 12/03/2024: Added in case request is to wait less than MS_PER_TICK
	if (ticksToWait==0)
		ticksToWait=1;
    waitTicks(ticksToWait);
}
