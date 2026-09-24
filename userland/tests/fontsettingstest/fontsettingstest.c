/* Guest driver for the public F5 API. It publishes the same session component
 * as Workshop; production applications have no private font selector. */
#include "os64/os64.h"
#include "os64/font_settings.h"
#include "os64/ui.h"
static void *allocate(void *user, size_t n) { (void)user; return os64_malloc(n); }
static void release(void *user, void *p, size_t n) { (void)user; (void)n; os64_free(p); }
int main(int argc, char **argv)
{
    char command = argc == 2 ? argv[1][0] : '0';
    os64_font_config_t config, current;
    os64_font_config_defaults(&config);
    uint64_t before = 0, after = 0;
    if (os64_font_settings_current(&current, &before)) return 1;
    if (command != '6') {
        os64_strcopy(config.roles[0].face[0], OS64_FONT_PATH_CAP, "/etc/fonts/DejaVuSans.ttf");
        os64_strcopy(config.roles[1].face[0], OS64_FONT_PATH_CAP, "/etc/fonts/DejaVuSansMono.ttf");
        os64_strcopy(config.roles[2].face[0], OS64_FONT_PATH_CAP, "/etc/fonts/DejaVuSans.ttf");
        config.roles[2].size = 20;
        if (command == '2' || command == '4') {
            config.roles[1].size = 24; config.roles[2].size = 28;
        }
    }
    os64_text_options_t options = {.memory = {NULL, allocate, release}};
    os64_text_context_t *context = NULL;
    if (os64_font_context_create(&options, &context)) return 1;
    os64_font_config_error_t error = {0};
    int result = 0;
    if (command == '3') {
        os64_strcopy(config.roles[1].face[0], OS64_FONT_PATH_CAP, "/etc/fonts/DejaVuSans.ttf");
        result = os64_font_settings_apply(context, &config, NULL, &error);
        if (!result || error.status != OS64_FONT_CONFIG_FACE ||
            os64_font_settings_current(&current, &after) || after != before) result = -1;
        else result = 0;
    } else if (command == '4') result = os64_font_settings_save(&config, &error);
    else if (command == '5') {
        if (before || current.roles[1].size != 24 || current.roles[2].size != 28) result = -1;
    } else if (command == '7') {
        os64_ui_theme_t theme; os64_ui_theme_init(&theme);
        os64_ui_theme_palette(&theme, OS64_UI_PALETTE_PAPER);
        result = os64_ui_theme_apply(&theme, OS64_UI_COMPONENT_PALETTE, NULL);
        if (!result && (os64_font_settings_current(&current, &after) || before != after)) result = -1;
    } else if (command == '8') result = os64_font_settings_apply(context, &current, NULL, &error);
    else if (command != '0') result = os64_font_settings_apply(context, &config, NULL, &error);
    if (os64_font_settings_current(&current, &after)) result = -1;
    os64_printf("F5 %s: command %c serial %lu ui=%u terminal=%u document=%u status=%d font=%u\n",
        result ? "FAIL" : "PASS", command, (unsigned long)after, current.roles[0].size,
        current.roles[1].size,current.roles[2].size,result,error.font_status);
    os64_text_destroy(context);
    return result != 0;
}
