#ifndef OS64_UI_RESULT_H
#define OS64_UI_RESULT_H
/* Toolkit results are independent of errno. ERROR carries an application
 * result through the action invocation API. */
typedef enum {
    OS64_UI_OK, OS64_UI_INVALID, OS64_UI_DUPLICATE, OS64_UI_LIMIT,
    OS64_UI_NO_MEMORY, OS64_UI_STALE, OS64_UI_BUSY, OS64_UI_REFUSED,
    OS64_UI_ERROR
} os64_ui_status_code_t;

#endif
