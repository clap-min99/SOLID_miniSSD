#ifndef TESTS_COMMON_UART_H
#define TESTS_COMMON_UART_H

#include <stdint.h>

/* USART2 TX on PA2 (AF7), 115200 baud, 8N1; requires APB1 = 16 MHz. */
void uart_init(void);
void uart_puts(const char *text);
void uart_hex(uint8_t value);

#endif
