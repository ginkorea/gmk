/*
 * GMK/cpu — Virtio-PCI legacy transport
 *
 * Legacy virtio uses I/O BAR (BAR0) for device registers.
 * Register layout (offsets from BAR0):
 *   0x00  Device Features    (4 bytes, R)
 *   0x04  Guest Features     (4 bytes, W)
 *   0x08  Queue Address      (4 bytes, W) — PFN of vring (addr >> 12)
 *   0x0C  Queue Size         (2 bytes, R)
 *   0x0E  Queue Select       (2 bytes, W)
 *   0x10  Queue Notify       (2 bytes, W)
 *   0x12  Device Status      (1 byte, RW)
 *   0x13  ISR Status         (1 byte, R)
 *   0x14+ Device-specific config
 */
#include "virtio_pci.h"
#include "../../arch/x86_64/serial.h"
#include "../../arch/x86_64/pmm.h"
#include "../../arch/x86_64/mem.h"

/* Legacy virtio I/O register offsets */
#define VIRTIO_PCI_HOST_FEATURES   0x00
#define VIRTIO_PCI_GUEST_FEATURES  0x04
#define VIRTIO_PCI_QUEUE_PFN       0x08
#define VIRTIO_PCI_QUEUE_SIZE      0x0C
#define VIRTIO_PCI_QUEUE_SEL       0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_STATUS          0x12
#define VIRTIO_PCI_ISR             0x13
#define VIRTIO_PCI_CONFIG          0x14

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void virtio_pci_enable_bus_master(pci_device_t *pci) {
    uint16_t cmd = pci_cfg_read16(pci->bus, pci->dev, pci->func, 0x04);
    cmd |= (1 << 2); /* Bus Master Enable */
    pci_cfg_write32(pci->bus, pci->dev, pci->func, 0x04,
                    (uint32_t)cmd | ((uint32_t)pci_cfg_read16(pci->bus, pci->dev, pci->func, 0x06) << 16));
}

int virtio_pci_init(pci_device_t *pci, uint32_t *io_base) {
    if (!pci || !io_base) return -1;

    /* BAR0 bit 0 = 1 means I/O space */
    if (!(pci->bar[0] & 1)) {
        kprintf("virtio: BAR0 is not I/O space\n");
        return -1;
    }

    *io_base = pci->bar[0] & ~0x3U;

    /* Enable bus mastering for DMA */
    virtio_pci_enable_bus_master(pci);

    /* Reset device */
    outb((uint16_t)(*io_base + VIRTIO_PCI_STATUS), VIRTIO_STATUS_RESET);

    /* Acknowledge: OS has noticed the device */
    outb((uint16_t)(*io_base + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);

    /* Driver: OS knows how to drive the device */
    uint8_t status = inb((uint16_t)(*io_base + VIRTIO_PCI_STATUS));
    outb((uint16_t)(*io_base + VIRTIO_PCI_STATUS), status | VIRTIO_STATUS_DRIVER);

    return 0;
}

int virtio_pci_setup_queue(uint32_t io_base, uint16_t queue_idx, virtqueue_t *vq) {
    if (!vq) return -1;

    /* Select queue */
    outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_SEL), queue_idx);

    /* Read queue size (device-reported, power of 2, 0 = queue doesn't exist) */
    uint16_t num = inw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_SIZE));
    if (num == 0) {
        kprintf("virtio: queue %u does not exist\n", queue_idx);
        return -1;
    }

    /* Calculate total memory needed */
    uint64_t total = vring_total_size(num);
    uint64_t pages = (total + 4095) / 4096;

    /* Allocate physically contiguous, page-aligned memory */
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) {
        kprintf("virtio: cannot allocate %lu pages for queue %u\n",
                (unsigned long)pages, queue_idx);
        return -1;
    }

    /* Zero the memory */
    uint8_t *virt = (uint8_t *)phys_to_virt(phys);
    for (uint64_t i = 0; i < pages * 4096; i++)
        virt[i] = 0;

    /* Set up vring pointers */
    vq->num        = num;
    vq->phys_base  = phys;
    vq->io_base    = io_base;
    vq->queue_index = queue_idx;
    vq->desc       = (struct vring_desc *)virt;
    vq->avail      = (struct vring_avail *)(virt + vring_desc_size(num));

    uint64_t used_offset = vring_align(vring_desc_size(num) + vring_avail_size(num), 4096);
    vq->used = (struct vring_used *)(virt + used_offset);

    vq->last_used_idx = 0;
    vq->num_free      = num;
    vq->free_head      = 0;

    /* Initialize free descriptor chain */
    for (uint16_t i = 0; i < num - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->desc[num - 1].next = 0xFFFF; /* end of chain */

    /* Tell device the physical page frame number of the vring */
    outl((uint16_t)(io_base + VIRTIO_PCI_QUEUE_PFN), (uint32_t)(phys >> 12));

    kprintf("virtio: queue %u size=%u phys=0x%lx\n",
            queue_idx, num, (unsigned long)phys);

    return 0;
}

void virtio_pci_notify(virtqueue_t *vq) {
    /* Compiler barrier: ensure descriptor/avail ring writes are visible */
    __asm__ volatile("" ::: "memory");
    outw((uint16_t)(vq->io_base + VIRTIO_PCI_QUEUE_NOTIFY), vq->queue_index);
}

uint8_t virtio_pci_get_status(uint32_t io_base) {
    return inb((uint16_t)(io_base + VIRTIO_PCI_STATUS));
}

void virtio_pci_set_status(uint32_t io_base, uint8_t status) {
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS), status);
}

uint32_t virtio_pci_get_features(uint32_t io_base) {
    return inl((uint16_t)(io_base + VIRTIO_PCI_HOST_FEATURES));
}

void virtio_pci_set_features(uint32_t io_base, uint32_t features) {
    outl((uint16_t)(io_base + VIRTIO_PCI_GUEST_FEATURES), features);
}

uint8_t virtio_pci_cfg_read8(uint32_t io_base, uint32_t offset) {
    return inb((uint16_t)(io_base + VIRTIO_PCI_CONFIG + offset));
}

uint16_t virtio_pci_cfg_read16(uint32_t io_base, uint32_t offset) {
    return inw((uint16_t)(io_base + VIRTIO_PCI_CONFIG + offset));
}

uint32_t virtio_pci_cfg_read32(uint32_t io_base, uint32_t offset) {
    return inl((uint16_t)(io_base + VIRTIO_PCI_CONFIG + offset));
}

uint8_t virtio_pci_isr_status(uint32_t io_base) {
    return inb((uint16_t)(io_base + VIRTIO_PCI_ISR));
}
