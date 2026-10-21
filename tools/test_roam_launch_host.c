/* Real Roam file-operation state machine and shared launch/config/slurp code;
 * deterministic filesystem, process and dialog boundaries. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define main roam_program_main
#include "../userland/apps/roam/roam.c"
#undef main
#include "../userland/libos64/directory.c"

static const char *mapping=".txt = /bin/scribe\n";
static size_t mapping_size;
static const char *settings="associations = file-associations.conf\n";
static size_t cursor[2];
static bool missing,read_error,allocation_failure,spawn_failure;
static bool menu_live,create_allowed=true,close_refused,open_failed;
static unsigned spawn_count,opens,closes,publishes;
static bool write_error,sync_error,rename_error,inherited,offered;
static char temporary[OS64_CONF_MAX*2],published[OS64_CONF_MAX*2],destination[256];
static size_t temporary_size;
static bool temporary_exists;
static int argument_count;
static char arguments[4][256],opened_path[256],message[256];
static os64_directory_t *test_directory;
#define test_row test_directory->entries[0]
#define generation test_directory->domain
static unsigned navigations;

void *os64_malloc(size_t n) { return allocation_failure?NULL:malloc(n); }
void os64_free(void *p) { free(p); }
int64_t os64_open(const char *path,const char *mode)
{
    if (!strcmp(mode,"w")) {
        temporary_size=0; temporary_exists=true; return 12;
    }
    assert(!strcmp(mode,"r"));
    if(!strcmp(path,"/config/roam.conf")) { cursor[0]=0;return 10; }
    snprintf(opened_path,sizeof(opened_path),"%s",path);
    if(missing) return -1;
    cursor[1]=0;return 11;
}
int64_t os64_read(int32_t fd,void *out,uint64_t cap)
{
    assert(fd==10 || fd==11);
    int i=fd-10;
    if(i && read_error && cursor[i]>=4) return -1;
    const char *bytes=i?mapping:settings;
    size_t size=i?mapping_size:strlen(settings);
    size_t n=size-cursor[i];
    if(n>cap)n=cap;
    if(n>4)n=4; /* Exercise short reads and a failure after a valid prefix. */
    memcpy(out,bytes+cursor[i],n);cursor[i]+=n;return (int64_t)n;
}
int64_t os64_close(int32_t fd) { (void)fd;return 0; }
uint64_t file_test_syscall(uint64_t n,uint64_t a,uint64_t b,uint64_t c,
    uint64_t d,uint64_t e,uint64_t f)
{
    (void)d;(void)f;
    assert(n==SYSCALL_CONF_RESOLVE);
    const char *name=(const char *)(uintptr_t)a;
    if(strchr(name,'/')) return (uint64_t)-1;
    int size=snprintf((char *)(uintptr_t)b,(size_t)c,"/%s/%s", inherited && !e && strcmp(name,"roam.conf") ? "etc" : "config",name);
    return size>0 && (uint64_t)size<c?1:(uint64_t)-1;
}
int64_t os64_write(int32_t fd,const void *data,uint64_t n)
{
    assert(fd==12);
    if (write_error) return -1;
    if (n>7) n=7;
    assert(temporary_size+n<sizeof(temporary));
    memcpy(temporary+temporary_size,data,n); temporary_size+=n;
    temporary[temporary_size]=0; return (int64_t)n;
}
int64_t os64_sync(int32_t fd) { assert(fd==12); return sync_error?-1:0; }
int64_t os64_unlink(const char *path) { (void)path;temporary_exists=false;return 0; }
uint64_t os64_taskid(void) { return 100; }
int64_t os64_rename_with_flags(const char *old,const char *path,uint64_t flags)
{
    (void)old;assert(flags==OS64_RENAME_REQUIRE_ATOMIC_REPLACE);
    if (rename_error) return -1;
    strcpy(published,temporary);strcpy(destination,path);++publishes;
    mapping=published;mapping_size=temporary_size;inherited=false;temporary_exists=false;
    return 0;
}
int64_t os64_rename(const char *old,const char *path)
{ (void)old;(void)path;assert(false);return -1; }
int64_t os64_spawn(const char *path,char *const argv[])
{
    assert(!strcmp(path,argv[0]));++spawn_count;argument_count=0;
    while(argv[argument_count]) {
        assert(argument_count<4);
        snprintf(arguments[argument_count],256,"%s",argv[argument_count]);
        ++argument_count;
    }
    return spawn_failure?-9:77;
}
bool os64_ui_menu_active(const os64_ui_menu_session_t *s)
{ (void)s;return menu_live; }
bool os64_ui_dispatcher_can_create(const os64_ui_dispatcher_t *d)
{ (void)d;return create_allowed; }
void os64_ui_set_enabled(os64_ui_t *ui,os64_ui_widget_t *w,bool enabled)
{ (void)ui;w->disabled=!enabled; }
void os64_ui_set_focus(os64_ui_t *ui,os64_ui_widget_t *w) { ui->focus=w; }
void os64_ui_shortcut_reset(os64_ui_shortcut_router_t *r) { memset(r,0,sizeof(*r)); }
void os64_ui_browser_message(os64_ui_browser_t *b,const char *s)
{ (void)b;snprintf(message,sizeof(message),"%s",s); }
const os64_ui_icon_provider_t *os64_icon_theme_provider(os64_icon_theme_t *t)
{ (void)t;return NULL; }
os64_ui_status_code_t os64_ui_file_dialog_open(os64_ui_file_dialog_t *d,
    os64_ui_dispatcher_t *dispatcher,int64_t owner,const os64_ui_file_options_t *o)
{
    (void)dispatcher;(void)owner;
    assert(create_allowed && !menu_live && o->mode==OS64_UI_FILE_APPLICATION);
    assert(!strcmp(o->directory,"/bin"));++opens;
    offered=o->offer_association;
    d->window=42;d->result=OS64_UI_FILE_PENDING;
    return open_failed?OS64_UI_REFUSED:OS64_UI_OK;
}
os64_ui_status_code_t os64_ui_file_dialog_close(os64_ui_file_dialog_t *d)
{
    ++closes;
    if(close_refused)return OS64_UI_BUSY;
    memset(d,0,sizeof(*d));return OS64_UI_OK;
}
const os64_directory_t *os64_browser_directory(const os64_browser_t *b)
{ (void)b;return test_directory; }
os64_ui_status_code_t os64_browser_activate(os64_browser_t *b,uint64_t domain,uint64_t id)
{ (void)b;assert(domain==generation && id==test_row.id);++navigations;return OS64_UI_OK; }

