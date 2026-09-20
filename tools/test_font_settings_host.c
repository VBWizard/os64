/* Exercise the production settings/installer against native files and the
 * same preserving writer as saved themes. Intercept only the session device. */
#define main saved_theme_fixture_main
#define os64_ui_theme_preserve_session saved_theme_preserve_stub
#define os64_open disk_open
#define os64_read disk_read
#define os64_write disk_write
#define os64_close disk_close
#include "test_appearance_saved_host.c"
#undef main
#undef os64_ui_theme_preserve_session
#undef os64_open
#undef os64_read
#undef os64_write
#undef os64_close
#include <sched.h>
#include "os64/appearance.h"
#include "os64/font_settings.h"
static char session[OS64_APPEARANCE_MAX + 1] = "generation = 0\n";
static size_t session_size = 15, session_pos;
static uint64_t session_generation;
void os64_yield(void) { sched_yield(); }
int64_t os64_open(const char *path, const char *mode)
{
    if (strcmp(path, OS64_APPEARANCE_PATH)) return disk_open(path, mode);
    session_pos = 0;
    return *mode == 'w' ? 1001 : 1000;
}
int64_t os64_read(int32_t fd, void *out, size_t n)
{
    if (fd != 1000) return disk_read(fd, out, n);
    if (n > session_size-session_pos) n = session_size-session_pos;
    memcpy(out,session+session_pos,n); session_pos += n; return (int64_t)n;
}
int64_t os64_write(int32_t fd, const void *bytes, size_t n)
{
    if (fd != 1001) return disk_write(fd,bytes,n);
    uint64_t expected; size_t head;
    assert(os64_appearance_header_read(bytes,n,&expected,&head));
    if (expected != session_generation || n-head > OS64_APPEARANCE_PAYLOAD_MAX) return -1;
    size_t new_head = os64_appearance_header_write(session,++session_generation);
    memcpy(session+new_head,(const char *)bytes+head,n-head);
    session_size = new_head+n-head; session[session_size] = 0;
    return (int64_t)n;
}
int64_t os64_close(int32_t fd) { return fd == 1000 || fd == 1001 ? 0 : disk_close(fd); }
static void *allocate(void *user, size_t size) { (void)user; return os64_malloc(size); }
static void deallocate(void *user, void *p, size_t size) { (void)user; (void)size; os64_free(p); }
static void path_join(char *out, size_t cap, const char *a, const char *b)
{ int n = snprintf(out,cap,"%s/%s",a,b); assert(n > 0 && (size_t)n < cap); }
static void read_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path,"rb"); assert(f);
    size_t n = fread(out,1,cap-1,f); assert(!ferror(f) && feof(f)); out[n] = 0; fclose(f);
}
static void clean_staging(void)
{
    char folder[512]; path_join(folder,sizeof(folder),root,"fonts");
    DIR *d = opendir(folder); assert(d); struct dirent *e;
    while ((e = readdir(d))) assert(e->d_name[0] != '.' || !strcmp(e->d_name,".") || !strcmp(e->d_name,".."));
    closedir(d);
}
int main(int argc, char **argv)
{
    assert(argc == 3 && strlen(argv[1]) < sizeof(root)); strcpy(root,argv[1]);
    os64_font_config_t config, current, desired;
    os64_font_config_error_t error;
    uint64_t generation;
    if (!strcmp(argv[2],"--reboot")) {
        assert(!os64_font_settings_current(&current,&generation) && !generation);
        assert(current.roles[OS64_FONT_ROLE_DOCUMENT].size == 24);
        assert(strstr(current.roles[OS64_FONT_ROLE_DOCUMENT].face[0],"/fonts/DejaVuSans.ttf"));
        puts("font settings: fresh process reads saved 24px startup selection");
        return 0;
    }
    os64_text_options_t options = {.memory = {NULL,allocate,deallocate}};
    os64_text_context_t *context = NULL;
    assert(os64_font_context_create(&options,&context) == OS64_FONT_OK);
    os64_font_config_defaults(&config);
    char source[256], installed[256] = "unchanged", destination[256], confpath[512];
    path_join(source,sizeof(source),argv[2],"DejaVuSans.ttf");
    strcpy(config.roles[OS64_FONT_ROLE_DOCUMENT].face[0],source);
    assert(!os64_font_config_install(context,&config,OS64_FONT_ROLE_DOCUMENT,installed,&error));
    assert(!error.status && strstr(installed,"/fonts/DejaVuSans.ttf"));
    strcpy(destination,installed);
    os64_font_set_t *set = NULL;
    strcpy(config.roles[OS64_FONT_ROLE_DOCUMENT].face[0],installed);
    assert(!os64_font_config_prepare(context,&config,&set,&error)); os64_font_set_release(set);
    strcpy(installed,"unchanged");
    assert(os64_font_config_install(context,&config,OS64_FONT_ROLE_DOCUMENT,installed,&error) == OS64_FONT_CONFIG_EXISTS);
    assert(error.status == OS64_FONT_CONFIG_EXISTS && !strcmp(installed,"unchanged"));
    // Proportional faces cannot be installed as the terminal choice.
    os64_font_config_defaults(&desired);
    strcpy(desired.roles[OS64_FONT_ROLE_TERMINAL].face[0],source);
    // Use another source basename so destination collision does not mask validation.
    char second[256]; path_join(second,sizeof(second),root,"proportional.otf");
    FILE *input = fopen(source,"rb"), *output = fopen(second,"wb"); assert(input && output);
    char buffer[8192]; size_t took;
    while ((took = fread(buffer,1,sizeof(buffer),input))) assert(fwrite(buffer,1,took,output) == took);
    fclose(input); fclose(output);
    strcpy(desired.roles[OS64_FONT_ROLE_TERMINAL].face[0],second);
    assert(os64_font_config_install(context,&desired,OS64_FONT_ROLE_TERMINAL,installed,&error) == OS64_FONT_CONFIG_FACE);
    assert(!strcmp(installed,"unchanged")); clean_staging();
    strcpy(desired.roles[OS64_FONT_ROLE_TERMINAL].face[0],"builtin");
    strcpy(desired.roles[OS64_FONT_ROLE_DOCUMENT].face[0],second);
    for (int failure = 0; failure < 3; ++failure) {
        bad_read = failure == 0; bad_write = failure == 1; bad_sync = failure == 2;
        assert(os64_font_config_install(context,&desired,OS64_FONT_ROLE_DOCUMENT,installed,&error) == OS64_FONT_CONFIG_IO);
        assert(error.status == OS64_FONT_CONFIG_IO && !strcmp(installed,"unchanged"));
        bad_read = bad_write = bad_sync = false; clean_staging();
    }
    raw("bad-font.ttf","bad",3); path_join(second,sizeof(second),root,"bad-font.ttf");
    strcpy(desired.roles[OS64_FONT_ROLE_DOCUMENT].face[0],second);
    assert(os64_font_config_install(context,&desired,OS64_FONT_ROLE_DOCUMENT,installed,&error) == OS64_FONT_CONFIG_FACE);
    clean_staging();

    const char *initial = "# Preserve my font comment\nui.face = builtin\nui.fallback.2 = fonts/DejaVuSans.ttf\nUI.fallback.2 = fonts/DejaVuSans.ttf\n";
    raw("fonts.conf",initial,strlen(initial));
    const char *future = "# Theme comment\nfuture.texture = linen  # exact\n";
    raw("theme.conf",future,strlen(future));
    assert(!os64_font_settings_current(&current,&generation) && !generation);
    // Save pins complete old choices without applying desired choices.
    assert(!os64_font_settings_save(&config,&error));
    assert(!os64_font_settings_current(&current,&generation) && generation);
    assert(!strcmp(current.roles[OS64_FONT_ROLE_DOCUMENT].face[0],"builtin"));
    assert(strstr(session,future));
    path_join(confpath,sizeof(confpath),root,"fonts.conf");
    char before[8192],after[8192]; read_file(confpath,before,sizeof(before));
    assert(strstr(before,"# Preserve my font comment\n"));
    assert(!strstr(before,"fallback.2"));
    assert(strstr(before,destination));
    assert(!os64_font_settings_apply(context,&config,NULL,&error));
    uint64_t active; assert(!os64_font_settings_current(&current,&active));
    assert(current.roles[OS64_FONT_ROLE_DOCUMENT].size == 16);
    config.roles[OS64_FONT_ROLE_DOCUMENT].size = 24;
    for (int failure = 0; failure < 3; ++failure) {
        bad_write = failure == 0; bad_sync = failure == 1; no_atomic = failure == 2;
        assert(os64_font_settings_save(&config,&error) != 0);
        bad_write = bad_sync = no_atomic = false;
        read_file(confpath,after,sizeof(after)); assert(!strcmp(before,after));
        assert(!os64_font_settings_current(&current,&generation) && generation == active);
        assert(current.roles[OS64_FONT_ROLE_DOCUMENT].size == 16);
    }
    assert(!os64_font_settings_save(&config,&error));
    assert(!os64_font_settings_current(&current,&generation) && generation == active);
    assert(current.roles[OS64_FONT_ROLE_DOCUMENT].size == 16);
    assert(!os64_font_config_read(&desired,&error));
    assert(desired.roles[OS64_FONT_ROLE_DOCUMENT].size == 24);
    assert(!strcmp(desired.roles[OS64_FONT_ROLE_DOCUMENT].face[0],destination));
    os64_text_destroy(context);
    puts("font settings: native installation, format/terminal refusal, staging cleanup, I/O failure, duplicate fallback deletion, preserved comments, independent Apply/Save passed");
    fflush(stdout);
    execl(argv[0],argv[0],root,"--reboot",(char *)NULL); abort();
}
