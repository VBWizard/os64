// The CLI presents the same read-only plan that a writer or UI can consume.
#include "transcript.h"
#include "transaction.h"
#include "os64/os64.h"

#define DEFAULT_PORT 6464

static void emit(porter_transcript_t *log, int32_t stream, const char *fmt, ...) OS64_PRINTF(3, 4);

static void emit(porter_transcript_t *log, int32_t stream, const char *fmt, ...)
{
    char buf[1024];
    char *text = buf;
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = os64_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n >= (int)sizeof(buf)) {
        text = os64_malloc((size_t)n + 1);
        if (text)
            os64_vsnprintf(text, (size_t)n + 1, fmt, copy);
    }
    va_end(copy);

    if (n >= 0 && text) {
        // A failed redirect must not stop the independent file transcript.
        porter_write_all(stream, text, (size_t)n);
        porter_transcript_write(log, text, (size_t)n);
    } else {
        static const char problem[] = "porter: could not format output\n";
        porter_write_all(OS64_STDERR, problem, sizeof(problem) - 1);
        porter_transcript_write(log, problem, sizeof(problem) - 1);
    }
    if (text != buf)
        os64_free(text);
}

typedef struct {
    const char *publication, *scope;
    size_t prefix;
    porter_selection_t selection;
    bool baseline, found, duplicate;
} choice_t;

static bool show_publication(const porter_publication_t *pub, void *context)
{
    porter_transcript_t *log = context;
    emit(log, OS64_STDOUT, "%s  %s%s  %s\n", pub->id, pub->name, pub->baseline ? " [baseline]" : "",
                pub->published_at);
    for (size_t i = 0; i < pub->count; i++)
        emit(log, OS64_STDOUT, "  %s  %s\n", pub->targets[i].scope, pub->targets[i].target);
    return true;
}

static bool choose_publication(const porter_publication_t *pub, void *context)
{
    choice_t *c = context;
    if (os64_memcmp(c->publication, pub->id, c->prefix))
        return true;
    if (c->found) {
        c->duplicate = true;
        return false;
    }
    c->found = true;
    c->baseline = pub->baseline;
    for (size_t i = 0; i < pub->count; i++)
        if (os64_streq(pub->targets[i].scope, c->scope))
            c->selection = pub->targets[i];
    return true;
}

static const char *const actions[] = {
    [PORTER_UNCHANGED] = "UNCHANGED",
    [PORTER_ADD] = "ADD",
    [PORTER_ADOPT] = "ADOPT",
    [PORTER_REPLACE] = "REPLACE",
    [PORTER_DOWNGRADE] = "DOWNGRADE",
    [PORTER_RETIRE] = "RETIRE",
    [PORTER_PRESERVE] = "PRESERVE",
    [PORTER_UNMANAGED] = "UNMANAGED",
    [PORTER_CONFLICT] = "CONFLICT",
};

static void show_identity(porter_transcript_t *log, const char *label, const porter_identity_t *id)
{
    if (id->kind == PORTER_FILE && *id->hash)
        emit(log, OS64_STDOUT, "  %s %lu bytes %s\n", label, (unsigned long)id->size, id->hash);
}

