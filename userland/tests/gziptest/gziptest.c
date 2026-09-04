// gziptest — ring-3 proof that the standalone libgzip dependency loads and
// streams both directions through the ordinary shared-object path.

#include "os64/os64.h"
#include "gzip/gzip.h"

#define GZIPTEST_OK   0x621A0000u
#define GZIPTEST_FAIL 0x621A0001u

static const uint8_t kCompressed[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0xcb, 0xc9, 0x4c, 0x4a, 0xaf, 0xca, 0x2c, 0x50, 0x28, 0x4a,
    0x4d, 0x4c, 0xce, 0x48, 0x2d, 0x56, 0x28, 0xca, 0xcc, 0x4b,
    0x57, 0x28, 0xc9, 0x28, 0x4a, 0x4d, 0x05, 0x91, 0xf9, 0xa5,
    0xe9, 0x19, 0x0a, 0x99, 0x25, 0xc5, 0x0a, 0xf9, 0xe5, 0x79,
    0x0a, 0xc5, 0x19, 0x89, 0x45, 0xa9, 0x29, 0x0a, 0xf9, 0x49,
    0x59, 0xa9, 0xc9, 0x25, 0x7a, 0x5c, 0x00, 0x92, 0x32, 0xf9,
    0x63, 0x3a, 0x00, 0x00, 0x00
};

static const char kExpected[] =
    "libgzip reaches ring three through its own shared object.\n";
static char gCompressedPath[96];
static char gSourcePath[96];
static char gEncodedPath[100];
static char gFatSourcePath[96];
static char gFatEncodedPath[100];
static const char *gExt2ScratchDirectory;
static const char *gFatScratchDirectory;
static uint8_t gCommandPayload[8192];
static uint8_t gCommandCompressed[sizeof(gCommandPayload) * 2];
static uint8_t gCommandOutput[sizeof(gCommandPayload) + 16];

static bool bytes_equal(const uint8_t *left, const char *right, size_t length)
{
    for (size_t i = 0; i < length; i++)
        if (left[i] != (uint8_t)right[i])
            return false;
    return true;
}

static void fail(const char *reason)
{
    char message[192];
    os64_snprintf(message, sizeof(message), "gziptest: %s", reason);
    os64_serial_log(message);
    os64_printf("%s\n", message);
    if (gCompressedPath[0] != '\0')
        os64_unlink(gCompressedPath);
    if (gSourcePath[0] != '\0')
        os64_unlink(gSourcePath);
    if (gEncodedPath[0] != '\0')
        os64_unlink(gEncodedPath);
    if (gFatSourcePath[0] != '\0')
        os64_unlink(gFatSourcePath);
    if (gFatEncodedPath[0] != '\0')
        os64_unlink(gFatEncodedPath);
    os64_exit(GZIPTEST_FAIL);
}

static int32_t spawn_and_wait(const char *path, char *const argv[])
{
    int64_t child = os64_spawn(path, argv);
    int32_t code = -1;
    if (child < 0 || os64_wait(child, &code) < 0)
        fail("could not run child command");
    return code;
}

static bool directory_exists(const char *path)
{
    os64_dirent_t entry = {0};
    return os64_stat(path, &entry) == 0 &&
           (entry.flags & OS64_DE_DIR) != 0;
}

static void select_filesystem_layout(void)
{
    // The secondary mount names the root's opposite. Keep command replacement
    // tests on ext2 and refusal tests on FAT in both supported boot layouts.
    bool fat_is_secondary = directory_exists("/fat");
    bool ext2_is_secondary = directory_exists("/ext2");
    if (fat_is_secondary == ext2_is_secondary)
        fail("could not identify the FAT/ext2 mount layout");
    gExt2ScratchDirectory = fat_is_secondary ? "/tmp" : "/ext2";
    gFatScratchDirectory = fat_is_secondary ? "/fat" : "";
}

