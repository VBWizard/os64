# runner.mk — the js runner's build, while the image does not ship libjs.so.
#
# js links by the ordinary APP_RULE (userland/GNUmakefile), against
# $(LIBJS_SO), at its own slot in the app placement map — but into a
# directory the image builders never read, so /bin gains no program whose
# library is missing. `make js-runner` builds it; tools/test_js_cli_*.sh test
# it. The row in DEBTS.md § Userland utilities says what joins it to APPS.
JS_RUNNER_DIR := $(OBJ)/js-runner
JS_RUNNER_BIN := $(JS_RUNNER_DIR)/js
JS_RUNNER_OBJS := $(patsubst %.c,$(OBJ)/%.c.o,$(wildcard apps/js/*.c))
$(JS_RUNNER_OBJS): CFLAGS += -I$(CURDIR)/libjs/include

.PHONY: js-runner
js-runner: $(JS_RUNNER_BIN)
