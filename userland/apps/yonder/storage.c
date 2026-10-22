// storage.c — where a page's storage is kept (storage.h).

#include "storage.h"
#include "os64/date.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/procfs.h"
#include "os64/slurp.h"
#include "os64/str.h"

// The largest store file read. libdom refuses a bigger image as one it did
// not write, so this is only how much is read before that answer.
#define STORE_MAX ((size_t)32 * 1024 * 1024)

// One origin's sessionStorage in this window: its image, and how many
// commits have changed it (its identity).
typedef struct Session {
    struct Session *next;
    char *origin;
    void *image;                    // os64_malloc'd, NULL for none
    size_t length;
    uint64_t generation;
} Session;

struct yonder_storage {
    char profile[OS64_PATH_MAX];
    Session *sessions;
    uint64_t generations;           // the last generation handed out
    uint32_t temps;                 // names the next temp file
    bool swept;                     // the dead's temps and locks are gone (sweep)
    char problem[OS64_PATH_MAX + 96];
};

static void sweep(yonder_storage_t *st);

yonder_storage_t *yonder_storage_new(const char *profile)
{
    yonder_storage_t *st = os64_calloc(1, sizeof(*st));
    if (st == NULL)
        return NULL;
    os64_strcopy(st->profile, sizeof(st->profile), profile != NULL ? profile : "");
    return st;
}

static void sessions_free(yonder_storage_t *st)
{
    while (st->sessions != NULL) {
        Session *s = st->sessions;
        st->sessions = s->next;
        os64_free(s->origin);
        os64_free(s->image);
        os64_free(s);
    }
}

void yonder_storage_free(yonder_storage_t *st)
{
    if (st == NULL)
        return;
    sessions_free(st);
    os64_free(st);
}

const char *yonder_storage_problem(const yonder_storage_t *st)
{
    return st != NULL && st->problem[0] != '\0' ? st->problem : NULL;
}

bool yonder_storage_file_name(const char *origin, char *out, size_t cap)
{
    static const char kHex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const char *at = origin; *at != '\0'; at++) {
        unsigned char c = (unsigned char)*at;
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '-';
        if (used + (keep ? 1 : 3) >= cap)
            return false;
        if (keep) {
            out[used++] = (char)c;
        } else {
            out[used++] = '%';
            out[used++] = kHex[c >> 4];
            out[used++] = kHex[c & 15];
        }
    }
    if (used >= cap)
        return false;
    out[used] = '\0';
    // "." and ".." are names a folder already has.
    return used != 0 && !os64_streq(out, ".") && !os64_streq(out, "..");
}

// A temp file is its store's path and "+<task>.<count>.new", and a store's
// lock its path and "+lock". The '+' is a byte a store's name never holds
// (it is %2B there), so no origin's store is another's temp or lock. The
// room they need is kept free in every store path. TEMP_FORMAT is the one
// spelling of a temp's name: temp_write makes names with it, and temp_task
// knows one only by making it again.
#define TEMP_FORMAT "%s+%lu.%u.new"
#define TEMP_SUFFIX_MAX (1 + 20 + 1 + 10 + 4)
#define LOCK_SUFFIX "+lock"

// <profile>/storage, and the store file for `origin` in it.
static bool store_dir(const yonder_storage_t *st, char *out, size_t cap)
{
    int n = os64_snprintf(out, cap, "%s/storage", st->profile);
    return st->profile[0] == '/' && n > 0 && (size_t)n < cap;
}

// False when the profile is not a full path, or the origin's file name
// and its temp's would not fit a path.
static bool store_path(const yonder_storage_t *st, const char *origin, char *out, size_t cap)
{
    char name[OS64_PATH_MAX];
    if (!store_dir(st, out, cap) || !yonder_storage_file_name(origin, name, sizeof(name)))
        return false;
    size_t used = os64_strlen(out);
    int n = os64_snprintf(out + used, cap - used, "/%s", name);
    return n > 0 && (size_t)n + TEMP_SUFFIX_MAX < cap - used;
}

