// Replacement backups and scratch ownership for os64get's download modes.
// File preparation performs the data I/O; publication moves prepared files.
#include "install.h"
#include "os64/os64.h"
#include "os64/crc32.h"
#include "os64/date.h"
#include "os64/slurp.h"

#define MOUNT_MAX 64
#define COPY_CHUNK 65536

typedef struct {
    char prefix[INSTALL_PATH_MAX];
    char run[INSTALL_PATH_MAX];
    bool writable;
    bool disk;
} install_mount_t;

static install_mount_t mounts[MOUNT_MAX];
static unsigned mount_count, sequence;
static char archive_root[INSTALL_PATH_MAX], archive_run[INSTALL_PATH_MAX];
static volatile bool cancelled;
static bool committing;
static uint8_t copy_buffer[COPY_CHUNK];

void install_cancel(int signo) { (void)signo; cancelled = true; }
bool install_cancelled(void) { return cancelled && !committing; }
bool install_cancel_requested(void) { return cancelled; }
const char *install_archive(void) { return archive_run; }

static bool problem(const char *action, const char *path)
{
    os64_hprintf(OS64_STDERR, "os64get: cannot %s: %s\n", action, path);
    return false;
}

static bool copy_path(char *out, const char *path)
{
    return os64_strcopy(out, INSTALL_PATH_MAX, path) < INSTALL_PATH_MAX;
}

static bool join(char *out, const char *parent, const char *name)
{
    int32_t n = os64_snprintf(out, INSTALL_PATH_MAX, "%s%s%s", parent,
                             os64_streq(parent, "/") ? "" : "/", name);
    return n > 0 && n < INSTALL_PATH_MAX;
}

static bool under(const char *path, const char *root)
{
    size_t n = os64_strlen(root);
    if (!n || os64_strlen(path) < n) return false;
    for (size_t i = 0; i < n; i++) if (path[i] != root[i]) return false;
    return n == 1 || path[n] == '/' || path[n] == '\0';
}

static const char *basename_of(const char *path)
{
    const char *base = path;
    for (; *path; path++) if (*path == '/') base = path + 1;
    return base;
}

static bool parent_of(char *out, const char *path)
{
    if (!copy_path(out, path)) return false;
    size_t n = (size_t)(basename_of(out) - out);
    if (!n) return false;
    out[n == 1 ? 1 : n - 1] = '\0';
    return true;
}

// Normalize before choosing a mount, then ask the filesystem for the spelling
// of each existing component. FAT aliases must identify the same parent.
static bool absolute_path(char *out, const char *path)
{
    char raw[INSTALL_PATH_MAX];
    if (path[0] == '/') {
        if (!copy_path(raw, path)) return false;
    } else {
        char cwd[INSTALL_PATH_MAX];
        if (os64_getcwd(cwd, sizeof(cwd)) < 0 || !join(raw, cwd, path)) return false;
    }
    out[0] = '/'; out[1] = '\0';
    size_t used = 1;
    const char *p = raw;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (!n || (n == 1 && start[0] == '.')) continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (used > 1 && out[used - 1] != '/') used--;
            if (used > 1) used--;
            out[used] = '\0';
            continue;
        }
        if (used > 1) out[used++] = '/';
        if (used + n >= INSTALL_PATH_MAX) return false;
        os64_memcpy(out + used, start, n);
        used += n; out[used] = '\0';
    }
    // Mount point names belong to the namespace, not to the mounted root's
    // directory entry (which can report "/" as its name).
    for (size_t i = 1; ; i++) {
        if (out[i] && out[i] != '/') continue;
        char saved = out[i]; out[i] = '\0';
        os64_dirent_t e;
        if (os64_stat(out, &e) == 0 && !(e.flags & OS64_DE_MOUNT)) {
            char canonical[INSTALL_PATH_MAX], parent[INSTALL_PATH_MAX];
            if (!parent_of(parent, out) || !join(canonical, parent, e.name)) return false;
            out[i] = saved;
            size_t n = os64_strlen(canonical);
            if (n + os64_strlen(out + i) >= INSTALL_PATH_MAX) return false;
            char tail[INSTALL_PATH_MAX];
            if (!copy_path(tail, out + i)) return false;
            os64_memcpy(out, canonical, n);
            os64_strcopy(out + n, INSTALL_PATH_MAX - n, tail);
            i = n;
        } else out[i] = saved;
        if (!saved) break;
    }
    return true;
}

