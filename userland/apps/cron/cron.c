// cron — run things later, without anybody waiting for them.
//
// THE NAME is Greek: chronos, time. The program is Ken Thompson's, from
// Version 7 (1979) — one process that woke every minute, read one system-wide
// crontab, ran whatever matched, and went back to sleep. Dumb, and reliable
// for exactly that reason: re-reading the file each minute meant editing it
// was live, with no daemon to restart and no state to get out of step. Paul
// Vixie's 1987 rewrite is the one nearly everybody has actually run since,
// and it contributed the `@` shorthands — including `@reboot`, which is the
// hook os64 wanted first.
//
// WHAT OS64 KEEPS AND WHAT IT DOES NOT (the argument, since this is a
// deliberate divergence — DIVERGENCES.md § cron):
//
//   * The five-field syntax is GONE. `0 3 * * 1` is compression for a file
//     that had to be parsed by a tiny C program and typed on a teletype, and
//     it has cost every Unix user since: nobody reads it right the first
//     time, and day-of-month with day-of-week is an OR rather than an AND,
//     which surprises people forever. os64 spells the schedule in words.
//   * A crontab is a LIST, not a settings file, so it is line-oriented like
//     /etc/hosts rather than `key = value` like logd.conf — and like hosts it
//     MERGES down the config ladder (Chris's 2026-08-22 ruling): /home/crontab
//     layers ON TOP of /etc/crontab instead of replacing it. Your jobs sit
//     beside the system's rather than erasing them.
//   * The clock is UTC, always, read straight from the kernel's epoch rather
//     than through anybody's TZ. Vixie's cron runs in local time and its
//     DST behaviour — an hour that never happens, an hour that happens twice
//     — is a genuine unsolved mess that he documented and worked around. A
//     UTC cron cannot have that bug. The cost is that `daily 03:00` is 03:00
//     UTC, and this file says so out loud rather than being clever.
//   * There is no crontab(1). That command exists because Unix has per-user
//     spools and permissions to enforce; os64 has neither. You edit the file.
//
// THE SCHEDULE VOCABULARY, left to right:
//
//     @reboot            /bin/testrun
//     every 5m           /bin/sync
//     every 2h           /bin/ntp
//     daily 03:00        /bin/os64get -a yogi
//     weekly sun 04:30   /bin/logrotate
//
// Everything after the schedule is the COMMAND, and it is handed to
// `husk -c`, so a crontab line is an ordinary command line: pipelines,
// redirection and `$()` all work because the shell is doing what it always
// does. That is Vixie's `/bin/sh -c` and it is right for the same reason —
// cron's job is to know WHEN, not to grow a second parser for WHAT.
//
// `every N` IS CALENDAR MATCHING, NOT AN INTERVAL, and the difference is
// worth stating because it shows at the hour boundary. `every 5m` fires when
// the minute divides by 5 — :00, :05, :10 — not five minutes after whenever
// cron happened to start. That makes the whole schedule STATELESS: due-ness
// is a question about the clock alone, so nothing has to be remembered
// across a re-read, and a restarted cron picks up exactly where a running one
// would be. It is also why `every 7m` is lumpy (it fires at :00, :07 … :56,
// then again at :00): 60 does not divide by 7, and no cron has ever pretended
// otherwise. Divisors of 60 behave the way you expect.
//
// WHAT IT DOES NOT WAIT FOR: a job is spawned and never waited on. Finished
// jobs are collected with os64_reap() at the top of each minute, which is
// what keeps a slow job from stalling the schedule and keeps corpses from
// accumulating — the same trick husk uses for `&`. The consequence, stated
// rather than hidden: a job that runs longer than its own period will be
// started again while the first is still going. cron has always done that.

#include <stdarg.h>

#include "os64/os64.h"
#include "os64/conf.h"   // the config search path: crontab is found, not hardcoded

#define CRON_MAX_JOBS   32
#define CRON_CMD_MAX    256
#define CRON_LINE_MAX   512