static void show_plan(porter_transcript_t *log, const porter_plan_t *plan, const porter_inventory_t *manifest, bool verbose)
{
    emit(log, OS64_STDOUT, "Scope %s   %s\n", manifest->scope, plan->adoption ? "baseline adoption" : "recorded switch");
    emit(log, OS64_STDOUT, "Installed %s\nTarget    %s\n", *plan->installed ? plan->installed : "(none)", plan->target);
    emit(log, OS64_STDOUT, "Lots");
    for (size_t i = 0; i < manifest->lot_count; i++)
        emit(log, OS64_STDOUT, " %s", manifest->lots[i]);
    emit(log, OS64_STDOUT, "\nKernel    %s\n", plan->kernel_status);
    if (manifest->kernel)
        emit(log, OS64_STDOUT, "  Booted  %s\n  Staged  %s\n  Target  %s\n",
                    *plan->booted_kernel ? plan->booted_kernel : "(unknown)",
                    *plan->staged_kernel ? plan->staged_kernel : "(unavailable)", manifest->kernel);

    size_t counts[sizeof(actions) / sizeof(*actions)] = {0};
    for (size_t i = 0; i < plan->count; i++) {
        const porter_plan_item_t *item = &plan->items[i];
        counts[item->action]++;
        if (!verbose && item->action == PORTER_UNCHANGED)
            continue;
        emit(log, OS64_STDOUT, "%s %s", actions[item->action], item->where.path);
        if (item->reason)
            emit(log, OS64_STDOUT, "  (%s)", item->reason);
        emit(log, OS64_STDOUT, "\n");
        if (item->action == PORTER_CONFLICT || item->action == PORTER_REPLACE ||
            item->action == PORTER_DOWNGRADE || item->action == PORTER_RETIRE ||
            (item->action == PORTER_ADOPT && item->reason)) {
            if (item->action == PORTER_CONFLICT)
                show_identity(log, "recorded", &item->expected);
            show_identity(log, "actual", &item->before);
            show_identity(log, "target", &item->after);
        }
    }
    for (size_t i = 0; i < sizeof(counts) / sizeof(*counts); i++)
        if (counts[i])
            emit(log, OS64_STDOUT, "%s %lu  ", actions[i], (unsigned long)counts[i]);
    if (plan->count)
        emit(log, OS64_STDOUT, "\n");
    emit(log, OS64_STDOUT, "Transfer payload %lu bytes   Backup budget %lu bytes\n",
                (unsigned long)plan->transfer_bytes, (unsigned long)plan->backup_bytes);
    for (size_t i = 0; i < plan->space_count; i++) {
        const porter_space_t *s = &plan->spaces[i];
        emit(log, OS64_STDOUT, "  %s %s  stage %lu  backup %lu", s->mount->where, s->mount->guid,
                    (unsigned long)s->stage, (unsigned long)s->backup);
        if (s->mount->space_known)
            emit(log, OS64_STDOUT, "  free %lu", (unsigned long)s->mount->free_bytes);
        emit(log, OS64_STDOUT, "\n");
    }
    if (plan->blocker)
        emit(log, OS64_STDOUT, "BLOCKED %s\n", plan->blocker);
    else if (plan->conflicts)
        emit(log, OS64_STDOUT, "BLOCKED resolve destination conflicts before installing\n");
    else
        emit(log, OS64_STDOUT, "Preview complete. Managed files and install history were left untouched.\n");
}

static void report_transaction(void *context, bool error, const char *message)
{
    emit(context, error ? OS64_STDERR : OS64_STDOUT, "%s%s\n", error ? "porter: " : "", message);
}

static int preview(porter_transcript_t *log, const char *host, uint16_t port, choice_t *choice, bool adopt, bool verbose, bool install, bool approved)
{
    if (!porter_catalog(host, port, choose_publication, choice)) {
        emit(log, OS64_STDERR, "porter: %s\n", choice->duplicate
                      ? "publication ID prefix is ambiguous" : "cannot read or verify catalog");
        return 1;
    }
    if (!choice->found || !*choice->selection.target) {
        emit(log, OS64_STDERR, "porter: publication or scope was not found in the catalog\n");
        return 1;
    }
    porter_inventory_t target;
    if (!porter_get_manifest(host, port, &choice->selection, &target)) {
        emit(log, OS64_STDERR, "porter: cannot read or verify target manifest\n");
        return 1;
    }
    porter_volumes_t volumes = {0};
    porter_config_t config = {0};
    int rc = 1;
    if (!porter_load_volumes(&volumes))
        emit(log, OS64_STDERR, "porter: cannot read mounted-volume identities\n");
    else if (!porter_load_config(&config, &volumes))
        emit(log, OS64_STDERR, "porter: cannot read effective /etc/porter.conf and /home/porter.conf\n");
    else {
        porter_plan_t plan;
        bool complete = install
            ? porter_preview_ext2(&plan, &target, choice->baseline, adopt, &config, &volumes)
            : porter_preview(&plan, &target, choice->baseline, adopt, &config, &volumes);
        show_plan(log, &plan, &target, verbose);
        if (plan.blocker)
            porter_tx_notice(report_transaction, log);
        rc = complete && !plan.blocker && !plan.conflicts ? 0 : 1;
        if (!rc && install) {
            if (!approved) {
                emit(log, OS64_STDERR, "porter: use install --yes to approve the displayed plan\n");
                rc = 1;
            } else {
                rc = porter_tx_install(host, port, &target, choice->baseline, adopt, &plan, &config,
                                       report_transaction, log) ? 0 : 1;
            }
        }
        porter_release_plan(&plan);
    }
    os64_free(config.rules);
    os64_free(volumes.mounts);
    porter_release_inventory(&target);
    return rc;
}

