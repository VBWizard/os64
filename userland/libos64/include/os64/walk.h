#ifndef OS64_WALK_H
#define OS64_WALK_H

// A policy-free directory-tree walk for userland tools.
//
// The walker owns traversal, path joining, dot-entry rejection, directory
// handles, and the awkward distinction between arriving at a directory and
// leaving it.  The caller owns every interesting decision: what to print,
// whether to prune, what an error should say, and what to do before or after
// the children.  That makes one mechanism fit preorder consumers such as cp
// and grep and postorder consumers such as du and rm.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/dirent.h"

// Every path-taking syscall currently accepts at most 255 bytes plus the NUL.
// A walk uses one mutable buffer of exactly that size rather than placing a
// fresh path array on every recursive stack frame.
#define OS64_WALK_PATH_MAX 256u

// Three standard handles plus twelve simultaneously open directories leaves
// one honest spare.  Callers that need more handles during a visit (cp needs
// source and destination files together) should choose a smaller limit.
#define OS64_WALK_MAX_DEPTH 12u

typedef enum {
    OS64_WALK_ENTRY,   // preorder: file or directory, before any children
    OS64_WALK_LEAVE,   // postorder: directory only, after/without its children
    OS64_WALK_ERROR    // traversal failed at visit->path
} os64_walk_event_t;

typedef enum {
    OS64_WALK_ERROR_STAT,
    OS64_WALK_ERROR_OPEN,
    OS64_WALK_ERROR_READ,
    OS64_WALK_ERROR_CLOSE,
    OS64_WALK_ERROR_PATH_TOO_LONG,
    OS64_WALK_ERROR_DEPTH
} os64_walk_error_t;

// Callback results are flags so a failed directory operation can also prune
// that directory: OS64_WALK_FAIL | OS64_WALK_PRUNE.
typedef uint32_t os64_walk_action_t;

#define OS64_WALK_CONTINUE ((os64_walk_action_t)0)
#define OS64_WALK_PRUNE    ((os64_walk_action_t)1u << 0)
#define OS64_WALK_FAIL     ((os64_walk_action_t)1u << 1)
#define OS64_WALK_STOP     ((os64_walk_action_t)1u << 2)

typedef struct {
    os64_walk_event_t event;
    const char *path;
    const os64_dirent_t *entry;
    uint32_t depth;             // command-line operand is depth zero

    // ERROR only.  For PATH_TOO_LONG, path names the parent and entry names
    // the child that could not be appended.  For all other errors, path is
    // the object whose operation failed.
    os64_walk_error_t error;

    // LEAVE only: false when traversal or a callback failed anywhere in this
    // directory's subtree.  An intentional PRUNE remains complete; PRUNE|FAIL
    // does not.  rm uses this to avoid deleting a partly processed directory.
    bool complete;
} os64_walk_visit_t;

typedef os64_walk_action_t (*os64_walk_callback_t)(
    const os64_walk_visit_t *visit, void *context);

typedef struct {
    // A directory at this depth is reported with ENTRY, then produces a DEPTH
    // error instead of being opened unless the callback prunes it first.
    // Zero therefore means "visit the operand, but do not descend into it".
    uint32_t open_depth_limit;
} os64_walk_options_t;

// Walk one file or directory tree.  The callback sees every ordinary entry;
// "." and ".." are rejected centrally.  Returns 0 after a complete clean
// walk, 1 when the callback requested STOP, and -1 if traversal or any
// callback reported FAIL (a prior failure wins over STOP).
int32_t os64_walk(const char *path, const os64_walk_options_t *options,
                  os64_walk_callback_t callback, void *context);

#endif // OS64_WALK_H
