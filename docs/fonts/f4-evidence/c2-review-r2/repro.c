/* C2 correction review: real widget events and paint, host canvas and clipboard. */
#define main upstream_test_main
#include F4_C2_HOST_TEST
#undef main

static const unsigned char *clip_bytes;
static size_t clip_len, clip_pos;
int64_t os64_open(const char *path, const char *mode)
{ (void)path; (void)mode; clip_pos=0; return 10; }
int64_t os64_read(int32_t fd, void *p, size_t n)
{ (void)fd; if(n>clip_len-clip_pos)n=clip_len-clip_pos;
  memcpy(p,clip_bytes+clip_pos,n); clip_pos+=n; return n; }
int64_t os64_close(int32_t fd) { (void)fd; return 0; }

static void hex(const char *s,size_t n)
{ for(size_t i=0;i<n;i++)printf("%02x",(unsigned char)s[i]); }
static size_t color_count(canvas_t *c,uint32_t color)
{ size_t n=0;for(size_t i=0;i<SURF_W*SURF_H;i++)n+=c->px[i]==color;return n; }
static void field_init(os64_ui_t *ui,os64_ui_textfield_t *tf,char *buf,size_t cap)
{
    memset(ui,0,sizeof(*ui));ui_test_view_theme(&ui->theme);
    ui->theme.field_fg=0xffffffff;ui->theme.field_bg=0xff000000;
    os64_ui_textfield(tf,buf,cap,NULL,NULL,NULL);
    tf->w.bounds=(os64_gui_rect_t){0,0,150,32};
    os64_ui_set_root(ui,&tf->w);tf->w.focused=true;
}
static void no_engine(os64_ui_t *ui)
{
    deny_countdown=1; /* binding succeeds, context allocation fails persistently */
    void *ctx=os64_ui_font_context(ui);deny_countdown=-1;
    printf("engine absent=%d status=%d\n",ctx==NULL,os64_ui_font_status(ui));
}
int main(void)
{
    for(int back=0;back<2;back++) {
        os64_ui_t ui;os64_ui_textfield_t tf;char buf[32];
        field_init(&ui,&tf,buf,sizeof(buf));
        os64_ui_textfield_set(&ui,&tf,"e1\xcc\x81");tf.cursor=back?2:1;
        if(back)ui_test_field_key(&tf,&ui,'\b');else ui_test_field_delete(&tf,&ui);
        printf("field %s joining accent: bytes=",back?"Backspace":"Delete");hex(buf,tf.len);
        printf(" caret=%zu snapped-before=%zu snapped-after=%zu\n",tf.cursor,
          os64_ui_text_snap(buf,tf.len,tf.cursor,false),os64_ui_text_snap(buf,tf.len,tf.cursor,true));
        if(back)ui_test_field_key(&tf,&ui,'\b');else ui_test_field_delete(&tf,&ui);
        printf("next same key: bytes=");hex(buf,tf.len);printf(" caret=%zu\n",tf.cursor);
        os64_ui_font_release(&ui);
    }
    {
        os64_ui_t ui;os64_ui_textfield_t tf;char buf[32];canvas_t c;
        field_init(&ui,&tf,buf,sizeof(buf));no_engine(&ui);
        os64_ui_textfield_set(&ui,&tf,"abc");
        canvas_init(&c,0xff000000);os64_draw_ctx_t ctx={0};ctx.surf=c.s;
        tf.w.cls->paint(&tf.w,&ctx,&ui.theme);
        printf("no-engine field: text pixels=%zu caret pixels=%zu\n",
          color_count(&c,ui.theme.field_fg),color_count(&c,ui.theme.text_caret));
        os64_ui_font_release(&ui);
    }
    {
        os64_ui_t ui={0};os64_ui_textview_t tv;canvas_t c;
        ui_test_view_theme(&ui.theme);
        os64_ui_textview(&tv,&kUiTestBuf,NULL,NULL,NULL);
        tv.w.bounds=(os64_gui_rect_t){0,0,300,60};
        os64_ui_set_root(&ui,&tv.w);no_engine(&ui);
        tv.w.focused=true;tv.cur_col=4;tv.sel=true;tv.sel_col=0;
        canvas_init(&c,0xff000000);os64_draw_ctx_t ctx={0};ctx.surf=c.s;
        tv.w.cls->paint(&tv.w,&ctx,&ui.theme);
        printf("no-engine view: text pixels=%zu caret pixels=%zu selection pixels=%zu\n",
          color_count(&c,ui.theme.text_fg),color_count(&c,ui.theme.text_caret),color_count(&c,ui.theme.text_sel_bg));
        os64_ui_font_release(&ui);
    }
    {
        os64_ui_t ui;os64_ui_textfield_t tf;char buf[5];
        field_init(&ui,&tf,buf,sizeof(buf));os64_ui_textfield_set(&ui,&tf,"abc");
        clip_bytes=(const unsigned char *)"\xc3\xa9";clip_len=2;
        size_t n=os64_ui_textfield_paste(&ui,&tf);
        printf("capacity paste of e-acute: added=%zu bytes=",n);hex(buf,tf.len);
        printf(" caret=%zu\n",tf.cursor);os64_ui_font_release(&ui);
    }
    return 0;
}
