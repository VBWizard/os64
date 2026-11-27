// A run transcript has its own file and is independent of the live UI sink.
#include "transcript.h"
#include "os64/os64.h"
#include "os64/date.h"

static bool ensure_directory(const char *path)
{
    os64_dirent_t e;
    if (os64_stat(path, &e) < 0) {
        // A concurrent invocation may create the directory first.
        os64_mkdir(path);
        if (os64_stat(path, &e) < 0)
            return false;
    }
    return (e.flags & OS64_DE_DIR) != 0;
}

bool porter_write_all(int32_t fd, const char *data, size_t size)
{
    while (size) {
        int64_t n = os64_write(fd, data, size);
        if (n <= 0 || (uint64_t)n > size)
            return false;
        data += n;
        size -= (size_t)n;
    }
    return true;
}

void porter_transcript_write(porter_transcript_t *log, const char *data, size_t size)
{
    if (log->fd >= 0 && !log->failed)
        log->failed = !porter_write_all(log->fd, data, size);
}

// Quote each argv element separately. Backslash, double quote and byte
// escapes let a reader recover spaces, empty values and nonprinting bytes.
static void argument(porter_transcript_t *log, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    char buf[256] = {' ', '"'};
    size_t size = 2;
    for (const unsigned char *p = (const unsigned char *)value; *p && !log->failed; p++) {
        if (sizeof(buf) - size < 4) {
            porter_transcript_write(log, buf, size);
            size = 0;
        }
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            buf[size++] = '\\';
            buf[size++] = (char)c;
        } else if (c < 32 || c >= 127) {
            buf[size++] = '\\';
            buf[size++] = 'x';
            buf[size++] = hex[c >> 4];
            buf[size++] = hex[c & 15];
        } else {
            buf[size++] = (char)c;
        }
    }
    porter_transcript_write(log, buf, size);
    porter_transcript_write(log, "\"", 1);
}

bool porter_transcript_open(porter_transcript_t *log, int argc, char *const argv[])
{
    *log = (porter_transcript_t){.fd = -1};
    if (!ensure_directory("/var") || !ensure_directory(PORTER_LOG_DIR))
        return false;
    os64_time_t now = {0};
    os64_time(&now);
    os64_date_t utc;
    os64_date_from_epoch(now.epoch, &utc);
    unsigned long task = (unsigned long)os64_taskid();
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        char path[PORTER_PATH];
        os64_snprintf(path, sizeof(path), PORTER_LOG_DIR "/run-%04d%02d%02d-%02d%02d%02dZ-%lu-%u.log",
                      utc.year, utc.month, utc.day, utc.hour, utc.minute, utc.second, task, attempt);
        int64_t fd = os64_open(path, "x");
        if (fd >= 0) {
            log->fd = (int32_t)fd;
            os64_strcopy(log->path, sizeof(log->path), path);
            porter_transcript_write(log, "Command:", 8);
            for (int i = 0; i < argc && !log->failed; i++)
                argument(log, argv[i]);
            porter_transcript_write(log, "\n", 1);
            return !log->failed;
        }
        os64_dirent_t e;
        if (os64_stat(path, &e) < 0)
            return false;
    }
    return false;
}

bool porter_transcript_close(porter_transcript_t *log, int status)
{
    if (log->fd < 0)
        return false;
    char footer[32];
    int n = os64_snprintf(footer, sizeof(footer), "\nExit: %d\n", status);
    porter_transcript_write(log, footer, (size_t)n);
    bool closed = os64_close(log->fd) == 0;
    log->fd = -1;
    return closed && !log->failed;
}
