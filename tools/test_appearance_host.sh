#!/usr/bin/env bash
set -eu
cd "$(git rev-parse --show-toplevel)"
appearance_test_dir=$(mktemp -d)
trap 'rm -rf "$appearance_test_dir"' EXIT
mkdir -p "$appearance_test_dir/gui"
cat > "$appearance_test_dir/gui/gui_internal.h" <<'HEADER'
#include <pthread.h>
#include <stdint.h>
extern pthread_mutex_t kGuiLock;
static inline uint64_t spinlock_acquire_irqsave(pthread_mutex_t *lock)
{ pthread_mutex_lock(lock); return 0; }
static inline void spinlock_release_irqrestore(pthread_mutex_t *lock, uint64_t flags)
{ (void)flags; pthread_mutex_unlock(lock); }
HEADER
cat > "$appearance_test_dir/gui/window.h" <<'HEADER'
#include <stdint.h>
void wm_appearance_changed(uint64_t generation);
HEADER
cc -D_XOPEN_SOURCE=700 -pthread -std=c11 -fno-builtin-memmove -g -O1 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I "$appearance_test_dir" -I userland/libos64/include -I abi/include -I kernel/include \
    tools/test_appearance_host.c tools/test_appearance_customizer_host.c tools/test_appearance_session_host.c userland/libos64/ui.c \
    userland/libos64/ui_theme.c userland/libos64/ui_session.c userland/libos64/ui_palette.c userland/libos64/ui_color.c userland/libos64/ui_controls.c \
    userland/libos64/ui_list.c userland/libos64/ui_text.c userland/libos64/str.c userland/libos64/draw.c \
    kernel/src/gui/event_queue.c kernel/src/appearance.c \
    userland/libos64/conf.c userland/libos64/fmt.c \
    -Wl,--gc-sections,--wrap=os64_draw_ctx_refresh,--wrap=os64_conf_find_read,--wrap=os64_conf_write_checked -o "$appearance_test_dir/test_appearance"
"$appearance_test_dir/test_appearance"