static int mount_for(const char *path)
{
    int best = -1;
    for (unsigned i = 0; i < mount_count; i++)
        if (under(path, mounts[i].prefix) &&
            (best < 0 || os64_strlen(mounts[i].prefix) > os64_strlen(mounts[best].prefix)))
            best = (int)i;
    return best;
}

static bool ensure_dir(const char *path)
{
    char walk[INSTALL_PATH_MAX];
    if (!copy_path(walk, path)) return false;
    for (size_t i = 1; ; i++) {
        if (walk[i] && walk[i] != '/') continue;
        char saved = walk[i]; walk[i] = '\0';
        os64_dirent_t e;
        if (os64_stat(walk, &e) < 0) {
            if (os64_mkdir(walk) < 0) return false;
        } else if (!(e.flags & OS64_DE_DIR)) return false;
        walk[i] = saved;
        if (!saved) return true;
    }
}

static bool close_file(int32_t handle)
{
    int64_t rc;
    do { rc = os64_close(handle); } while (rc == OS64_INTERRUPTED);
    return rc >= 0;
}

static bool remove_owned(char *path)
{
    if (!path[0]) return true;
    int64_t rc;
    do { rc = os64_unlink(path); } while (rc == OS64_INTERRUPTED);
    if (rc < 0) return problem("remove temporary path", path);
    path[0] = '\0';
    return true;
}

bool install_init(const char *archive)
{
    mount_count = sequence = 0;
    cancelled = committing = false;
    os64_memset(mounts, 0, sizeof(mounts));
    archive_root[0] = archive_run[0] = '\0';
    uint8_t *report = NULL;
    size_t size = 0;
    if (os64_slurp("/sys/mounts", 16384, &report, &size) != OS64_SLURP_OK)
        return problem("read mount table", "/sys/mounts");
    (void)size;
    bool ok = true;
    char *line = (char *)report;
    // The first line is the report's column header.
    while (*line && *line != '\n') line++;
    if (*line) line++;
    while (*line && ok) {
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = '\0';
        char *fields[12]; unsigned count = 0;
        char *p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\r') p++;
            if (!*p) break;
            if (count == 12) { ok = false; break; }
            fields[count++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
            if (*p) *p++ = '\0';
        }
        if (count) {
            if (count != 12 || mount_count == MOUNT_MAX) { ok = false; break; }
            os64_unescape_field(fields[0]);
            install_mount_t *m = &mounts[mount_count++];
            ok = copy_path(m->prefix, fields[0]) && fields[0][0] == '/';
            m->writable = os64_streq(fields[6], "rw");
            m->disk = os64_streq(fields[1], "ext2") || os64_streq(fields[1], "fat");
        }
        line = next;
    }
    os64_free(report);
    if (!ok || mount_for("/") < 0) return problem("parse mount table", "/sys/mounts");
    if (archive && archive[0] && !absolute_path(archive_root, archive))
        return problem("resolve archive", archive);
    return true;
}

static bool run_directory(char *out, const char *parent)
{
    if (!ensure_dir(parent)) return false;
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        char name[64], candidate[INSTALL_PATH_MAX];
        os64_snprintf(name, sizeof(name), "run-%lu-%u", os64_taskid(), sequence++);
        if (!join(candidate, parent, name)) return false;
        if (os64_mkdir(candidate) == 0) return copy_path(out, candidate);
        os64_dirent_t e;
        if (os64_stat(candidate, &e) < 0) return false;
    }
    return false;
}

static bool scratch_run(int mount, const char **out)
{
    if (mount < 0 || !mounts[mount].writable || !mounts[mount].disk) return false;
    install_mount_t *m = &mounts[mount];
    if (!m->run[0]) {
        char parent[INSTALL_PATH_MAX];
        if (mount_for("/tmp/os64get") == mount) {
            if (!copy_path(parent, "/tmp/os64get")) return false;
        } else if (!join(parent, m->prefix, ".os64get-tmp")) return false;
        if (mount_for(parent) != mount || !run_directory(m->run, parent)) return false;
    }
    *out = m->run;
    return true;
}

static bool reserved_path(const char *dest)
{
    if (under(dest, "/tmp/os64get") || under(dest, archive_root)) return true;
    for (unsigned i = 0; i < mount_count; i++) {
        char root[INSTALL_PATH_MAX];
        if (!join(root, mounts[i].prefix, ".os64get-tmp") || under(dest, root)) return true;
    }
    return false;
}

