/* grootmenu [x y] [name]: menu.conf adapter for the shared native menus.
 * The desktop launches this independent process. A selected child is
 * deliberately orphaned at exit; the kernel re-parents and reaps it.
 * Grammar, config lookup and argv quoting belong to os64/menu.h. */
#include "os64/os64.h"
#include "os64/ui_menu.h"
#include "os64/menu.h"
#include "os64/proc.h"
#include "os64/io.h"
#include "os64/fmt.h"
#include "os64/str.h"
#include "image/icon_theme.h"

#define ARGV_MAX 16
static os64_menu_t gMenu;
static os64_ui_t ui;
static os64_ui_actions_t *actions;
static os64_ui_action_view_t *view;
static os64_ui_menu_session_t *session;
static os64_ui_dispatcher_t dispatcher;
static os64_icon_theme_t *icon_theme;
/* Cache by first node, so references share models instead of expanding a DAG. */
static os64_ui_menu_model_t *models[OS64_MENU_NODES_MAX];
static bool building[OS64_MENU_NODES_MAX];
static bool failed, error_reported;

static const char *command_icon(const char *command)
{
    char buf[OS64_MENU_COMMAND_MAX];
    char *argv[ARGV_MAX];
    int64_t argc = os64_menu_argv(command, buf, sizeof(buf), argv, ARGV_MAX);
    if (argc <= 0) return "app";
    if (os64_streq(argv[0], "/bin/scribe")) return "file";
    if (os64_streq(argv[0], "/bin/controlcenter") ||
        os64_streq(argv[0], "/bin/appearance") ||
        os64_streq(argv[0], "/bin/mousesettings")) return "settings";
    return "app";
}

static void launch(const os64_menu_node_t *node)
{
    char  buf[OS64_MENU_COMMAND_MAX];
    char *argv[ARGV_MAX];
    int64_t argc = os64_menu_argv(node->command, buf, sizeof(buf), argv, ARGV_MAX);
    if (argc < 0) {
        os64_complain("grootmenu: \"%s\": command refused — over %d words, or too long\n",
                     node->label, ARGV_MAX - 1);
        return;
    }
    if (argc == 0) {
        os64_complain("grootmenu: \"%s\": empty command\n", node->label);
        return;
    }
    int64_t pid = os64_spawn(argv[0], argv);
    if (pid < 0)
        os64_complain("grootmenu: \"%s\": could not start %s (%ld)\n",
                     node->label, argv[0], (long)pid);
}

static bool target_valid(void *cookie, uint64_t generation)
{ (void)cookie; return generation == 1; }
static void target_release(void *cookie, uint64_t generation)
{ (void)cookie; (void)generation; }
static void query(void *app, const os64_ui_action_context_t *context,
                  uint32_t id, os64_ui_action_state_t *out)
{
    (void)app; (void)context;
    out->enabled = id && id <= gMenu.count && gMenu.nodes[id-1].kind == OS64_MENU_ITEM;
}
static os64_ui_action_result_t invoke(void *app,
    const os64_ui_action_context_t *context, uint32_t id)
{
    (void)app; (void)context;
    /* The controller closes the native family before invoking this callback. */
    if (!id || id > gMenu.count || gMenu.nodes[id-1].kind != OS64_MENU_ITEM)
        return (os64_ui_action_result_t){OS64_UI_REFUSED, 0};
    launch(&gMenu.nodes[id-1]);
    return (os64_ui_action_result_t){OS64_UI_OK, 0};
}
static os64_ui_status_code_t convert(int16_t first, unsigned depth)
{
    if (first < 0) return OS64_UI_EMPTY;
    if (first >= gMenu.count) return OS64_UI_INVALID;
    if (depth >= OS64_UI_MENU_DEPTH) return OS64_UI_LIMIT;
    if (building[first]) return OS64_UI_INVALID;
    if (models[first]) return os64_ui_menu_model_count(models[first]) ? OS64_UI_OK : OS64_UI_EMPTY;
    building[first] = true;
    os64_ui_menu_entry_t entries[OS64_UI_MENU_ENTRIES] = {0};
    size_t count = 0;
    os64_ui_status_code_t rc = OS64_UI_OK;
    for (int16_t i = first; i >= 0; i = gMenu.nodes[i].next) {
        if (i >= gMenu.count) { rc = OS64_UI_INVALID; break; }
        if (count == OS64_UI_MENU_ENTRIES) { rc = OS64_UI_LIMIT; break; }
        const os64_menu_node_t *node = &gMenu.nodes[i];
        os64_ui_menu_entry_t *entry = &entries[count++];
        if (node->kind == OS64_MENU_ITEM) entry->action = (uint32_t)i + 1;
        else if (node->kind == OS64_MENU_SEPARATOR) entry->kind = OS64_UI_MENU_SEPARATOR;
        else if (node->kind == OS64_MENU_SUBMENU) {
            rc = convert(node->first_child, depth + 1);
            // Empty placeholders do not prevent usable siblings from opening.
            if (rc == OS64_UI_EMPTY) { --count; rc = OS64_UI_OK; continue; }
            if (rc) break;
            *entry = (os64_ui_menu_entry_t){.kind=OS64_UI_MENU_SUBMENU,
                .label=node->label, .child=models[node->first_child]};
        } else { rc = OS64_UI_INVALID; break; }
    }
    if (!rc) rc = os64_ui_menu_model_create(actions, entries, count, &models[first]);
    building[first] = false;
    if (!rc && !os64_ui_menu_model_count(models[first])) rc = OS64_UI_EMPTY;
    return rc;
}
static os64_ui_status_code_t build(int16_t first)
{
    os64_ui_action_spec_t specs[OS64_MENU_NODES_MAX] = {0};
    size_t count = 0;
    for (unsigned i = 0; i < gMenu.count; ++i)
        if (gMenu.nodes[i].kind == OS64_MENU_ITEM)
            specs[count++] = (os64_ui_action_spec_t){.id=i+1, .label=gMenu.nodes[i].label,
                .icon=command_icon(gMenu.nodes[i].command)};
    if (!count) return OS64_UI_EMPTY;
    const os64_ui_action_callbacks_t callbacks = {query, invoke};
    os64_ui_status_code_t rc = os64_ui_actions_create(specs,count,&callbacks,NULL,&actions);
    if (rc) return rc;
    const os64_ui_action_context_t context = {NULL,1,target_valid,target_release,target_valid};
    rc = os64_ui_action_view_create(actions,&context,&view);
    return rc ? rc : convert(first,0);
}
static void launcher_error(void *user, os64_ui_status_code_t rc)
{
    (void)user;
    error_reported = true;
    const char *why = rc == OS64_UI_EMPTY ? "empty menu or cascade" :
        rc == OS64_UI_LIMIT ? "menu exceeds toolkit row, graph, depth or screen limits" :
        rc == OS64_UI_NO_MEMORY ? "out of memory" :
        rc == OS64_UI_INVALID ? "unsupported label or invalid menu graph" :
        "native menu or font operation failed";
    os64_complain("grootmenu: %s: %s (%u)\n", gMenu.path, why, (unsigned)rc);
}
static void startup_result(os64_ui_status_code_t rc)
{
    if (rc && rc != OS64_UI_BUSY) {
        failed = true;
        // Some open failures return before the controller calls its error hook.
        if (!error_reported) launcher_error(NULL,rc);
    }
}
static bool dispose(void)
{
    if (os64_ui_menu_session_dispose(session) != OS64_UI_OK) return false;
    session = NULL;
    for (unsigned i = 0; i < OS64_MENU_NODES_MAX; ++i) {
        os64_ui_menu_model_release(models[i]); models[i] = NULL;
    }
    if (view) { (void)os64_ui_action_view_destroy(view); view = NULL; }
    if (actions) { (void)os64_ui_actions_destroy(actions); actions = NULL; }
    if (icon_theme) {
        if (os64_icon_theme_destroy(icon_theme) != OS64_UI_OK) return false;
        icon_theme = NULL;
    }
    return os64_ui_font_release(&ui) == OS64_FONT_OK;
}

