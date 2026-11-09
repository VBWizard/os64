// walk.c — one directory-tree mechanism, with policy supplied by callbacks.

#include "os64/io.h"
#include "os64/str.h"
#include "os64/walk.h"

typedef struct {
    char path[OS64_WALK_PATH_MAX];
    size_t length;
    uint32_t depth_limit;
    os64_walk_callback_t callback;
    void *context;
    bool failed;
    bool stopped;
} walk_state_t;

static os64_walk_action_t notify(walk_state_t *state,
                                 const os64_walk_visit_t *visit)
{
    os64_walk_action_t action = state->callback(visit, state->context);
    if (action & OS64_WALK_FAIL)
        state->failed = true;
    if (action & OS64_WALK_STOP)
        state->stopped = true;
    return action;
}

static void report_error(walk_state_t *state, uint32_t depth,
                         os64_walk_error_t error,
                         const os64_dirent_t *entry)
{
    state->failed = true;
    os64_walk_visit_t visit = {
        .event = OS64_WALK_ERROR,
        .path = state->path,
        .entry = entry,
        .depth = depth,
        .error = error,
        .complete = false
    };
    notify(state, &visit);
}

static bool append_child(walk_state_t *state, const char *name)
{
    bool needs_slash = state->length > 0 &&
                       state->path[state->length - 1] != '/';
    size_t name_length = os64_strlen(name);
    size_t wanted = state->length + (needs_slash ? 1u : 0u) +
                    name_length + 1u;
    if (wanted > sizeof(state->path))
        return false;

    if (needs_slash)
        state->path[state->length++] = '/';
    for (size_t i = 0; i < name_length; i++)
        state->path[state->length++] = name[i];
    state->path[state->length] = '\0';
    return true;
}

static bool walk_one(walk_state_t *state, const os64_dirent_t *known,
                     uint32_t depth)
{
    os64_dirent_t probed = {0};
    const os64_dirent_t *entry = known;
    bool complete = true;

    if (entry == NULL)
    {
        if (os64_stat(state->path, &probed) < 0)
        {
            report_error(state, depth, OS64_WALK_ERROR_STAT, NULL);
            return false;
        }
        entry = &probed;
    }

    os64_walk_visit_t visit = {
        .event = OS64_WALK_ENTRY,
        .path = state->path,
        .entry = entry,
        .depth = depth,
        .complete = true
    };
    os64_walk_action_t action = notify(state, &visit);
    if (action & OS64_WALK_FAIL)
        complete = false;
    if (state->stopped || (entry->flags & OS64_DE_DIR) == 0)
        return complete;

    if ((action & OS64_WALK_PRUNE) == 0)
    {
        if (depth >= state->depth_limit)
        {
            report_error(state, depth, OS64_WALK_ERROR_DEPTH, entry);
            complete = false;
        }
        else
        {
            int32_t directory = (int32_t)os64_opendir(state->path);
            if (directory < 0)
            {
                report_error(state, depth, OS64_WALK_ERROR_OPEN, entry);
                complete = false;
            }
            else
            {
                int64_t read_result;
                os64_dirent_t child = {0};
                while (!state->stopped &&
                       (read_result = os64_readdir(directory, &child)) == 1)
                {
                    if (os64_streq(child.name, ".") ||
                        os64_streq(child.name, ".."))
                        continue;

                    size_t parent_length = state->length;
                    if (!append_child(state, child.name))
                    {
                        report_error(state, depth + 1,
                                     OS64_WALK_ERROR_PATH_TOO_LONG, &child);
                        complete = false;
                        continue;
                    }

                    if (!walk_one(state, &child, depth + 1))
                        complete = false;
                    state->length = parent_length;
                    state->path[parent_length] = '\0';
                }

                if (!state->stopped && read_result < 0)
                {
                    report_error(state, depth, OS64_WALK_ERROR_READ, entry);
                    complete = false;
                }
                if (os64_close(directory) < 0)
                {
                    report_error(state, depth, OS64_WALK_ERROR_CLOSE, entry);
                    complete = false;
                }
            }
        }
    }

    if (!state->stopped)
    {
        visit.event = OS64_WALK_LEAVE;
        visit.complete = complete;
        action = notify(state, &visit);
        if (action & OS64_WALK_FAIL)
            complete = false;
    }
    return complete;
}

int32_t os64_walk(const char *path, const os64_walk_options_t *options,
                  os64_walk_callback_t callback, void *context)
{
    if (path == NULL || path[0] == '\0' || callback == NULL)
        return -1;

    walk_state_t state = {
        .depth_limit = options == NULL ? OS64_WALK_MAX_DEPTH
                                       : options->open_depth_limit,
        .callback = callback,
        .context = context
    };
    state.length = os64_strcopy(state.path, sizeof(state.path), path);
    if (state.length >= sizeof(state.path))
    {
        state.path[0] = '\0';
        report_error(&state, 0, OS64_WALK_ERROR_PATH_TOO_LONG, NULL);
        return -1;
    }

    walk_one(&state, NULL, 0);
    if (state.failed)
        return -1;
    return state.stopped ? 1 : 0;
}
