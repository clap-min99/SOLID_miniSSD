#include "nand.h"
#include "uart.h"

static void print_number(unsigned int value)
{
    char digits[11];
    char *first = digits + sizeof(digits) - 1;
    *first = '\0';
    do
    {
        *--first = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    uart_puts(first);
}

static void print_error(nand_result_t result)
{
    uart_puts(" error=0x");
    uart_hex((uint8_t)result);
    uart_puts("\r\n");
}

int main(void)
{
    const uint8_t chip = 0;
    uart_init();
    uart_puts("FACTORY BAD BLOCK SCAN chip=");
    print_number(chip);
    uart_puts("\r\n");

    nand_result_t result = nand_init();
    if (result != NAND_OK)
    {
        uart_puts("FAIL INIT");
        print_error(result);
        return 1;
    }

    result = nand_probe(chip);
    if (result != NAND_OK)
    {
        uart_puts("FAIL PROBE");
        print_error(result);
        return 1;
    }

    unsigned int good = 0;
    unsigned int bad = 0;
    unsigned int errors = 0;
    for (uint16_t block = 0; block < NAND_BLOCKS_PER_CHIP; ++block)
    {
        bool is_bad;
        result = nand_is_factory_bad_block(chip, block, &is_bad);
        if (result != NAND_OK)
        {
            ++errors;
            uart_puts("ERROR block=");
            print_number(block);
            print_error(result);
        }
        else if (is_bad)
        {
            ++bad;
            uart_puts("BAD block=");
            print_number(block);
            uart_puts("\r\n");
        }
        else
        {
            ++good;
        }
    }

    uart_puts(errors == 0 ? "PASS" : "FAIL");
    uart_puts(" total=");
    print_number(good + bad + errors);
    uart_puts(" good=");
    print_number(good);
    uart_puts(" bad=");
    print_number(bad);
    uart_puts(" errors=");
    print_number(errors);
    uart_puts("\r\n");
    return errors == 0 ? 0 : 1;
}