bool install_plan(install_file_t *f, const char *destination)
{
    os64_memset(f, 0, sizeof(*f));
    char parent[INSTALL_PATH_MAX];
    os64_dirent_t e;
    if (install_cancelled() || !absolute_path(f->dest, destination) ||
        !parent_of(parent, f->dest) || os64_stat(parent, &e) < 0 ||
        !(e.flags & OS64_DE_DIR) || !basename_of(f->dest)[0] || reserved_path(f->dest))
        return problem("plan destination", destination);
    if (os64_stat(f->dest, &e) == 0 && (e.flags & OS64_DE_DIR))
        return problem("replace a directory", f->dest);
    const char *run;
    if (!scratch_run(mount_for(f->dest), &run) || !run_directory(f->directory, run))
        return problem("create staging directory", f->dest);
    char candidate[INSTALL_PATH_MAX];
    if (!join(candidate, f->directory, basename_of(f->dest))) return false;
    int64_t h = os64_open(candidate, "x");
    if (h < 0) return problem("create staging file", candidate);
    copy_path(f->part, candidate);
    return close_file((int32_t)h);
}

bool install_conflicts(const install_file_t *a, const install_file_t *b)
{
    char ap[INSTALL_PATH_MAX], bp[INSTALL_PATH_MAX], probe[INSTALL_PATH_MAX];
    if (!parent_of(ap, a->dest) || !parent_of(bp, b->dest)) return true;
    if (!os64_streq(ap, bp)) return false;
    // The private staging file has the destination's basename on the same
    // filesystem. Probe it using the other name to let FAT resolve aliases,
    // including names absent at the real destination, without case guesses.
    os64_dirent_t e;
    if (!join(probe, a->directory, basename_of(b->dest))) return true;
    return os64_stat(probe, &e) == 0;
}

static bool fingerprint(const char *path, uint64_t *length, uint32_t *crc)
{
    if (install_cancelled()) return false;
    int64_t h = os64_open(path, "r");
    if (h < 0) return false;
    *length = 0;
    uint32_t state = os64_crc32_begin();
    int64_t n = -1;
    while (!install_cancelled() && (n = os64_read((int32_t)h, copy_buffer, sizeof(copy_buffer))) > 0) {
        *length += (uint64_t)n;
        state = os64_crc32_update(state, copy_buffer, (size_t)n);
    }
    bool closed = close_file((int32_t)h);
    *crc = os64_crc32_end(state);
    return n == 0 && closed && !install_cancelled();
}

static bool matches(const char *path, uint64_t length, uint32_t crc)
{
    uint64_t actual_length; uint32_t actual_crc;
    return fingerprint(path, &actual_length, &actual_crc) &&
           length == actual_length && crc == actual_crc;
}

static bool archive_directory(void)
{
    if (archive_run[0]) return true;
    os64_time_t now; os64_date_t date;
    if (os64_time(&now) < 0) return false;
    os64_date_from_epoch(now.epoch, &date);
    char day[INSTALL_PATH_MAX];
    int32_t n = os64_snprintf(day, sizeof(day), "%s/%04d-%02d-%02d",
                            archive_root, date.year, date.month, date.day);
    if (n <= 0 || n >= (int32_t)sizeof(day) || !ensure_dir(day)) return false;
    for (unsigned i = 0; i < 1000; i++) {
        char name[64], candidate[INSTALL_PATH_MAX];
        os64_snprintf(name, sizeof(name), "%02d%02d%02d-%lu-%u",
                      date.hour, date.minute, date.second, os64_taskid(), sequence++);
        if (!join(candidate, day, name)) return false;
        if (os64_mkdir(candidate) == 0) return copy_path(archive_run, candidate);
        os64_dirent_t e;
        if (os64_stat(candidate, &e) < 0) return false;
    }
    return false;
}

