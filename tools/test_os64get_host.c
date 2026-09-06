// Run the actual os64get batch/URL control flow against host filesystem and
// transport adapters. Faults are injected at I/O boundaries, not in the app.
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

// The host must never execute the guest syscall instruction. Inline ABI
// wrappers still compile, but their raw entry points are replaced here.
#define OS64_ABI_SYSCALL_H
#include "os64/syscall_numbers.h"
uint64_t os64_syscall0(uint64_t n);
uint64_t os64_syscall1(uint64_t n, uint64_t a);
uint64_t os64_syscall2(uint64_t n, uint64_t a, uint64_t b);
uint64_t os64_syscall3(uint64_t n, uint64_t a, uint64_t b, uint64_t c);
uint64_t os64_syscall4(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
uint64_t os64_syscall6(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
#define main os64get_entry
#include "../userland/apps/os64get/os64get.c"
#undef main
#include "os64/slurp.h"

static char sandbox[512];
static char fdpaths[1024][256];
static const char *scenario;
static unsigned installs, backup_reads;
static bool injected;
static os64_signal_fn handler;
static const char *names[] = { "a", "b", "c" };
static const char *payloads[] = { "incoming A", "incoming B", "incoming C" };
static const char *targets[] = { "/bin/a", "/home/b", "/fat/c" };
static char network[16][1024];
static size_t network_len[16], network_pos[16];
static unsigned connections;

static bool is(const char *name) { return strcmp(scenario, name) == 0; }
static void interrupt_run(void) { assert(handler); handler(OS64_SIGINT); injected = true; }

static void host_path(const char *path, char out[1024])
{
    assert(path[0] == '/' && strstr(path, "/../") == NULL);
    assert(snprintf(out, 1024, "%s%s", sandbox, path) < 1024);
    // Model FAT lookup through the filesystem adapter, including case-folded
    // directory components. Ext2 paths retain case-sensitive host lookup.
    if (strncmp(path, "/fat/", 5) != 0) return;
    size_t begin = strlen(sandbox) + 5;
    for (size_t end = begin; ; end++) {
        if (out[end] && out[end] != '/') continue;
        char saved = out[end]; out[end] = 0;
        char parent[1024]; memcpy(parent, out, begin); parent[begin ? begin-1 : 0] = 0;
        DIR *dir = opendir(parent);
        if (dir) {
            struct dirent *e;
            while ((e = readdir(dir))) {
                if (strcasecmp(e->d_name, out + begin) == 0) {
                    assert(strlen(e->d_name) == end - begin);
                    memcpy(out + begin, e->d_name, end - begin);
                    break;
                }
            }
            closedir(dir);
        }
        out[end] = saved;
        if (!saved) break;
        begin = end + 1;
    }
}

uint64_t os64_syscall2(uint64_t n, uint64_t a, uint64_t b)
{
    assert(n == SYSCALL_SIGNAL_HANDLER && a == OS64_SIGINT);
    handler = (os64_signal_fn)b; return 0;
}
void *os64_malloc(size_t n) { return malloc(n); }
void os64_free(void *p) { free(p); }
uint64_t os64_taskid(void) { return 42; }
int64_t os64_getcwd(char *out, size_t cap) { return snprintf(out, cap, "/bin"); }
const char *os64_getenv(const char *key)
{ return is("url-https") && !strcmp(key, "https_proxy") ? "http://proxy:8888/" : NULL; }
int64_t __wrap_os64_time(os64_time_t *out) { memset(out, 0, sizeof(*out)); out->epoch = 1788739200; return 0; }
bool os64_parse_ipv4(const char *s, const char *end, uint32_t *ip)
{ (void)s; (void)end; *ip = 0x7f000001; return true; }
const char *os64_dial_reason(int64_t error) { (void)error; return "fixture refusal"; }
int64_t os64_conf_find(const char *name, char *out, size_t cap)
{ (void)name; snprintf(out, cap, "/etc/os64get.conf"); return 0; }
int64_t os64_conf_read(const char *path, os64_conf_fn fn, void *ctx)
{
    (void)path;
    fn("a", "/bin", ctx); fn("b", "/home", ctx); fn("c", "/fat", ctx);
    fn("archive", "/home/archive", ctx);
    return 0;
}
os64_slurp_status_t os64_slurp(const char *path, size_t cap, uint8_t **out, size_t *len)
{
    assert(strcmp(path, "/sys/mounts") == 0);
    const char *text = "# prefix fstype device part name guid mode blocksz total free open_files open_dirs\n"
        "/ ext2 d 1 root g rw 4096 10000000 9000000 0 0\n"
        "/home ext2 d 2 home g rw 4096 10000000 9000000 0 0\n"
        "/fat fat d 3 fat g rw 512 10000000 9000000 0 0\n";
    *len = strlen(text); assert(*len < cap); *out = (uint8_t *)strdup(text); return OS64_SLURP_OK;
}
int64_t os64_open(const char *path, const char *mode)
{
    char full[1024]; host_path(path, full);
    int flags = strcmp(mode, "r") == 0 ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
    if (strcmp(mode, "x") == 0) flags = O_WRONLY | O_CREAT | O_EXCL;
    int h = open(full, flags, 0600);
    if (h >= 0) { assert(h < 1024); snprintf(fdpaths[h], 256, "%s", path); }
    return h;
}
int64_t os64_close(int32_t h)
{
    if (h >= 10000) return 0;
    bool fail = is("close") && !injected && strstr(fdpaths[h], "backup-");
    int rc = close(h); fdpaths[h][0] = 0;
    if (fail) { injected = true; return -1; }
    return rc;
}
int64_t os64_stat(const char *path, os64_dirent_t *out)
{
    char full[1024]; host_path(path, full); struct stat st;
    if (stat(full, &st) < 0) return -1;
    memset(out, 0, sizeof(*out)); out->size = st.st_size; out->mtime = st.st_mtime;
    if (S_ISDIR(st.st_mode)) out->flags |= OS64_DE_DIR;
    if (!strcmp(path, "/") || !strcmp(path, "/home") || !strcmp(path, "/fat")) out->flags |= OS64_DE_MOUNT;
    snprintf(out->name, sizeof(out->name), "%s", strrchr(full, '/') + 1);
    return 0;
}
int64_t os64_mkdir(const char *path) { char full[1024]; host_path(path, full); return mkdir(full, 0700); }
int64_t os64_unlink(const char *path)
{
    if (is("cancel-cleanup")) interrupt_run();
    char full[1024]; host_path(path, full);
    struct stat st; if (stat(full, &st) < 0) return -1;
    return S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
}
int64_t os64_sync(int32_t h)
{
    if (is("sync") && !injected && strstr(fdpaths[h], "backup-")) { injected = true; return -1; }
    return fsync(h);
}
int64_t os64_rename_with_flags(const char *from, const char *to, uint64_t flags)
{
    bool publish = !strcmp(to, targets[0]) || !strcmp(to, targets[1]) || !strcmp(to, targets[2]);
    if (publish && is("cancel-commit") && !injected) { interrupt_run(); return OS64_INTERRUPTED; }
    if (publish && is("publish") && !strcmp(to, targets[1])) return -1;
    if (publish) installs++;
    // Assert that application publication does not attempt a cross-mount move.
    int a = !strncmp(from, "/home/", 6) ? 1 : !strncmp(from, "/fat/", 5) ? 2 : 0;
    int b = !strncmp(to, "/home/", 6) ? 1 : !strncmp(to, "/fat/", 5) ? 2 : 0;
    assert(a == b);
    char f[1024], t[1024]; host_path(from, f); host_path(to, t);
    if ((flags & OS64_RENAME_NOREPLACE) && access(t, F_OK) == 0) return -1;
    return rename(f, t);
}
int64_t os64_read(int32_t h, void *buf, size_t cap)
{
    if (h >= 10000) {
        unsigned i = (unsigned)(h - 10000);
        if (is("url-cancel") && !injected && network_pos[i] >= 39) { interrupt_run(); return OS64_INTERRUPTED; }
        if (!injected && ((is("cancel-list") && i == 0) || (is("cancel-download") && i == 2 && network_pos[i] > 18))) {
            interrupt_run(); return OS64_INTERRUPTED;
        }
        size_t n = network_len[i] - network_pos[i]; if (n > cap) n = cap;
        if (n > 3) n = 3;
        memcpy(buf, network[i] + network_pos[i], n); network_pos[i] += n; return (int64_t)n;
    }
    if (strstr(fdpaths[h], "backup-")) {
        backup_reads++;
        if (is("backup-read") && !injected) { injected = true; return -1; }
        if (is("cancel-verify") && !injected) { interrupt_run(); return OS64_INTERRUPTED; }
    }
    int64_t result = read(h, buf, cap);
    if (is("backup-corrupt") && !injected && result > 0 && strstr(fdpaths[h], "backup-")) {
        ((unsigned char *)buf)[0] ^= 1; injected = true;
    }
    return result;
}
int64_t os64_read_for(int32_t h, void *buf, size_t cap, uint64_t ms)
{ (void)ms; return os64_read(h, buf, cap); }
int64_t os64_dial(const char *dial)
{ (void)dial; assert(connections < 16); return (int64_t)(10000 + connections++); }
int64_t os64_write(int32_t h, const void *buf, size_t n)
{
    if (h < 3) return (int64_t)fwrite(buf, 1, n, h == 2 ? stderr : stdout);
    if (h >= 10000) {
        unsigned i = (unsigned)(h - 10000);
        if (n == 5 && !memcmp(buf, "LIST\n", 5)) {
            size_t used = 0;
            for (unsigned j = 0; j < 3; j++)
                used += (size_t)snprintf(network[i] + used, sizeof(network[i]) - used,
                                        "%s %zu %08x\n", names[j], strlen(payloads[j]),
                                        os64_crc32(payloads[j], strlen(payloads[j])));
            strcat(network[i], ".\n");
        } else if ((n > 5 && !memcmp(buf, "GET /", 5)) ||
                   (n > 12 && !memcmp(buf, "GET https://", 12))) {
            snprintf(network[i], sizeof(network[i]), "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n%s",
                     is("url-short") ? "cut" : "incoming A");
        } else {
            unsigned j = ((const char *)buf)[4] == 'a' ? 0 : ((const char *)buf)[4] == 'b' ? 1 : 2;
            uint32_t crc = os64_crc32(payloads[j], strlen(payloads[j]));
            if (is("crc") && j == 1) crc++;
            snprintf(network[i], sizeof(network[i]), "OK %zu %08x\n%s", strlen(payloads[j]), crc,
                     is("short") && j == 1 ? "cut" : payloads[j]);
        }
        network_len[i] = strlen(network[i]); return (int64_t)n;
    }
    if (strstr(fdpaths[h], "backup-")) {
        if (is("backup-write") && !injected) { injected = true; return -1; }
        if (is("cancel-backup") && !injected) { interrupt_run(); return OS64_INTERRUPTED; }
    }
    return write(h, buf, n);
}

static void put(const char *path, const char *contents)
{
    int h = (int)os64_open(path, "w"); assert(h >= 0);
    assert(write(h, contents, strlen(contents)) == (ssize_t)strlen(contents)); assert(os64_close(h) == 0);
}
static void contents(const char *path, const char *want)
{
    int h = (int)os64_open(path, "r"); assert(h >= 0);
    char buf[128] = {0}; ssize_t n = read(h, buf, sizeof(buf)); assert(n >= 0); os64_close(h);
    assert(n == (ssize_t)strlen(want) && !memcmp(buf, want, (size_t)n));
}
static unsigned file_count(const char *path)
{
    char full[1024]; host_path(path, full); DIR *dir = opendir(full); if (!dir) return 0;
    unsigned count = 0; struct dirent *e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[512]; assert(snprintf(child, sizeof(child), "%s/%s", path, e->d_name) < (int)sizeof(child));
        os64_dirent_t de; assert(os64_stat(child, &de) == 0);
        count += de.flags & OS64_DE_DIR ? file_count(child) : 1;
    }
    closedir(dir); return count;
}

int main(int argc, char **argv)
{
    assert(argc == 3); scenario = argv[1]; snprintf(sandbox, sizeof(sandbox), "%s", argv[2]);
    assert(os64_mkdir("/bin") == 0); assert(os64_mkdir("/home") == 0);
    assert(os64_mkdir("/fat") == 0); assert(os64_mkdir("/tmp") == 0);
    if (is("archive-overlap")) {
        install_file_t f;
        assert(install_init("/fat/.OS64GET-TMP"));
        put("/bin/a", "original");
        assert(install_plan(&f, "/bin/a"));
        put(f.part, "incoming");
        assert(!install_prepare(&f));
        assert(install_cleanup(&f, 1));
        contents("/bin/a", "original");
        assert(file_count("/fat/.os64get-tmp") == 0);
        puts("PASS archive-overlap");
        return 0;
    }
    if (is("aliases") || is("appeared")) {
        assert(install_init("/home/archive"));
        install_file_t f[2];
        if (is("aliases")) {
            assert(os64_mkdir("/fat/Directory") == 0);
            assert(install_plan(&f[0], "/fat/directory/New"));
            assert(install_plan(&f[1], "/fat/Directory/new"));
            assert(install_conflicts(&f[0], &f[1]));
            assert(install_cleanup(f, 2));
            assert(install_init("/home/archive"));
            assert(install_plan(&f[0], "/bin/New"));
            assert(install_plan(&f[1], "/bin/new"));
            assert(!install_conflicts(&f[0], &f[1]));
            assert(install_cleanup(f, 2));
        } else {
            assert(install_plan(&f[0], "/bin/new"));
            put(f[0].part, "download");
            assert(install_prepare(&f[0]));
            put("/bin/new", "external writer");
            assert(install_begin_commit());
            assert(!install_commit(&f[0]));
            contents("/bin/new", "external writer");
            assert(install_cleanup(f, 1));
            assert(file_count("/home/archive") == 0);
        }
        printf("PASS %s\n", scenario);
        return 0;
    }
    if (is("unsafe-name")) names[1] = "../b";
    if (!is("absent")) {
        put(targets[0], is("unchanged") || is("force-identical") ? payloads[0] : "local A");
        put(targets[1], is("unchanged") || is("force-identical") ? payloads[1] : "local B");
        put(targets[2], is("unchanged") || is("force-identical") ? payloads[2] : "local C");
    }
    char *args[] = { "os64get", "-a", "-q", "host", NULL };
    char *force[] = { "os64get", "-a", "-q", "-f", "host", NULL };
    char *noarchive[] = { "os64get", "-a", "-q", "-n", "host", NULL };
    char *url[] = { "os64get", "-q", "http://host/a", "/bin/a", NULL };
    if (is("url-https")) url[2] = "https://host/a";
    if (is("url-archive-blocked")) put("/home/archive", "not a directory");
    bool url_mode = !strncmp(scenario, "url", 3);
    char *single[] = { "os64get", "-q", "host", "a", "/bin/a", NULL };
    int rc = is("single") ? os64get_entry(5, single) : is("force-identical") ? os64get_entry(5, force) :
             is("no-archive") ? os64get_entry(5, noarchive) :
             url_mode ? os64get_entry(4, url) : os64get_entry(4, args);
    bool cancel = !strncmp(scenario, "cancel-", 7) || is("url-cancel");
    bool success = is("success") || is("absent") || is("unchanged") || is("force-identical") || is("no-archive") || is("url") || is("url-https") || is("url-archive-blocked") || is("single");
    assert(success ? rc == 0 : rc != 0);
    bool published = success || is("cancel-commit") || is("cancel-cleanup") || is("publish");
    assert(!cancel || rc == GET_CANCELLED);
    for (unsigned i = 0; i < 3; i++) {
        bool landed = published && !(is("publish") && i == 1) && !((url_mode || is("single")) && i != 0);
        contents(targets[i], landed ? payloads[i] : i == 0 ? "local A" : i == 1 ? "local B" : "local C");
        if (stages[i].backup[0]) contents(stages[i].backup, i == 0 ? "local A" : i == 1 ? "local B" : "local C");
    }
    if (is("absent") || is("unchanged") || is("force-identical") || is("no-archive")) assert(file_count("/home/archive") == 0);
    if (is("success") || is("publish") || is("cancel-commit")) assert(file_count("/home/archive") == 3);
    if (url_mode) {
        assert(backup_reads == 0);
        if (is("url-archive-blocked")) contents("/home/archive", "not a directory");
        else {
            os64_dirent_t archive;
            assert(os64_stat("/home/archive", &archive) < 0);
        }
    }
    if (is("single")) assert(file_count("/home/archive") == 1);
    if (!published) assert(installs == 0);
    assert(file_count("/tmp/os64get") == 0);
    assert(file_count("/home/.os64get-tmp") == 0);
    assert(file_count("/fat/.os64get-tmp") == 0);
    for (int i = 3; i < 1024; i++) assert(fdpaths[i][0] == 0);
    printf("PASS %s\n", scenario);
    return 0;
}
