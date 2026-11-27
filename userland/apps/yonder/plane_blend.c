// The compositor's blend of a plane's content over its picture, compiled
// into yonder from the kernel's own source so `--plane-check` composes a
// hole exactly as the compositor will (VIDEO_PLANE.md § 5.6). The include
// path for its header is this file's own rule in userland/GNUmakefile.
#include "../../../kernel/src/gui/plane_compose.c"
