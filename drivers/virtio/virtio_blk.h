/*
 * GMK/cpu — Virtio-blk driver
 */
#ifndef GMK_VIRTIO_BLK_H
#define GMK_VIRTIO_BLK_H

#include <stdint.h>
#include "../../arch/x86_64/pci.h"

/* Virtio-blk request types */
#define VIRTIO_BLK_T_IN   0  /* read */
#define VIRTIO_BLK_T_OUT  1  /* write */

/* Virtio-blk status values (returned in status byte) */
#define VIRTIO_BLK_S_OK          0
#define VIRTIO_BLK_S_IOERR       1
#define VIRTIO_BLK_S_UNSUPP      2

/* Virtio-blk request header */
struct virtio_blk_req {
    uint32_t type;      /* VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT */
    uint32_t reserved;
    uint64_t sector;    /* starting sector (512 bytes each) */
} __attribute__((packed));

/* Initialize the virtio-blk device. Returns 0 on success. */
int virtio_blk_init(pci_device_t *pci);

/* Read a single 512-byte sector. Synchronous (polls for completion).
 * Returns 0 on success. */
int virtio_blk_read(uint64_t sector, void *buf);

/* Write a single 512-byte sector. Synchronous (polls for completion).
 * Returns 0 on success. */
int virtio_blk_write(uint64_t sector, const void *buf);

/* Get device capacity in 512-byte sectors. */
uint64_t virtio_blk_capacity(void);

#endif /* GMK_VIRTIO_BLK_H */
