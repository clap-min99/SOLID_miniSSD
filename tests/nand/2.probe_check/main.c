#include "nand.h"
#include "uart.h"

static void print_probe_result(nand_result_t result)
{
    switch (result)
    {
    case NAND_OK:
        uart_puts("PASS\r\n");
        break;
    case NAND_ERR_NO_DEVICE:
        uart_puts("FAIL (no supported JEDEC ID)\r\n");
        break;
    case NAND_ERR_TIMEOUT:
        uart_puts("FAIL (SPI timeout)\r\n");
        break;
    default:
        uart_puts("FAIL (error 0x");
        uart_hex((uint8_t)result);
        uart_puts(")\r\n");
        break;
    }
}

static void pause_between_probes(void)
{
    for (volatile unsigned int delay = 0; delay < 1000000; ++delay)
    {
    }
}

int main(void)
{
    uart_init();

    nand_result_t result = nand_init();
    if (result != NAND_OK)
    {
        uart_puts("NAND INIT: ");
        print_probe_result(result);
        for (;;)
        {
        }
    }

    for (;;)
    {
        pause_between_probes();
        result = nand_probe(0);
        uart_puts("NAND PROBE CHIP 0: ");
        print_probe_result(result);
    }
}
