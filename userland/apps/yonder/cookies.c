// cookies.c — the cookie file (cookies.h).

#include "cookies.h"

#include "os64/date.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"
#include "storage.h"
#include "ticker.h"

// The largest file read: the longest a jar writes, so no site can set
// cookies enough to make the file unreadable; a bigger one is not
// yonder's. A read takes what the file holds, not this.
#define FILE_MAX WAY_JAR_TEXT_MAX
// A write reads the file again when another window published between its
// read and its rename, up to this many reads in all.
#define WRITE_TRIES 3

struct yonder_cookies {
    way_jar_t *jar;
    yonder_cookies_mode_t mode;
    char path[OS64_PATH_MAX];       // "" when the profile gives none
    uint64_t identity;              // the file's when this window last read or wrote it, 0 for none
    uint64_t saved;                 // the jar's change the file has heard up to (way_jar_changes)
    uint64_t wrote_ms;              // when the last write was tried, 0 for never
    bool failed;                    // ...and it failed
    bool swept;                     // the dead's temps are gone
    uint32_t temps;                 // names the next temp
    char problem[OS64_PATH_MAX + 96];
};

bool yonder_cookies_mode_of(const char *value, yonder_cookies_mode_t *out)
{
    static const char *const kWords[] = {"on", "session", "off"};
    for (int i = 0; i < 3; i++)
        if (os64_streq_nocase(value, kWords[i])) {
            *out = (yonder_cookies_mode_t)i;
            return true;
        }
    return false;
}

const char *yonder_cookies_mode_word(yonder_cookies_mode_t mode)
{
    return mode == YONDER_COOKIES_OFF ? "off" : mode == YONDER_COOKIES_SESSION ? "session" : "on";
}

// The wall clock a cookie's expiry is judged by, in UTC seconds.
static int64_t now_utc(void)
{
    os64_time_t t;
    return os64_time(&t) == 0 ? t.epoch : 0;
}

// A stat's identity: the size and the modification time, never 0 for a
// file that is there (storage.c's file_identity, for the same reason).
static uint64_t identity_of(const char *path)
{
    os64_dirent_t entry;
    if (os64_stat(path, &entry) < 0)
        return 0;
    uint64_t identity = entry.mtime * 1000003u ^ entry.size;
    return identity != 0 ? identity : 1;
}

// The file's text as it stands: true with *text NULL for none; false, with
// the problem said, when it is there and cannot be read whole.
static bool read_file(yonder_cookies_t *k, uint8_t **text, size_t *len)
{
    *text = NULL;
    *len = 0;
    os64_dirent_t entry;
    if (os64_stat(k->path, &entry) < 0)
        return true;
    // A folder by the file's name is somebody's, so it is never removed
    // here: said, with what to do, since "cannot be read" sends a person
    // looking for a damaged file.
    if (entry.flags & OS64_DE_DIR) {
        os64_snprintf(k->problem, sizeof(k->problem),
                      "Cookies are not kept: %s is a folder, and yonder keeps them in a file of that name.",
                      k->path);
        return false;
    }
    os64_slurp_status_t status = os64_slurp(k->path, FILE_MAX, text, len);
    if (status == OS64_SLURP_OK || (status == OS64_SLURP_NO_FILE && os64_stat(k->path, &entry) < 0))
        return true;
    os64_snprintf(k->problem, sizeof(k->problem),
                  status == OS64_SLURP_NO_MEMORY ? "Cookies are not kept: no memory to read %s."
                                                 : "Cookies are not kept: %s cannot be read.",
                  k->path);
    return false;
}

// The jar follows the file as it stands (way_jar_follow).
static void follow(yonder_cookies_t *k)
{
    uint64_t identity = identity_of(k->path);
    uint8_t *text;
    size_t len;
    if (!read_file(k, &text, &len))
        return;
    way_jar_read_t report;
    way_jar_follow(k->jar, (const char *)text, len, now_utc(), &report);
    os64_free(text);
    // A follow short of memory is not this file followed: the identity
    // stays as it was, so the next look reads it again.
    if (!report.short_of_memory)
        k->identity = identity;
    if (report.refused > 0)
        os64_debug_log("yonder: lines of the cookie file were not read (malformed, or refused by the jar's rules)");
}

typedef enum { PUBLISHED, MOVED, NOT_PUBLISHED } Published;

