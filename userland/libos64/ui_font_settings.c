#include "os64/ui.h"
#include "os64/font_settings.h"
#include "os64/fmt.h"
#include "os64/str.h"

/* Fixed-coordinate applications can adopt a new face only while their text
 * rows fit. Applications that reflow install their own planner before follow.
 * Include inactive pages: switching tabs must remain usable after adoption. */
static os64_font_status_t fixed_rows(os64_ui_t *ui, os64_ui_widget_t *w)
{
    if (!w) return OS64_FONT_OK;
    const char *kind = w->cls ? w->cls->name : "";
    bool label = os64_streq(kind, "label");
    bool button = os64_streq(kind, "button");
    if (w->bounds.h > 0 && (label || button || os64_streq(kind, "textfield") ||
                           os64_streq(kind, "checkbox"))) {
        int32_t row = os64_ui_font_row_height(ui, OS64_FONT_ROLE_UI);
        if (row + (label ? 0 : 4) > w->bounds.h) return OS64_FONT_LIMIT;
    }
    for (os64_ui_widget_t *c = w->first_child; c; c = c->next_sibling) {
        os64_font_status_t status = fixed_rows(ui, c);
        if (status) return status;
    }
    return OS64_FONT_OK;
}
static os64_font_status_t fixed_plan(os64_ui_t *ui, void *user, void **out)
{ (void)user; *out = NULL; return fixed_rows(ui, ui->root); }
static void fixed_done(os64_ui_t *ui, void *user, void *plan)
{ (void)ui; (void)user; (void)plan; }

static void refresh_fonts(os64_ui_t *ui)
{
    os64_font_config_t config;
    uint64_t generation;
    int result = os64_font_settings_current(&config, &generation);
    if (result) { ui->font_settings_result = result; return; }
    if (ui->font_settings_ready && ui->font_generation == generation) return;
    os64_text_context_t *context = os64_ui_font_context(ui);
    os64_font_set_t *set = NULL;
    os64_font_config_error_t error;
    if (os64_font_config_prepare(context, &config, &set, &error)) {
        os64_printf("libui: font line %lu: %s (%u); keeping current fonts\n",
                     (unsigned long)error.line, os64_font_config_status_name(error.status),
                     error.font_status);
        ui->font_settings_result = OS64_UI_APPLY_INVALID;
        return;
    }
    os64_font_consumer_t consumer;
    os64_ui_font_consumer(ui, &consumer);
    os64_font_status_t adopted = os64_font_adopt(set, &consumer, 1, NULL);
    os64_font_set_release(set);
    ui->font_settings_result = adopted ? OS64_UI_APPLY_INVALID : 0;
    if (!adopted) {
        ui->font_generation = generation;
        ui->font_settings_ready = true;
    } else os64_printf("libui: font layout refused (%u); keeping current fonts\n", adopted);
}

int os64_ui_font_follow(os64_ui_t *ui)
{
    if (!ui) return OS64_UI_APPLY_INVALID;
    if (!ui->font_plan)
        (void)os64_ui_font_planner(ui, fixed_plan, fixed_done, fixed_done, NULL);
    ui->font_session = refresh_fonts;
    refresh_fonts(ui);
    return ui->font_settings_result;
}
