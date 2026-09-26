#ifndef YONDER_JOBS_H
#define YONDER_JOBS_H

// What kind of job a reaped input is: the window's pool runs pages and
// pictures, and each is let go by its own release. Every job's input
// begins with its kind.
enum { YONDER_JOB_TRIP = 1, YONDER_JOB_PICTURE = 2 };

#endif