typedef enum {
    SCHED_REBOOT,       // once, when cron starts
    SCHED_EVERY_MIN,    // minute % n == 0
    SCHED_EVERY_HOUR,   // hour % n == 0 && minute == 0
    SCHED_DAILY,        // hour == h && minute == m
    SCHED_WEEKLY        // weekday == w && hour == h && minute == m
} sched_kind_t;

typedef struct {
    sched_kind_t kind;
    int32_t n;          // `every N`
    int32_t hour;
    int32_t minute;
    int32_t weekday;    // 0 = Sunday
    char    command[CRON_CMD_MAX];
} cron_job_t;

static cron_job_t s_jobs[CRON_MAX_JOBS];
static int32_t    s_njobs;
static bool       s_verbose;

// ── saying things ───────────────────────────────────────────────────────────
// A daemon with no terminal talks to the log. Job outcomes and parse
// complaints both go there, because both are things you go LOOKING for after
// the fact — "did the 3am job run, and what did it say" is the entire
// question a cron log exists to answer.
static void cron_log(const char *fmt, ...)
{
    char line[CRON_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    os64_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    os64_debug_log(line);
    if (s_verbose)
        os64_printf("%s\n", line);
}

// ── the parser ──────────────────────────────────────────────────────────────

static const char *kDayNames[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

// Advance past blanks and return the token's end. Returns NULL when the line
// is spent, so a caller can tell "no more words" from "an empty word".
static char *next_token(char **p)
{
    char *s = *p;
    while (is_blank(*s))
        s++;
    if (*s == '\0')
        return NULL;
    char *tok = s;
    while (*s != '\0' && !is_blank(*s))
        s++;
    if (*s != '\0')
        *s++ = '\0';
    *p = s;
    return tok;
}

// "HH:MM" -> hour, minute. Refuses anything it does not fully understand:
// a schedule half-read is a job that runs at the wrong time, which is worse
// than a job that does not run and says why.
static bool parse_hhmm(const char *s, int32_t *hour, int32_t *minute)
{
    int32_t h = 0, m = 0, digits = 0;
    while (*s >= '0' && *s <= '9') { h = h * 10 + (*s++ - '0'); digits++; }
    if (digits == 0 || digits > 2 || *s != ':')
        return false;
    s++;
    digits = 0;
    while (*s >= '0' && *s <= '9') { m = m * 10 + (*s++ - '0'); digits++; }
    if (digits != 2 || *s != '\0')
        return false;
    if (h > 23 || m > 59)
        return false;
    *hour = h;
    *minute = m;
    return true;
}

// "5m" / "2h" -> count and unit.
static bool parse_every(const char *s, sched_kind_t *kind, int32_t *n)
{
    int32_t v = 0, digits = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); digits++; }
    if (digits == 0 || v <= 0)
        return false;
    if (s[0] == 'm' && s[1] == '\0') {
        if (v > 59) return false;      // above an hour, say it in hours
        *kind = SCHED_EVERY_MIN;
    } else if (s[0] == 'h' && s[1] == '\0') {
        if (v > 23) return false;      // above a day, say it with `daily`
        *kind = SCHED_EVERY_HOUR;
    } else {
        return false;
    }
    *n = v;
    return true;
}

static int32_t day_index(const char *s)
{
    for (int32_t i = 0; i < 7; i++)
        if (os64_streq_nocase(s, kDayNames[i]))
            return i;
    return -1;
}

