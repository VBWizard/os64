/* Drive the production install handler with a published file and a refusing
 * preview planner. Filesystem publication itself has a separate host suite. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern pid_t waitpid(pid_t, int *, int);
#include "../userland/apps/appearance/font_page.c"

static bool mock_files, installed_file, fail_discovery;
static unsigned discoveries, plans;
static char status_text[256];
os64_font_config_status_t __real_os64_font_config_install(os64_text_context_t *,
    const os64_font_config_t *, os64_font_role_t, char *, os64_font_config_error_t *);
os64_font_config_status_t __real_os64_font_config_discover(os64_text_context_t *,
    const os64_font_config_t *, os64_font_catalog_t **);

os64_font_config_status_t __wrap_os64_font_config_install(os64_text_context_t *ctx,
    const os64_font_config_t *config, os64_font_role_t r, char *out, os64_font_config_error_t *error)
{
    if (!mock_files) return __real_os64_font_config_install(ctx,config,r,out,error);
    assert(!strcmp(config->roles[r].face[0], "/test/scalable"));
    installed_file = true;
    strcpy(out, "/test/scalable");
    return OS64_FONT_CONFIG_OK;
}

os64_font_config_status_t __wrap_os64_font_config_discover(os64_text_context_t *ctx,
    const os64_font_config_t *config, os64_font_catalog_t **out)
{
    if (!mock_files) return __real_os64_font_config_discover(ctx,config,out);
    ++discoveries;
    *out = NULL;
    if (fail_discovery) return OS64_FONT_CONFIG_IO;
    *out = os64_malloc(sizeof(**out)); assert(*out);
    memset(*out, 0, sizeof(**out));
    (*out)->count = installed_file ? 2 : 1;
    strcpy((*out)->entries[0].path, "builtin");
    if (installed_file) strcpy((*out)->entries[1].path, "/test/scalable");
    return OS64_FONT_CONFIG_OK;
}
static void capture_status(const char *s) { os64_strcopy(status_text,sizeof(status_text),s); }
static os64_font_status_t refuse_preview(os64_ui_t *ui, void *user, void **out)
{ (void)ui; (void)user; *out = NULL; ++plans; return OS64_FONT_LIMIT; }
static void unused_plan(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; assert(!plan); }

void font_page_install_contracts(void)
{
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        mock_files = true;
        os64_ui_t edit, sample;
        os64_ui_widget_t page, root;
        os64_ui_init(&edit, NULL); os64_ui_init(&sample, NULL);
        os64_ui_panel(&page); os64_ui_set_root(&edit, &page);
        os64_ui_panel(&root); os64_ui_set_root(&sample, &root);
        font_page_init(&edit, &sample, &page, capture_status);
        font_page_activate();
        assert(catalog && catalog->count == 1 && !have_undo);
        os64_font_config_t old = draft;
        void *old_font = sample.font;
        assert(!os64_ui_font_planner(&sample, refuse_preview, unused_plan, unused_plan, NULL));
        strcpy(install_path, "/test/scalable");
        unsigned before = discoveries;
        use_file(NULL, NULL);
        assert(installed_file && plans == 1);
        assert(discoveries == before + 1 && catalog->count == 2);
        assert(os64_font_catalog_find(catalog, "/test/scalable") >= 0);
        assert(!memcmp(&draft,&old,sizeof(old)) && !have_undo && sample.font == old_font);
        assert(strstr(status_text,"Font installed") && strstr(status_text,"preview kept"));
        os64_font_catalog_t *old_catalog = catalog;
        fail_discovery = true;
        use_file(NULL, NULL);
        assert(plans == 2 && catalog == old_catalog);
        assert(!memcmp(&draft,&old,sizeof(old)) && !have_undo && sample.font == old_font);
        assert(strstr(status_text,"Font installed") && strstr(status_text,"list refresh failed"));
        font_page_close();
        os64_ui_font_release(&edit); os64_ui_font_release(&sample);
        _exit(0);
    }
    int status; assert(waitpid(pid,&status,0) == pid && !status);
    puts("font install: refused preview refreshes list without changing draft/undo/fonts; discovery failure is reported");
}
