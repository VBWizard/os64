#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/charset.h"
#define main gterm_unused_main
#include "../userland/apps/gterm/gterm.c"
#undef main

static size_t checks, live, calls, deny;
#define CHECK(x) do {checks++; if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}} while (0)
typedef union {max_align_t align; size_t size;} allocation;
static void *allocate(void *user, size_t n)
{
    (void)user;
    if (++calls == deny) return NULL;
    allocation *p = malloc(sizeof(*p)+n); if (!p) abort();
    p->size = n; live += n; return p+1;
}
static void release(void *user, void *ptr, size_t n)
{
    (void)user; allocation *p = (allocation *)ptr-1;
    if (p->size != n || live < n) abort();
    live -= n; free(p);
}
static char clipboard[1024]; static size_t clipboard_len;
int64_t os64_open(const char *path, const char *mode)
{ CHECK(!strcmp(path, OS64_CLIPBOARD_PATH) && !strcmp(mode,"w")); clipboard_len=0; return 91; }
int64_t os64_write(int32_t fd, const void *p, uint64_t n)
{ CHECK(fd==91 && clipboard_len+n<=sizeof(clipboard)); memcpy(clipboard+clipboard_len,p,n); clipboard_len+=n;return (int64_t)n; }
int64_t os64_close(int32_t fd) {CHECK(fd==91);return 0;}
static bool refuse;
static unsigned resizes;
static uint32_t external_cols, external_rows;
static int64_t resize_fixture(void *user, uint32_t cols, uint32_t rows)
{
    (void)user; resizes++;
    if (refuse) return -1;
    external_cols=cols; external_rows=rows;
    /* Any allocation after a successful external mutation would fail. */
    deny=calls+1;
    return 0;
}
static os64_font_set_t *load_set(const char *path, unsigned size, os64_font_status_t expected)
{
    FILE *f=fopen(path,"rb"); CHECK(f!=NULL); CHECK(!fseek(f,0,SEEK_END));
    long length=ftell(f); CHECK(length>0); rewind(f);
    uint8_t *data=malloc((size_t)length); CHECK(data!=NULL);
    CHECK(fread(data,1,(size_t)length,f)==(size_t)length); fclose(f);
    os64_font_role_spec_t specs[3]={0}; specs[1].pixel_height=size;
    specs[1].primary=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,data,(size_t)length};
    os64_font_set_t *set=NULL;
    CHECK(os64_font_set_prepare(gText,specs,&set)==expected); free(data);return set;
}
static void geometry(void)
{
    uint32_t c=99,r=99;
    CHECK(gterm_grid_geometry(800,608,8,16,&c,&r) && c==100 && r==38);
    CHECK(gterm_grid_geometry(801,609,8,16,&c,&r) && c==100 && r==38);
    CHECK(!gterm_grid_geometry(15,608,8,16,&c,&r));
    CHECK(!gterm_grid_geometry(800,31,8,16,&c,&r));
    CHECK(!gterm_grid_geometry(4104,32,8,16,&c,&r));
    CHECK(!gterm_grid_geometry(16,4112,8,16,&c,&r));
    CHECK(gterm_grid_geometry(4096,512,8,16,&c,&r) && c*r==GTERM_MAX_CELLS);
    CHECK(!gterm_grid_geometry(4096,528,8,16,&c,&r));
    CHECK(!gterm_grid_geometry(UINT32_MAX,UINT32_MAX,1,1,&c,&r));
    CHECK(!gterm_grid_geometry(800,608,0,16,&c,&r));
}
static void pixels(void)
{
    const os64_font_role_view_t *f=gterm_grid_font(&gGrid);
    uint32_t w=(uint32_t)f->cell_width_px,h=(uint32_t)f->row_height_px;
    uint32_t pixels[128*256]; CHECK(w*3<=128 && h*3<=256);
    os64_gui_surface_t s={.pixels=pixels,.width=3*w,.height=3*h,.pitch_px=3*w};
    for(unsigned b=32;b<256;b++) for(unsigned cs=0;cs<2;cs++) {
        for(unsigned i=0;i<9*w*h;i++) pixels[i]=0xff123456;
        size_t before=calls;
        gterm_grid_draw_cell(&gGrid,&s,1,1,(uint8_t)b,(uint8_t)cs,0xffffffff);
        CHECK(calls==before);
        for(unsigned y=0;y<3*h;y++) for(unsigned x=0;x<3*w;x++)
            if(x<w || x>=2*w || y<h || y>=2*h) CHECK(pixels[y*3*w+x]==0xff123456);
    }
    /* NoBoxes primary lacks these: adjacent horizontal/vertical strokes
     * must meet across the exact cell boundary at both tested sizes. */
    memset(pixels,0,sizeof(pixels));
    gterm_grid_draw_cell(&gGrid,&s,0,0,0xc4,OS64_CHARSET_CP437,0xffffffff);
    gterm_grid_draw_cell(&gGrid,&s,0,1,0xc4,OS64_CHARSET_CP437,0xffffffff);
    unsigned joins=0;
    for(unsigned y=0;y<h;y++) if(pixels[y*3*w+w-1]) {CHECK(pixels[y*3*w+w]);joins++;}
    CHECK(joins>0);
    memset(pixels,0,sizeof(pixels));
    gterm_grid_draw_cell(&gGrid,&s,0,0,0xb3,OS64_CHARSET_CP437,0xffffffff);
    gterm_grid_draw_cell(&gGrid,&s,1,0,0xb3,OS64_CHARSET_CP437,0xffffffff);
    joins=0;
    for(unsigned x=0;x<w;x++) if(pixels[(h-1)*3*w+x]) {CHECK(pixels[h*3*w+x]);joins++;}
    CHECK(joins>0);
    memset(pixels,0,sizeof(pixels));
    gterm_grid_draw_cell(&gGrid,&s,0,0,0xdb,OS64_CHARSET_CP437,0xffffffff);
    for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) CHECK(pixels[y*3*w+x]==0xffffffff);
    uint32_t cp437[128*256]; memcpy(cp437,pixels,sizeof(pixels));
    memset(pixels,0,sizeof(pixels));
    gterm_grid_draw_cell(&gGrid,&s,0,0,0xdb,OS64_CHARSET_LATIN1,0xffffffff);
    CHECK(memcmp(cp437,pixels,sizeof(pixels))!=0);
}
static void selection(void)
{
    gHdr=(os64_pty_header_t){.cols=gGrid.cols,.rows=gGrid.rows};
    memset(gCells,0,MAX_CELLS*sizeof(*gCells));
    gCells[0].ch='A';gCells[1].ch=(char)0xe9;gCells[2].ch=' ';
    gSelLive=gDragging=gSnapshotValid=true;gAnchorRow=gEndRow=0;gAnchorCol=0;gEndCol=2;
    uint32_t a,b;CHECK(row_highlight(0,&a,&b) && a==0 && b==1);
    selection_copy();CHECK(clipboard_len==2 && clipboard[0]=='A' && (uint8_t)clipboard[1]==0xe9);
    const os64_font_role_view_t *f=gterm_grid_font(&gGrid);
    cell_at(f->cell_width_px*2+1,f->row_height_px*3+1,&a,&b);CHECK(a==3 && b==2);
    cell_at(-1,-1,&a,&b);CHECK(a==0 && b==0);
    cell_at(INT32_MAX,INT32_MAX,&a,&b);CHECK(a==gGrid.rows-1 && b==gGrid.cols-1);
    os64_gui_rect_t cursor=gterm_grid_cell_rect(&gGrid,3,2);
    CHECK(cursor.x==f->cell_width_px*2 && cursor.y==f->row_height_px*3 && cursor.w==f->cell_width_px && cursor.h==f->row_height_px);
}
int main(int argc,char **argv)
{
    CHECK(argc==3);geometry();
    os64_text_options_t options={.memory={NULL,allocate,release}};
    CHECK(os64_font_context_create(&options,&gText)==OS64_FONT_OK);
    gGrid=(gterm_grid_t){.memory=options.memory,.width=800,.height=608,
        .resize=resize_fixture,.invalidate=invalidate_grid};
    os64_font_set_t *builtin=NULL;CHECK(os64_font_set_prepare(gText,NULL,&builtin)==OS64_FONT_OK);
    CHECK(replace_fonts(builtin)==OS64_FONT_OK);deny=0;selection();
    char path[1024];snprintf(path,sizeof(path),"%s/DejaVuSans.ttf",argv[1]);
    CHECK(load_set(path,12,OS64_FONT_UNSUPPORTED)==NULL);
    size_t denial_cases=0;
    for(unsigned size=12;size<=28;size+=16) {
        os64_font_set_t *candidate=load_set(argv[2],size,OS64_FONT_OK);
        uint64_t identity=gterm_grid_font(&gGrid)->identity;
        unsigned before=resizes;refuse=true;
        CHECK(replace_fonts(candidate)==OS64_FONT_ENGINE_ERROR && resizes==before+1);
        CHECK(gterm_grid_font(&gGrid)->identity==identity && gSelLive && gDragging && gSnapshotValid);
        refuse=false;
        /* Warm caches, then fail each callback allocation in prepare until a
         * full adoption succeeds. Failed attempts may evict cache entries. */
        for(size_t n=1;;n++) {
            before=resizes;deny=calls+n;
            os64_font_status_t status=replace_fonts(candidate);deny=0;
            if(status==OS64_FONT_OK) break;
            CHECK(status==OS64_FONT_NO_MEMORY);
            CHECK(resizes==before && gterm_grid_font(&gGrid)->identity==identity);
            CHECK(gSelLive && gDragging && gSnapshotValid);denial_cases++;
            CHECK(n<10000);
        }
        CHECK(!gSelLive && !gDragging && !gSnapshotValid);
        CHECK(gGrid.cols==external_cols && gGrid.rows==external_rows);
        const os64_font_role_view_t *f=gterm_grid_font(&gGrid);
        CHECK((size==12 && f->cell_width_px==7 && f->row_height_px==15) ||
              (size==28 && f->cell_width_px==17 && f->row_height_px==33));
        os64_pty_header_t h={.cols=gGrid.cols,.rows=gGrid.rows};
        CHECK(gterm_grid_snapshot_matches(&gGrid,&h,gGrid.cols*gGrid.rows));
        CHECK(!gterm_grid_snapshot_matches(&gGrid,&h,-1));
        CHECK(!gterm_grid_snapshot_matches(&gGrid,&h,gGrid.cols*gGrid.rows-1));
        h.cols++;CHECK(!gterm_grid_snapshot_matches(&gGrid,&h,gGrid.cols*gGrid.rows));
        pixels();selection();
        before=resizes;CHECK(replace_fonts(candidate)==OS64_FONT_OK && resizes==before);deny=0;
        selection();refuse=true;CHECK(!gterm_grid_resize(&gGrid,700,500));
        CHECK(gSelLive && gDragging && gSnapshotValid && gGrid.cols==external_cols);
        refuse=false;CHECK(gterm_grid_resize(&gGrid,700,500));deny=0;
        CHECK(!gSelLive && !gDragging && !gSnapshotValid);selection();
        CHECK(!gterm_grid_resize(&gGrid,1,1) && gSelLive);
        gGrid.width=800;gGrid.height=608;
        os64_font_set_release(candidate);
        f=gterm_grid_font(&gGrid);
        printf("gterm fonts: %upx passed, cell %dx%d\n",size,f->cell_width_px,f->row_height_px);
    }
    os64_font_set_release(builtin);gterm_grid_destroy(&gGrid);
    CHECK(os64_text_destroy(gText)==OS64_FONT_OK && live==0);
    printf("gterm fonts: PASS %zu checks, %zu allocation denial cases, zero live bytes\n",checks,denial_cases);
    return 0;
}
