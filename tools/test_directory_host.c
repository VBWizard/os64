#include "test_directory_host_support.h"
#include "os64/directory.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>

static os64_dirent_t entries[10001];
static void set(size_t i,const char *name,uint32_t flags,uint64_t size,uint64_t mtime)
{ entries[i]=(os64_dirent_t){.size=size,.flags=flags,.mtime=mtime};strcpy(entries[i].name,name); }
static void *ref_loop(void *p)
{ os64_directory_t *d=p;for (int i=0;i<10000;++i) {os64_directory_retain(d);os64_directory_release(d);}return NULL; }
static bool cancel(void *p) { return *(bool *)p; }
int main(void)
{
    char path[OS64_DIRECTORY_PATH];
    assert(os64_directory_path("/a/b","../c//./d/../../x",path)==OS64_UI_OK && !strcmp(path,"/a/x"));
    assert(os64_directory_path("/","../../",path)==OS64_UI_OK && !strcmp(path,"/"));
    char longname[256];memset(longname,'x',255);longname[255]=0;
    assert(os64_directory_join("/a",longname,path)==OS64_UI_LIMIT);
    assert(os64_directory_join("/a","bad/name",path)==OS64_UI_INVALID);
    test_reset_io();
    set(0,"z.txt",0,42,0);set(1,"folder",OS64_DE_DIR,0,123);
    set(2,".secret",0,1,0);set(3,"line\nfeed",0,3,0);
    test_entries=entries;test_entry_count=4;
    os64_directory_t *d=NULL;int64_t io=0;
    assert(os64_directory_read("/a",9,NULL,NULL,NULL,&d,&io)==OS64_UI_OK);
    assert(os64_directory_count(d)==4 && !os64_directory_incomplete(d));
    pthread_t t;assert(!pthread_create(&t,NULL,ref_loop,d));ref_loop(d);pthread_join(t,NULL);
    os64_ui_data_t *data=NULL;assert(os64_directory_data(d,&data)==OS64_UI_OK);
    const os64_ui_data_row_t *row=os64_ui_data_row(data,os64_directory_at(d,3)->id);
    assert(row && !strcmp(row->cells[0].display.bytes,"line\\x0Afeed"));
    assert(!os64_ui_data_row(data,os64_directory_at(d,0)->id)->cells[3].key.present);
    os64_ui_data_projection_t *p=NULL;
    assert(os64_directory_project(d,data,"",false,(os64_ui_data_sort_t){1,true},&p)==OS64_UI_OK);
    assert(os64_ui_data_projection_count(p)==3);
    assert(os64_ui_data_projection_id(p,0)==os64_directory_at(d,1)->id);
    os64_ui_data_projection_release(p);
    assert(os64_directory_project(d,data,"SECRET",true,(os64_ui_data_sort_t){1,false},&p)==OS64_UI_OK);
    assert(os64_ui_data_projection_count(p)==1);os64_ui_data_projection_release(p);
    os64_ui_data_release(data);
    test_alloc_fail=1;
    assert(os64_directory_data(d,&data)==OS64_UI_NO_MEMORY && !data);
    test_alloc_fail=0;

    set(0,"z.txt",0,43,0);set(1,"new",0,1,0);test_entry_count=2;
    os64_directory_t *refresh=NULL;
    assert(os64_directory_read("/a",9,d,NULL,NULL,&refresh,&io)==OS64_UI_OK);
    assert(os64_directory_at(refresh,0)->id==os64_directory_at(d,0)->id);
    assert(os64_directory_at(refresh,1)->id>os64_directory_at(d,3)->id);
    os64_directory_release(refresh);
    test_read_error_at=1;
    assert(os64_directory_read("/a",9,d,NULL,NULL,&refresh,&io)==OS64_UI_ERROR && io==-7 && !refresh);
    test_reset_io();test_close_error=1;
    assert(os64_directory_read("/a",9,d,NULL,NULL,&refresh,&io)==OS64_UI_ERROR && io==-8 && !refresh);
    test_reset_io();test_alloc_fail=1;
    assert(os64_directory_read("/a",9,d,NULL,NULL,&refresh,&io)==OS64_UI_NO_MEMORY && !refresh);
    test_reset_io();bool stopped=true;
    assert(os64_directory_read("/a",9,d,cancel,&stopped,&refresh,&io)==OS64_UI_STALE && !refresh);
    test_reset_io();set(0,"dup",0,0,0);set(1,"dup",0,0,0);
    assert(os64_directory_read("/a",9,NULL,NULL,NULL,&refresh,&io)==OS64_UI_DUPLICATE);
    set(1,"bad/name",0,0,0);
    assert(os64_directory_read("/a",9,NULL,NULL,NULL,&refresh,&io)==OS64_UI_INVALID);
    set(1,"badmount",OS64_DE_MOUNT,0,0);
    assert(os64_directory_read("/a",9,NULL,NULL,NULL,&refresh,&io)==OS64_UI_INVALID);
    os64_directory_release(d);
    for (size_t i=0;i<10001;++i) {
        entries[i].flags=0;
        snprintf(entries[i].name,sizeof(entries[i].name),"n%zu",i);
    }
    test_entry_count=10001;test_reset_io();
    assert(os64_directory_read("/",10,NULL,NULL,NULL,&d,&io)==OS64_UI_OK);
    assert(os64_directory_count(d)==10000 && os64_directory_incomplete(d));
    os64_directory_release(d);
    test_entry_count=0;test_reset_io();
    assert(os64_directory_read("/empty",11,NULL,NULL,NULL,&d,&io)==OS64_UI_OK);
    assert(os64_directory_count(d)==0 && !os64_directory_incomplete(d));
    assert(os64_directory_data(d,&data)==OS64_UI_OK && os64_ui_data_count(data)==0);
    os64_ui_data_release(data);os64_directory_release(d);
    test_mounts="# prefix fstype ...\n/mnt/data ext2 nvme 1 Data guid rw 4096 1 1 0 0\n";
    os64_directory_places_t places;os64_directory_places(&places);
    assert(!places.unavailable && places.count==3 && places.favorite_count==2 &&
        !strcmp(places.items[2].path,"/mnt/data"));
    assert(os64_directory_home(path)==OS64_UI_OK && !strcmp(path,"/home/chris"));
    test_home=NULL;os64_directory_places(&places);
    assert(os64_directory_home(path)==OS64_UI_OK && !strcmp(path,"/home"));
    assert(places.favorite_count==2 && places.count==3 &&
        !strcmp(places.items[1].path,"/home") && !strcmp(places.items[2].path,"/mnt/data"));
    test_home="relative/home";os64_directory_places(&places);
    assert(places.favorite_count==2 && !strcmp(places.items[1].path,"/home"));
    char overlong_home[258];overlong_home[0]='/';
    memset(overlong_home+1,'x',256);overlong_home[257]=0;
    test_home=overlong_home;os64_directory_places(&places);
    assert(places.favorite_count==2 && !strcmp(places.items[1].path,"/home"));
    test_home="/";os64_directory_places(&places);
    assert(places.favorite_count==1 && places.count==2);
    assert(os64_directory_home(path)==OS64_UI_OK && !strcmp(path,"/"));
    test_mounts="# prefix fstype ...\n/mnt/line\\x0Afeed ext2 nvme 1 Data guid rw 4096 1 1 0 0\n";
    os64_directory_places(&places);
    assert(places.count==2 && !strcmp(places.items[1].label,"line\\x0Afeed") &&
        !strcmp(places.items[1].path,"/mnt/line\nfeed"));
    puts("directory host: ok");return 0;
}
