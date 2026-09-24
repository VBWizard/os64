// Exercise full command reads and ps -f output without changing process records.
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../userland/libos64/proc_command.c"
#include "../userland/apps/ps/ps_format.c"

static char *input;
static size_t length, position, chunk, output_size;
static char output[2 * 1024 * 1024];
static int allocations, fail_allocation;
static bool open_failure, read_failure, close_failure;
static unsigned closes;
void *os64_malloc(size_t size) { return ++allocations == fail_allocation ? NULL : malloc(size); }
void *os64_realloc(void *ptr, size_t size) { return ++allocations == fail_allocation ? NULL : realloc(ptr, size); }
void os64_free(void *ptr) { free(ptr); }
int64_t os64_open(const char *path, const char *mode)
{
    (void)mode;
    assert(strcmp(path, "/proc/42/cmdline") == 0);
    position = 0;
    return open_failure ? -1 : 3;
}
int64_t os64_read(int32_t handle, void *buffer, size_t capacity)
{
    assert(handle == 3);
    if (read_failure) return -1;
    size_t n = length - position;
    if (n > capacity) n = capacity;
    if (n > chunk) n = chunk;
    memcpy(buffer, input + position, n);
    position += n;
    return (int64_t)n;
}
int64_t os64_close(int32_t handle)
{
    assert(handle == 3); closes++;
    return close_failure ? -1 : 0;
}
int64_t os64_write(int32_t handle, const void *data, size_t size)
{
    assert(handle == 1 && output_size + size < sizeof(output));
    if (size > 127) size = 127; // Force partial writes of long command text.
    memcpy(output + output_size, data, size);
    output_size += size; output[output_size] = 0;
    return (int64_t)size;
}
int64_t os64_puts(const char *s) { return os64_write(1, s, strlen(s)); }
char os64_proc_state_letter(os64_proc_state_t state) { (void)state; return 'R'; }
int32_t os64_proc_threads(uint64_t pid, os64_thread_info_t *out, size_t cap)
{ (void)pid; (void)out; (void)cap; return 0; }

static void setup(size_t bytes)
{
    free(input); input = malloc(bytes + 1); assert(input);
    memset(input, 'x', bytes); input[bytes] = 0;
    length = bytes; position = output_size = 0; chunk = 73;
    allocations = fail_allocation = 0;
    open_failure = read_failure = close_failure = false; closes = 0;
}
static void fails(void)
{
    char *text = (char *)1;
    assert(os64_proc_command(42, &text) == -1 && text == NULL);
    assert(closes == (open_failure ? 0u : 1u));
}
int main(void)
{
    size_t sizes[] = {0, 255, 256, 512, 513, 128 * 1024, 1024 * 1024};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        setup(sizes[i]); char *text;
        assert(os64_proc_command(42, &text) == 0);
        assert(strlen(text) == length && memcmp(text, input, length) == 0);
        assert(closes == 1); free(text);
    }
    setup(1024 * 1024 + 1); fails();
    setup(1024); fail_allocation = 1; fails();
    setup(1024); fail_allocation = 2; fails();
    setup(1024); open_failure = true; fails();
    setup(1024); read_failure = true; fails();
    setup(1024); close_failure = true; fails();
    setup(512); fail_allocation = 2;
    char *text; assert(os64_proc_command(42, &text) == 0 && allocations == 1); free(text);
    setup(1024); input[0] = '\n'; input[6] = '\n'; input[1023] = '\n';
    assert(os64_proc_command(42, &text) == 0);
    assert(strlen(text) == 1023 && text[0] == ' ' && text[6] == ' '); free(text);

    setup(128 * 1024); input[length - 1] = '\n';
    os64_proc_info_t task = {.pid = 42};
    strcpy(task.name, "example"); strcpy(task.command, "truncated summary");
    ps_options_t options = {.full = true, .all = true};
    ps_print(&task, 1, &options);
    assert(output_size >= length);
    assert(memcmp(output + output_size - length, input, length) == 0);
    setup(1024); open_failure = true;
    ps_print(&task, 1, &options);
    assert(strstr(output, "example [command unavailable]"));
    assert(!strstr(output, "truncated summary"));
    free(input);
    puts("proc command: complete reads, failure cleanup, and long ps -f output PASS");
}