// The file published with `text` in it, by temp and rename — unless
// `check` and the file's identity is no longer `read` once the temp is
// written (another window published since this one read; 0 is "not
// there"), which is MOVED, with the temp gone. NOT_PUBLISHED with the
// problem said.
static Published publish(yonder_cookies_t *k, const char *text, size_t len, bool check, uint64_t read)
{
    const char *slash = os64_strrchr(k->path, '/');
    char dir[OS64_PATH_MAX];
    size_t dlen = slash != NULL ? (size_t)(slash - k->path) : 0;
    os64_memcpy(dir, k->path, dlen);
    dir[dlen] = '\0';
    char temp[OS64_PATH_MAX];
    if (dlen == 0 || !yonder_profile_make_dirs(dir)) {
        os64_snprintf(k->problem, sizeof(k->problem), "Cookies are not kept: %s cannot be made.", dir);
        return NOT_PUBLISHED;
    }
    if (!yonder_profile_temp_write(&k->temps, k->path, text, len, temp, sizeof(temp))) {
        os64_snprintf(k->problem, sizeof(k->problem), "Cookies are not kept: %s cannot be written.", k->path);
        return NOT_PUBLISHED;
    }
    if (check && identity_of(k->path) != read) {
        (void)os64_unlink(temp);
        return MOVED;
    }
    if (os64_rename(temp, k->path) != 0) {
        (void)os64_unlink(temp);
        os64_snprintf(k->problem, sizeof(k->problem), "Cookies are not kept: %s cannot be written.", k->path);
        return NOT_PUBLISHED;
    }
    return PUBLISHED;
}

// READ, MERGE, WRITE (cookies.h § Two windows, one file): the jar's changes
// up to now over the file as it stands. A file that is there and cannot be
// read is never written over. True when the file has heard them.
static bool write_now(yonder_cookies_t *k)
{
    if (k->path[0] == '\0')
        return false;
    if (!k->swept) {
        k->swept = true;
        yonder_profile_sweep(k->path);
    }
    uint64_t through = way_jar_changes(k->jar);
    int64_t now = now_utc();
    for (int tries = 1;; tries++) {
        uint64_t before = identity_of(k->path);
        uint8_t *text;
        size_t len;
        if (!read_file(k, &text, &len))
            return false;
        way_jar_t *merged = way_jar_new();
        bool whole = false;         // the file read and the changes laid over it, all of both
        if (merged != NULL) {
            way_jar_read_t report;
            way_jar_read(merged, (const char *)text, len, now, &report);
            whole = !report.short_of_memory && way_jar_overlay(merged, k->jar, through, now);
        }
        os64_free(text);
        size_t out_len = 0;
        char *out = whole ? way_jar_text(merged, now, &out_len) : NULL;
        way_jar_free(merged);
        if (out == NULL) {
            os64_snprintf(k->problem, sizeof(k->problem), "Cookies are not kept: no memory to write %s.", k->path);
            return false;
        }
        // Another window published since the read: its file is read again,
        // up to WRITE_TRIES, the last written whatever moved.
        Published done = publish(k, out, out_len, tries < WRITE_TRIES, before);
        if (done == MOVED) {
            os64_free(out);
            continue;
        }
        bool ok = done == PUBLISHED;
        if (ok) {
            way_jar_saved(k->jar, through);
            way_jar_follow(k->jar, out, out_len, now, NULL);
            k->identity = identity_of(k->path);
            k->problem[0] = '\0';
        }
        os64_free(out);
        return ok;
    }
}

// The file emptied: written with no cookie in it, its comment lines
// alone, and the temps a window that died left beside it, which hold
// cookies too, gone. False with the problem said.
static bool empty_file(yonder_cookies_t *k)
{
    if (k->path[0] == '\0')
        return false;
    yonder_profile_sweep(k->path);
    way_jar_t *none = way_jar_new();
    size_t len = 0;
    char *text = none != NULL ? way_jar_text(none, 0, &len) : NULL;
    way_jar_free(none);
    if (text == NULL)
        os64_snprintf(k->problem, sizeof(k->problem), "Cookies are not emptied: no memory to write %s.", k->path);
    bool ok = text != NULL && publish(k, text, len, false, 0) == PUBLISHED;
    os64_free(text);
    if (ok)
        k->identity = identity_of(k->path);
    return ok;
}

// The file forgotten: emptied when it is there, and the temps a dead window
// left swept either way, since a crash between a first save's temp and its
// rename leaves every login in a temp with no file beside it. False with
// the problem said; true with no profile path, nothing being kept.
static bool forget_file(yonder_cookies_t *k)
{
    if (k->path[0] == '\0')
        return true;
    if (identity_of(k->path) != 0)
        return empty_file(k);
    yonder_profile_sweep(k->path);
    return true;
}

