/*
 * GMK/cpu — PCI bus enumeration via legacy I/O configuration ports
 *
 * Port 0xCF8: CONFIG_ADDRESS (bus/dev/func/offset selector)
 * Port 0xCFC: CONFIG_DATA (read/write the selected register)
 */
#include "pci.h"
#include "serial.h"

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static pci_device_t devices[PCI_MAX_DEVICES];
static uint32_t n_devices;

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return (1U << 31)                  /* enable bit */
         | ((uint32_t)bus  << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | ((uint32_t)(offset & 0xFC));
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_cfg_read32(bus, dev, func, offset & 0xFC);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_cfg_read32(bus, dev, func, offset & 0xFC);
    return (uint8_t)(val >> ((offset & 3) * 8));
}

void pci_init(void) {
    n_devices = 0;

    for (uint8_t dev = 0; dev < 32 && n_devices < PCI_MAX_DEVICES; dev++) {
        uint32_t id = pci_cfg_read32(0, dev, 0, 0x00);
        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        uint16_t device = (uint16_t)(id >> 16);

        if (vendor == 0xFFFF) continue;

        pci_device_t *d = &devices[n_devices];
        d->bus       = 0;
        d->dev       = dev;
        d->func      = 0;
        d->vendor_id = vendor;
        d->device_id = device;

        uint32_t class_rev = pci_cfg_read32(0, dev, 0, 0x08);
        d->class_code = (uint8_t)(class_rev >> 24);
        d->subclass   = (uint8_t)(class_rev >> 16);
        d->prog_if    = (uint8_t)(class_rev >> 8);

        d->header_type = pci_cfg_read8(0, dev, 0, 0x0E) & 0x7F;

        /* Read BARs (type 0 headers only) */
        if (d->header_type == 0) {
            for (int i = 0; i < 6; i++) {
                d->bar[i] = pci_cfg_read32(0, dev, 0, 0x10 + i * 4);
            }
        }

        d->irq_line = pci_cfg_read8(0, dev, 0, 0x3C);

        kprintf("PCI %u:%u.%u %x:%x class %x:%x",
                d->bus, d->dev, d->func,
                d->vendor_id, d->device_id,
                d->class_code, d->subclass);
        if (d->header_type == 0 && d->bar[0]) {
            kprintf(" BAR0=0x%x", d->bar[0]);
        }
        kprintf("\n");

        n_devices++;

        /* Check multi-function device (bit 7 of header type) */
        uint8_t ht_raw = pci_cfg_read8(0, dev, 0, 0x0E);
        if (ht_raw & 0x80) {
            for (uint8_t func = 1; func < 8 && n_devices < PCI_MAX_DEVICES; func++) {
                id = pci_cfg_read32(0, dev, func, 0x00);
                vendor = (uint16_t)(id & 0xFFFF);
                device = (uint16_t)(id >> 16);
                if (vendor == 0xFFFF) continue;

                pci_device_t *df = &devices[n_devices];
                df->bus       = 0;
                df->dev       = dev;
                df->func      = func;
                df->vendor_id = vendor;
                df->device_id = device;

                class_rev = pci_cfg_read32(0, dev, func, 0x08);
                df->class_code = (uint8_t)(class_rev >> 24);
                df->subclass   = (uint8_t)(class_rev >> 16);
                df->prog_if    = (uint8_t)(class_rev >> 8);
                df->header_type = pci_cfg_read8(0, dev, func, 0x0E) & 0x7F;

                if (df->header_type == 0) {
                    for (int i = 0; i < 6; i++) {
                        df->bar[i] = pci_cfg_read32(0, dev, func, 0x10 + i * 4);
                    }
                }
                df->irq_line = pci_cfg_read8(0, dev, func, 0x3C);

                kprintf("PCI %u:%u.%u %x:%x class %x:%x\n",
                        df->bus, df->dev, df->func,
                        df->vendor_id, df->device_id,
                        df->class_code, df->subclass);

                n_devices++;
            }
        }
    }

    kprintf("PCI: %u devices found\n", n_devices);
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (uint32_t i = 0; i < n_devices; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id)
            return &devices[i];
    }
    return 0;
}

uint32_t pci_device_count(void) {
    return n_devices;
}

pci_device_t *pci_get_device(uint32_t index) {
    if (index >= n_devices) return 0;
    return &devices[index];
}
