#ifndef YONDER_JOBS_H
#define YONDER_JOBS_H

// What kind of job a reaped input is: the window's pool runs pages,
// pictures, style sheets, scripts, fonts, a script's requests and
// downloads, and each is let go by its own release. Every job's input
// begins with its kind.
enum {
    YONDER_JOB_TRIP = 1,
    YONDER_JOB_PICTURE = 2,
    YONDER_JOB_SHEET = 3,
    YONDER_JOB_SCRIPT = 4,
    YONDER_JOB_FONT = 5,
    YONDER_JOB_FETCH = 6,
    YONDER_JOB_DOWNLOAD = 7,
};

#endif
