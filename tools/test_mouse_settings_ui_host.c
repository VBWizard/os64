#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/font_settings.h"
#define main mouse_settings_guest_main
#include "../userland/apps/mousesettings/mousesettings.c"
#undef main
static os64_mouse_snapshot_t fixture;
static os64_mouse_setting_t saved_setting;
static bool fail_save;
int os64_mouse_read(os64_mouse_snapshot_t *out) { *out=fixture; return 0; }
int os64_mouse_apply(const os64_mouse_command_t *command)
{
    if(command->expected_generation!=fixture.generation) return -1;
    assert(command->count==1);
    for(unsigned i=0;i<fixture.count;i++) if(!strcmp(fixture.devices[i].setting.key,command->settings[0].key)) {
        fixture.devices[i].setting=command->settings[0]; fixture.generation++; return 0;
    }
    return -1;
}
int os64_mouse_save(const os64_mouse_setting_t *setting)
{ if(fail_save) return -1; saved_setting=*setting; return 0; }
static void preview(const char *path)
{
    os64_gui_rect_t damage; os64_ui_mark_dirty(&ui,&root); assert(os64_ui_render(&ui,&damage));
    FILE *out=fopen(path,"wb"); assert(out);
    fprintf(out,"P6\n%u %u\n255\n",draw.surf.width,draw.surf.height);
    const uint32_t *pixels=(const uint32_t *)(uintptr_t)draw.surf.pixels;
    for(unsigned y=0;y<draw.surf.height;y++) for(unsigned x=0;x<draw.surf.width;x++) {
        uint32_t pixel=pixels[y*draw.surf.pitch_px+x];
        uint8_t rgb[]={pixel>>16,pixel>>8,pixel}; assert(fwrite(rgb,1,3,out)==3);
    }
    assert(!fclose(out));
}
void mouse_settings_ui_contracts(void)
{
    draw.surf.width=1100; draw.surf.height=760; draw.surf.pitch_px=1100;
    draw.surf.pixels=calloc(1100*760,4); assert(draw.surf.pixels);
    os64_ui_init(&ui,&draw); os64_ui_theme_defaults(&ui.theme);
    os64_ui_theme_palette(&ui.theme,OS64_UI_PALETTE_MIDNIGHT); ui.on_resize=resized;
    widgets_init(); assert(!measure(&ui,&layout_state)); resized(&ui);
    refresh_devices(NULL,NULL); assert(apply.disabled && speed.w.disabled);
    fixture.version=1; fixture.count=2;
    strcpy(fixture.devices[0].setting.key,"bt-1-de5233bdaca6");
    strcpy(fixture.devices[0].name,"Bluetooth mouse (de:52:33:bd:ac:a6)");
    fixture.devices[0].setting.speed=100; fixture.devices[0].connected=1;
    strcpy(fixture.devices[1].setting.key,"usb-01-p2"); strcpy(fixture.devices[1].name,"USB mouse (port 2)");
    fixture.devices[1].setting.speed=100; fixture.devices[1].connected=1;
    refresh_devices(NULL,NULL); assert(!apply.disabled && !next.disabled);
    os64_ui_slider_set(&ui,&speed,200); speed_changed(&speed,NULL);
    os64_ui_checkbox_set(&ui,&primary,true); publish(&save,NULL);
    assert(fixture.devices[0].setting.speed==200 && fixture.devices[0].setting.right_primary==1);
    assert(saved_setting.speed==200 && fixture.devices[1].setting.speed==100);
    select_device(&next,NULL); assert(selected==1 && speed.value==100 && !primary.checked);
    select_device(&previous,NULL); assert(speed.value==200 && primary.checked);
    fixture.generation++; os64_ui_slider_set(&ui,&speed,300); publish(&save,NULL);
    assert(strstr(status_text,"Apply failed") && saved_setting.speed==200);
    refresh_devices(NULL,NULL); fail_save=true; publish(&save,NULL);
    assert(strstr(status_text,"saving failed")); fail_save=false;
    reset_defaults(NULL,NULL); assert(speed.value==100 && !primary.checked);
    assert(!os64_ui_font_planner(&ui,plan_font,commit_font,discard_font,NULL));
    assert(!os64_ui_font_follow(&ui));
    os64_font_config_t config; os64_font_config_defaults(&config);
    strcpy(config.roles[0].face[0],"/test/scalable"); config.roles[0].size=28;
    assert(!os64_font_settings_apply(os64_ui_font_context(&ui),&config,NULL,NULL));
    assert(!os64_ui_font_follow(&ui));
    for(os64_ui_widget_t *w=root.first_child;w;w=w->next_sibling) {
        assert(w->bounds.x>=0 && w->bounds.y>=0);
        assert(w->bounds.x+w->bounds.w<=(int)draw.surf.width);
        assert(w->bounds.y+w->bounds.h<=(int)draw.surf.height);
    }
    assert(primary.w.bounds.y+primary.w.bounds.h<apply.bounds.y);
    mouse_layout_t accepted=layout_state; config.roles[0].size=96;
    assert(!os64_font_settings_apply(os64_ui_font_context(&ui),&config,NULL,NULL));
    assert(os64_ui_font_follow(&ui)); assert(!memcmp(&accepted,&layout_state,sizeof(accepted)));
    os64_font_config_defaults(&config);
    assert(!os64_font_settings_apply(os64_ui_font_context(&ui),&config,NULL,NULL));
    assert(!os64_ui_font_follow(&ui));
    draw.surf.width=600; draw.surf.height=440; draw.surf.pitch_px=1100; resized(&ui);
    refresh_devices(NULL,NULL); os64_ui_slider_set(&ui,&speed,200); speed_changed(&speed,NULL);
    message("Applied and saved for restart.");
    preview("/tmp/os64-mouse-settings.ppm");
    os64_ui_font_release(&ui); free((void *)(uintptr_t)draw.surf.pixels); draw.surf.pixels=0;
    puts("PASS: Mouse Settings selection, isolated Apply/Save, conflict/error feedback, defaults and 28px font layout");
}
