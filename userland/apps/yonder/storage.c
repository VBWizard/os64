// storage.c — where a page's storage is kept (storage.h).

#include "storage.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
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
    char problem[OS64_PATH_MAX + 96];
};

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

// The image published at `path` by temp and rename, so a reader never
// meets half of one. False with nothing left behind but what was there.
static bool publish(yonder_storage_t *st, const char *path, const void *image, size_t length)
{
    char temp[OS64_PATH_MAX];
    int n = os64_snprintf(temp, sizeof(temp), "%s+%lu.%u.new", path, (unsigned long)os64_taskid(),
                          (unsigned)++st->temps);
    if (n <= 0 || (size_t)n >= sizeof(temp))
        return false;
    int64_t h = os64_open(temp, "w");
    if (h < 0)
        return false;
    bool ok = write_all((int32_t)h, image, length);
    ok = ok && os64_sync((int32_t)h) == 0;
    ok = os64_close((int32_t)h) == 0 && ok;
    ok = ok && os64_rename(temp, path) == 0;
    if (!ok)
        (void)os64_unlink(temp);
    return ok;
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
    // Read, merge, write: the store as it stands, this page's changes on
    // top. A store that is there and cannot be read is never written over.
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
    // A store left empty is no store, and one another window removed
    // first is gone as asked.
    os64_dirent_t entry;
    bool ok = merged != NULL ? publish(st, path, merged, merged_length)
                             : !had || os64_unlink(path) == 0 || os64_stat(path, &entry) < 0;
    os64_free(merged);
    if (!ok) {
        os64_snprintf(st->problem, sizeof(st->problem), "Page storage is not kept: %s cannot be written.", path);
        return OS64_DOM_STORAGE_FAILED;
    }
    return OS64_DOM_STORAGE_OK;
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
    int64_t h = os64_opendir(dir);
    if (h < 0)
        return -1;
    // The names first, then the removals: a folder is not changed under a
    // listing of it.
    char (*names)[OS64_DIRENT_NAME_MAX + 1] = NULL;
    size_t count = 0, cap = 0;
    bool listed = true;
    while (os64_readdir((int32_t)h, &entry) == 1) {
        if (entry.flags & OS64_DE_DIR)
            continue;
        if (count == cap) {
            size_t more = cap != 0 ? cap * 2 : 16;
            void *grown = os64_realloc(names, more * sizeof(*names));
            if (grown == NULL) {
                listed = false;
                break;
            }
            names = grown;
            cap = more;
        }
        os64_strcopy(names[count++], sizeof(names[0]), entry.name);
    }
    os64_close((int32_t)h);
    int64_t removed = 0;
    bool all = listed;
    for (size_t i = 0; i < count; i++) {
        char path[OS64_PATH_MAX];
        int n = os64_snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (n > 0 && (size_t)n < sizeof(path) && os64_unlink(path) == 0)
            removed++;
        else
            all = false;
    }
    os64_free(names);
    st->problem[0] = '\0';
    return all ? removed : -1;
}