static void rules(const char *text) { mapping=text;mapping_size=strlen(text); }
static void resolve(const char *file,os64_launch_status_t want,const char *application)
{
    char out[OS64_LAUNCH_PATH]="stale";
    assert(os64_launch_association("file-associations.conf",file,out)==want);
    assert(!strcmp(out,application));
}
static void configuration_tests(void)
{
    rules(".txt = /bin/old\n.TXT = /apps/a program\n.md=/bin/scribe\n");
    resolve("/home/a folder/a \"quote\";$(literal).TxT",OS64_LAUNCH_OK,"/apps/a program");
    resolve("/dir.txt/no-extension",OS64_LAUNCH_UNASSOCIATED,"");
    resolve("/home/.txt",OS64_LAUNCH_UNASSOCIATED,"");
    resolve("/home/.notes.txt",OS64_LAUNCH_OK,"/apps/a program");
    resolve("/home/trailing.",OS64_LAUNCH_UNASSOCIATED,"");
    rules(".txt=/bin/scribe\n.txt=\n");resolve("/a.txt",OS64_LAUNCH_UNASSOCIATED,"");
    rules(".txt=/bin/scribe\nbroken tail\n");resolve("/a.txt",OS64_LAUNCH_CONFIG_ERROR,"");
    rules(".txt=relative\n");resolve("/a.txt",OS64_LAUNCH_CONFIG_ERROR,"");
    rules(".txt=/bin/scribe\n.tar.gz=/bin/archive\n");resolve("/a.txt",OS64_LAUNCH_CONFIG_ERROR,"");
    rules(".txt=/bin/scribe\n");mapping_size++;resolve("/a.txt",OS64_LAUNCH_CONFIG_ERROR,"");
    --mapping_size;missing=true;resolve("/a.txt",OS64_LAUNCH_CONFIG_ERROR,"");missing=false;
    read_error=true;resolve("/a.txt",OS64_LAUNCH_CONFIG_ERROR,"");read_error=false;
    allocation_failure=true;resolve("/a.txt",OS64_LAUNCH_CONFIG_ERROR,"");allocation_failure=false;
    char huge[OS64_CONF_MAX+1];memset(huge,' ',sizeof(huge));
    memcpy(huge,".txt=/bin/scribe\n",16);mapping=huge;mapping_size=OS64_CONF_MAX;
    resolve("/a.txt",OS64_LAUNCH_CONFIG_ERROR,"");
    mapping_size=OS64_CONF_MAX-1;resolve("/a.txt",OS64_LAUNCH_OK,"/bin/scribe");
    rules(".txt=/bin/scribe\n");
    char out[256];assert(os64_launch_association("/home/yogi/my associations.conf","/a.txt",out)==OS64_LAUNCH_OK);
    assert(!strcmp(opened_path,"/home/yogi/my associations.conf"));
    assert(os64_launch_association("relative/path","/a.txt",out)==OS64_LAUNCH_CONFIG_ERROR);
    int64_t pid=123;
    assert(os64_launch_file("/apps/a program","/home/\";$(x).txt",&pid)==OS64_LAUNCH_OK);
    assert(pid==77 && argument_count==2 && !strcmp(arguments[1],"/home/\";$(x).txt"));
    spawn_failure=true;
    assert(os64_launch_file("/bin/scribe","/a.txt",&pid)==OS64_LAUNCH_SPAWN_ERROR && pid==0);
    spawn_failure=false;
    unsigned count=spawn_count;
    assert(os64_launch_file("scribe","/a.txt",&pid)==OS64_LAUNCH_INVALID && pid==0);
    char longpath[257];memset(longpath,'x',256);longpath[0]='/';longpath[256]=0;
    assert(os64_launch_file("/bin/scribe",longpath,&pid)==OS64_LAUNCH_INVALID);
    assert(count==spawn_count);
}
static void remember_tests(void)
{
    rules("# user mappings\n.txt=/bin/old # preferred\n.TXT=/bin/later\n.md=/bin/scribe\n");
    inherited=true;
    assert(os64_launch_remember("file-associations.conf","/a.TXT","/apps/a program")==OS64_LAUNCH_OK);
    assert(!strcmp(destination,"/config/file-associations.conf"));
    assert(!strcmp(opened_path,"/etc/file-associations.conf"));
    assert(strstr(published,"# user mappings") && strstr(published,"# preferred") &&
        strstr(published,".md=/bin/scribe") && !strstr(published,"/bin/later"));
    resolve("/a.txt",OS64_LAUNCH_OK,"/apps/a program");
    assert(os64_launch_remember("/home/yogi/custom.conf","/a.xyz","/bin/scribe")==OS64_LAUNCH_OK);
    assert(!strcmp(destination,"/home/yogi/custom.conf"));
    unsigned before=publishes;
    const char *badfiles[]={"/no-extension","/.txt","/a.","/a.x#y","/a.x=y"};
    for(size_t i=0;i<sizeof(badfiles)/sizeof(*badfiles);++i)
        assert(os64_launch_remember("file-associations.conf",badfiles[i],"/bin/scribe")!=OS64_LAUNCH_OK);
    const char *badapps[]={"relative","/apps/hash#name","/apps/new\nline","/apps/trailing ","/apps/trailing\t"};
    for(size_t i=0;i<sizeof(badapps)/sizeof(*badapps);++i)
        assert(os64_launch_remember("file-associations.conf","/a.txt",badapps[i])!=OS64_LAUNCH_OK);
    rules(".txt=/bin/scribe\nbroken\n");
    assert(os64_launch_remember("file-associations.conf","/a.txt","/bin/new")!=OS64_LAUNCH_OK);
    rules(".txt=/bin/scribe\n"); ++mapping_size;
    assert(os64_launch_remember("file-associations.conf","/a.txt","/bin/new")!=OS64_LAUNCH_OK);
    --mapping_size;
    bool *faults[]={&missing,&read_error,&allocation_failure,&write_error,&sync_error,&rename_error};
    for(size_t i=0;i<sizeof(faults)/sizeof(*faults);++i) {
        *faults[i]=true;
        assert(os64_launch_remember("file-associations.conf","/a.txt","/bin/new")!=OS64_LAUNCH_OK);
        *faults[i]=false;
        assert(publishes==before && !temporary_exists);
        resolve("/a.txt",OS64_LAUNCH_OK,"/bin/scribe");
    }
    char huge[OS64_CONF_MAX];memset(huge,' ',sizeof(huge));mapping=huge;mapping_size=sizeof(huge);
    assert(os64_launch_remember("file-associations.conf","/a.txt","/bin/new")!=OS64_LAUNCH_OK);
    assert(publishes==before);rules(".txt=/bin/scribe\n");
}
static void remembering_roam_tests(void)
{
    roam_t a={0};unsigned before=publishes,spawns=spawn_count;
    file_queue(&a,FILE_CHOOSE,"/file.txt");file_step(&a);assert(offered);
    a.chooser.remember_association=true;a.chooser.result=OS64_UI_FILE_CANCELLED;
    file_step(&a);assert(publishes==before && spawn_count==spawns);
    file_queue(&a,FILE_CHOOSE,"/file.txt");file_step(&a);
    a.chooser.remember_association=true;a.chooser.result=OS64_UI_FILE_ACCEPTED;
    strcpy(a.chooser.path,"/bin/new");spawn_failure=true;
    file_step(&a);spawn_failure=false;assert(publishes==before);
    settings="associations=/home/yogi/chosen.conf\n";
    file_queue(&a,FILE_CHOOSE,"/file.txt");file_step(&a);
    settings="associations=/home/changed.conf\n";
    a.chooser.remember_association=true;a.chooser.result=OS64_UI_FILE_ACCEPTED;
    strcpy(a.chooser.path,"/bin/new");close_refused=true;
    file_step(&a);assert(publishes==before);close_refused=false;file_step(&a);
    assert(publishes==before+1 && !strcmp(destination,"/home/yogi/chosen.conf"));
    assert(strstr(message,"association saved"));
    file_queue(&a,FILE_CHOOSE,"/file.txt");file_step(&a);
    a.chooser.remember_association=true;a.chooser.result=OS64_UI_FILE_ACCEPTED;
    strcpy(a.chooser.path,"/bin/other");sync_error=true;file_step(&a);sync_error=false;
    assert(publishes==before+1 && strstr(message,"started, but"));
    file_queue(&a,FILE_CHOOSE,"/extensionless");file_step(&a);assert(!offered);
    a.chooser.result=OS64_UI_FILE_CANCELLED;file_step(&a);
    settings="associations=file-associations.conf\n";
}
static void roam_tests(void)
{
    roam_t a={0};unsigned before=spawn_count,saves=publishes;
    strcpy(test_row.entry.name,"a \"quote\";$(literal).txt");
    activate(&a,generation-1,test_row.id);assert(a.file_operation==FILE_NONE);
    test_row.entry.flags=OS64_DE_DIR;activate(&a,generation,test_row.id);
    assert(navigations==1 && a.file_operation==FILE_NONE);
    test_row.entry.flags=0;activate(&a,generation,test_row.id);
    assert(a.file_operation==FILE_OPEN && a.root.disabled && a.bar.w.disabled);
    ++generation;strcpy(test_row.entry.name,"another.txt"); /* Owned path survives model change. */
    menu_live=true;file_step(&a);assert(spawn_count==before);
    menu_live=false;file_step(&a);
    assert(spawn_count==before+1 && a.children==1 && !a.root.disabled);
    assert(!strcmp(arguments[1],"/home/a folder/a \"quote\";$(literal).txt"));
    settings="associations=/home/yogi/custom.conf\n";
    file_queue(&a,FILE_OPEN,"/a.txt");file_step(&a);
    assert(!strcmp(opened_path,"/home/yogi/custom.conf"));
    settings="associations=file-associations.conf\n";
    rules("bad\n");before=spawn_count;
    file_queue(&a,FILE_OPEN,"/a.txt");file_step(&a);
    assert(spawn_count==before && a.file_operation==FILE_NONE && strstr(message,"associations"));
    rules(".txt=/bin/scribe\n");
    file_queue(&a,FILE_OPEN,"/a.unknown");create_allowed=false;file_step(&a);
    assert(a.file_operation==FILE_CHOOSE && !a.chooser.window);
    create_allowed=true;file_step(&a);assert(a.chooser.window && opens==1);
    file_queue(&a,FILE_OPEN,"/replacement.txt");assert(!strcmp(a.file_path,"/a.unknown"));
    a.chooser.result=OS64_UI_FILE_CANCELLED;file_step(&a);
    assert(a.file_operation==FILE_NONE && spawn_count==before);
    file_queue(&a,FILE_CHOOSE,"/one-off.txt");file_step(&a);
    a.chooser.result=OS64_UI_FILE_ACCEPTED;strcpy(a.chooser.path,"/apps/chosen program");
    close_refused=true;file_step(&a);
    assert(a.chooser.window && a.root.disabled && spawn_count==before);
    close_refused=false;file_step(&a);
    assert(spawn_count==before+1 && !a.chooser.window && !a.root.disabled);
    assert(!strcmp(arguments[0],"/apps/chosen program") && !strcmp(arguments[1],"/one-off.txt"));
    before=spawn_count;open_failed=true;close_refused=true;
    file_queue(&a,FILE_CHOOSE,"/a.txt");file_step(&a);
    assert(a.file_cleanup && a.chooser.window && a.root.disabled);
    close_refused=false;file_step(&a);
    assert(a.file_operation==FILE_NONE && spawn_count==before);
    open_failed=false;
    file_queue(&a,FILE_TERMINAL,"/home/a folder;$(literal)");file_step(&a);
    assert(argument_count==3 && !strcmp(arguments[0],"/bin/gterm") &&
        !strcmp(arguments[1],"--directory") && !strcmp(arguments[2],"/home/a folder;$(literal)"));
    assert(publishes==saves);
    printf("Roam lifecycle: %u chooser opens, %u close attempts; literal argv verified.\n",opens,closes);
}
int main(void)
{
    test_directory=calloc(1,sizeof(*test_directory)+sizeof(test_row));
    assert(test_directory);test_directory->count=1;generation=8;test_row.id=7;
    strcpy(test_directory->path,"/home/a folder");
    configuration_tests();remember_tests();roam_tests();remembering_roam_tests();
    free(test_directory);
    puts("PASS: launch configuration, failure paths, Roam activation and chooser lifecycle");
    return 0;
}