static int usage(porter_transcript_t *log)
{
    emit(log, OS64_STDERR, "Usage: porter catalog HOST [--port N]\n"
                 "       porter preview HOST PUBLICATION [SCOPE] [--adopt] [--verbose] [--port N]\n"
                 "       porter install HOST PUBLICATION [SCOPE] [--adopt] [--yes] [--port N]\n"
                 "       porter history [SCOPE]\n"
                 "       porter undo [SCOPE] [--yes]\n"
                 "       porter recover\n"
                 "PUBLICATION is the full ID or a unique prefix of at least eight hex digits.\n");
    return 2;
}

static int run_cli(porter_transcript_t *log, int argc, char **argv)
{
    if (argc < 2)
        return usage(log);
    if (os64_streq(argv[1], "recover"))
        return argc == 2 ? (porter_tx_recover(report_transaction, log) ? 0 : 1) : usage(log);
    bool history = os64_streq(argv[1], "history"), undo = os64_streq(argv[1], "undo");
    if (history || undo) {
        const char *scope = "system";
        int next = 2;
        if (argc > next && argv[next][0] != '-') {
            scope = argv[next++];
            if (!porter_lot(scope))
                return usage(log);
        }
        bool approved = undo && argc == next + 1 && os64_streq(argv[next], "--yes");
        if (argc != next && !approved)
            return usage(log);
        bool ok = history ? porter_tx_history(scope, report_transaction, log)
                          : porter_tx_undo(scope, approved, report_transaction, log);
        return ok ? 0 : 1;
    }
    bool catalog = os64_streq(argv[1], "catalog");
    bool install = os64_streq(argv[1], "install");
    if (argc < 3 || (!catalog && !install && !os64_streq(argv[1], "preview")))
        return usage(log);
    const char *host = argv[2];
    choice_t choice = {.scope = "system"};
    int first = 3;
    if (!catalog) {
        if (argc <= first)
            return usage(log);
        choice.prefix = os64_strlen(argv[first]);
        if (choice.prefix < 8 || choice.prefix > 32 || !porter_hex(argv[first], choice.prefix))
            return usage(log);
        choice.publication = argv[first++];
        if (argc > first && argv[first][0] != '-') {
            if (!porter_lot(argv[first]))
                return usage(log);
            choice.scope = argv[first++];
        }
    }
    uint64_t port = DEFAULT_PORT;
    bool adopt = false, verbose = false, approved = false;
    for (int i = first; i < argc; i++) {
        if (os64_streq(argv[i], "--port")) {
            if (++i == argc || !os64_parse_u64(argv[i], &port) || !port || port > 65535)
                return usage(log);
        } else if (!catalog && os64_streq(argv[i], "--adopt")) {
            adopt = true;
        } else if (!catalog && os64_streq(argv[i], "--verbose")) {
            verbose = true;
        } else if (install && os64_streq(argv[i], "--yes")) {
            approved = true;
        } else {
            return usage(log);
        }
    }
    if (!catalog)
        return preview(log, host, (uint16_t)port, &choice, adopt, verbose, install, approved);
    if (porter_catalog(host, (uint16_t)port, show_publication, log))
        return 0;
    emit(log, OS64_STDERR, "porter: cannot read or verify catalog\n");
    return 1;
}

static void progress_pipe_closed(int signal)
{
    (void)signal;
}

int main(int argc, char **argv)
{
    // A progress reader can disappear after a transaction has been armed.
    // Its SIGPIPE must not interrupt the operation or its independent log.
    os64_signal_set_handler(OS64_SIGPIPE, progress_pipe_closed);
    porter_transcript_t log;
    bool opened = porter_transcript_open(&log, argc, argv);
    if (opened)
        emit(&log, OS64_STDERR, "porter: transcript %s\n", log.path);
    else
        emit(&log, OS64_STDERR, "porter: could not start a transcript in " PORTER_LOG_DIR "\n");
    int status = run_cli(&log, argc, argv);
    if (!porter_transcript_close(&log, status) && opened)
        emit(&log, OS64_STDERR, "porter: incomplete transcript %s\n", log.path);
    return status;
}