// Why the store for `origin` has no path, for the status line.
static void say_no_path(yonder_storage_t *st, const char *origin)
{
    char dir[OS64_PATH_MAX];
    if (st->profile[0] != '/')
        os64_snprintf(st->problem, sizeof(st->problem),
                      "Page storage is not kept: the profile folder is not a full path (%s).", st->profile);
    else if (!store_dir(st, dir, sizeof(dir)))
        os64_snprintf(st->problem, sizeof(st->problem),
                      "Page storage is not kept: the profile folder's path is too long (%s).", st->profile);
    else
        os64_snprintf(st->problem, sizeof(st->problem),
                      "Page storage is not kept for %s: its address is too long to name a file.", origin);
}

// Every folder of `path` that is missing, made, parents first. False when
// one cannot be, or a name on the way is a file.
static bool make_dirs(const char *path)
{
    char prefix[OS64_PATH_MAX];
    size_t length = os64_strlen(path);
    if (length == 0 || length >= sizeof(prefix) || path[0] != '/')
        return false;
    for (size_t at = 1; at <= length; at++) {
        if (path[at] != '/' && path[at] != '\0')
            continue;
        os64_memcpy(prefix, path, at);
        prefix[at] = '\0';
        os64_dirent_t entry;
        if (os64_stat(prefix, &entry) == 0) {
            if (!(entry.flags & OS64_DE_DIR))
                return false;
            continue;
        }
        if (os64_mkdir(prefix) < 0 && (os64_stat(prefix, &entry) < 0 || !(entry.flags & OS64_DE_DIR)))
            return false;
    }
    return true;
}

static Session *session_of(yonder_storage_t *st, const char *origin)
{
    for (Session *s = st->sessions; s != NULL; s = s->next)
        if (os64_streq(s->origin, origin))
            return s;
    return NULL;
}

// A stat's identity: the size and the modification time, never 0 for a
// file that is there. Two writes of one size within one second of the
// clock read alike; the store's next change is seen.
static uint64_t file_identity(const os64_dirent_t *entry)
{
    uint64_t identity = entry->mtime * 1000003u ^ entry->size;
    return identity != 0 ? identity : 1;
}

uint64_t yonder_storage_identity(yonder_storage_t *st, os64_dom_storage_area_t area, const char *origin)
{
    if (st == NULL || origin == NULL)
        return 0;
    if (area == OS64_DOM_STORAGE_SESSION) {
        Session *s = session_of(st, origin);
        return s != NULL ? s->generation : 0;
    }
    sweep(st);
    char path[OS64_PATH_MAX];
    os64_dirent_t entry;
    if (!store_path(st, origin, path, sizeof(path)) || os64_stat(path, &entry) < 0)
        return 0;
    return file_identity(&entry);
}

// The store file's bytes as they stand: true with *bytes NULL and *length
// 0 when there is none (or it went between the asking and the reading);
// false, with the problem said, when it is there and cannot be read whole,
// is a folder, or there is no memory to read it into.
static bool read_store(yonder_storage_t *st, const char *path, uint8_t **bytes, size_t *length)
{
    *bytes = NULL;
    *length = 0;
    os64_dirent_t entry;
    if (os64_stat(path, &entry) < 0)
        return true;
    os64_slurp_status_t status = entry.flags & OS64_DE_DIR ? OS64_SLURP_IO_ERROR
                                                           : os64_slurp(path, STORE_MAX, bytes, length);
    if (status == OS64_SLURP_OK)
        return true;
    if (status == OS64_SLURP_NO_FILE && os64_stat(path, &entry) < 0)
        return true;
    os64_snprintf(st->problem, sizeof(st->problem),
                  status == OS64_SLURP_NO_MEMORY ? "Page storage is not kept: no memory to read %s."
                                                 : "Page storage is not kept: %s cannot be read.",
                  path);
    return false;
}