// One crontab line into the table. `where` names the file for complaints —
// with two files merged, "line 3" alone would be ambiguous.
static void parse_line(char *line, const char *where, int32_t lineno)
{
    // A '#' ends the line, wherever it appears. There is no escape for a
    // literal '#' and there does not need to be: the shell has quoting, and
    // this file is a list of schedules, not a place to write essays.
    for (char *p = line; *p != '\0'; p++) {
        if (*p == '#' || *p == '\n' || *p == '\r') { *p = '\0'; break; }
    }

    char *cursor = line;
    char *verb = next_token(&cursor);
    if (verb == NULL)
        return;                        // blank or comment-only: not an error

    cron_job_t job = {0};

    if (os64_streq_nocase(verb, "@reboot")) {
        job.kind = SCHED_REBOOT;
    } else if (os64_streq_nocase(verb, "every")) {
        char *spec = next_token(&cursor);
        if (spec == NULL || !parse_every(spec, &job.kind, &job.n)) {
            cron_log("cron: %s:%ld: `every` wants a count and a unit, like `every 5m` or `every 2h`",
                     where, (long)lineno);
            return;
        }
    } else if (os64_streq_nocase(verb, "daily")) {
        char *at = next_token(&cursor);
        if (at == NULL || !parse_hhmm(at, &job.hour, &job.minute)) {
            cron_log("cron: %s:%ld: `daily` wants a time, like `daily 03:00`",
                     where, (long)lineno);
            return;
        }
        job.kind = SCHED_DAILY;
    } else if (os64_streq_nocase(verb, "weekly")) {
        char *day = next_token(&cursor);
        char *at  = (day != NULL) ? next_token(&cursor) : NULL;
        if (day == NULL || at == NULL ||
            (job.weekday = day_index(day)) < 0 ||
            !parse_hhmm(at, &job.hour, &job.minute)) {
            cron_log("cron: %s:%ld: `weekly` wants a day and a time, like `weekly sun 04:30`",
                     where, (long)lineno);
            return;
        }
        job.kind = SCHED_WEEKLY;
    } else {
        cron_log("cron: %s:%ld: don't know the schedule `%s` — expected @reboot, every, daily or weekly",
                 where, (long)lineno, verb);
        return;
    }

    // The rest of the line, verbatim, is the command. Leading blanks are
    // skipped; nothing else is touched, because husk is the thing that gets
    // to interpret it.
    while (is_blank(*cursor))
        cursor++;
    if (*cursor == '\0') {
        cron_log("cron: %s:%ld: a schedule with no command", where, (long)lineno);
        return;
    }
    if (os64_strcopy(job.command, sizeof(job.command), cursor) >= sizeof(job.command)) {
        cron_log("cron: %s:%ld: command longer than %ld bytes — ignored rather than truncated",
                 where, (long)lineno, (long)sizeof(job.command));
        return;
    }

    if (s_njobs >= CRON_MAX_JOBS) {
        cron_log("cron: %s:%ld: more than %ld jobs — ignored",
                 where, (long)lineno, (long)CRON_MAX_JOBS);
        return;
    }
    s_jobs[s_njobs++] = job;
}

static void read_crontab_file(const char *path)
{
    int64_t h = os64_open(path, "r");
    if (h < 0)
        return;

    char line[CRON_LINE_MAX];
    int32_t lineno = 0;
    while (os64_readline((int32_t)h, line, sizeof(line)) == 1)
        parse_line(line, path, ++lineno);
    os64_close((int32_t)h);
}

// Every crontab on the ladder, in ladder order — /home's first, then /etc's.
// MERGED rather than first-hit-wins, which is the `hosts` treatment and for
// the same reason: this is a list of things to do, not a setting with one
// right value, so your jobs should join the system's rather than replace them.
static void load_crontabs(void)
{
    s_njobs = 0;

    size_t from = 0;
    char path[OS64_CONF_PATH_MAX];
    int64_t next;
    while ((next = os64_conf_find_from("crontab", from, path, sizeof(path))) >= 1) {
        read_crontab_file(path);
        from = (size_t)next;
    }
}

// ── the clock ───────────────────────────────────────────────────────────────

static bool job_is_due(const cron_job_t *j, const os64_date_t *now)
{
    switch (j->kind) {
        case SCHED_REBOOT:      return false;    // fired once, at startup
        case SCHED_EVERY_MIN:   return (now->minute % j->n) == 0;
        case SCHED_EVERY_HOUR:  return (now->hour % j->n) == 0 && now->minute == 0;
        case SCHED_DAILY:       return now->hour == j->hour && now->minute == j->minute;
        case SCHED_WEEKLY:      return now->weekday == j->weekday &&
                                       now->hour == j->hour && now->minute == j->minute;
    }
    return false;
}

// UTC, deliberately: the raw kernel epoch, broken down without anybody's
// timezone. See the header — a cron that runs in local time inherits DST's
// missing and duplicated hours, and this one cannot.
static bool utc_now(os64_date_t *out)
{
    os64_time_t t = {0};
    if (os64_time(&t) < 0)
        return false;
    os64_date_from_epoch(t.epoch, out);
    return true;
}

