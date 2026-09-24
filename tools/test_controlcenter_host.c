// Exercise the production planner and pagination without launching tools.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "os64/font_settings.h"
#define main controlcenter_guest_main
#include "../userland/apps/controlcenter/controlcenter.c"
#undef main

void controlcenter_layout_contracts(void)
{
    gCtx.surf.width = 900; gCtx.surf.height = 400;
    os64_ui_init(&gUi, &gCtx);
    os64_ui_panel(&gRoot); os64_ui_set_root(&gUi, &gRoot);
    os64_ui_widget_t *labels[] = {&gTitle, &gSubtitle, &gStatus};
    for (unsigned i = 0; i < 3; ++i) {
        os64_ui_label(labels[i], "Caption");
        os64_ui_add_child(&gRoot, labels[i]);
    }
    for (unsigned i = 0; i < ROWS; ++i) {
        os64_ui_button(&gRows[i], gCaptions[i], NULL, NULL);
        os64_ui_add_child(&gRoot, &gRows[i]);
    }
    os64_ui_button(&gBack, "Back", NULL, NULL);
    os64_ui_button(&gPrev, "Previous", NULL, NULL);
    os64_ui_button(&gNext, "Next", NULL, NULL);
    os64_ui_add_child(&gRoot, &gBack);
    os64_ui_add_child(&gRoot, &gPrev);
    os64_ui_add_child(&gRoot, &gNext);
    for (int i = 0; i < 9; ++i) {
        gMenu.nodes[i].kind = OS64_MENU_ITEM;
        strcpy(gMenu.nodes[i].label, "Tool");
        gMenu.nodes[i].next = i == 8 ? -1 : i + 1;
    }
    gFirst = 0;
    assert(!os64_ui_font_planner(&gUi, plan_font, commit_font, discard_font, NULL));
    assert(!os64_ui_font_follow(&gUi));
    unsigned old_page = gPageSize;
    os64_ui_set_focus(&gUi, &gRows[old_page - 1]);
    os64_font_config_t config; os64_font_config_defaults(&config);
    strcpy(config.roles[0].face[0], "/test/scalable"); config.roles[0].size = 28;
    assert(!os64_font_settings_apply(os64_ui_font_context(&gUi), &config, NULL, NULL));
    assert(!os64_ui_font_follow(&gUi));
    assert(os64_ui_font_row_height(&gUi, OS64_FONT_ROLE_UI) == 28);
    assert(gPageSize < old_page && gPageSize > 0);
    assert(!gUi.focus);
    for (os64_ui_widget_t *w = gRoot.first_child; w; w = w->next_sibling) {
        if (w->hidden) continue;
        assert(w->bounds.x >= 0 && w->bounds.y >= 0);
        assert(w->bounds.x + w->bounds.w <= (int)gCtx.surf.width);
        assert(w->bounds.y + w->bounds.h <= (int)gCtx.surf.height);
    }
    for (unsigned i = 0; i < gPageSize; ++i)
        assert(gRows[i].bounds.y + gRows[i].bounds.h < gNext.bounds.y);
    unsigned seen = 0;
    do {
        for (unsigned i = 0; i < gPageSize; ++i)
            if (!gRows[i].hidden) assert(gNodes[i] == (int)seen++);
        if (!gMore) break;
        next(NULL, NULL);
    } while (1);
    assert(seen == 9);
    previous(NULL, NULL); assert(gMore);
    center_layout_t accepted = gLayout;
    config.roles[0].size = 96;
    assert(!os64_font_settings_apply(os64_ui_font_context(&gUi), &config, NULL, NULL));
    assert(os64_ui_font_follow(&gUi));
    assert(!memcmp(&accepted, &gLayout, sizeof(accepted)));
    os64_font_config_defaults(&config);
    assert(!os64_font_settings_apply(os64_ui_font_context(&gUi), &config, NULL, NULL));
    assert(!os64_ui_font_follow(&gUi));
    assert(gLayout.min_h == 250);
    os64_ui_font_release(&gUi);
    puts("controlcenter responsive: 28px adoption, fewer rows, every tool reachable, refusal and shrinking minimum passed");
}
