##
## GMK/cpu — Makefile
##

CC      := gcc
AR      := ar
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -pthread -I include -D_GNU_SOURCE
LDFLAGS := -pthread -lm

BUILD   := build
SRC     := src
TEST    := test
INC     := include/gmk

# ── Source files ─────────────────────────────────────────────
SRCS := $(SRC)/ring_spsc.c \
        $(SRC)/ring_mpmc.c \
        $(SRC)/alloc_arena.c \
        $(SRC)/alloc_slab.c \
        $(SRC)/alloc_block.c \
        $(SRC)/alloc_bump.c \
        $(SRC)/alloc.c \
        $(SRC)/trace.c \
        $(SRC)/metrics.c \
        $(SRC)/sched_rq.c \
        $(SRC)/sched_lq.c \
        $(SRC)/sched_evq.c \
        $(SRC)/sched.c \
        $(SRC)/enqueue.c \
        $(SRC)/chan.c \
        $(SRC)/module.c \
        $(SRC)/worker.c \
        $(SRC)/boot.c

OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(SRCS))

# ── Library ──────────────────────────────────────────────────
LIB := $(BUILD)/libgmk_cpu.a

# ── Test binaries ────────────────────────────────────────────
TEST_BINS := $(BUILD)/test_ring_spsc \
             $(BUILD)/test_ring_mpmc \
             $(BUILD)/test_alloc_slab \
             $(BUILD)/test_alloc_block \
             $(BUILD)/test_alloc_bump \
             $(BUILD)/test_trace \
             $(BUILD)/test_metrics \
             $(BUILD)/test_sched_rq \
             $(BUILD)/test_sched_lq \
             $(BUILD)/test_sched_evq \
             $(BUILD)/test_enqueue \
             $(BUILD)/test_chan \
             $(BUILD)/test_module \
             $(BUILD)/test_worker \
             $(BUILD)/test_boot

# ── Phony targets ────────────────────────────────────────────
.PHONY: all lib test clean \
        test-ring test-alloc test-sched test-chan test-module test-worker test-boot

all: lib

lib: $(LIB)

# ── Build object files ───────────────────────────────────────
$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

# ── Static library ───────────────────────────────────────────
$(LIB): $(OBJS)
	$(AR) rcs $@ $^

# ── Test compilation ─────────────────────────────────────────
$(BUILD)/test_%: $(TEST)/test_%.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -I $(TEST) $< -L $(BUILD) -lgmk_cpu $(LDFLAGS) -o $@

# ── Run all tests ────────────────────────────────────────────
test: $(TEST_BINS)
	@echo "=== Running all GMK/cpu tests ==="
	@for t in $(TEST_BINS); do \
		echo "--- Running $$t ---"; \
		$$t || exit 1; \
	done
	@echo "=== All tests passed ==="

# ── Grouped test targets ────────────────────────────────────
test-ring: $(BUILD)/test_ring_spsc $(BUILD)/test_ring_mpmc
	$(BUILD)/test_ring_spsc
	$(BUILD)/test_ring_mpmc

test-alloc: $(BUILD)/test_alloc_slab $(BUILD)/test_alloc_block $(BUILD)/test_alloc_bump
	$(BUILD)/test_alloc_slab
	$(BUILD)/test_alloc_block
	$(BUILD)/test_alloc_bump

test-sched: $(BUILD)/test_sched_rq $(BUILD)/test_sched_lq $(BUILD)/test_sched_evq $(BUILD)/test_enqueue
	$(BUILD)/test_sched_rq
	$(BUILD)/test_sched_lq
	$(BUILD)/test_sched_evq
	$(BUILD)/test_enqueue

test-chan: $(BUILD)/test_chan
	$(BUILD)/test_chan

test-module: $(BUILD)/test_module
	$(BUILD)/test_module

test-worker: $(BUILD)/test_worker
	$(BUILD)/test_worker

test-boot: $(BUILD)/test_boot
	$(BUILD)/test_boot

# ── Clean ────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