// ── running things ──────────────────────────────────────────────────────────

// Through husk, so a crontab line is an ordinary command line. cron does not
// wait: os64_reap() collects finished jobs at the top of each minute, so a
// long job delays nothing and leaves no corpse.
static void run_job(const cron_job_t *j)
{
    char *const argv[] = { "husk", "-c", (char *)j->command, 0 };
    int64_t pid = os64_spawn("/bin/husk", argv);
    if (pid < 0)
        cron_log("cron: could not start `%s` (spawn failed: %ld)", j->command, (long)pid);
    else
        cron_log("cron: started [%ld] %s", (long)pid, j->command);
}

// Collect whatever finished since the last pass. A job's exit status is
// reported EVERY time, not just when it is bad: the whole reason to look at a
// cron log is to find out what happened while nobody was watching, and
// "nothing was printed" is not an answer you can act on.
static void reap_finished(void)
{
    int32_t code;
    int64_t pid;
    while ((pid = os64_reap(&code)) > 0)
        cron_log("cron: finished [%ld] exit %ld", (long)pid, (long)code);
}

int main(int argc, char **argv)
{
    bool once = false;
    const os64_optspec_t specs[] = {
        {'v', "verbose", false, "also print what is happening to stdout", .flag = &s_verbose},
        {'1', "once", false, "run what is due right now, then exit (does not loop)",
         .flag = &once}
    };
    os64_args_t args = {0};
    os64_args_init(&args, argc, argv, specs, 2);
    args.about = "Run commands on a schedule.";
    args.details = "Reads `crontab` from the config search path, MERGING every copy it "
                   "finds (/home's over /etc's) the way hosts does. Schedules are "
                   "@reboot, `every 5m`, `every 2h`, `daily 03:00` and `weekly sun 04:30`; "
                   "everything after the schedule is handed to husk -c. Times are UTC. "
                   "The file is re-read every minute, so an edit takes effect without a "
                   "restart.";
    int32_t parsed = os64_args_parse(&args, "cron [-v] [-1]", NULL, 0);
    if (parsed == OS64_ARG_HELP) return 0;
    if (parsed < 0) return 2;

    load_crontabs();
    cron_log("cron: started, %ld job(s), times are UTC", (long)s_njobs);

    // @reboot, before the loop: these are what the system wanted done once it
    // was up, and "up" is now.
    for (int32_t i = 0; i < s_njobs; i++)
        if (s_jobs[i].kind == SCHED_REBOOT)
            run_job(&s_jobs[i]);

    if (once) {
        os64_date_t now;
        if (utc_now(&now))
            for (int32_t i = 0; i < s_njobs; i++)
                if (job_is_due(&s_jobs[i], &now))
                    run_job(&s_jobs[i]);
        return 0;
    }

    // THE GAIT IS V7's: wake near the top of each minute, re-read the file,
    // run what matches. Re-reading is what makes an edit live, and it costs
    // nothing at this rate — the SysV-era optimisation of sleeping until the
    // next event and watching the spool's mtime was for machines with forty
    // crontabs and one job a day.
    //
    // `last_minute` is the guard against firing twice: the sleep below aims at
    // the top of the minute and can land slightly early or late, so due-ness
    // is checked against a minute we have not already served.
    int32_t last_minute = -1;
    for (;;) {
        os64_date_t now;
        if (!utc_now(&now)) {
            os64_sleep(60 * 1000);
            continue;
        }

        if (now.minute != last_minute) {
            last_minute = now.minute;
            reap_finished();
            load_crontabs();
            for (int32_t i = 0; i < s_njobs; i++)
                if (job_is_due(&s_jobs[i], &now))
                    run_job(&s_jobs[i]);
        }

        // Sleep to just past the top of the next minute. Aiming AT the second
        // boundary would make an early wake-up spin through this loop; a
        // second of overshoot costs nothing and lands squarely inside the
        // minute we mean to serve.
        uint64_t ms = (uint64_t)(60 - now.second) * 1000 + 1000;
        os64_sleep(ms);
    }
}
