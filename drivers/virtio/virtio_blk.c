/*
 * GMK/cpu — Virtio-blk driver (legacy, synchronous, single-sector)
 *
 * Each I/O operation is a 3-descriptor chain:
 *   [0] Header (virtio_blk_req): type + sector — device-readable
 *   [1] Data (512 bytes): device-writable for read, device-readable for write
 *   [2] Status (1 byte): device-writable — 0=OK, 1=err, 2=unsupported
 */
#include "virtio_blk.h"
#include "virtio_pci.h"
#include "../../arch/x86_64/serial.h"
#include "../../arch/x86_64/pmm.h"
#include "../../arch/x86_64/mem.h"

static virtqueue_t requestq;
static uint32_t    blk_io_base;
static uint64_t    blk_capacity; /* in 512-byte sectors */

/* DMA buffers: must be at known physical addresses for device DMA.
 * Allocated from PMM at init time, accessed via HHDM. */
static struct virtio_blk_req *req_header;
static uint8_t               *req_data;
static uint8_t               *req_status;
static uint64_t               req_header_phys;
static uint64_t               req_data_phys;
static uint64_t               req_status_phys;

int virtio_blk_init(pci_device_t *pci) {
    /* Allocate a physical page for DMA buffers (header + data + status all fit in one page) */
    uint64_t dma_phys = pmm_alloc_pages(1);
    if (!dma_phys) {
        kprintf("virtio-blk: cannot allocate DMA page\n");
        return -1;
    }
    uint8_t *dma_virt = (uint8_t *)phys_to_virt(dma_phys);
    /* Zero the page */
    for (int i = 0; i < 4096; i++) dma_virt[i] = 0;

    /* Layout within the DMA page: header at +0, data at +512, status at +1024 */
    req_header      = (struct virtio_blk_req *)dma_virt;
    req_header_phys = dma_phys;
    req_data        = dma_virt + 512;
    req_data_phys   = dma_phys + 512;
    req_status      = dma_virt + 1024;
    req_status_phys = dma_phys + 1024;

    int rc = virtio_pci_init(pci, &blk_io_base);
    if (rc < 0) return rc;

    /* Read and negotiate features — we accept none for now (bare minimum) */
    uint32_t features = virtio_pci_get_features(blk_io_base);
    kprintf("virtio-blk: host features 0x%x\n", features);
    virtio_pci_set_features(blk_io_base, 0); /* accept no optional features */

    /* Set up requestq (queue 0) */
    rc = virtio_pci_setup_queue(blk_io_base, 0, &requestq);
    if (rc < 0) return rc;

    /* Set DRIVER_OK — device is live */
    uint8_t status = virtio_pci_get_status(blk_io_base);
    virtio_pci_set_status(blk_io_base, status | VIRTIO_STATUS_DRIVER_OK);

    /* Read capacity from device config.
     * Legacy virtio-blk config starts at offset 0x14 from BAR0.
     * Capacity is a 64-bit LE at config offset 0x00. */
    uint32_t cap_lo = virtio_pci_cfg_read32(blk_io_base, 0);
    uint32_t cap_hi = virtio_pci_cfg_read32(blk_io_base, 4);
    blk_capacity = (uint64_t)cap_hi << 32 | cap_lo;

    kprintf("virtio-blk: capacity %lu sectors (%lu KB)\n",
            (unsigned long)blk_capacity, (unsigned long)(blk_capacity / 2));

    return 0;
}

/* Submit a 3-descriptor chain and poll for completion */
static int virtio_blk_do_io(uint32_t type, uint64_t sector, void *buf) {
    if (requestq.num_free < 3) {
        kprintf("virtio-blk: no free descriptors\n");
        return -1;
    }

    /* Prepare header */
    req_header->type     = type;
    req_header->reserved = 0;
    req_header->sector   = sector;

    /* Copy write data into our DMA buffer */
    if (type == VIRTIO_BLK_T_OUT && buf) {
        uint8_t *src = (uint8_t *)buf;
        for (int i = 0; i < 512; i++)
            req_data[i] = src[i];
    }

    /* Reset status */
    *req_status = 0xFF;

    /* Allocate 3 descriptors from free chain */
    uint16_t head = requestq.free_head;
    uint16_t d0 = head;
    uint16_t d1 = requestq.desc[d0].next;
    uint16_t d2 = requestq.desc[d1].next;
    requestq.free_head = requestq.desc[d2].next;
    requestq.num_free -= 3;

    /* Descriptor 0: header (device-readable) */
    requestq.desc[d0].addr  = req_header_phys;
    requestq.desc[d0].len   = sizeof(struct virtio_blk_req);
    requestq.desc[d0].flags = VRING_DESC_F_NEXT;
    requestq.desc[d0].next  = d1;

    /* Descriptor 1: data buffer */
    requestq.desc[d1].addr  = req_data_phys;
    requestq.desc[d1].len   = 512;
    requestq.desc[d1].flags = VRING_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN) {
        requestq.desc[d1].flags |= VRING_DESC_F_WRITE; /* device writes data */
    }
    requestq.desc[d1].next = d2;

    /* Descriptor 2: status (device-writable) */
    requestq.desc[d2].addr  = req_status_phys;
    requestq.desc[d2].len   = 1;
    requestq.desc[d2].flags = VRING_DESC_F_WRITE;
    requestq.desc[d2].next  = 0;

    /* Add to available ring */
    uint16_t avail_idx = requestq.avail->idx;
    requestq.avail->ring[avail_idx % requestq.num] = head;

    /* Memory barrier before updating avail idx */
    __asm__ volatile("" ::: "memory");
    requestq.avail->idx = avail_idx + 1;

    /* Notify device */
    virtio_pci_notify(&requestq);

    /* Poll for completion (bounded timeout) */
    for (uint32_t timeout = 0; timeout < 10000000; timeout++) {
        /* Memory barrier before reading used idx */
        __asm__ volatile("" ::: "memory");
        if (requestq.used->idx != requestq.last_used_idx)
            break;
        __builtin_ia32_pause();
    }

    if (requestq.used->idx == requestq.last_used_idx) {
        kprintf("virtio-blk: timeout waiting for completion\n");
        return -1;
    }

    requestq.last_used_idx++;

    /* Return descriptors to free chain */
    requestq.desc[d2].next = requestq.free_head;
    requestq.desc[d1].next = d2;
    requestq.desc[d0].next = d1;
    requestq.free_head = d0;
    requestq.num_free += 3;

    /* Check status */
    if (*req_status != VIRTIO_BLK_S_OK) {
        kprintf("virtio-blk: I/O error status=%u\n", *req_status);
        return -1;
    }

    /* Copy read data out */
    if (type == VIRTIO_BLK_T_IN && buf) {
        uint8_t *dst = (uint8_t *)buf;
        for (int i = 0; i < 512; i++)
            dst[i] = req_data[i];
    }

    return 0;
}

int virtio_blk_read(uint64_t sector, void *buf) {
    return virtio_blk_do_io(VIRTIO_BLK_T_IN, sector, buf);
}

int virtio_blk_write(uint64_t sector, const void *buf) {
    return virtio_blk_do_io(VIRTIO_BLK_T_OUT, sector, (void *)buf);
}

uint64_t virtio_blk_capacity(void) {
    return blk_capacity;
}
