// slurp.c — the tree's SHARED whole-file reader (os64/slurp.h carries the
// argument). Shared, not yet sole: kernel/src/conf.c and both read paths in
// libos64/conf.c still carry hand-written loops as this is committed (Codex
// #30 rd1, rd8; gui/desktop.c's copy died with the kernel desktop, 2026-08-25)
// — the DEBTS row on the conf readers is the remaining payment, and anyone
// fixing a whole-file-read bug has to visit all of them until it is paid.

#include "os64/os64.h"
#include "os64/slurp.h"
#include "os64/io.h"
#include "os64/mem.h"

const char *os64_slurp_status_name(os64_slurp_status_t status)
{
    switch (status) {
        case OS64_SLURP_OK:        return "ok";
        case OS64_SLURP_NO_FILE:   return "could not open";   // not "no such file" — see the NO_FILE paragraph in slurp.h
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

    if (path == NULL || out == NULL || out_len == NULL)
        return OS64_SLURP_NO_FILE;
    // cap == 0 is NOT refused (Codex #30 rd9): it means "at most zero bytes",
    // and the normal path already answers that truthfully — an empty file is
    // OK with len 0, anything else is TOO_BIG. Answering NO_FILE instead,
    // as this used to, told a caller a file that EXISTS was absent — and
    // NO_FILE is reserved for a file that could not be OPENED (rd10).

    // A CAP OF SIZE_MAX HAS NO ROOM FOR THE TERMINATOR (Codex #30 rd2).
    // `cap + 1` would wrap to zero, os64_malloc(0) hands back a real
    // minimum-sized block, and the loop below would then read up to SIZE_MAX
    // bytes into it — a heap overflow reached by asking for a cap instead of
    // by supplying a file. Refused as TOO_BIG rather than clamped, because
    // silently reading less than a caller asked for is how the truncation
    // bugs in this function's own history started: a cap this reader cannot
    // honour is a request it should decline, not reinterpret.
    if (cap == (size_t)-1)
        return OS64_SLURP_TOO_BIG;

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
