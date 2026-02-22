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

/* Disable interrupts, print crash header, halt. Never returns. */
__attribute__((noreturn, format(printf, 3, 4)))
void panic(const char *file, int line, const char *fmt, ...);

#define PANIC(fmt, ...) panic(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* GMK_SERIAL_H */