int64_t yonder_storage_load(yonder_storage_t *st, os64_dom_storage_area_t area, const char *origin, void *out,
                            size_t cap)
{
    if (st == NULL || origin == NULL)
        return -1;
    if (area == OS64_DOM_STORAGE_SESSION) {
        Session *s = session_of(st, origin);
        if (s == NULL || s->image == NULL)
            return 0;
        if (s->length <= cap)
            os64_memcpy(out, s->image, s->length);
        return (int64_t)s->length;
    }
    char path[OS64_PATH_MAX];
    if (!store_path(st, origin, path, sizeof(path))) {
        say_no_path(st, origin);
        return -1;
    }
    os64_dirent_t entry;
    if (os64_stat(path, &entry) < 0)
        return 0;
    // Bigger than the room: its size says so without reading it.
    if (!(entry.flags & OS64_DE_DIR) && entry.size > cap)
        return entry.size > STORE_MAX ? (int64_t)STORE_MAX + 1 : (int64_t)entry.size;
    uint8_t *bytes;
    size_t length;
    if (!read_store(st, path, &bytes, &length))
        return -1;
    // An empty file is there and is no image (every image begins with
    // libdom's magic line): another's, never yonder's own "none".
    if (bytes != NULL && length == 0) {
        os64_free(bytes);
        os64_snprintf(st->problem, sizeof(st->problem),
                      "Page storage is not kept for %s: its file is not one yonder wrote.", origin);
        return -1;
    }
    if (length <= cap)
        os64_memcpy(out, bytes, length);
    os64_free(bytes);
    return (int64_t)length;
}

// The keys a refused commit could not keep, named in the page's record.
typedef struct {
    char text[256];
    size_t used;
    uint32_t more;
} Refused;

static bool refused_key(void *opaque, const char *key, size_t key_length, const char *value, size_t value_length)
{
    (void)value_length;
    Refused *r = opaque;
    if (value == NULL)
        return true;
    if (r->used + key_length + 3 >= sizeof(r->text) - 24) {
        r->more++;
        return true;
    }
    if (r->used != 0) {
        r->text[r->used++] = ',';
        r->text[r->used++] = ' ';
    }
    // A key is any string; a NUL in one would end the line early.
    for (size_t i = 0; i < key_length; i++)
        r->text[r->used++] = key[i] != '\0' ? key[i] : '?';
    r->text[r->used] = '\0';
    return true;
}

static bool every_record(void *opaque, const char *key, size_t key_length, const char *value,
                         size_t value_length)
{
    (void)opaque, (void)key, (void)key_length, (void)value, (void)value_length;
    return true;
}

static void say_full(yonder_diag_t *diag, const char *area, const char *origin, const void *changes,
                     size_t length)
{
    if (diag == NULL)
        return;
    Refused r = {{0}, 0, 0};
    (void)os64_dom_storage_walk(changes, length, refused_key, &r);
    char message[512];
    if (r.more != 0)
        os64_snprintf(message, sizeof(message), "%s of %s is full; not kept: %s and %u more", area, origin, r.text,
                      (unsigned)r.more);
    else
        os64_snprintf(message, sizeof(message), "%s of %s is full; not kept: %s", area, origin,
                      r.used != 0 ? r.text : "(only removals)");
    yonder_diag_failed(diag, "storage", message);
}

