#ifndef STM32F411_REGS_H
#define STM32F411_REGS_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define RCC_AHB1ENR REG32(0x40023830)
#define RCC_APB2ENR REG32(0x40023844)
#define RCC_APB2RSTR REG32(0x40023824)

#define GPIOA_MODER REG32(0x40020000)
#define GPIOA_AFRL REG32(0x40020020)
#define GPIOB_MODER REG32(0x40020400)
#define GPIOB_BSRR REG32(0x40020418)

#define SPI1_CR1 REG32(0x40013000)
#define SPI1_SR REG32(0x40013008)
#define SPI1_DR (*(volatile uint8_t *)0x4001300C)

#endif