static void test_gunzip_filter(void)
{
    if (os64_snprintf(gCompressedPath, sizeof(gCompressedPath),
                      "%s/gziptest-%lu.gz", gExt2ScratchDirectory,
                      os64_taskid()) >=
        (int32_t)sizeof(gCompressedPath))
        fail("temporary path is too long");

    int32_t file = (int32_t)os64_open(gCompressedPath, "w");
    if (file < 0)
        fail("could not write gzip input for gunzip");
    bool wrote = os64_write(file, kCompressed, sizeof(kCompressed)) ==
                 (int64_t)sizeof(kCompressed);
    bool closed = os64_close(file) == 0;
    if (!wrote || !closed)
        fail("could not write gzip input for gunzip");

    int32_t pipe_ends[2];
    if (os64_pipe(pipe_ends) < 0)
        fail("could not create gunzip output pipe");
    char *const argv[] = { "/bin/gunzip", gCompressedPath, NULL };
    int64_t child = os64_spawn_redirected("/bin/gunzip", argv, -1,
                                          pipe_ends[1], -1, 0);
    os64_close(pipe_ends[1]);
    if (child < 0) {
        os64_close(pipe_ends[0]);
        fail("could not spawn gunzip");
    }

    uint8_t output[sizeof(kExpected) + 16];
    size_t have = 0;
    int64_t received;
    while ((received = os64_read(pipe_ends[0], output + have,
                                 sizeof(output) - have)) > 0)
        have += (size_t)received;
    os64_close(pipe_ends[0]);

    int32_t code = -1;
    if (received < 0 || os64_wait(child, &code) < 0 || code != 0 ||
        have != sizeof(kExpected) - 1 ||
        !bytes_equal(output, kExpected, sizeof(kExpected) - 1))
        fail("gunzip filter did not reproduce the payload");

    if (os64_unlink(gCompressedPath) < 0)
        fail("could not remove gunzip input");
    gCompressedPath[0] = '\0';
}

static os64_gzip_status_t decode(const uint8_t *compressed, size_t length,
                                 uint8_t *decoded, size_t *decoded_length)
{
    size_t decoded_capacity = *decoded_length;
    os64_gzip_t *stream = os64_gzip_create(decoded_capacity);
    if (stream == NULL)
        fail("could not allocate decoder");

    size_t input_position = 0;
    size_t output_position = 0;
    os64_gzip_status_t status = OS64_GZIP_NEED_INPUT;
    while (status == OS64_GZIP_NEED_INPUT ||
           status == OS64_GZIP_NEED_OUTPUT) {
        size_t input_available = input_position < length ? 1 : 0;
        size_t output_available = decoded_capacity - output_position;
        if (output_available > 3)
            output_available = 3;
        const uint8_t *input = compressed + input_position;
        uint8_t *output = decoded + output_position;
        bool finish = input_position + input_available == length;
        status = os64_gzip_process(stream, &input, &input_available,
                                   &output, &output_available, finish);
        input_position = (size_t)(input - compressed);
        output_position = (size_t)(output - decoded);
    }
    if (status == OS64_GZIP_DONE && os64_gzip_member_count(stream) != 1)
        fail("valid stream did not report one member");
    os64_gzip_destroy(stream);
    *decoded_length = output_position;
    return status;
}

static size_t encode(const uint8_t *plain, size_t length, uint8_t *compressed,
                     size_t capacity)
{
    os64_gzip_encoder_t *stream = os64_gzip_encoder_create(1234567890u);
    if (stream == NULL)
        fail("could not allocate encoder");

    size_t input_position = 0;
    size_t output_position = 0;
    os64_gzip_encode_status_t status = OS64_GZIP_ENCODE_NEED_INPUT;
    while (status == OS64_GZIP_ENCODE_NEED_INPUT ||
           status == OS64_GZIP_ENCODE_NEED_OUTPUT) {
        size_t input_available = input_position < length ? 1 : 0;
        size_t output_available = capacity - output_position;
        if (output_available == 0)
            fail("streaming encoder exceeded fixture output capacity");
        if (output_available > 2)
            output_available = 2;
        const uint8_t *input = plain + input_position;
        uint8_t *output = compressed + output_position;
        bool finish = input_position + input_available == length;
        status = os64_gzip_encoder_process(stream, &input, &input_available,
                                            &output, &output_available, finish);
        input_position = (size_t)(input - plain);
        output_position = (size_t)(output - compressed);
    }
    if (status != OS64_GZIP_ENCODE_DONE || input_position != length ||
        os64_gzip_encoder_input_size(stream) != length ||
        os64_gzip_encoder_output_size(stream) != output_position)
        fail("streaming encoder did not finish cleanly");
    os64_gzip_encoder_destroy(stream);
    return output_position;
}

static void write_command_source(void)
{
    static const char line[] =
        "[2026-09-03 03:00:00] cpu=0 task=logd daily compression line\n";
    size_t used = 0;
    while (used + sizeof(line) - 1 <= sizeof(gCommandPayload)) {
        os64_memcpy(gCommandPayload + used, line, sizeof(line) - 1);
        used += sizeof(line) - 1;
    }
    while (used < sizeof(gCommandPayload))
        gCommandPayload[used++] = '\n';

    int32_t file = (int32_t)os64_open(gSourcePath, "w");
    if (file < 0)
        fail("could not create gzip command input");
    size_t written = 0;
    while (written < sizeof(gCommandPayload)) {
        int64_t amount = os64_write(file, gCommandPayload + written,
                                    sizeof(gCommandPayload) - written);
        if (amount <= 0) {
            os64_close(file);
            fail("could not write gzip command input");
        }
        written += (size_t)amount;
    }
    if (os64_close(file) < 0)
        fail("could not close gzip command input");
}

