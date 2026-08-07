// linereader.c — buffered, forward-only line reading for files.
//
// Born 2026-08-06 from a grep autopsy (full story at the typedef in io.h):
// os64_readline's seek-back gait bills one backward seek plus a chunk
// re-read per line, and on a 46MB log the platform made both expensive —
// FAT walks the cluster chain from the top for every backward seek
// (FF_USE_FASTSEEK=0, booked in DEBTS), and ext2 re-serves the same disk
// blocks from the NVMe because no page cache exists yet to say "you just
// read that". This reader touches every byte of the file EXACTLY ONCE:
// one 64KB read per 64KB of file, zero seeks, lines dispensed from memory.
//
// The 1976 lesson, applied 50 years on: buffering belongs between read()
// and the byte-consumer. Lesk's portable I/O library earned its keep on
// machines where a system call cost milliseconds; ours cost microseconds
// plus a filesystem's worth of amplification, and the arithmetic came out
// the same.

#include <stdbool.h>
#include "os64/io.h"
#include "os64/mem.h"   // os64_map/os64_unmap — the chunk's landlord

// 64KB: sixteen pages, one syscall per 64KB of file. Big enough that the
// syscall count vanishes from any profile; small enough that a dozen
// concurrent readers wouldn't dent an 8GB machine (or embarrass a smaller
// one — this is a hobby OS, not a promise that RAM is infinite).
#define LR_CHUNK_SIZE (64u * 1024u)

int64_t os64_linereader_open(os64_linereader_t *lr, const char *path)
{
    if (lr == NULL || path == NULL)
        return -1;

    lr->handle = (int32_t)os64_open(path, "r");
    if (lr->handle < 0)
        return -1;

    lr->chunk = os64_map(LR_CHUNK_SIZE);
    if (lr->chunk == NULL)
    {
        os64_close(lr->handle);
        lr->handle = -1;
        return -1;
    }

    lr->cap = LR_CHUNK_SIZE;
    lr->len = 0;
    lr->pos = 0;
    lr->eof = false;
    return 0;
}

// Same contract as os64_readline, verbatim: 1 = *buf holds a line (ending
// stripped, NUL-terminated, "\r\n" treated as one ending), 0 = end of
// input (a final unterminated line is delivered as a line FIRST), negative
// = the underlying read's error. Over-long lines are truncated to cap-1
// and the remainder CONSUMED, so the next call starts at the next line —
// never the severed tail of this one.
int64_t os64_linereader_line(os64_linereader_t *lr, char *buf, size_t cap)
{
    if (lr == NULL || buf == NULL || cap == 0 || lr->chunk == NULL)
        return -1;

    size_t stored = 0;
    bool sawAny = false;
    bool hitNewline = false;
    bool dropped = false;

    while (!hitNewline)
    {
        // Refill when the buffer runs dry. This is the ONLY read in the
        // reader's life, and it never looks back.
        if (lr->pos >= lr->len)
        {
            if (lr->eof)
                break;
            int64_t n = os64_read(lr->handle, lr->chunk, lr->cap);
            if (n < 0)
                return n;
            if (n == 0)
            {
                lr->eof = true;
                break;
            }
            lr->len = (size_t)n;
            lr->pos = 0;
        }

        // Scan the buffered bytes for this line's newline.
        size_t nl = lr->pos;
        while (nl < lr->len && lr->chunk[nl] != '\n')
            nl++;

        // Copy what fits; consume it all either way (truncation contract).
        size_t take = nl - lr->pos;
        size_t room = (stored + 1 < cap) ? (cap - 1 - stored) : 0;
        size_t copy = (take < room) ? take : room;
        for (size_t i = 0; i < copy; i++)
            buf[stored + i] = lr->chunk[lr->pos + i];
        stored += copy;
        if (copy < take)
            dropped = true;
        if (take > 0 || nl < lr->len)
            sawAny = true;

        if (nl < lr->len)
        {
            lr->pos = nl + 1;   // step past the '\n'
            hitNewline = true;
        }
        else
            lr->pos = lr->len;  // chunk exhausted mid-line — refill and go on
    }

    // "\r\n" is one ending — strip the '\r' only when it truly was the byte
    // before the '\n' AND made it into buf (same guard as os64_readline: if
    // bytes were dropped, buf's last byte is interior data).
    if (hitNewline && !dropped && stored > 0 && buf[stored - 1] == '\r')
        stored--;

    buf[stored] = '\0';
    return sawAny ? 1 : 0;
}

void os64_linereader_close(os64_linereader_t *lr)
{
    if (lr == NULL)
        return;
    if (lr->chunk != NULL)
    {
        os64_unmap(lr->chunk);
        lr->chunk = NULL;
    }
    if (lr->handle >= 0)
    {
        os64_close(lr->handle);
        lr->handle = -1;
    }
}
