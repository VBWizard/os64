#!/usr/bin/env python3
"""Exercise Gopher's request admission with real wire encoding and short I/O."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'userland/apps/gopher/gopher.c').read_text()
helper = re.search(r'static bool request_send\([^}]+\}', source).group(0)
patience = re.search(r'^#define GOPHER_IDLE_MS\s+\d+', source, re.M).group(0)
# Both callers must stop before reading a response or creating a partial file.
assert source.count('if (!request_send((int32_t)conn, addr)) {\n        os64_close((int32_t)conn);') == 2
program = '''#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "wire.h"
#include "os64/io.h"
#include "os64/signal.h"
int64_t os64_write(int32_t handle, const void *buf, size_t len) {
    (void)handle; (void)buf; (void)len;
    assert(!"request must use finite patience");
    return -1;
}
static int64_t result;
static unsigned calls;
static const char expected[] = "/search\\twords\\r\\n";
int64_t os64_write_for(int32_t conn, const void *buf, size_t len, uint64_t ms) {
    assert(conn == 3 && ms == 30000 && len == sizeof(expected)-1);
    assert(memcmp(buf, expected, len) == 0);
    calls++;
    return result;
}
''' + patience + '\n' + helper + '''
int main(void) {
    gopher_addr_t addr = {0};
    addr.type = '7';
    strcpy(addr.selector, "/search");
    strcpy(addr.query, "words");
    // A partial request is a failed request, including zero progress. No
    // retry may resend its prefix or restart the request's finite budget.
    for (result = 0; result <= (int64_t)sizeof(expected)-1; result++) {
        calls = 0;
        assert(request_send(3, &addr) == (result == (int64_t)sizeof(expected)-1));
        assert(calls == 1);
    }
    int64_t errors[] = {OS64_ERR_TIMEOUT, OS64_INTERRUPTED, -1};
    for (unsigned i = 0; i < sizeof(errors)/sizeof(*errors); i++) {
        result = errors[i]; calls = 0;
        assert(!request_send(3, &addr) && calls == 1);
    }
    puts("Gopher request: finite budget, exact bytes, every short prefix and timeout/interruption/error PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='gopher-request-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'userland/apps/gopher'),
                    '-I'+str(root/'userland/libos64/include'), '-I'+str(root/'abi/include'),
                    str(root/'userland/apps/gopher/wire.c'),
                    *[str(root/'userland/libos64'/f) for f in ('url.c', 'str.c', 'fmt.c')],
                    str(work/'test.c'), '-o', str(work/'test')], check=True)
    subprocess.run([str(work/'test')], check=True)
