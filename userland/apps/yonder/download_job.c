// download_job.c — a download, on a worker (download_job.h).

#include "download_job.h"
#include "dom/dom.h"

#include "os64/fmt.h"
#include "os64/gui.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "ticker.h"

// How often the window hears that bytes arrived: often enough for a meter
// to move, seldom enough that a fast download costs the window nothing.
#define RING_MS 200

static void fail(yonder_download_job_t *j, const char *why)
{
    os64_strcopy(j->why, sizeof(j->why), why);
}

// What the server said, when it said something other than a body to keep:
// an error's body is its page, not the file asked for.
static bool refused(yonder_download_job_t *j, const os64_fetch_head_t *head)
{
    if (head->status >= 200 && head->status < 300)
        return false;
    os64_snprintf(j->why, sizeof(j->why), "the server answered %d%s%s", (int)head->status,
                  head->reason[0] != '\0' ? " " : "", head->reason);
    return true;
}

// Every byte of `buf[0..len)` written, or false.
static bool write_all(int32_t h, const uint8_t *buf, size_t len)
{
    while (len > 0) {
        int64_t n = os64_write(h, buf, len);
        if (n <= 0)
            return false;
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

// How many staging names a download tries before it gives up on the folder.
#define STAGING_TRIES 100

// A staging file of this job's own, created for writing beside `path`:
// `<path>.part`, else `<path>.1.part`, `<path>.2.part`... Each is created
// exclusively, so a file already there (another download of the same name,
// or something of the person's) is never truncated and never removed; the
// name taken is left in `j->part`. A handle, or -1 with `j->why` saying why.
static int64_t staging_open(yonder_download_job_t *j)
{
    for (int32_t k = 0; k < STAGING_TRIES; k++) {
        char name[sizeof(j->part)];
        int n = k == 0 ? os64_snprintf(name, sizeof(name), "%s.part", j->path)
                       : os64_snprintf(name, sizeof(name), "%s.%d.part", j->path, (int)k);
        if (n < 0 || (size_t)n >= sizeof(name)) {
            fail(j, "its name is too long");
            return -1;
        }
        int64_t h = os64_open(name, "x");
        if (h >= 0) {
            os64_strcopy(j->part, sizeof(j->part), name);
            return h;
        }
        // A refusal for anything but the name being taken is the folder's.
        os64_dirent_t e;
        if (os64_stat(name, &e) < 0)
            break;
    }
    fail(j, "the folder would not take a new file");
    return -1;
}

static int64_t publish(yonder_download_job_t *j, int64_t h, bool whole, bool stopped,
                       bool (*cancelled)(void *ctx), void *ctx);

// A blob: address's download (BLOB_URLS.md): its entry's bytes written
// from where they are, a chunk at a time so the meter moves.
static int64_t blob_run(yonder_download_job_t *j, bool (*cancelled)(void *ctx), void *ctx)
{
    const uint8_t *bytes = os64_dom_blob_bytes(j->blob);
    size_t len = os64_dom_blob_length(j->blob), at = 0;
    __atomic_store_n(&j->total, (uint64_t)len, __ATOMIC_RELAXED);
    int64_t h = staging_open(j);
    if (h < 0)
        return 0;
    uint64_t rang = yonder_now_ms();
    bool written = true;
    while (written && at < len && !cancelled(ctx)) {
        size_t n = len - at < YONDER_DOWNLOAD_CHUNK ? len - at : YONDER_DOWNLOAD_CHUNK;
        written = write_all((int32_t)h, bytes + at, n);
        at += n;
        __atomic_store_n(&j->got, (uint64_t)at, __ATOMIC_RELAXED);
        if (yonder_now_ms() - rang >= RING_MS) {
            rang = yonder_now_ms();
            (void)os64_gui_event_ring(j->window, j->bell);
        }
    }
    bool stopped = cancelled(ctx);
    if (!written)
        fail(j, "the disk would not take more: full, or past its largest file");
    return publish(j, h, written && !stopped && at == len, stopped, cancelled, ctx);
}

int64_t yonder_download_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_download_job_t *j = job;
    (void)out;                          // no product: the job is its own record
    if (cancelled(ctx))
        return -1;
    if (j->blob != NULL)
        return blob_run(j, cancelled, ctx);
    os64_fetch_options_t opt = {0};
    opt.user_agent = j->agent;
    opt.accept = "*/*";
    // The bytes as the server keeps them: a .tar.gz is not to arrive
    // decompressed because the server also spoke gzip.
    opt.identity = true;
    j->hooks.cancelled = cancelled;
    j->hooks.cancel_ctx = ctx;
    way_fetch_hooks(&j->hooks, &opt);
    os64_fetch_t *f = os64_fetch_open(j->url, &opt);
    if (f == NULL) {
        fail(j, "out of memory");
        return 0;
    }
    const os64_fetch_head_t *head = os64_fetch_head(f);
    if (head == NULL || refused(j, head)) {
        if (head == NULL)
            fail(j, os64_fetch_reason(f));
        os64_fetch_close(f);
        return cancelled(ctx) ? -1 : 0;
    }
    __atomic_store_n(&j->total, head->has_length ? head->length : 0, __ATOMIC_RELAXED);
    uint8_t *buf = os64_malloc(YONDER_DOWNLOAD_CHUNK);
    if (buf == NULL)
        fail(j, "out of memory");
    int64_t h = buf != NULL ? staging_open(j) : -1;
    if (h < 0) {
        os64_free(buf);
        os64_fetch_close(f);
        return 0;
    }
    uint64_t got = 0, rang = yonder_now_ms();
    int64_t n;
    bool written = true;
    while (written && !cancelled(ctx) && (n = os64_fetch_read(f, buf, YONDER_DOWNLOAD_CHUNK)) > 0) {
        written = write_all((int32_t)h, buf, (size_t)n);
        got += (uint64_t)n;
        __atomic_store_n(&j->got, got, __ATOMIC_RELAXED);
        if (yonder_now_ms() - rang >= RING_MS) {
            rang = yonder_now_ms();
            (void)os64_gui_event_ring(j->window, j->bell);
        }
    }
    os64_free(buf);
    bool stopped = cancelled(ctx);
    bool whole = written && !stopped && os64_fetch_status(f) == OS64_FETCH_OK;
    // A file system refuses a write for want of room and for a file grown
    // past the largest it holds, and says neither: the row names both, and
    // the size it stopped at (download_row) tells them apart.
    if (!written)
        fail(j, "the disk would not take more: full, or past its largest file");
    else if (!stopped && !whole)
        fail(j, os64_fetch_reason(f));
    os64_fetch_close(f);
    return publish(j, h, whole, stopped, cancelled, ctx);
}

// The staging file `h` made the download, or removed: synced, closed and
// renamed under its name when `whole` held, and the window told either way.
static int64_t publish(yonder_download_job_t *j, int64_t h, bool whole, bool stopped,
                       bool (*cancelled)(void *ctx), void *ctx)
{
    if (whole && os64_sync((int32_t)h) < 0) {
        whole = false;
        fail(j, "the disk would not take it (is it full?)");
    }
    if (os64_close((int32_t)h) < 0 && whole) {
        whole = false;
        fail(j, "the disk would not take it (is it full?)");
    }
    // The sync and the close are disk work, long enough for a cancel to
    // land in; the row already says cancelled, so the file must not appear.
    if (whole && cancelled(ctx)) {
        whole = false;
        stopped = true;
    }
    if (whole) {
        int64_t rc = j->replace ? os64_rename(j->part, j->path)
                                : os64_rename_with_flags(j->part, j->path, OS64_RENAME_NOREPLACE);
        if (rc < 0) {
            whole = false;
            fail(j, j->replace ? "it could not be put under its name"
                               : "a file of that name appeared meanwhile");
        }
    }
    if (!whole)
        (void)os64_unlink(j->part);
    j->whole = whole;
    (void)os64_gui_event_ring(j->window, j->bell);
    return whole ? 1 : stopped ? -1 : 0;
}

void yonder_download_release(void *job, void *product)
{
    (void)product;
    // Nothing to remove: a run cancelled before it began wrote nothing, and
    // one that ran published or removed its own staging file before returning.
    if (job != NULL)
        os64_dom_blob_release(((yonder_download_job_t *)job)->blob);
    os64_free(job);
}
