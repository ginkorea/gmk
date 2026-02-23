##
## GGMK/cpu — Makefile
##

CC      := gcc
AR      := ar
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -pthread -I include -D_GNU_SOURCE
LDFLAGS := -pthread -lm -ldl

BUILD   := build
SRC     := src
TEST    := test
INC     := include/ggmk

# ── Core source files ───────────────────────────────────────
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

# ── Linux HAL sources (hosted build) ────────────────────────
HAL_LINUX := hal/linux
HAL_LINUX_SRCS := $(HAL_LINUX)/thread.c \
                  $(HAL_LINUX)/lock.c \
                  $(HAL_LINUX)/park.c \
                  $(HAL_LINUX)/time.c \
                  $(HAL_LINUX)/mem.c

# ── Hosted build: core + linux HAL ──────────────────────────
HOSTED_SRCS := $(SRCS) $(HAL_LINUX_SRCS)
CORE_OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(SRCS))
HAL_LINUX_OBJS := $(patsubst $(HAL_LINUX)/%.c,$(BUILD)/hal_linux_%.o,$(HAL_LINUX_SRCS))
OBJS := $(CORE_OBJS) $(HAL_LINUX_OBJS)

# ── Library ──────────────────────────────────────────────────
LIB := $(BUILD)/libggmk_cpu.a

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

# ── Kernel (freestanding) ────────────────────────────────────
KERN_CC     := gcc
KERN_CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 \
  -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel \
  -mno-sse -mno-sse2 -mno-mmx \
  -DGMK_FREESTANDING -I include -I arch/x86_64
KERN_LDFLAGS := -nostdlib -static -T arch/x86_64/linker.ld -z max-page-size=0x1000 -Wl,--build-id=none

ARCH     := arch/x86_64
KERN_BUILD := $(BUILD)/kern

# Kernel arch sources
KERN_ARCH_C := $(ARCH)/entry.c \
               $(ARCH)/serial.c \
               $(ARCH)/gdt.c \
               $(ARCH)/idt.c \
               $(ARCH)/pmm.c \
               $(ARCH)/memops.c \
               $(ARCH)/boot_alloc.c \
               $(ARCH)/paging.c \
               $(ARCH)/vmm.c \
               $(ARCH)/pci.c \
               $(ARCH)/lapic.c \
               $(ARCH)/smp.c \
               $(ARCH)/cli.c \
               $(ARCH)/kmain.c

KERN_ARCH_S := $(ARCH)/idt_stubs.S \
               $(ARCH)/ctx_switch.S

# Kernel driver sources
DRIVERS := drivers
KERN_DRIVER_C := $(DRIVERS)/virtio/virtio_pci.c \
                 $(DRIVERS)/virtio/virtio_blk.c

# ── Baremetal HAL sources ───────────────────────────────────
HAL_BM := hal/x86_baremetal
HAL_BM_SRCS := $(HAL_BM)/thread.c \
               $(HAL_BM)/lock.c \
               $(HAL_BM)/park.c \
               $(HAL_BM)/time.c \
               $(HAL_BM)/mem.c

# Kernel objects: core + baremetal HAL + arch + drivers
KERN_SRC_OBJS := $(patsubst $(SRC)/%.c,$(KERN_BUILD)/%.o,$(SRCS))
KERN_HAL_OBJS := $(patsubst $(HAL_BM)/%.c,$(KERN_BUILD)/hal_%.o,$(HAL_BM_SRCS))
KERN_ARCH_C_OBJS := $(patsubst $(ARCH)/%.c,$(KERN_BUILD)/arch_%.o,$(KERN_ARCH_C))
KERN_ARCH_S_OBJS := $(patsubst $(ARCH)/%.S,$(KERN_BUILD)/arch_%.o,$(KERN_ARCH_S))
KERN_DRV_OBJS := $(KERN_BUILD)/drv_virtio_pci.o $(KERN_BUILD)/drv_virtio_blk.o
KERN_ALL_OBJS := $(KERN_SRC_OBJS) $(KERN_HAL_OBJS) $(KERN_ARCH_C_OBJS) $(KERN_ARCH_S_OBJS) $(KERN_DRV_OBJS)

KERNEL_ELF := $(BUILD)/gmk_kernel.elf
KERNEL_ISO := $(BUILD)/ggmk.iso

# ── Phony targets ────────────────────────────────────────────
.PHONY: all lib test clean kernel iso run run-debug \
        test-ring test-alloc test-sched test-chan test-module test-worker test-boot

