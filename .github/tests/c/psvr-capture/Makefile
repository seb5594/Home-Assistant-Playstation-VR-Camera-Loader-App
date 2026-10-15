# Unit tests and simulations for psvr-capture.
#
#   make test                      build the engine objects and run every test_*.c
#   make test CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"
#   python3 end_to_end.py ../../../../sony_playstation_vr_camera_loader/src/c/psvr-capture/psvr-capture
#
# Objects go to ./build, the add-on source folder stays clean.
ENGINE ?= ../../../../sony_playstation_vr_camera_loader/src/c/psvr-capture
BUILD ?= build
CC ?= cc
CFLAGS ?= -O2
TEST_CFLAGS = $(CFLAGS) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I$(ENGINE)
LDLIBS = -lrnnoise -lturbojpeg -lm

ENGINE_SOURCES = $(filter-out $(ENGINE)/main.c,$(wildcard $(ENGINE)/*.c))
ENGINE_OBJECTS = $(patsubst $(ENGINE)/%.c,$(BUILD)/engine/%.o,$(ENGINE_SOURCES))
TESTS = $(patsubst %.c,$(BUILD)/%,$(wildcard test_*.c))

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

$(BUILD)/engine/%.o: $(ENGINE)/%.c $(wildcard $(ENGINE)/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(BUILD)/test_%: test_%.c test_util.h $(ENGINE_OBJECTS)
	$(CC) $(TEST_CFLAGS) $(LDFLAGS) -o $@ $< $(ENGINE_OBJECTS) $(LDLIBS)

$(BUILD)/bench_pipeline: bench_pipeline.c $(ENGINE_OBJECTS)
	$(CC) $(TEST_CFLAGS) $(LDFLAGS) -o $@ $< $(ENGINE_OBJECTS) $(LDLIBS)

bench: $(BUILD)/bench_pipeline
	./$(BUILD)/bench_pipeline

clean:
	rm -rf $(BUILD)

.PHONY: test bench clean
