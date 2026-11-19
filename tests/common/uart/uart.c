#include "uart.h"

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define RCC_AHB1ENR REG32(0x40023830)
#define RCC_APB1ENR REG32(0x40023840)
#define GPIOA_MODER REG32(0x40020000)
#define GPIOA_AFRL REG32(0x40020020)
#define USART2_SR REG32(0x40004400)
#define USART2_DR REG32(0x40004404)
#define USART2_BRR REG32(0x40004408)
#define USART2_CR1 REG32(0x4000440C)

void uart_init(void)
{
    RCC_AHB1ENR |= 1u << 0;
    RCC_APB1ENR |= 1u << 17;
    (void)RCC_AHB1ENR;
    (void)RCC_APB1ENR;

    GPIOA_MODER = (GPIOA_MODER & ~(3u << 4)) | (2u << 4);
    GPIOA_AFRL = (GPIOA_AFRL & ~(15u << 8)) | (7u << 8);
    USART2_BRR = 139;
    USART2_CR1 = (1u << 13) | (1u << 3);
}

void uart_puts(const char *text)
{
    while (*text)
    {
        unsigned int timeout = 1000000;
        while (!(USART2_SR & (1u << 7)))
        {
            if (--timeout == 0)
                return;
        }
        USART2_DR = (uint8_t)*text++;
    }
}

void uart_hex(uint8_t value)
{
    const char hex[] = "0123456789ABCDEF";
    char text[] = {hex[value >> 4], hex[value & 15], '\0'};
    uart_puts(text);
}