static bool write_all(int32_t h, const uint8_t *p, size_t n)
{
    while (n != 0) {
        int64_t w = os64_write(h, p, n);
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

// The image written whole to a temp beside `path` (named into `temp`), for
// a rename to publish, so a reader never meets half of one. False with no
// temp left behind.
static bool temp_write(yonder_storage_t *st, const char *path, const void *image, size_t length, char *temp,
                       size_t cap)
{
    int n = os64_snprintf(temp, cap, TEMP_FORMAT, path, (unsigned long)os64_taskid(), (unsigned)++st->temps);
    if (n <= 0 || (size_t)n >= cap)
        return false;
    int64_t h = os64_open(temp, "w");
    if (h < 0)
        return false;
    bool ok = write_all((int32_t)h, image, length);
    ok = ok && os64_sync((int32_t)h) == 0;
    ok = os64_close((int32_t)h) == 0 && ok;
    if (!ok)
        (void)os64_unlink(temp);
    return ok;
}

// ── Two yonders, one store ──────────────────────────────────────────────
//
// THE STORE'S LOCK is `<store>+lock`, made with an exclusive create and
// holding "<task> <seconds>\n" (its taker and the wall clock's second when
// it was taken). It is held across the read, the merge and the rename, so
// two yonders (two processes) never merge onto one image and lose each
// other's changes. A lock whose task is not running (gone, or a zombie),
// or is this window's own (lock_dead), or that is older than LOCK_STALE_S
// (a commit takes milliseconds; a task number is reused at the next boot),
// was left by a crash and is broken. A live one is waited for,
// LOCK_STEP_MS at a time, up to LOCK_WAIT_MS; then the commit is BUSY and
// libdom keeps the changes for the next. The last commit (libdom's drain:
// the page's window is closing or its next page has arrived) waits up to
// LOCK_FINAL_WAIT_MS, since nothing on screen waits for it and there is no
// commit after it; one still refused then loses its changes, and the page's
// record says how many. One that names nothing is being
// written, or was left half made: it is waited for the same way and broken
// at the end of the wait. Under the lock, the store's identity is read
// again before the rename: a writer that ignored the lock (a crash's lock
// broken by two waiters at once) moved it, and the merge is made again
// onto what it wrote. What the second look cannot see is such a writer's
// image of the same size in the same second (file_identity), or one that
// lands between the look and the rename.
#define LOCK_WAIT_MS 200
// Ten times a live commit's: past it, a holder has stalled for longer than
// a closing window should hang, and still well short of LOCK_STALE_S, the
// age at which its lock is a crash's.
#define LOCK_FINAL_WAIT_MS 2000
#define LOCK_STEP_MS 5
#define LOCK_STALE_S 10
#define MERGE_TRIES 3

static uint64_t wall_seconds(void)
{
    os64_time_t wall;
    return os64_time(&wall) == 0 && wall.epoch > 0 ? (uint64_t)wall.epoch : 0;
}

// Whether task `id` is running: there, and not a zombie.
static bool task_alive(uint64_t id)
{
    os64_proc_info_t info;
    return id != 0 && os64_proc_read(id, &info) == 0 && info.state != OS64_PROC_ZOMBIE;
}

// A lock's holder and when it was taken; false for a lock that names none
// (empty, half written, or not one of these).
static bool lock_read(const char *lock, uint64_t *task, uint64_t *taken)
{
    uint8_t *bytes;
    size_t length;
    if (os64_slurp(lock, 64, &bytes, &length) != OS64_SLURP_OK)
        return false;
    uint64_t number[2] = {0, 0};
    size_t which = 0, digits = 0;
    bool ok = false;
    for (size_t i = 0; i < length; i++) {
        uint8_t c = bytes[i];
        if (c >= '0' && c <= '9' && digits < 19) {
            number[which] = number[which] * 10 + (uint64_t)(c - '0');
            digits++;
        } else if (c == ' ' && which == 0 && digits != 0) {
            which = 1;
            digits = 0;
        } else {
            ok = c == '\n' && which == 1 && digits != 0 && i + 1 == length;
            break;
        }
    }
    os64_free(bytes);
    if (!ok)
        return false;
    *task = number[0];
    *taken = number[1];
    return true;
}

// Whether a lock that names its holder was left by a crash (above). One
// that names this task is a lock this window failed to let go, or an
// earlier boot's task of the same number: this window holds none across a
// commit, so it is never one in use.
static bool lock_dead(uint64_t task, uint64_t taken)
{
    uint64_t now = wall_seconds();
    return task == os64_taskid() || !task_alive(task) || (now > taken && now - taken > LOCK_STALE_S);
}

static bool lock_stale(const char *lock)
{
    uint64_t task, taken;
    return lock_read(lock, &task, &taken) && lock_dead(task, taken);
}

// The lock at `lock`: taken, held by a live window past the wait, or not
// to be made (a folder that cannot be written).
typedef enum { LOCK_TAKEN, LOCK_HELD, LOCK_REFUSED } lock_t;

static lock_t lock_take(const char *lock, uint32_t wait_ms)
{
    char text[48];
    int n = os64_snprintf(text, sizeof(text), "%lu %lu\n", (unsigned long)os64_taskid(),
                          (unsigned long)wall_seconds());
    if (n <= 0 || (size_t)n >= sizeof(text))
        return LOCK_REFUSED;
    // Every turn waits a step or breaks a lock; a lock that will not go is
    // a folder that will not let it, and the turns end.
    for (uint32_t turn = 0, waited = 0; turn < 2 * (wait_ms / LOCK_STEP_MS) + 4; turn++) {
        int64_t h = os64_open(lock, "x");
        if (h >= 0) {
            bool ok = write_all((int32_t)h, (const uint8_t *)text, (size_t)n);
            ok = os64_close((int32_t)h) == 0 && ok;
            if (ok)
                return LOCK_TAKEN;
            (void)os64_unlink(lock);
            return LOCK_REFUSED;
        }
        os64_dirent_t entry;
        if (os64_stat(lock, &entry) < 0)
            continue;               // let go between the create and the look, or not to be made
        uint64_t task, taken;
        bool named = lock_read(lock, &task, &taken);
        if (named && lock_dead(task, taken)) {
            (void)os64_unlink(lock);
            continue;
        }
        if (waited >= wait_ms) {
            if (named)
                return LOCK_HELD;   // a live holder: the changes wait for the next commit
            (void)os64_unlink(lock);    // half made, and still so at the wait's end
            continue;
        }
        (void)os64_sleep(LOCK_STEP_MS);
        waited += LOCK_STEP_MS;
    }
    return LOCK_REFUSED;
}

// A decimal number at `*at`, no larger than `max`: a run of digits, read
// whole. False for anything else. A leading zero is read here; what refuses
// it is temp_task spelling the name again, as %lu or %u never writes one.
static bool spelled_number(const char **at, uint64_t max, uint64_t *out)
{
    uint64_t value = 0;
    const char *p = *at;
    if (*p < '0' || *p > '9')
        return false;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (value > (max - (uint64_t)(*p - '0')) / 10)
            return false;
        value = value * 10 + (uint64_t)(*p - '0');
    }
    *at = p;
    *out = value;
    return true;
}

// The task in a temp's name, when `name` is one temp_write could have
// made: a store's name, then TEMP_FORMAT's own spelling, checked by
// spelling it again. False for any other name, which is never ours to
// remove.
static bool temp_task(const char *name, uint64_t *task)
{
    const char *plus = os64_strchr(name, '+');
    if (plus == NULL || plus == name || (size_t)(plus - name) > OS64_DIRENT_NAME_MAX)
        return false;
    const char *at = plus + 1;
    uint64_t count;
    if (!spelled_number(&at, UINT64_MAX, task) || *at++ != '.' || !spelled_number(&at, UINT32_MAX, &count))
        return false;
    char base[OS64_DIRENT_NAME_MAX + 1], spelled[OS64_PATH_MAX];
    os64_memcpy(base, name, (size_t)(plus - name));
    base[plus - name] = '\0';
    int n = os64_snprintf(spelled, sizeof(spelled), TEMP_FORMAT, base, (unsigned long)*task, (unsigned)count);
    return n > 0 && (size_t)n < sizeof(spelled) && os64_streq(spelled, name);
}

// A temp's or a lock's owner is gone: its task is not running, or it is
// this one, whose own are never in the middle of anything when this is
// asked. A lock is gone by lock_stale's rule. A name that is neither a
// temp nor a lock by its whole spelling is not ours, and is left alone.
static bool orphan(const char *dir, const char *name)
{
    const char *plus = os64_strchr(name, '+');
    if (plus == NULL || plus == name)
        return false;
    char path[OS64_PATH_MAX];
    int n = os64_snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    if (os64_streq(plus, LOCK_SUFFIX))
        return lock_stale(path);
    uint64_t task;
    return temp_task(name, &task) && (task == os64_taskid() || !task_alive(task));
}

// The names in `dir` that are files, os64_malloc'd: the listing first and
// the removals after, since a folder is not changed under a listing of it.
// NULL with *count 0 for none; *whole false when the listing stopped short.
static char (*listing(const char *dir, size_t *count, bool *whole))[OS64_DIRENT_NAME_MAX + 1]
{
    *count = 0;
    *whole = false;
    int64_t h = os64_opendir(dir);
    if (h < 0)
        return NULL;
    char (*names)[OS64_DIRENT_NAME_MAX + 1] = NULL;
    size_t cap = 0;
    os64_dirent_t entry;
    *whole = true;
    while (os64_readdir((int32_t)h, &entry) == 1) {
        if (entry.flags & OS64_DE_DIR)
            continue;
        if (*count == cap) {
            size_t more = cap != 0 ? cap * 2 : 16;
            void *grown = os64_realloc(names, more * sizeof(*names));
            if (grown == NULL) {
                *whole = false;
                break;
            }
            names = grown;
            cap = more;
        }
        os64_strcopy(names[(*count)++], sizeof(names[0]), entry.name);
    }
    os64_close((int32_t)h);
    return names;
}

// At the first touch of a profile's stores: the temps and locks a crash
// left (orphan) are removed, so they neither grow without end nor stand
// for a site.
static void sweep(yonder_storage_t *st)
{
    if (st->swept)
        return;
    st->swept = true;
    char dir[OS64_PATH_MAX];
    os64_dirent_t entry;
    if (!store_dir(st, dir, sizeof(dir)) || os64_stat(dir, &entry) < 0)
        return;
    size_t count;
    bool whole;
    char (*names)[OS64_DIRENT_NAME_MAX + 1] = listing(dir, &count, &whole);
    for (size_t i = 0; i < count; i++) {
        char path[OS64_PATH_MAX];
        int n = os64_snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (n > 0 && (size_t)n < sizeof(path) && orphan(dir, names[i]))
            (void)os64_unlink(path);
    }
    os64_free(names);
}

static os64_dom_storage_status_t commit_session(yonder_storage_t *st, const char *origin, const void *changes,
                                                size_t length, yonder_diag_t *diag)
{
    Session *s = session_of(st, origin);
    void *merged;
    size_t merged_length;
    os64_dom_storage_status_t status = os64_dom_storage_merge(s != NULL ? s->image : NULL,
                                                              s != NULL ? s->length : 0, changes, length,
                                                              &merged, &merged_length);
    if (status == OS64_DOM_STORAGE_FULL)
        say_full(diag, "sessionStorage", origin, changes, length);
    if (status != OS64_DOM_STORAGE_OK)
        return status;
    if (s == NULL) {
        size_t n = os64_strlen(origin) + 1;
        s = os64_calloc(1, sizeof(*s));
        char *copy = s != NULL ? os64_malloc(n) : NULL;
        if (copy == NULL) {
            os64_free(s);
            os64_free(merged);
            return OS64_DOM_STORAGE_FAILED;
        }
        os64_memcpy(copy, origin, n);
        s->origin = copy;
        s->next = st->sessions;
        st->sessions = s;
    }
    os64_free(s->image);
    s->image = merged;
    s->length = merged_length;
    s->generation = ++st->generations;
    return OS64_DOM_STORAGE_OK;
}

static bool count_record(void *opaque, const char *key, size_t key_length, const char *value, size_t value_length)
{
    (void)key, (void)key_length, (void)value, (void)value_length;
    (*(uint32_t *)opaque)++;
    return true;
}

// A store another window kept past the wait, in the page's record: the
// changes wait for the next commit, or, at the last, how many keys were
// not kept.
static void say_busy(yonder_diag_t *diag, const char *origin, const void *changes, size_t length, bool last)
{
    if (diag == NULL)
        return;
    char message[OS64_PATH_MAX + 128];
    if (last) {
        uint32_t keys = 0;
        (void)os64_dom_storage_walk(changes, length, count_record, &keys);
        os64_snprintf(message, sizeof(message),
                      "localStorage of %s was busy (another window was writing it) as the page left; "
                      "%u key%s not kept",
                      origin, (unsigned)keys, keys == 1 ? "" : "s");
    } else {
        os64_snprintf(message, sizeof(message),
                      "localStorage of %s is busy (another window is writing it); its changes wait for the next commit",
                      origin);
    }
    yonder_diag_failed(diag, "storage", message);
}

// The store's identity now, 0 for none (as yonder_storage_identity's).
static uint64_t path_identity(const char *path)
{
    os64_dirent_t entry;
    return os64_stat(path, &entry) < 0 ? 0 : file_identity(&entry);
}

// Read, merge, write, under the store's lock: the store as it stands, this
// page's changes on top. A store that is there and cannot be read is never
// written over. A store that moved between the read and the rename (above)
// is merged onto again, up to MERGE_TRIES merges in all, and then the
// commit is BUSY.
static os64_dom_storage_status_t commit_locked(yonder_storage_t *st, const char *origin, const char *path,
                                               const void *changes, size_t length, bool last,
                                               yonder_diag_t *diag)
{
    for (int tries = 1;; tries++) {
        uint64_t before = path_identity(path);
        uint8_t *image;
        size_t image_length;
        if (!read_store(st, path, &image, &image_length))
            return OS64_DOM_STORAGE_FAILED;
        void *merged;
        size_t merged_length;
        os64_dom_storage_status_t status = os64_dom_storage_merge(image, image_length, changes, length, &merged,
                                                                  &merged_length);
        bool had = image != NULL;
        // A refused merge is a file in no format of libdom's, or no memory.
        if (status == OS64_DOM_STORAGE_FAILED)
            os64_snprintf(st->problem, sizeof(st->problem),
                          os64_dom_storage_walk(image, image_length, every_record, NULL)
                              ? "Page storage is not kept for %s: no memory to merge its changes."
                              : "Page storage is not kept for %s: its file is not one yonder wrote.",
                          origin);
        os64_free(image);
        if (status == OS64_DOM_STORAGE_FULL)
            say_full(diag, "localStorage", origin, changes, length);
        if (status != OS64_DOM_STORAGE_OK)
            return status;
        char temp[OS64_PATH_MAX];
        bool publishing = merged != NULL;
        bool ok = !publishing || temp_write(st, path, merged, merged_length, temp, sizeof(temp));
        os64_free(merged);
        if (ok && path_identity(path) != before) {
            if (publishing)
                (void)os64_unlink(temp);
            if (tries < MERGE_TRIES)
                continue;
            say_busy(diag, origin, changes, length, last);
            return OS64_DOM_STORAGE_BUSY;
        }
        // A store left empty is no store, and one another window removed
        // first is gone as asked.
        os64_dirent_t entry;
        if (ok && publishing) {
            ok = os64_rename(temp, path) == 0;
            if (!ok)
                (void)os64_unlink(temp);
        } else if (ok) {
            ok = !had || os64_unlink(path) == 0 || os64_stat(path, &entry) < 0;
        }
        if (!ok) {
            os64_snprintf(st->problem, sizeof(st->problem), "Page storage is not kept: %s cannot be written.",
                          path);
            return OS64_DOM_STORAGE_FAILED;
        }
        return OS64_DOM_STORAGE_OK;
    }
}

os64_dom_storage_status_t yonder_storage_commit(yonder_storage_t *st, os64_dom_storage_area_t area,
                                                const char *origin, const void *changes, size_t length,
                                                bool last, yonder_diag_t *diag)
{
    if (st == NULL || origin == NULL || changes == NULL)
        return OS64_DOM_STORAGE_FAILED;
    if (area == OS64_DOM_STORAGE_SESSION)
        return commit_session(st, origin, changes, length, diag);
    char dir[OS64_PATH_MAX], path[OS64_PATH_MAX];
    if (!store_dir(st, dir, sizeof(dir)) || !store_path(st, origin, path, sizeof(path))) {
        say_no_path(st, origin);
        return OS64_DOM_STORAGE_FAILED;
    }
    if (!make_dirs(dir)) {
        os64_snprintf(st->problem, sizeof(st->problem), "Page storage is not kept: %s cannot be made.", dir);
        return OS64_DOM_STORAGE_FAILED;
    }
    char lock[OS64_PATH_MAX];
    os64_snprintf(lock, sizeof(lock), "%s" LOCK_SUFFIX, path);   // store_path kept the room
    sweep(st);
    lock_t taken = lock_take(lock, last ? LOCK_FINAL_WAIT_MS : LOCK_WAIT_MS);
    if (taken == LOCK_HELD) {
        say_busy(diag, origin, changes, length, last);
        return OS64_DOM_STORAGE_BUSY;
    }
    if (taken == LOCK_REFUSED) {
        os64_snprintf(st->problem, sizeof(st->problem), "Page storage is not kept: %s cannot be written.", path);
        return OS64_DOM_STORAGE_FAILED;
    }
    os64_dom_storage_status_t status = commit_locked(st, origin, path, changes, length, last, diag);
    (void)os64_unlink(lock);
    return status;
}

// The listing's store names (those with no '+'), an open-addressed index
// of their positions plus one: whether a lock's store was listed is a few
// probes whatever the folder holds.
typedef struct {
    uint32_t *slots;
    size_t nslots;
} Listed;

static uint32_t name_hash(const char *name, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) hash = (hash ^ (unsigned char)name[i]) * 16777619u;
    return hash;
}

