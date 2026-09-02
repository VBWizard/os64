// tartest — the ring-3 fixture for the ustar utility.
//
// Exit codes 0x7A1200xx ("TAR" with the step in the low byte):
//   0x7A120000  success
//   0x7A120001  could not create the private test directory
//   0x7A120002  could not create the source tree
//   0x7A120003  create failed
//   0x7A120004  list failed or omitted a member
//   0x7A120005  extract failed or restored wrong bytes
//   0x7A120006  create-to-pipe/list-from-pipe failed
//   0x7A120007  an archive included itself
//   0x7A120008  failed creation replaced an existing archive
//   0x7A120009  an escaping member wrote outside the extraction directory
//   0x7A12000A  truncated extraction replaced the existing target

#include "os64/os64.h"
#include "os64/slurp.h"
#include "../../apps/tar/tar_format.h"

#define STEP(n) (0x7A120000u | (uint32_t)(n))

static char gWorkPath[TAR_PATH_CAP];

static bool put_bytes(const char *path, const void *data, size_t length)
{
    int32_t handle = (int32_t)os64_open(path, "w");
    if (handle < 0)
        return false;

    const uint8_t *bytes = data;
    size_t written = 0;
    while (written < length) {
        int64_t result = os64_write(handle, bytes + written, length - written);
        if (result <= 0)
            break;
        written += (size_t)result;
    }
    bool closed = os64_close(handle) == 0;
    return written == length && closed;
}

static bool file_equals(const char *path, const char *expected)
{
    uint8_t *contents = NULL;
    size_t length = 0;
    os64_slurp_status_t status = os64_slurp(path, 16384, &contents, &length);
    size_t expected_length = os64_strlen(expected);
    bool equal = status == OS64_SLURP_OK && length == expected_length;
    for (size_t i = 0; equal && i < length; i++)
        if (contents[i] != (uint8_t)expected[i])
            equal = false;
    os64_free(contents);
    return equal;
}

static bool file_contains(const char *path, const char *needle)
{
    uint8_t *contents = NULL;
    size_t length = 0;
    if (os64_slurp(path, 16384, &contents, &length) != OS64_SLURP_OK)
        return false;

    size_t needle_length = os64_strlen(needle);
    bool found = false;
    for (size_t i = 0; !found && i + needle_length <= length; i++) {
        size_t matched = 0;
        while (matched < needle_length &&
               contents[i + matched] == (uint8_t)needle[matched])
            matched++;
        found = matched == needle_length;
    }
    os64_free(contents);
    return found;
}

static int run_tar(char *const argv[], int32_t input, int32_t output)
{
    int64_t task = os64_spawn_redirected("/bin/tar", argv, input, output,
                                         -1, 0);
    if (task < 0)
        return -1;
    int32_t code = -1;
    return os64_wait(task, &code) < 0 ? -1 : code;
}

static void cleanup(void)
{
    if (gWorkPath[0] == '\0')
        return;

    if (os64_chdir(gWorkPath) == 0) {
        os64_unlink("source/sub/beta.txt");
        os64_unlink("source/sub");
        os64_unlink("source/alpha.txt");
        os64_unlink("source/inside.tar");
        os64_unlink("source");
        os64_unlink("archive.tar");
        os64_unlink("listing.txt");
        os64_unlink("pipe-list.txt");
        os64_unlink("self-list.txt");
        os64_unlink("keep.tar");
        os64_unlink("unsafe.tar");
        os64_unlink("truncated.tar");
        os64_unlink("victim.txt");
    }
    os64_chdir("/tmp");
    os64_unlink(gWorkPath);
    os64_unlink("/tmp/tartest-escaped.txt");
}

static void die(uint32_t step, const char *reason)
{
    char message[256];
    os64_snprintf(message, sizeof(message), "tartest: %s", reason);
    os64_serial_log(message);
    os64_printf("%s\n", message);
    cleanup();
    os64_exit(STEP(step));
}

static bool write_test_archive(const char *path, const char *member,
                               uint64_t declared_size, const char *payload,
                               size_t payload_size, bool finish)
{
    tar_entry_t entry = {
        .size = declared_size,
        .mtime = 1,
        .mode = 0644,
        .type = TAR_TYPE_REGULAR
    };
    if (os64_strcopy(entry.path, sizeof(entry.path), member) >=
        sizeof(entry.path))
        return false;

    uint8_t header[TAR_BLOCK_SIZE];
    if (tar_header_encode(header, &entry) != TAR_FORMAT_OK)
        return false;

    int32_t handle = (int32_t)os64_open(path, "w");
    if (handle < 0)
        return false;
    bool ok = os64_write(handle, header, sizeof(header)) == sizeof(header) &&
              os64_write(handle, payload, payload_size) == (int64_t)payload_size;

    if (ok && finish) {
        uint8_t zero[TAR_BLOCK_SIZE] = {0};
        size_t padding = (size_t)((TAR_BLOCK_SIZE -
                                   declared_size % TAR_BLOCK_SIZE) %
                                  TAR_BLOCK_SIZE);
        ok = os64_write(handle, zero, padding) == (int64_t)padding &&
             os64_write(handle, zero, sizeof(zero)) == sizeof(zero) &&
             os64_write(handle, zero, sizeof(zero)) == sizeof(zero);
    }
    if (os64_close(handle) < 0)
        ok = false;
    return ok;
}