all: lib

lib: $(LIB)

# ── Build object files (hosted core) ─────────────────────────
$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Build object files (hosted linux HAL) ────────────────────
$(BUILD)/hal_linux_%.o: $(HAL_LINUX)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(KERN_BUILD):
	mkdir -p $(KERN_BUILD)

# ── Static library ──────────────────────────────────────────
$(LIB): $(OBJS)
	$(AR) rcs $@ $^

# ── Test compilation ─────────────────────────────────────────
$(BUILD)/test_%: $(TEST)/test_%.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -I $(TEST) $< -L $(BUILD) -lggmk_cpu $(LDFLAGS) -o $@

# ── Run all tests ────────────────────────────────────────────
test: $(TEST_BINS)
	@echo "=== Running all GGMK/cpu tests ==="
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

# ── Kernel build (freestanding) ──────────────────────────────
$(KERN_BUILD)/%.o: $(SRC)/%.c | $(KERN_BUILD)
	$(KERN_CC) $(KERN_CFLAGS) -c $< -o $@

# ── Kernel build (baremetal HAL) ─────────────────────────────
$(KERN_BUILD)/hal_%.o: $(HAL_BM)/%.c | $(KERN_BUILD)
	$(KERN_CC) $(KERN_CFLAGS) -c $< -o $@

$(KERN_BUILD)/arch_%.o: $(ARCH)/%.c | $(KERN_BUILD)
	$(KERN_CC) $(KERN_CFLAGS) -c $< -o $@

$(KERN_BUILD)/arch_%.o: $(ARCH)/%.S | $(KERN_BUILD)
	$(KERN_CC) $(KERN_CFLAGS) -c $< -o $@

$(KERN_BUILD)/drv_virtio_pci.o: $(DRIVERS)/virtio/virtio_pci.c | $(KERN_BUILD)
	$(KERN_CC) $(KERN_CFLAGS) -c $< -o $@

$(KERN_BUILD)/drv_virtio_blk.o: $(DRIVERS)/virtio/virtio_blk.c | $(KERN_BUILD)
	$(KERN_CC) $(KERN_CFLAGS) -c $< -o $@

kernel: $(KERNEL_ELF)

$(KERNEL_ELF): $(KERN_ALL_OBJS)
	$(KERN_CC) $(KERN_LDFLAGS) -o $@ $^

# ── Limine bootloader location ──────────────────────────────
# Set LIMINE_DIR to wherever limine was built/installed.
LIMINE_DIR ?= /tmp/limine

# ── ISO (requires xorriso + limine) ─────────────────────────
iso: $(KERNEL_ELF)
	@mkdir -p $(BUILD)/iso/boot
	cp $(KERNEL_ELF) $(BUILD)/iso/boot/gmk_kernel.elf
	cp $(ARCH)/limine.conf $(BUILD)/iso/boot/limine.conf
	cp $(LIMINE_DIR)/bin/limine-bios.sys $(BUILD)/iso/boot/
	cp $(LIMINE_DIR)/bin/limine-bios-cd.bin $(BUILD)/iso/boot/
	xorriso -as mkisofs -b boot/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table \
		--protective-msdos-label $(BUILD)/iso -o $(KERNEL_ISO)

# ── Test disk image ─────────────────────────────────────────
DISK_IMG := $(BUILD)/test.img

$(DISK_IMG): | $(BUILD)
	dd if=/dev/zero of=$@ bs=512 count=2048 2>/dev/null

# ── Run in QEMU ─────────────────────────────────────────────
run: iso $(DISK_IMG)
	qemu-system-x86_64 \
		-cdrom $(KERNEL_ISO) \
		-serial stdio \
		-smp 4 \
		-m 256M \
		-no-reboot \
		-no-shutdown \
		-display none \
		-device virtio-blk-pci,drive=d0 \
		-drive id=d0,file=$(DISK_IMG),format=raw,if=none

run-debug: iso $(DISK_IMG)
	qemu-system-x86_64 \
		-cdrom $(KERNEL_ISO) \
		-serial stdio \
		-smp 4 \
		-m 256M \
		-no-reboot \
		-no-shutdown \
		-display none \
		-device virtio-blk-pci,drive=d0 \
		-drive id=d0,file=$(DISK_IMG),format=raw,if=none \
		-S -s

# ── Clean ────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)
