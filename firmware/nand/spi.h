#ifndef SPI_H
#define SPI_H

#include <stdint.h>

/* SPI1, PA5/PA6/PA7 AF5, Mode 0, 8-bit, MSB first, APB2 / 256.
 * Call before starting a transfer; CS is controlled by the caller. */
void spi_init(void);
/* Exchange one byte: returns 0..255, or -1 on timeout. */
int spi_transfer(uint8_t tx);

#endif
