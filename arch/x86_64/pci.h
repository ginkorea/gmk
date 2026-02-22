/*
 * GMK/cpu — PCI bus enumeration via legacy I/O ports (0xCF8/0xCFC)
 */
#ifndef GMK_PCI_H
#define GMK_PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 32

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  header_type;
    uint32_t bar[6];
    uint8_t  irq_line;
} pci_device_t;

/* Scan bus 0 and populate the device table. */
void pci_init(void);

/* Read/write PCI configuration space (32-bit, offset must be 4-byte aligned). */
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint8_t  pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

/* Find a device by vendor:device ID. Returns NULL if not found. */
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Get the number of discovered devices. */
uint32_t pci_device_count(void);

/* Get device by index. */
pci_device_t *pci_get_device(uint32_t index);

#endif /* GMK_PCI_H */
