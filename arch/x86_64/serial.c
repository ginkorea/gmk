/*
 * GMK/cpu — COM1 serial output (115200 8N1)
 */
#include "serial.h"

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);  /* Disable interrupts */
    outb(COM1 + 3, 0x80);  /* Enable DLAB */
    outb(COM1 + 0, 0x01);  /* Divisor lo: 115200 baud */
    outb(COM1 + 1, 0x00);  /* Divisor hi */
    outb(COM1 + 3, 0x03);  /* 8N1 */
    outb(COM1 + 2, 0xC7);  /* Enable FIFO, 14-byte threshold */
    outb(COM1 + 4, 0x0B);  /* IRQs enabled, RTS/DSR set */
}

void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

/* ── Minimal kprintf (%s %d %u %x %p %lu %lx %ld) ─────────────── */

static void print_uint64(uint64_t val) {
    char buf[21];
    int i = 0;
    if (val == 0) { serial_putc('0'); return; }
    while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    while (--i >= 0) serial_putc(buf[i]);
}

static void print_int64(int64_t val) {
    if (val < 0) { serial_putc('-'); val = -val; }
    print_uint64((uint64_t)val);
}

static void print_hex64(uint64_t val) {
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    int i = 0;
    if (val == 0) { serial_putc('0'); return; }
    while (val > 0) { buf[i++] = hex[val & 0xF]; val >>= 4; }
    while (--i >= 0) serial_putc(buf[i]);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') serial_putc('\r');
            serial_putc(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Check for 'l' modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            serial_puts(s ? s : "(null)");
            break;
        }
        case 'd': {
            if (is_long) print_int64(va_arg(ap, int64_t));
            else         print_int64(va_arg(ap, int));
            break;
        }
        case 'u': {
            if (is_long) print_uint64(va_arg(ap, uint64_t));
            else         print_uint64(va_arg(ap, unsigned int));
            break;
        }
        case 'x': {
            if (is_long) print_hex64(va_arg(ap, uint64_t));
            else         print_hex64(va_arg(ap, unsigned int));
            break;
        }
        case 'p': {
            serial_puts("0x");
            print_hex64((uint64_t)va_arg(ap, void *));
            break;
        }
        case '%':
            serial_putc('%');
            break;
        default:
            serial_putc('%');
            if (is_long) serial_putc('l');
            serial_putc(*fmt);
            break;
        }
        fmt++;
    }

    va_end(ap);
}
