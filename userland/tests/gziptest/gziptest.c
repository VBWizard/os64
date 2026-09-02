// gziptest — ring-3 proof that the standalone libgzip dependency loads and
// decodes through the ordinary shared-object path.

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
    os64_exit(GZIPTEST_FAIL);
}

static void test_gunzip_filter(void)
{
    if (os64_snprintf(gCompressedPath, sizeof(gCompressedPath),
                      "/tmp/gziptest-%lu.gz", os64_taskid()) >=
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
    os64_gzip_t *stream = os64_gzip_create(1024);
    if (stream == NULL)
        fail("could not allocate decoder");

    size_t input_position = 0;
    size_t output_position = 0;
    os64_gzip_status_t status = OS64_GZIP_NEED_INPUT;
    while (status == OS64_GZIP_NEED_INPUT ||
           status == OS64_GZIP_NEED_OUTPUT) {
        size_t input_available = input_position < length ? 1 : 0;
        size_t output_available = 3;
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

int main(void)
{
    uint8_t decoded[sizeof(kExpected) + 16];
    size_t decoded_length = 0;
    os64_gzip_status_t status = decode(kCompressed, sizeof(kCompressed),
                                       decoded, &decoded_length);
    if (status != OS64_GZIP_DONE)
        fail(os64_gzip_status_name(status));
    if (decoded_length != sizeof(kExpected) - 1 ||
        !bytes_equal(decoded, kExpected, sizeof(kExpected) - 1))
        fail("decoded bytes differ");

    uint8_t corrupt[sizeof(kCompressed)];
    os64_memcpy(corrupt, kCompressed, sizeof(corrupt));
    corrupt[sizeof(corrupt) - 8] ^= 0x01;
    status = decode(corrupt, sizeof(corrupt), decoded, &decoded_length);
    if (status != OS64_GZIP_CHECKSUM)
        fail("damaged trailer was accepted");

    test_gunzip_filter();

    return GZIPTEST_OK;
}
