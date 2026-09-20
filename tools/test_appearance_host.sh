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
# The widgets measure text through the font provider, which asks libos64 for
# the production FreeType backend. Nothing this harness checks needs an
# outline engine, so the fake backend answers for it and the builtin 8x16
# face — the one these tests have always drawn with — comes out the same.
cat > "$appearance_test_dir/freetype_stub.c" <<'STUB'
#include "os64/font_backend.h"
#include "fake_backend.h"
const os64_font_backend_t *os64_freetype_backend_v1(void)
{ return os64_fake_font_backend(); }
STUB
cc -D_XOPEN_SOURCE=700 -pthread -std=c11 -fno-builtin-memmove -g -O1 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I "$appearance_test_dir" -I userland/libos64/include -I abi/include -I kernel/include -I tools/fonts \
    tools/test_appearance_host.c tools/test_appearance_customizer_host.c tools/test_controlcenter_host.c tools/test_appearance_session_host.c userland/libos64/ui.c \
    userland/libos64/ui_theme.c userland/libos64/ui_session.c userland/libos64/ui_envelope.c userland/libos64/font_config.c userland/libos64/font_discovery.c userland/libos64/font_install.c userland/libos64/slurp.c userland/apps/appearance/font_page.c userland/libos64/ui_palette.c userland/libos64/ui_color.c userland/libos64/ui_controls.c \
    userland/libos64/ui_list.c userland/libos64/ui_text.c userland/libos64/str.c userland/libos64/draw.c \
    userland/libos64/ui_font_settings.c userland/libos64/ui_font.c userland/libos64/font_provider.c userland/libos64/font_adopt.c \
    userland/libos64/text.c userland/libos64/text_cache.c userland/libos64/text_decode.c \
    userland/libos64/text_bitmap.c userland/libos64/text_draw.c tools/fonts/fake_backend.c \
    kernel/src/gui/event_queue.c kernel/src/appearance.c \
    userland/libos64/conf.c userland/libos64/fmt.c "$appearance_test_dir/freetype_stub.c" \
    -Wl,--gc-sections,--wrap=os64_draw_ctx_refresh,--wrap=os64_conf_find_read,--wrap=os64_conf_find_bytes,--wrap=os64_conf_find,--wrap=os64_conf_target,--wrap=os64_conf_write_checked -o "$appearance_test_dir/test_appearance"
"$appearance_test_dir/test_appearance"
