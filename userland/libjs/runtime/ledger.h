/* ledger.h — where a runtime's script time goes, by kind, for the JS speed
 * ledger (os64_js_ledger_*, os64/js.h). Pure: no engine, no system header.
 * Times are the caller's clock ticks (the TSC, in libjs); a ledger only
 * subtracts and adds them.
 *
 * The ledger is a stack of open frames. A frame is pushed when work of one
 * kind begins and popped when it ends; popping charges the frame's OWN time
 * (its elapsed less its children's elapsed) to its kind, and its elapsed to
 * its parent's children. So the kinds' totals add up to the outermost
 * frames' elapsed exactly, however the frames nest:
 *   ENGINE     the interpreter running script, and the engine's own C
 *              builtins (sort, regular expressions, JSON, string work),
 *              which pass through no door of their own;
 *   COMPILE    a script, module or bytecode the runtime compiles or reads;
 *   COLLECTOR  a cycle collection;
 *   NATIVE     a call into a host function (libdom's), by callee;
 *   LAYOUT     style and layout a host forced while answering script.
 * A NATIVE frame's own time is also charged to its callee's row; a LAYOUT
 * frame's elapsed is charged to the nearest open NATIVE frame's row as what
 * that callee forced, which is how "which read forces layout" is answered.
 *
 * The profile's samples are charged interpreter time only: what ENGINE
 * frames gained since the previous sample, never time inside another kind.
 */
#ifndef OS64_JS_LEDGER_H
#define OS64_JS_LEDGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LEDGER_ENGINE,
    LEDGER_COMPILE,
    LEDGER_COLLECTOR,
    LEDGER_NATIVE,
    LEDGER_LAYOUT,
    LEDGER_KINDS
} ledger_kind_t;

/* Frames deeper than this are counted as lost and not timed: their time
 * stays in the frame below them. */
#define LEDGER_DEPTH 128u
/* Callee rows, open-addressed by (function, magic); a callee past a full
 * table is counted in `callees_lost` and its time stays in NATIVE. */
#define LEDGER_CALLEES 1024u
/* Profile rows, open-addressed by (function, caller). */
#define LEDGER_SAMPLES 512u
#define LEDGER_NAME 64u
#define LEDGER_FILE 128u

typedef struct {
    const void *function;
    int magic;
    bool used;
    uint64_t calls;
    uint64_t self;            /* own time, nested script and layout excluded */
    uint64_t forced;          /* layouts forced inside it */
    uint64_t forced_ticks;    /* their elapsed */
} ledger_callee_t;

/* Where a sample found the interpreter: the running function and its
 * caller, each as identity (a key only) and its definition's place. */
typedef struct {
    const void *function;
    char name[LEDGER_NAME];
    char file[LEDGER_FILE];
    int line, column;
} ledger_place_t;

typedef struct {
    bool used;
    ledger_place_t at, caller;
    uint64_t samples;
    uint64_t ticks;           /* interpreter time charged to it */
} ledger_sample_t;

typedef struct {
    ledger_kind_t kind;
    uint64_t start;
    uint64_t children;
    int callee;               /* row index for NATIVE, -1 otherwise */
} ledger_frame_t;

typedef struct {
    uint64_t self[LEDGER_KINDS];
    uint64_t count[LEDGER_KINDS];
    uint64_t frames_lost;     /* pushed past LEDGER_DEPTH */
    uint64_t callees_lost;    /* native calls past a full callee table */
    uint64_t samples_lost;    /* samples past a full profile table */
    uint64_t samples;         /* samples taken */
    unsigned depth;           /* open frames, lost ones included */
    ledger_frame_t stack[LEDGER_DEPTH];
    ledger_callee_t callees[LEDGER_CALLEES];
    ledger_sample_t profile[LEDGER_SAMPLES];
    uint64_t engine_marked;   /* interpreter time at the previous sample */
} ledger_t;

/* Zero every total; open frames stay open, their clocks restarted at now,
 * so a reading begun inside a task counts only what follows. */
void ledger_reset(ledger_t *l, uint64_t now);
void ledger_push(ledger_t *l, ledger_kind_t kind, uint64_t now, const void *function, int magic);
void ledger_pop(ledger_t *l, uint64_t now);
/* Closes the innermost frame charging nothing, to it or its parent: for a
 * host frame opened for work that never came (the time it was open stays
 * in the frame below, as if it had not been pushed). */
void ledger_drop(ledger_t *l);
/* Interpreter time so far, the open ENGINE frames' share included. */
uint64_t ledger_engine_live(const ledger_t *l, uint64_t now);
/* One profile sample at `now`: charged the interpreter time since the
 * previous one. False when the table is full (counted). */
bool ledger_sample(ledger_t *l, uint64_t now, const ledger_place_t *at, const ledger_place_t *caller);

#endif