static bool backup_original(install_file_t *f)
{
    const char *run;
    if (!scratch_run(mount_for(archive_root), &run)) return false;
    // Scratch creation may make a formerly absent FAT alias resolvable.
    // Re-resolve before checking overlap so backups cannot live in a tree
    // reserved for temporary-file cleanup, or enclose that scratch tree.
    char resolved[INSTALL_PATH_MAX], scratch[INSTALL_PATH_MAX];
    if (!absolute_path(resolved, archive_root)) return false;
    copy_path(archive_root, resolved);
    if (!absolute_path(scratch, "/tmp/os64get") ||
        under(archive_root, scratch) || under(scratch, archive_root))
        return problem("use overlapping archive and scratch directories", archive_root);
    for (unsigned i = 0; i < mount_count; i++) {
        char raw[INSTALL_PATH_MAX];
        if (!join(raw, mounts[i].prefix, ".os64get-tmp") || !absolute_path(scratch, raw) ||
            under(archive_root, scratch) || under(scratch, archive_root))
            return problem("use overlapping archive and scratch directories", archive_root);
    }
    char name[64], candidate[INSTALL_PATH_MAX];
    os64_snprintf(name, sizeof(name), "backup-%u.part", sequence++);
    if (!join(candidate, run, name)) return false;
    int64_t out = os64_open(candidate, "x");
    if (out < 0) return false;
    copy_path(f->backup_part, candidate);
    int64_t in = os64_open(f->dest, "r");
    bool ok = in >= 0;
    uint64_t length = 0; uint32_t crc = os64_crc32_begin();
    while (ok && !install_cancelled()) {
        int64_t n = os64_read((int32_t)in, copy_buffer, sizeof(copy_buffer));
        if (n <= 0) { ok = n == 0; break; }
        if (os64_write((int32_t)out, copy_buffer, (size_t)n) != n) { ok = false; break; }
        length += (uint64_t)n;
        crc = os64_crc32_update(crc, copy_buffer, (size_t)n);
    }
    if (in >= 0 && !close_file((int32_t)in)) ok = false;
    if (install_cancelled() || length != f->old_length || os64_crc32_end(crc) != f->old_crc) ok = false;
    if (ok && os64_sync((int32_t)out) < 0) ok = false;
    if (!close_file((int32_t)out)) ok = false;
    if (!ok || !matches(f->backup_part, f->old_length, f->old_crc) ||
        !matches(f->dest, f->old_length, f->old_crc)) return false;
    if (!archive_directory() || !join(candidate, archive_run, f->dest + 1)) return false;
    char parent[INSTALL_PATH_MAX];
    if (!parent_of(parent, candidate) || !ensure_dir(parent) || install_cancelled()) return false;
    if (os64_rename_with_flags(f->backup_part, candidate, OS64_RENAME_NOREPLACE) < 0) return false;
    f->backup_part[0] = '\0';
    copy_path(f->backup, candidate);
    return true;
}

bool install_prepare(install_file_t *f)
{
    if (install_cancelled()) return false;
    os64_dirent_t e;
    f->existed = os64_stat(f->dest, &e) == 0;
    if (f->existed) {
        if ((e.flags & OS64_DE_DIR) || !fingerprint(f->dest, &f->old_length, &f->old_crc))
            return problem("read original", f->dest);
        uint64_t length; uint32_t crc;
        if (!fingerprint(f->part, &length, &crc)) return problem("verify staged file", f->part);
        f->skip = length == f->old_length && crc == f->old_crc;
        if (!f->skip && archive_root[0] && !backup_original(f)) {
            if (install_cancelled()) return false;
            return problem("back up original; replacement stopped", f->dest);
        }
    }
    f->ready = !install_cancelled();
    return f->ready;
}

bool install_recheck(const install_file_t *f)
{
    if (!f->ready || install_cancelled()) return false;
    if (f->existed && !matches(f->dest, f->old_length, f->old_crc))
        return problem("install over changed original", f->dest);
    return true;
}

bool install_begin_commit(void)
{
    if (install_cancelled()) return false;
    committing = true;
    return true;
}

bool install_commit(install_file_t *f)
{
    if (!committing || !f->ready) return false;
    if (f->skip) return true;
    int64_t rc;
    // Interrupted operations accomplish nothing under the ABI. During commit
    // cancellation is deferred, so finish the metadata operation before cleanup.
    do {
        rc = os64_rename_with_flags(f->part, f->dest,
                                   f->existed ? 0 : OS64_RENAME_NOREPLACE);
    } while (rc == OS64_INTERRUPTED);
    if (rc < 0) {
        problem("publish download", f->dest);
        if (f->backup[0]) os64_hprintf(OS64_STDERR, "os64get: original kept at %s\n", f->backup);
        return false;
    }
    f->part[0] = '\0';
    return true;
}

bool install_cleanup(install_file_t *files, unsigned count)
{
    bool ok = true;
    for (unsigned i = 0; i < count; i++) {
        if (!remove_owned(files[i].part)) ok = false;
        if (!remove_owned(files[i].backup_part)) ok = false;
        if (!remove_owned(files[i].directory)) ok = false;
    }
    for (unsigned i = 0; i < mount_count; i++)
        if (!remove_owned(mounts[i].run)) ok = false;
    return ok;
}
