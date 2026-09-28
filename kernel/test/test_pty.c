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

// Both orderings of seat reservation versus last-seat departure. Pipe
// closure is irreversible, while an unused reservation may be cancelled.
bool test_pty_stream_seats(void)
{
    tty_t *t = pty_create_slave(80, 24, PTY_MODE_STREAM);
    if (t == NULL)
        return false;
    bool reserved = tty_pty_reserve_seat(t);
    bool ok = reserved && !t->everSeated;
    if (reserved)
        tty_pty_unref(t);
    char byte;
    // Kernel pipe reads take an absolute tick deadline; 1 has passed at post-boot.
    ok &= !t->stream_writer_closed && pipe_read(t->stream, &byte, 1, 1) == PIPE_ERR_TIMEOUT;

    bool seated = tty_pty_ref(t);
    reserved = tty_pty_reserve_seat(t);
    ok &= seated && reserved;
    if (seated)
        tty_pty_unref(t);
    ok &= !t->stream_writer_closed && pipe_read(t->stream, &byte, 1, 1) == PIPE_ERR_TIMEOUT;
    if (reserved)
        tty_pty_unref(t);
    ok &= t->stream_writer_closed && pipe_read(t->stream, &byte, 1, 1) == 0;
    seated = tty_pty_ref(t);
    reserved = tty_pty_reserve_seat(t);
    ok &= !seated && !reserved;
    if (seated)
        tty_pty_unref(t);
    if (reserved)
        tty_pty_unref(t);

    pty_master_hold(t);
    pty_master_close(t);
    seated = tty_pty_ref(t);
    ok &= !seated;
    if (seated)
        tty_pty_unref(t);
    pty_master_unhold(t);
    return ok;
}

bool test_pty_history(void)
{
    if(kmalloc_try(0) || kmalloc_try(UINT64_MAX) || kmalloc_try(1UL<<48))return false;
    tty_t *t=pty_create_slave(4,2,PTY_MODE_GRID);
    if(!t)return false;
    bool ok=tty_pty_history(t,4)==1;
    tty_write(t,"A\nB\nC\nD\nE\nF\n",12);
    os64_pty_viewport_t v;os64_pty_cell_t cells[24];
    uint64_t flags=spinlock_acquire_irqsave(&t->lock);
    uint32_t copied=tty_pty_view_locked(t,&v,cells,24,2,t->view_epoch);
    spinlock_release_irqrestore(&t->lock,flags);
    ok &= copied==8 && v.history_lines==4 && v.live_line==5 && v.first_line==2 && cells[0].ch=='C';
    uint64_t epoch=v.epoch;
    tty_write(t,"G\nH\n",4);
    flags=spinlock_acquire_irqsave(&t->lock);
    tty_pty_view_locked(t,&v,cells,24,2,epoch);
    spinlock_release_irqrestore(&t->lock,flags);
    ok &= v.first_line==3 && cells[0].ch=='D'; // old anchor evicted
    ok &= tty_resize(t,6,3)==1 && t->total_lines==7;
    flags=spinlock_acquire_irqsave(&t->lock);
    tty_pty_view_locked(t,&v,cells,24,3,epoch);
    spinlock_release_irqrestore(&t->lock,flags);
    ok &= v.epoch!=epoch && v.first_line==v.live_line && v.history_limit==4;
    ok &= tty_pty_history(t,1)==1 && t->hist_lines==1;
    tty_cell_t *before=t->cells;uint64_t generation=t->generation;
    ok &= tty_pty_history(t,10001)<0 && t->cells==before && t->generation==generation;
    ok &= tty_pty_history(t,0)==1 && t->total_lines==t->rows && !t->hist_lines;
    tty_write(t,"I\nJ\nK\n",6);
    ok &= !t->hist_lines;
    pty_master_close(t);
    t=pty_create_slave(4,2,PTY_MODE_STREAM);
    if(!t)return false;
    ok &= tty_pty_history(t,4)<0 && t->cells==NULL;
    pty_master_close(t);
    // Exercise aggregate refusal, including a replacement's temporary bytes.
    tty_t *many[4]={0};unsigned count=0;bool refused=false;
    for(;count<4;++count){
        many[count]=pty_create_slave(512,2,PTY_MODE_GRID);
        if(!many[count]){refused=true;break;}
        before=many[count]->cells;generation=many[count]->generation;
        int result=tty_pty_history(many[count],10000);
        if(result<0){
            ok &= result==OS64_PTY_ERR_HISTORY_BUDGET;
            ok &= many[count]->cells==before && many[count]->generation==generation;
            refused=true;++count;break;
        }
    }
    // Fill the quota exactly: 32768 retained rows at 4096 bytes per row.
    if(count==4 && refused){
        ok &= tty_pty_history(many[3],0)>=0;
        ok &= tty_pty_history(many[3],2768)>=0;
        tty_t *fresh=pty_create_slave(512,256,PTY_MODE_GRID);
        ok &= fresh && fresh->total_lines==256 && fresh->history_bytes==0;
        if(fresh){
            ok &= tty_pty_history(fresh,1)==OS64_PTY_ERR_HISTORY_BUDGET;
            ok &= tty_pty_history(fresh,0)>=0 && tty_resize(fresh,500,250)==1;
            pty_master_close(fresh);
        }
    }
    while(count)pty_master_close(many[--count]);
    t=pty_create_slave(512,2,PTY_MODE_GRID);
    ok &= t && tty_pty_history(t,10000)>=0;
    if(t)pty_master_close(t);
    return ok && refused;
}
