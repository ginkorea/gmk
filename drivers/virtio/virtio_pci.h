/*
 * GMK/cpu — Virtio-PCI legacy transport (I/O BAR)
 */
#ifndef GMK_VIRTIO_PCI_H
#define GMK_VIRTIO_PCI_H

#include "virtio.h"
#include "../../arch/x86_64/pci.h"

/* Initialize a virtio device: read BAR0, reset, set ACKNOWLEDGE + DRIVER.
 * Returns 0 on success, -1 on failure. Sets *io_base to the I/O port base. */
int virtio_pci_init(pci_device_t *pci, uint32_t *io_base);

/* Set up a virtqueue: select queue, read size, allocate vring memory,
 * write queue address. Returns 0 on success, -1 on failure. */
int virtio_pci_setup_queue(uint32_t io_base, uint16_t queue_idx, virtqueue_t *vq);

/* Notify the device that a queue has new available buffers. */
void virtio_pci_notify(virtqueue_t *vq);

/* Read/write device status register. */
uint8_t  virtio_pci_get_status(uint32_t io_base);
void     virtio_pci_set_status(uint32_t io_base, uint8_t status);

/* Read device features (host features). */
uint32_t virtio_pci_get_features(uint32_t io_base);

/* Write guest features. */
void     virtio_pci_set_features(uint32_t io_base, uint32_t features);

/* Read device-specific config byte/word/dword at offset (from config base). */
uint8_t  virtio_pci_cfg_read8(uint32_t io_base, uint32_t offset);
uint16_t virtio_pci_cfg_read16(uint32_t io_base, uint32_t offset);
uint32_t virtio_pci_cfg_read32(uint32_t io_base, uint32_t offset);

/* Read ISR status (acknowledges interrupt). */
uint8_t  virtio_pci_isr_status(uint32_t io_base);

/* Enable bus mastering on the PCI device (required for DMA). */
void virtio_pci_enable_bus_master(pci_device_t *pci);

#endif /* GMK_VIRTIO_PCI_H */