static int grootmenu_main(int argc, char **argv)
{
    // `grootmenu [x y] [name]`, decided by ARITY, not by what an argument
    // looks like. Sniffing the first character made a menu named "123"
    // unopenable: it read as a coordinate and the name silently stayed
    // "root". How data is spelled may not change how it is read.
    failed = error_reported = false;
    int32_t x = 0, y = 0;
    const char *name = "root";
    if (argc == 2) {
        name = argv[1];              // a name alone; one coordinate means nothing
    } else if (argc >= 3) {
        x = (int32_t)os64_atoi(argv[1]);
        y = (int32_t)os64_atoi(argv[2]);
        if (argc >= 4)
            name = argv[3];
    }

    uint32_t gScreenW = 0, gScreenH = 0;
    if (os64_gui_screen_info(&gScreenW, &gScreenH) != 0 || (gScreenW == 0 || gScreenH == 0)) {
        os64_complain("grootmenu: no GUI here\n");
        return 1;
    }

    char err[OS64_MENU_ERR_MAX];
    os64_menu_status_t st = os64_menu_load(&gMenu, "menu.conf", err, sizeof(err));
    if (st != OS64_MENU_OK) {
        // Say WHICH file and WHY: a menu that does not appear is a config
        // bug with no other handle on it.
        if (err[0])
            os64_complain("grootmenu: %s\n", err);
        else
            os64_complain("grootmenu: %s: %s\n", gMenu.path, os64_menu_status_name(st));
        return 1;
    }
    if (!os64_menu_named_exists(&gMenu, name)) {
        os64_complain("grootmenu: %s defines no menu named \"%s\"\n",
                     gMenu.path, name);
        return 1;
    }

    int16_t first = os64_menu_find(&gMenu, name);
    if (first < 0) {
        os64_complain("grootmenu: menu \"%s\" is empty\n", name);
        return 1;
    }
    os64_ui_init(&ui, NULL);
    os64_ui_dispatcher_init(&dispatcher, NULL);
    (void)os64_icon_theme_create(&icon_theme);
    os64_ui_status_code_t rc = build(first);
    if (!rc) rc = os64_ui_menu_session_create(&dispatcher,&ui,0,NULL,NULL,
        icon_theme ? os64_icon_theme_provider(icon_theme) : NULL,&session);
    if (!rc) {
        os64_ui_menu_hooks(session,NULL,launcher_error,NULL);
        /* Popups borrow this host's configured Interface font context. */
        if (os64_ui_font_follow(&ui) != OS64_FONT_OK) rc = OS64_UI_ERROR;
    }
    if (!rc) rc = os64_ui_menu_open(session,models[first],view,
        (os64_gui_rect_t){x,y,0,0},OS64_UI_MENU_CONTEXT);
    startup_result(rc);
    while (!failed && os64_ui_menu_active(session)) {
        if (os64_ui_dispatcher_turn(&dispatcher,true) < 0) { failed = true; break; }
    }
    if (!dispose()) {
        /* Process exit releases native windows. Keep their borrowed state
         * alive if native destruction refused rather than freeing underneath it. */
        os64_complain("grootmenu: popup cleanup refused\n");
        return 1;
    }
    return failed ? 1 : 0;
}
#ifndef GROOTMENU_HOST
int main(int argc, char **argv) { return grootmenu_main(argc,argv); }
#endif
