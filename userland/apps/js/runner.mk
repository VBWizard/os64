# runner.mk — what the js runner needs beyond an ordinary app's build.
#
# js is an ordinary app (apps/js/ → $(BIN)/js, APP_RULE, its own placement
# slot) that also links $(LIBJS_SO) and includes <os64/js.h>. Building it
# builds the library, so `make -C userland` leaves both in $(BIN), where
# os64serve.py serves them: `os64get js libjs.so` installs the pair. The
# image is another matter, because the root GNUmakefile holds js back from
# its app list until I1 puts libjs.so on the image too (DEBTS.md § Userland
# utilities). `make js-runner` builds just the runner and its library.
JS_RUNNER_OBJS := $(patsubst %.c,$(OBJ)/%.c.o,$(wildcard apps/js/*.c))
$(JS_RUNNER_OBJS): CFLAGS += -I$(CURDIR)/libjs/include

.PHONY: js-runner
js-runner: $(BIN)/js
