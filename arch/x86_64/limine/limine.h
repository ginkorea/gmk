/* BSD Zero Clause License (0BSD)
 *
 * Limine Boot Protocol Header - v8.x compatible
 * Minimal vendored subset for GMK/cpu kernel.
 */
#ifndef LIMINE_H
#define LIMINE_H

#include <stdint.h>

/* ── Magic numbers ──────────────────────────────────────────────── */
#define LIMINE_COMMON_MAGIC_0 0xc7b1dd30df4c8b88
#define LIMINE_COMMON_MAGIC_1 0x0a82e883a194f07b

/* ── Request/response ID helpers ────────────────────────────────── */

/* ── Bootloader Info ────────────────────────────────────────────── */
#define LIMINE_BOOTLOADER_INFO_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      0xf55038d8e2a1202f, 0x279426fcf5f59740 }

struct limine_bootloader_info_response {
    uint64_t revision;
    char    *name;
    char    *version;
};

struct limine_bootloader_info_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_bootloader_info_response *response;
};

/* ── HHDM (Higher Half Direct Map) ──────────────────────────────── */
#define LIMINE_HHDM_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      0x48dcf1cb8ad2b852, 0x63984e959a98244b }

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response *response;
};

/* ── Memory Map ─────────────────────────────────────────────────── */
#define LIMINE_MEMMAP_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      0x67cf3d9d378a806f, 0xe304acdfc50c3c62 }

#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry **entries;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response *response;
};

/* ── Entry Point ────────────────────────────────────────────────── */
#define LIMINE_ENTRY_POINT_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      0x13d86c035a1cd3e1, 0x2b0571a0c82847ce }

typedef void (*limine_entry_point)(void);

struct limine_entry_point_response {
    uint64_t revision;
};

struct limine_entry_point_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_entry_point_response *response;
    limine_entry_point entry;
};

/* ── SMP ────────────────────────────────────────────────────────── */
#define LIMINE_SMP_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      0x95a67b819a1b857e, 0xa0b61b723b6a73e0 }

#define LIMINE_SMP_X2APIC (1 << 0)

struct limine_smp_info;
typedef void (*limine_goto_address)(struct limine_smp_info *);

struct limine_smp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    uint64_t reserved;
    limine_goto_address goto_address;
    uint64_t extra_argument;
};

struct limine_smp_response {
    uint64_t revision;
    uint32_t flags;
    uint32_t bsp_lapic_id;
    uint64_t cpu_count;
    struct limine_smp_info **cpus;
};

struct limine_smp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_smp_response *response;
    uint64_t flags;
};

/* ── Kernel Address ─────────────────────────────────────────────── */
#define LIMINE_KERNEL_ADDRESS_REQUEST \
    { LIMINE_COMMON_MAGIC_0, LIMINE_COMMON_MAGIC_1, \
      0x71ba76863cc55f63, 0xb2644a48c516a487 }

struct limine_kernel_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct limine_kernel_address_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_kernel_address_response *response;
};

/* ── Base Revision ──────────────────────────────────────────────── */
#define LIMINE_BASE_REVISION(N) \
    __attribute__((used, section(".limine_reqs"))) \
    static volatile uint64_t limine_base_revision[3] = { \
        0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, (N) };

#define LIMINE_BASE_REVISION_SUPPORTED \
    (limine_base_revision[2] == 0)

/* ── Request macro helper ───────────────────────────────────────── */
#define LIMINE_REQ __attribute__((used, section(".limine_reqs")))

#endif /* LIMINE_H */