static void check_command_output(void)
{
    int32_t pipe_ends[2];
    if (os64_pipe(pipe_ends) < 0)
        fail("could not create gzip command output pipe");
    char *const argv[] = { "/bin/gunzip", gEncodedPath, NULL };
    int64_t child = os64_spawn_redirected("/bin/gunzip", argv, -1,
                                          pipe_ends[1], -1, 0);
    os64_close(pipe_ends[1]);
    if (child < 0) {
        os64_close(pipe_ends[0]);
        fail("could not spawn gunzip for command output");
    }

    size_t have = 0;
    int64_t received;
    while (have < sizeof(gCommandOutput) &&
           (received = os64_read(pipe_ends[0], gCommandOutput + have,
                                 sizeof(gCommandOutput) - have)) > 0)
        have += (size_t)received;
    os64_close(pipe_ends[0]);
    int32_t code = -1;
    if (received < 0 || os64_wait(child, &code) < 0 || code != 0 ||
        have != sizeof(gCommandPayload) ||
        !bytes_equal(gCommandOutput, (const char *)gCommandPayload,
                     sizeof(gCommandPayload)))
        fail("gzip command output did not round-trip");
}

static void test_gzip_stdout(void)
{
    int32_t pipe_ends[2];
    if (os64_pipe(pipe_ends) < 0)
        fail("could not create gzip stdout pipe");
    char *const argv[] = { "/bin/gzip", "-c", gSourcePath, NULL };
    int64_t child = os64_spawn_redirected("/bin/gzip", argv, -1,
                                          pipe_ends[1], -1, 0);
    os64_close(pipe_ends[1]);
    if (child < 0) {
        os64_close(pipe_ends[0]);
        fail("could not spawn gzip -c");
    }

    size_t have = 0;
    int64_t received = 0;
    while (have < sizeof(gCommandCompressed) &&
           (received = os64_read(pipe_ends[0], gCommandCompressed + have,
                                 sizeof(gCommandCompressed) - have)) > 0)
        have += (size_t)received;
    os64_close(pipe_ends[0]);
    int32_t code = -1;
    os64_dirent_t entry = {0};
    if (received < 0 || os64_wait(child, &code) < 0 || code != 0 ||
        os64_stat(gSourcePath, &entry) < 0 ||
        os64_stat(gEncodedPath, &entry) == 0)
        fail("gzip -c did not leave only its input file");

    size_t decoded_size = sizeof(gCommandOutput);
    os64_gzip_status_t status = decode(gCommandCompressed, have,
                                       gCommandOutput, &decoded_size);
    if (status != OS64_GZIP_DONE ||
        decoded_size != sizeof(gCommandPayload) ||
        !bytes_equal(gCommandOutput, (const char *)gCommandPayload,
                     sizeof(gCommandPayload)))
        fail("gzip -c output did not round-trip");
}

static void test_gzip_command(void)
{
    if (os64_snprintf(gSourcePath, sizeof(gSourcePath),
                      "%s/gziptest-%lu.log", gExt2ScratchDirectory,
                      os64_taskid()) >=
            (int32_t)sizeof(gSourcePath) ||
        os64_snprintf(gEncodedPath, sizeof(gEncodedPath), "%s.gz",
                      gSourcePath) >= (int32_t)sizeof(gEncodedPath))
        fail("gzip command path is too long");

    write_command_source();
    test_gzip_stdout();
    char *const replace_argv[] = { "/bin/gzip", gSourcePath, NULL };
    if (spawn_and_wait("/bin/gzip", replace_argv) != 0)
        fail("gzip command could not compress a named file");
    os64_dirent_t entry = {0};
    if (os64_stat(gSourcePath, &entry) == 0 ||
        os64_stat(gEncodedPath, &entry) < 0 ||
        entry.size >= sizeof(gCommandPayload))
        fail("gzip command did not replace its input with a smaller .gz");
    check_command_output();
    if (os64_unlink(gEncodedPath) < 0)
        fail("could not remove first gzip command output");

    write_command_source();
    char *const keep_argv[] = { "/bin/gzip", "-k", gSourcePath, NULL };
    if (spawn_and_wait("/bin/gzip", keep_argv) != 0 ||
        os64_stat(gSourcePath, &entry) < 0 ||
        os64_stat(gEncodedPath, &entry) < 0)
        fail("gzip -k did not keep both files");
    uint64_t first_size = entry.size;
    if (spawn_and_wait("/bin/gzip", keep_argv) == 0 ||
        os64_stat(gEncodedPath, &entry) < 0 || entry.size != first_size)
        fail("gzip replaced an existing output without -f");

    char *const force_argv[] = {
        "/bin/gzip", "-f", "-k", gSourcePath, NULL
    };
    if (spawn_and_wait("/bin/gzip", force_argv) != 0 ||
        os64_stat(gSourcePath, &entry) < 0 ||
        os64_stat(gEncodedPath, &entry) < 0)
        fail("gzip -fk did not safely replace its output");
    check_command_output();

    if (os64_unlink(gSourcePath) < 0 || os64_unlink(gEncodedPath) < 0)
        fail("could not clean up gzip command files");
    gSourcePath[0] = '\0';
    gEncodedPath[0] = '\0';
}

