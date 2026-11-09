#include "os64/ui_file_dialog.h"
#include "os64/fmt.h"
#include "os64/str.h"

/* Exercise the shared windows without performing file I/O on acceptance.
 * /tests/filedialogtest [save|open] [absolute-directory] [suggested-name] */
static os64_ui_file_dialog_t dialog;
int main(int argc, char **argv)
{
    os64_ui_file_options_t options = {
        .mode = argc > 1 && os64_streq(argv[1], "open") ? OS64_UI_FILE_OPEN : OS64_UI_FILE_SAVE,
        .directory = argc > 2 ? argv[2] : NULL,
        .filename = argc > 3 ? argv[3] : NULL
    };
    os64_ui_status_code_t rc = os64_ui_file_dialog_open(&dialog, NULL, 0, &options);
    if (rc != OS64_UI_OK) {
        os64_printf("filedialogtest: could not open dialog (%u)\n", rc);
        (void)os64_ui_file_dialog_close(&dialog);
        return 1;
    }
    while (dialog.result == OS64_UI_FILE_PENDING) {
        if (os64_gui_event_wait(dialog.window, NULL) != 1) break;
        os64_ui_file_dialog_pump(&dialog);
    }
    if (dialog.result == OS64_UI_FILE_ACCEPTED)
        os64_printf("Selected: %s (replace consent: %s)\n", dialog.path,
            dialog.replace_existing ? "yes" : "no");
    else os64_printf("Cancelled.\n");
    return os64_ui_file_dialog_close(&dialog) == OS64_UI_OK ? 0 : 1;
}