yonder_cookies_t *yonder_cookies_new(way_jar_t *jar, const char *profile, yonder_cookies_mode_t mode)
{
    if (jar == NULL)
        return NULL;
    yonder_cookies_t *k = os64_calloc(1, sizeof(*k));
    if (k == NULL)
        return NULL;
    k->jar = jar;
    k->mode = YONDER_COOKIES_SESSION;
    int n = profile != NULL && profile[0] == '/'
                ? os64_snprintf(k->path, sizeof(k->path), "%s/" YONDER_COOKIES_FILE, profile)
                : -1;
    if (n <= 0 || (size_t)n + YONDER_PROFILE_TEMP_SUFFIX_MAX >= sizeof(k->path)) {
        k->path[0] = '\0';
        os64_snprintf(k->problem, sizeof(k->problem), "Cookies are not kept: the profile folder's path is not one.");
    }
    yonder_cookies_set_mode(k, mode);
    // A SESSION yonder that never reached yonder_cookies_flush (a crash)
    // left the file as it was: emptied now, as that flush would have.
    if (mode == YONDER_COOKIES_SESSION)
        (void)forget_file(k);               // a failure is said as the problem
    return k;
}

void yonder_cookies_free(yonder_cookies_t *k)
{
    os64_free(k);
}

yonder_cookies_mode_t yonder_cookies_mode(const yonder_cookies_t *k)
{
    return k != NULL ? k->mode : YONDER_COOKIES_SESSION;
}

void yonder_cookies_set_mode(yonder_cookies_t *k, yonder_cookies_mode_t mode)
{
    if (k == NULL)
        return;
    yonder_cookies_mode_t was = k->mode;
    k->mode = mode;
    if (mode == YONDER_COOKIES_OFF) {
        way_jar_refuse(k->jar, true);
        way_jar_clear(k->jar);
        k->saved = way_jar_changes(k->jar);
        (void)forget_file(k);               // a failure is said as the problem
        return;
    }
    way_jar_refuse(k->jar, false);
    // To ON from either: what the file holds now, and every change made
    // while it was not written is owed to it (k->saved is where it was).
    if (mode == YONDER_COOKIES_ON && was != YONDER_COOKIES_ON && k->path[0] != '\0')
        follow(k);
}

uint64_t yonder_cookies_due(const yonder_cookies_t *k)
{
    if (k == NULL || k->mode != YONDER_COOKIES_ON || k->path[0] == '\0' || way_jar_changes(k->jar) <= k->saved)
        return YONDER_NEVER;
    if (k->wrote_ms == 0)
        return 0;
    return k->wrote_ms + (k->failed ? YONDER_COOKIES_RETRY_MS : YONDER_COOKIES_WRITE_MS);
}

void yonder_cookies_turn(yonder_cookies_t *k, uint64_t now_ms)
{
    uint64_t due = yonder_cookies_due(k);
    if (due == YONDER_NEVER || due > now_ms)
        return;
    uint64_t through = way_jar_changes(k->jar);
    k->wrote_ms = now_ms != 0 ? now_ms : 1;
    k->failed = !write_now(k);
    if (!k->failed)
        k->saved = through;
}

void yonder_cookies_flush(yonder_cookies_t *k)
{
    if (k == NULL)
        return;
    // SESSION: nothing this window heard outlives it, and neither does
    // what the file held when it left ON — it was kept until now so that
    // ticking the box again lost nothing.
    if (k->mode == YONDER_COOKIES_SESSION) {
        (void)forget_file(k);
        return;
    }
    if (yonder_cookies_due(k) == YONDER_NEVER)
        return;
    uint64_t through = way_jar_changes(k->jar);
    if (write_now(k))
        k->saved = through;
}

void yonder_cookies_look(yonder_cookies_t *k)
{
    if (k == NULL || k->mode != YONDER_COOKIES_ON || k->path[0] == '\0' || identity_of(k->path) == k->identity)
        return;
    follow(k);
}

int64_t yonder_cookies_clear(yonder_cookies_t *k)
{
    if (k == NULL)
        return -1;
    int64_t had = way_jar_count(k->jar);
    way_jar_clear(k->jar);
    k->saved = way_jar_changes(k->jar);
    return forget_file(k) ? had : -1;
}

const char *yonder_cookies_problem(const yonder_cookies_t *k)
{
    return k != NULL && k->problem[0] != '\0' ? k->problem : NULL;
}

const char *yonder_cookies_path(const yonder_cookies_t *k)
{
    return k != NULL ? k->path : "";
}