static bool plant_file(const char *path, const uint8_t *bytes, size_t length)
{
    int32_t file = (int32_t)os64_open(path, "w");
    if (file < 0)
        return false;
    size_t written = 0;
    while (written < length) {
        int64_t amount = os64_write(file, bytes + written, length - written);
        if (amount <= 0)
            break;
        written += (size_t)amount;
    }
    bool ok = written == length && os64_sync(file) == 0;
    if (os64_close(file) < 0)
        ok = false;
    return ok;
}

static bool file_holds(const char *path, const uint8_t *bytes, size_t length)
{
    int32_t file = (int32_t)os64_open(path, "r");
    if (file < 0)
        return false;
    size_t have = 0;
    uint8_t byte = 0;
    while (have < length) {
        int64_t amount = os64_read(file, &byte, 1);
        if (amount != 1 || byte != bytes[have])
            break;
        have++;
    }
    bool ok = have == length && os64_read(file, &byte, 1) == 0;
    if (os64_close(file) < 0)
        ok = false;
    return ok;
}

static void test_gzip_fat_force_refusal(void)
{
    static const uint8_t source[] = "new log bytes\n";
    static const uint8_t destination[] = "old gzip bytes\n";
    if (os64_snprintf(gFatSourcePath, sizeof(gFatSourcePath),
                      "%s/gziptest-%lu.log", gFatScratchDirectory,
                      os64_taskid()) >=
            (int32_t)sizeof(gFatSourcePath) ||
        os64_snprintf(gFatEncodedPath, sizeof(gFatEncodedPath), "%s.gz",
                      gFatSourcePath) >= (int32_t)sizeof(gFatEncodedPath))
        fail("FAT gzip command path is too long");

    os64_unlink(gFatSourcePath);
    os64_unlink(gFatEncodedPath);
    if (!plant_file(gFatSourcePath, source, sizeof(source) - 1) ||
        !plant_file(gFatEncodedPath, destination, sizeof(destination) - 1))
        fail("could not create FAT force fixtures");

    char *const force_argv[] = {
        "/bin/gzip", "-f", "-k", gFatSourcePath, NULL
    };
    if (spawn_and_wait("/bin/gzip", force_argv) == 0 ||
        !file_holds(gFatSourcePath, source, sizeof(source) - 1) ||
        !file_holds(gFatEncodedPath, destination,
                    sizeof(destination) - 1))
        fail("gzip -f did not preserve both FAT files on refusal");

    if (os64_unlink(gFatSourcePath) < 0 ||
        os64_unlink(gFatEncodedPath) < 0)
        fail("could not clean up FAT force fixtures");
    gFatSourcePath[0] = '\0';
    gFatEncodedPath[0] = '\0';
}

int main(void)
{
    uint8_t encoded[256];
    size_t encoded_length = encode((const uint8_t *)kExpected,
                                   sizeof(kExpected) - 1, encoded,
                                   sizeof(encoded));
    uint8_t decoded[sizeof(kExpected) + 16];
    size_t decoded_length = sizeof(decoded);
    os64_gzip_status_t status = decode(encoded, encoded_length,
                                       decoded, &decoded_length);
    if (status != OS64_GZIP_DONE)
        fail(os64_gzip_status_name(status));
    if (decoded_length != sizeof(kExpected) - 1 ||
        !bytes_equal(decoded, kExpected, sizeof(kExpected) - 1))
        fail("decoded bytes differ");

    decoded_length = 8;
    status = decode(kCompressed, sizeof(kCompressed), decoded, &decoded_length);
    if (status != OS64_GZIP_LIMIT)
        fail("decoder did not enforce the caller's output buffer bound");

    uint8_t corrupt[sizeof(kCompressed)];
    os64_memcpy(corrupt, kCompressed, sizeof(corrupt));
    corrupt[sizeof(corrupt) - 8] ^= 0x01;
    decoded_length = sizeof(decoded);
    status = decode(corrupt, sizeof(corrupt), decoded, &decoded_length);
    if (status != OS64_GZIP_CHECKSUM)
        fail("damaged trailer was accepted");

    select_filesystem_layout();
    test_gunzip_filter();
    test_gzip_command();
    test_gzip_fat_force_refusal();

    return GZIPTEST_OK;
}
