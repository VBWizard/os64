#include "tty.h"
#include "task.h"
#include "handle.h"
#include "pipe.h"
#include "memory/kmalloc.h"

/* Exercise the commit's three ownership outcomes deterministically. A
 * successful publication can close itself or be closed by a later sweep;
 * the creator's hold must survive either until its final diagnostic read. */
bool test_pty_publication_hold(void)
{
    for (unsigned mode = PTY_MODE_GRID; mode <= PTY_MODE_STREAM; mode++)
    {
        for (unsigned outcome = 0; outcome < 3; outcome++)
        {
            task_t *owner = kmalloc(sizeof(*owner));
            int slot = handle_reserve(owner);
            tty_t *slave = pty_create_slave(80, 24, mode);
            if (slot < 0 || slave == NULL)
                return false;
            pty_master_hold(slave);
            uint32_t index = slave->index;
            if (outcome == 0)
                handle_cancel_reserved(owner, slot);
            if (outcome == 1)
                owner->tearingDown = true;
            bool committed = handle_commit_reserved(owner, slot, HANDLE_PTY_MASTER, slave);
            if (!committed)
                pty_master_close(slave);
            if (outcome == 2)
                handle_close_all(owner);
            bool ok = committed == (outcome != 0) && slave->masterClosed &&
                      slave->index == index && slave->cols == 80 && slave->rows == 24;
            pty_master_unhold(slave);
            /* Registry lookup compares the pointer before touching fields. */
            bool survived = pty_seat_hold(slave);
            if (survived)
                pty_seat_unhold(slave);
            kfree(owner);
            if (!ok || survived)
                return false;
        }
    }
    return true;
}

bool test_pty_resize_modes(void)
{
    tty_t *stream = pty_create_slave(80, 24, PTY_MODE_STREAM);
    if (stream == NULL)
        return false;
    bool ok = stream->cells == NULL && stream->total_lines == 0;
    pipe_t *pipe = stream->stream;
    for (unsigned i = 0; i < 1024; i++)
    {
        uint32_t cols = i & 1 ? 80 : 512, rows = i & 1 ? 24 : 256;
        uint64_t gen = stream->generation;
        ok &= tty_resize(stream, cols, rows) == 1;
        ok &= stream->cols == cols && stream->rows == rows && stream->generation == gen + 1;
        ok &= tty_resize(stream, cols, rows) == 0 && stream->generation == gen + 1;
        ok &= stream->cells == NULL && stream->total_lines == 0 && stream->stream == pipe;
    }
    uint64_t gen = stream->generation;
    ok &= tty_resize(stream, 513, 24) == -1 && stream->cols == 80 && stream->generation == gen;
    tty_write(stream, "stream", 6);
    char bytes[6];
    ok &= pipe_read(pipe, bytes, sizeof(bytes), 0) == 6;
    ok &= bytes[0] == 's' && bytes[5] == 'm';
    pty_master_close(stream);

    tty_t *grid = pty_create_slave(4, 3, PTY_MODE_GRID);
    if (grid == NULL)
        return false;
    grid->cells[0].ch = 'A';
    grid->cells[4].ch = 'B';
    tty_cell_t *cells = grid->cells;
    gen = grid->generation;
    ok &= tty_resize(grid, 4, 3) == 0 && grid->cells == cells && grid->generation == gen;
    ok &= tty_resize(grid, 6, 4) == 1;
    ok &= grid->cells[0].ch == 'A' && grid->cells[6].ch == 'B';
    ok &= tty_resize(grid, 3, 2) == 1;
    ok &= grid->cells[0].ch == 'A' && grid->cells[3].ch == 'B';
    pty_master_close(grid);
    return ok;
}
