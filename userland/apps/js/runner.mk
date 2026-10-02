# runner.mk — the js runner's build, while it cannot yet link.
#
# The runner's objects compile through the ordinary $(OBJ)/%.c.o rule with
# userland's full CFLAGS, plus the one include directory that holds
# <os64/js.h>. `make js-runner` builds them so the strict warnings judge the
# runner on every change. Linking waits for R2's runtime; the row in DEBTS.md
# § Userland utilities says what joins it to APPS.
JS_RUNNER_OBJS := $(patsubst %.c,$(OBJ)/%.c.o,$(wildcard apps/js/*.c))
$(JS_RUNNER_OBJS): CFLAGS += -I$(CURDIR)/libjs/include

.PHONY: js-runner
js-runner: $(JS_RUNNER_OBJS)
