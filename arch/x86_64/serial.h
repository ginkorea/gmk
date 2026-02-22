/*
 * GMK/cpu — COM1 serial I/O
 */
#ifndef GMK_SERIAL_H
#define GMK_SERIAL_H

#include <stdint.h>
#include <stdarg.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void kprintf(const char *fmt, ...);

#endif /* GMK_SERIAL_H */
