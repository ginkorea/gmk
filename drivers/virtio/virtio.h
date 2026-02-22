/*
 * GMK/cpu — Virtio constants and virtqueue structures
 *
 * Implements the split virtqueue layout per virtio spec v1.0 (legacy).
 */
#ifndef GMK_VIRTIO_H
#define GMK_VIRTIO_H

#include <stdint.h>

/* ── Virtio device status bits ───────────────────────────────────── */
#define VIRTIO_STATUS_RESET         0
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_FAILED        128

/* ── Virtqueue descriptor flags ──────────────────────────────────── */
#define VRING_DESC_F_NEXT     1   /* buffer continues in next descriptor */
#define VRING_DESC_F_WRITE    2   /* buffer is device-writable (read by device) */

/* ── Virtqueue used ring flags ───────────────────────────────────── */
#define VRING_USED_F_NO_NOTIFY  1

/* ── Vring structures ────────────────────────────────────────────── */

struct vring_desc {
    uint64_t addr;   /* physical address of buffer */
    uint32_t len;    /* length of buffer */
    uint16_t flags;  /* VRING_DESC_F_* */
    uint16_t next;   /* next descriptor index (if F_NEXT) */
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;    /* next slot to fill (wraps) */
    uint16_t ring[]; /* descriptor chain heads */
    /* followed by: uint16_t used_event (if VIRTIO_F_EVENT_IDX) */
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;     /* descriptor chain head that was used */
    uint32_t len;    /* total bytes written by device */
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;              /* next slot device will fill */
    struct vring_used_elem ring[];
    /* followed by: uint16_t avail_event (if VIRTIO_F_EVENT_IDX) */
} __attribute__((packed));

/* ── Virtqueue state ─────────────────────────────────────────────── */

typedef struct {
    uint16_t             num;           /* queue size (power of 2) */
    struct vring_desc   *desc;          /* descriptor table */
    struct vring_avail  *avail;         /* available ring */
    struct vring_used   *used;          /* used ring */
    uint16_t             free_head;     /* head of free descriptor chain */
    uint16_t             num_free;      /* number of free descriptors */
    uint16_t             last_used_idx; /* last seen used index */
    uint32_t             io_base;       /* PCI I/O BAR base */
    uint16_t             queue_index;   /* which virtqueue (0, 1, ...) */
    uint64_t             phys_base;     /* physical address of vring memory */
} virtqueue_t;

/* ── Vring size calculations ─────────────────────────────────────── */

/* Total size of descriptor table */
static inline uint64_t vring_desc_size(uint16_t num) {
    return (uint64_t)num * sizeof(struct vring_desc);
}

/* Total size of available ring */
static inline uint64_t vring_avail_size(uint16_t num) {
    return sizeof(struct vring_avail) + (uint64_t)num * sizeof(uint16_t) + sizeof(uint16_t);
}

/* Total size of used ring */
static inline uint64_t vring_used_size(uint16_t num) {
    return sizeof(struct vring_used) + (uint64_t)num * sizeof(struct vring_used_elem) + sizeof(uint16_t);
}

/* Align up to boundary */
static inline uint64_t vring_align(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

/* Total memory needed for a vring with `num` entries */
static inline uint64_t vring_total_size(uint16_t num) {
    /* desc + avail must be contiguous, then aligned to 4096 for used ring */
    uint64_t a = vring_desc_size(num) + vring_avail_size(num);
    a = vring_align(a, 4096);
    a += vring_used_size(num);
    return a;
}

#endif /* GMK_VIRTIO_H */
