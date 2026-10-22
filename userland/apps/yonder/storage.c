// storage.c — where a page's storage is kept (storage.h).

#include "storage.h"
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
    bool swept;                     // the dead's temps are gone (sweep)
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

// A temp file is its store's path and "+<task>.<count>.new". The '+' is a
// byte a store's name never holds (it is %2B there), so no origin's store
// is another's temp. The room it needs is kept free in every store path.
// TEMP_FORMAT is the one spelling of a temp's name: temp_write makes names
// with it, and temp_task knows one only by making it again.
#define TEMP_FORMAT "%s+%lu.%u.new"
#define TEMP_SUFFIX_MAX (1 + 20 + 1 + 10 + 4)

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
// A commit reads the store, merges this page's change list onto it and
// publishes the result by temp and rename, which is atomic: a reader meets
// the old image or the new one, never half of either. Just before the
// rename it reads the store's identity again, and a store another window
// published since the read is read and merged onto again, up to
// MERGE_TRIES merges, the last published whatever moved. Nothing else
// stands between two windows: two committing one origin's store in the
// same instant can lose one window's changes (booked in DEBTS.md).
#define MERGE_TRIES 3

// Whether task `id` is running: there, and not a zombie.
static bool task_alive(uint64_t id)
{
    os64_proc_info_t info;
    return id != 0 && os64_proc_read(id, &info) == 0 && info.state != OS64_PROC_ZOMBIE;
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

// A temp whose owner is gone: its task is not running, or it is this one,
// whose own are never in the middle of anything when this is asked. A name
// that is not a temp by its whole spelling is not ours, and is left alone.
static bool orphan(const char *name)
{
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

// At the first touch of a profile's stores: the temps a crash left
// (orphan) are removed, so they neither grow without end nor stand for a
// site.
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
        if (n > 0 && (size_t)n < sizeof(path) && orphan(names[i]))
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

// The store's identity now, 0 for none (as yonder_storage_identity's).
static uint64_t path_identity(const char *path)
{
    os64_dirent_t entry;
    return os64_stat(path, &entry) < 0 ? 0 : file_identity(&entry);
}

// Read, merge, write: the store as it stands, this page's changes on top.
// A store that is there and cannot be read is never written over. A store
// that moved between the read and the rename (above) is merged onto again,
// up to MERGE_TRIES merges in all.
static os64_dom_storage_status_t commit_store(yonder_storage_t *st, const char *origin, const char *path,
                                              const void *changes, size_t length, yonder_diag_t *diag)
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
        if (ok && tries < MERGE_TRIES && path_identity(path) != before) {
            if (publishing)
                (void)os64_unlink(temp);
            continue;
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
                                                yonder_diag_t *diag)
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
    sweep(st);
    return commit_store(st, origin, path, changes, length, diag);
}

int64_t yonder_storage_clear(yonder_storage_t *st)
{
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
    // Every store goes, one unlink each, and every temp a crash left; a
    // live yonder's temp is that window's, mid-commit, and stays (and what
    // it publishes is its own changes, after the clear). Only a store is a
    // site.
    int64_t removed = 0;
    bool all = whole;
    for (size_t i = 0; i < count; i++) {
        bool store = os64_strchr(names[i], '+') == NULL;
        if (!store && !orphan(names[i]))
            continue;
        char path[OS64_PATH_MAX];
        int n = os64_snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (n > 0 && (size_t)n < sizeof(path) && os64_unlink(path) == 0)
            removed += store ? 1 : 0;
        else if (n <= 0 || (size_t)n >= sizeof(path) || os64_stat(path, &entry) == 0)
            all = false;
    }
    os64_free(names);
    st->problem[0] = '\0';
    return all ? removed : -1;
}