static bool listed_has(const Listed *l, char (*names)[OS64_DIRENT_NAME_MAX + 1], const char *name, size_t length)
{
    size_t mask = l->nslots - 1;
    for (size_t at = name_hash(name, length) & mask; l->slots[at] != 0; at = (at + 1) & mask) {
        const char *held = names[l->slots[at] - 1];
        if (os64_strlen(held) == length && os64_memcmp(held, name, length) == 0)
            return true;
    }
    return false;
}

// False on no memory.
static bool listed_index(Listed *l, char (*names)[OS64_DIRENT_NAME_MAX + 1], size_t count)
{
    size_t nslots = 16;
    while (nslots < count * 2)
        nslots *= 2;
    l->slots = os64_calloc(nslots, sizeof(*l->slots));
    l->nslots = nslots;
    if (l->slots == NULL)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (os64_strchr(names[i], '+') != NULL)
            continue;
        size_t at = name_hash(names[i], os64_strlen(names[i])) & (nslots - 1);
        while (l->slots[at] != 0)
            at = (at + 1) & (nslots - 1);
        l->slots[at] = (uint32_t)i + 1;
    }
    return true;
}

int64_t yonder_storage_clear(yonder_storage_t *st, uint32_t *busy)
{
    *busy = 0;
    if (st == NULL)
        return -1;
    sessions_free(st);
    char dir[OS64_PATH_MAX];
    os64_dirent_t entry;
    if (!store_dir(st, dir, sizeof(dir)) || os64_stat(dir, &entry) < 0)
        return 0;
    size_t count;
    bool whole;
    char (*names)[OS64_DIRENT_NAME_MAX + 1] = listing(dir, &count, &whole);
    if (!whole && count == 0)
        return -1;
    // Each store goes under its own lock, as a commit takes it, so a window
    // mid-commit finishes first and nothing it publishes outlives the
    // clear; and so does each lock whose store was not listed, a window
    // making an origin's first store: its lock is taken (made, if that
    // window has let it go since the listing) and the store it published
    // goes. One whose lock a live window holds past the wait is left, and
    // counted. Then every temp a crash left; a live yonder's is that
    // window's, mid-commit, and stays. Only a store is a site.
    Listed stores = {NULL, 0};
    if (!listed_index(&stores, names, count)) {
        os64_free(names);
        return -1;
    }
    int64_t removed = 0;
    bool all = whole;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            const char *plus = os64_strchr(names[i], '+');
            size_t base = plus != NULL ? (size_t)(plus - names[i]) : os64_strlen(names[i]);
            // The first pass takes the stores, the second the locks whose
            // store was not listed.
            if (pass == 0 ? plus != NULL
                          : plus == NULL || !os64_streq(plus, LOCK_SUFFIX) || base == 0 ||
                            listed_has(&stores, names, names[i], base))
                continue;
            char path[OS64_PATH_MAX], lock[OS64_PATH_MAX];
            int n = os64_snprintf(path, sizeof(path), "%s/%.*s", dir, (int)base, names[i]);
            int m = os64_snprintf(lock, sizeof(lock), "%s" LOCK_SUFFIX, path);
            if (n <= 0 || (size_t)n >= sizeof(path) || m <= 0 || (size_t)m >= sizeof(lock)) {
                all = false;
                continue;
            }
            lock_t taken = lock_take(lock, LOCK_WAIT_MS);
            if (taken != LOCK_TAKEN) {
                if (taken == LOCK_HELD && *busy < UINT32_MAX)
                    (*busy)++;
                all = false;
                continue;
            }
            if (os64_unlink(path) == 0)
                removed++;
            else if (os64_stat(path, &entry) == 0)
                all = false;
            (void)os64_unlink(lock);
        }
    }
    os64_free(stores.slots);
    for (size_t i = 0; i < count; i++) {
        const char *plus = os64_strchr(names[i], '+');
        if (plus == NULL || os64_streq(plus, LOCK_SUFFIX) || !orphan(dir, names[i]))
            continue;
        char path[OS64_PATH_MAX];
        int n = os64_snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (n <= 0 || (size_t)n >= sizeof(path) || (os64_unlink(path) != 0 && os64_stat(path, &entry) == 0))
            all = false;
    }
    os64_free(names);
    st->problem[0] = '\0';
    return all ? removed : -1;
}
