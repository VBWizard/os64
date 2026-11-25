// The CLI presents the same read-only plan that a writer or UI can consume.
#include "porter.h"
#include "os64/os64.h"

#define DEFAULT_PORT 6464

typedef struct {
    const char *publication, *scope;
    size_t prefix;
    porter_selection_t selection;
    bool baseline, found, duplicate;
} choice_t;

static bool show_publication(const porter_publication_t *pub, void *context)
{
    (void)context;
    os64_printf("%s  %s%s  %s\n", pub->id, pub->name, pub->baseline ? " [baseline]" : "",
                pub->published_at);
    for (size_t i = 0; i < pub->count; i++)
        os64_printf("  %s  %s\n", pub->targets[i].scope, pub->targets[i].target);
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

static void show_identity(const char *label, const porter_identity_t *id)
{
    if (id->kind == PORTER_FILE && *id->hash)
        os64_printf("  %s %lu bytes %s\n", label, (unsigned long)id->size, id->hash);
}

static void show_plan(const porter_plan_t *plan, const porter_inventory_t *manifest, bool verbose)
{
    os64_printf("Scope %s   %s\n", manifest->scope, plan->adoption ? "baseline adoption" : "recorded switch");
    os64_printf("Installed %s\nTarget    %s\n", *plan->installed ? plan->installed : "(none)", plan->target);
    os64_printf("Lots");
    for (size_t i = 0; i < manifest->lot_count; i++)
        os64_printf(" %s", manifest->lots[i]);
    os64_printf("\nKernel    %s\n", plan->kernel_status);
    if (manifest->kernel)
        os64_printf("  Booted  %s\n  Staged  %s\n  Target  %s\n",
                    *plan->booted_kernel ? plan->booted_kernel : "(unknown)",
                    *plan->staged_kernel ? plan->staged_kernel : "(unavailable)", manifest->kernel);

    size_t counts[sizeof(actions) / sizeof(*actions)] = {0};
    for (size_t i = 0; i < plan->count; i++) {
        const porter_plan_item_t *item = &plan->items[i];
        counts[item->action]++;
        if (!verbose && item->action == PORTER_UNCHANGED)
            continue;
        os64_printf("%s %s", actions[item->action], item->where.path);
        if (item->reason)
            os64_printf("  (%s)", item->reason);
        os64_printf("\n");
        if (item->action == PORTER_CONFLICT || item->action == PORTER_REPLACE ||
            item->action == PORTER_DOWNGRADE || item->action == PORTER_RETIRE ||
            (item->action == PORTER_ADOPT && item->reason)) {
            if (item->action == PORTER_CONFLICT)
                show_identity("recorded", &item->expected);
            show_identity("actual", &item->before);
            show_identity("target", &item->after);
        }
    }
    for (size_t i = 0; i < sizeof(counts) / sizeof(*counts); i++)
        if (counts[i])
            os64_printf("%s %lu  ", actions[i], (unsigned long)counts[i]);
    if (plan->count)
        os64_printf("\n");
    os64_printf("Transfer payload %lu bytes   Backup budget %lu bytes\n",
                (unsigned long)plan->transfer_bytes, (unsigned long)plan->backup_bytes);
    for (size_t i = 0; i < plan->space_count; i++) {
        const porter_space_t *s = &plan->spaces[i];
        os64_printf("  %s %s  stage %lu  backup %lu", s->mount->where, s->mount->guid,
                    (unsigned long)s->stage, (unsigned long)s->backup);
        if (s->mount->space_known)
            os64_printf("  free %lu", (unsigned long)s->mount->free_bytes);
        os64_printf("\n");
    }
    if (plan->blocker)
        os64_printf("BLOCKED %s\n", plan->blocker);
    else if (plan->conflicts)
        os64_printf("BLOCKED resolve destination conflicts before installing\n");
    else
        os64_printf("Preview complete. No files changed.\n");
}

static int preview(const char *host, uint16_t port, choice_t *choice, bool adopt, bool verbose)
{
    if (!porter_catalog(host, port, choose_publication, choice)) {
        os64_hprintf(OS64_STDERR, "porter: %s\n", choice->duplicate
                      ? "publication ID prefix is ambiguous" : "cannot read or verify catalog");
        return 1;
    }
    if (!choice->found || !*choice->selection.target) {
        os64_hprintf(OS64_STDERR, "porter: publication or scope was not found in the catalog\n");
        return 1;
    }
    porter_inventory_t target;
    if (!porter_get_manifest(host, port, &choice->selection, &target)) {
        os64_hprintf(OS64_STDERR, "porter: cannot read or verify target manifest\n");
        return 1;
    }
    porter_volumes_t volumes = {0};
    porter_config_t config = {0};
    int rc = 1;
    if (!porter_load_volumes(&volumes))
        os64_hprintf(OS64_STDERR, "porter: cannot read mounted-volume identities\n");
    else if (!porter_load_config(&config, &volumes))
        os64_hprintf(OS64_STDERR, "porter: cannot read effective /etc/porter.conf and /home/porter.conf\n");
    else {
        porter_plan_t plan;
        bool complete = porter_preview(&plan, &target, choice->baseline, adopt, &config, &volumes);
        show_plan(&plan, &target, verbose);
        rc = complete && !plan.blocker && !plan.conflicts ? 0 : 1;
        porter_release_plan(&plan);
    }
    os64_free(config.rules);
    os64_free(volumes.mounts);
    porter_release_inventory(&target);
    return rc;
}

static int usage(void)
{
    os64_hprintf(OS64_STDERR, "Usage: porter catalog HOST [--port N]\n"
                 "       porter preview HOST PUBLICATION [SCOPE] [--adopt] [--verbose] [--port N]\n"
                 "PUBLICATION is the full ID or a unique prefix of at least eight hex digits.\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    bool catalog = os64_streq(argv[1], "catalog");
    if (!catalog && !os64_streq(argv[1], "preview"))
        return usage();
    const char *host = argv[2];
    choice_t choice = {.scope = "system"};
    int first = 3;
    if (!catalog) {
        if (argc <= first)
            return usage();
        choice.prefix = os64_strlen(argv[first]);
        if (choice.prefix < 8 || choice.prefix > 32 || !porter_hex(argv[first], choice.prefix))
            return usage();
        choice.publication = argv[first++];
        if (argc > first && argv[first][0] != '-') {
            if (!porter_lot(argv[first]))
                return usage();
            choice.scope = argv[first++];
        }
    }
    uint64_t port = DEFAULT_PORT;
    bool adopt = false, verbose = false;
    for (int i = first; i < argc; i++) {
        if (os64_streq(argv[i], "--port")) {
            if (++i == argc || !os64_parse_u64(argv[i], &port) || !port || port > 65535)
                return usage();
        } else if (!catalog && os64_streq(argv[i], "--adopt")) {
            adopt = true;
        } else if (!catalog && os64_streq(argv[i], "--verbose")) {
            verbose = true;
        } else {
            return usage();
        }
    }
    if (!catalog)
        return preview(host, (uint16_t)port, &choice, adopt, verbose);
    if (porter_catalog(host, (uint16_t)port, show_publication, NULL))
        return 0;
    os64_hprintf(OS64_STDERR, "porter: cannot read or verify catalog\n");
    return 1;
}