int main(void)
{
    if (os64_snprintf(gWorkPath, sizeof(gWorkPath), "/tmp/tartest-%lu",
                      os64_taskid()) >= (int32_t)sizeof(gWorkPath) ||
        os64_mkdir(gWorkPath) < 0 || os64_chdir(gWorkPath) < 0)
        die(1, "could not create private directory");

    if (os64_mkdir("source") < 0 || os64_mkdir("source/sub") < 0 ||
        !put_bytes("source/alpha.txt", "alpha\n", 6) ||
        !put_bytes("source/sub/beta.txt", "beta\n", 5))
        die(2, "could not create source tree");

    char *const create[] = {
        "/bin/tar", "-cf", "archive.tar", "source", NULL
    };
    if (run_tar(create, -1, -1) != 0)
        die(3, "create failed");

    int32_t listing = (int32_t)os64_open("listing.txt", "w");
    char *const list[] = {"/bin/tar", "-tf", "archive.tar", NULL};
    if (listing < 0 || run_tar(list, -1, listing) != 0 ||
        os64_close(listing) < 0 ||
        !file_contains("listing.txt", "source/alpha.txt\n") ||
        !file_contains("listing.txt", "source/sub/beta.txt\n"))
        die(4, "list failed or omitted a member");

    os64_unlink("source/sub/beta.txt");
    os64_unlink("source/sub");
    os64_unlink("source/alpha.txt");
    os64_unlink("source");
    char *const extract[] = {"/bin/tar", "-xf", "archive.tar", NULL};
    if (run_tar(extract, -1, -1) != 0 ||
        !file_equals("source/alpha.txt", "alpha\n") ||
        !file_equals("source/sub/beta.txt", "beta\n"))
        die(5, "extract failed or restored wrong bytes");

    int32_t pipe_ends[2];
    int32_t pipe_listing = (int32_t)os64_open("pipe-list.txt", "w");
    char *const pipe_create[] = {"/bin/tar", "-cf", "-", "source", NULL};
    char *const pipe_list[] = {"/bin/tar", "-tf", "-", NULL};
    if (pipe_listing < 0 || os64_pipe(pipe_ends) < 0)
        die(6, "could not create pipeline");
    int64_t writer = os64_spawn_redirected("/bin/tar", pipe_create, -1,
                                            pipe_ends[1], -1, 0);
    int64_t reader = os64_spawn_redirected("/bin/tar", pipe_list, pipe_ends[0],
                                            pipe_listing, -1, 0);
    os64_close(pipe_ends[0]);
    os64_close(pipe_ends[1]);
    os64_close(pipe_listing);
    int32_t writer_code = -1;
    int32_t reader_code = -1;
    if (writer < 0 || reader < 0 ||
        os64_wait(writer, &writer_code) < 0 ||
        os64_wait(reader, &reader_code) < 0 ||
        writer_code != 0 || reader_code != 0 ||
        !file_contains("pipe-list.txt", "source/sub/beta.txt\n"))
        die(6, "create-to-pipe/list-from-pipe failed");

    char *const self_create[] = {
        "/bin/tar", "-cf", "source/inside.tar", "source", NULL
    };
    char *const self_list[] = {
        "/bin/tar", "-tf", "source/inside.tar", NULL
    };
    int32_t self_listing = (int32_t)os64_open("self-list.txt", "w");
    if (self_listing < 0 || run_tar(self_create, -1, -1) != 0 ||
        run_tar(self_list, -1, self_listing) != 0 ||
        os64_close(self_listing) < 0 ||
        file_contains("self-list.txt", "inside.tar"))
        die(7, "archive included itself");

    if (!put_bytes("keep.tar", "keep this\n", 10))
        die(8, "could not prepare existing archive");
    char *const fail_create[] = {
        "/bin/tar", "-cf", "keep.tar", "missing-source", NULL
    };
    if (run_tar(fail_create, -1, -1) == 0 ||
        !file_equals("keep.tar", "keep this\n"))
        die(8, "failed creation replaced existing archive");

    os64_unlink("/tmp/tartest-escaped.txt");
    if (!write_test_archive("unsafe.tar", "../tartest-escaped.txt", 4,
                            "bad\n", 4, true))
        die(9, "could not prepare unsafe archive");
    char *const unsafe_extract[] = {
        "/bin/tar", "-xf", "unsafe.tar", NULL
    };
    os64_dirent_t escaped = {0};
    if (run_tar(unsafe_extract, -1, -1) == 0 ||
        os64_stat("/tmp/tartest-escaped.txt", &escaped) == 0)
        die(9, "escaping member wrote outside extraction directory");

    if (!put_bytes("victim.txt", "original\n", 9) ||
        !write_test_archive("truncated.tar", "victim.txt", 12,
                            "cut", 3, false))
        die(10, "could not prepare truncated archive");
    char *const truncated_extract[] = {
        "/bin/tar", "-xf", "truncated.tar", NULL
    };
    if (run_tar(truncated_extract, -1, -1) == 0 ||
        !file_equals("victim.txt", "original\n"))
        die(10, "truncated extraction replaced existing target");

    cleanup();
    return STEP(0);
}
