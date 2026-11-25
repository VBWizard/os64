#ifndef HANDLE_BLOCKS_H
#define HANDLE_BLOCKS_H

// handle_blocks.h — where a handle's number lives in a task's table, and the
// table's counts (handle.h is the table; docs/design/pending/VIDEO_PLANE.md
// § 7 says why it grows).
//
// THE TABLE IS BLOCKS, AND NOTHING IN IT EVER MOVES. Block 0 is the task's
// own, inside task_t, OS64_TASK_HANDLES entries: no allocation, so the
// kernel task, the idle tasks and a task born short of memory have it as
// they always had. Blocks 1.. are added as the task needs them, each twice
// the last (64, 128, 256 ...), so after k added blocks the room is
// OS64_TASK_HANDLES * 2^k, and handle n lives at one block and slot for the
// task's whole life. A pointer to an entry stays good as long as the task.
//
// Pure: no kernel headers and no allocation; tools/test_handle_blocks_host.sh
// runs it beside a model.

#include <stdbool.h>
#include <stdint.h>

#include "os64/file.h"

// THE CEILING: the most handles one task may hold. The one place it is said;
// the blocks it takes follow from it.
#define TASK_HANDLES_CEILING 4096u

// The blocks added past block 0 to reach the ceiling.
#define TASK_HANDLE_BLOCKS ((unsigned)__builtin_ctz(TASK_HANDLES_CEILING / OS64_TASK_HANDLES))

_Static_assert((TASK_HANDLES_CEILING / OS64_TASK_HANDLES) * OS64_TASK_HANDLES == TASK_HANDLES_CEILING &&
               ((TASK_HANDLES_CEILING / OS64_TASK_HANDLES) & (TASK_HANDLES_CEILING / OS64_TASK_HANDLES - 1)) == 0,
               "the ceiling is block 0 doubled a whole number of times");

// Block k's entries (k = 0 is the task's own) and the number of its first.
uint32_t handle_block_size(unsigned k);
uint32_t handle_block_first(unsigned k);
// The room with `added` blocks past block 0.
uint32_t handle_room(unsigned added);
// Where handle n lives: false for a number below 0 or at the ceiling or
// past it, which no task can hold.
bool handle_place(int64_t n, unsigned *block, uint32_t *slot);

// THE COUNTS, for /proc/<pid>/handles' first line: the entries not free
// (held), the most ever held, and the opens refused for want of room (at the
// ceiling, or a block that could not be had). Atomic: every thread of a task
// opens and closes in it at once.
typedef struct {
	uint32_t held, most;
	uint64_t failed;
} handle_counts_t;
// A free entry was taken: held rises, and most with it when held passes it.
void handle_counts_took(handle_counts_t *c);
// An entry became free again.
void handle_counts_gave(handle_counts_t *c);
// An open was refused for want of room.
void handle_counts_refused(handle_counts_t *c);

#endif
