// slurp.c — the one whole-file reader (os64/slurp.h carries the argument).

#include "os64/os64.h"
#include "os64/slurp.h"
#include "os64/io.h"
#include "os64/mem.h"

const char *os64_slurp_status_name(os64_slurp_status_t status)
{
    switch (status) {
        case OS64_SLURP_OK:        return "ok";
        case OS64_SLURP_NO_FILE:   return "no such file";
        case OS64_SLURP_TOO_BIG:   return "too big";
        case OS64_SLURP_IO_ERROR:  return "read error";
        case OS64_SLURP_NO_MEMORY: return "out of memory";
    }
    return "unknown";
}

os64_slurp_status_t os64_slurp(const char *path, size_t cap,
                               uint8_t **out, size_t *out_len)
{
    // Answer the out-parameters FIRST, so every early return below is already
    // telling the truth: "nothing partial is ever handed back" is a promise
    // the header makes, and a promise kept by construction beats one kept by
    // remembering to clear on each path.
    if (out != NULL)
        *out = NULL;
    if (out_len != NULL)
        *out_len = 0;

    if (path == NULL || out == NULL || out_len == NULL || cap == 0)
        return OS64_SLURP_NO_FILE;

    int32_t h = (int32_t)os64_open(path, "r");
    if (h < 0)
        return OS64_SLURP_NO_FILE;

    // cap + 1 so the NUL always has a home, even for a file that fills the
    // cap exactly. The terminator is a courtesy to text callers and is never
    // counted in the length.
    uint8_t *buf = (uint8_t *)os64_malloc(cap + 1);
    if (buf == NULL) {
        os64_close(h);
        return OS64_SLURP_NO_MEMORY;
    }

    size_t len = 0;
    os64_slurp_status_t status = OS64_SLURP_OK;

    for (;;) {
        if (len >= cap) {
            // AT THE CAP — and this is distinction 1 and 2 together. One more
            // byte is the only way to learn whether the file ended exactly
            // here or continues past what we were allowed to take.
            uint8_t probe;
            int64_t pr = os64_read(h, &probe, 1);
            if (pr == 0)
                break;                       // a clean end exactly at the cap: whole
            // Anything else means we cannot hand this back as the file. A
            // POSITIVE answer says it continues (genuinely too big); a
            // NEGATIVE one says we could not find out, and "could not find
            // out" has never been a licence to publish a prefix.
            status = (pr > 0) ? OS64_SLURP_TOO_BIG : OS64_SLURP_IO_ERROR;
            break;
        }

        int64_t n = os64_read(h, buf + len, cap - len);
        if (n < 0) { status = OS64_SLURP_IO_ERROR; break; }   // distinction 3
        if (n == 0) break;                                     // genuine end of file
        len += (size_t)n;                                      // distinction 4: keep going
    }

    os64_close(h);

    if (status != OS64_SLURP_OK) {
        os64_free(buf);
        return status;
    }

    buf[len] = 0;
    *out = buf;
    *out_len = len;
    return OS64_SLURP_OK;
}
