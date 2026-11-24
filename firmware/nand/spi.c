#include "spi.h"
#include "stm32f411_regs.h"

static int wait_flag(uint32_t mask, int set)
{
    unsigned int timeout = 1000000;
    while (((SPI1_SR & mask) != 0) != set)
    {
        if (--timeout == 0)
            return 0;
    }
    return 1;
}

void spi_init(void)
{
    // 1. GPIO, SPI 클록 활성화
    RCC_AHB1ENR |= 1u << 0;
    RCC_APB2ENR |= 1u << 12;
    (void)RCC_AHB1ENR;
    (void)RCC_APB2ENR;

    // 2. PA5~PA7을 주변장치 기능(AF) 모드로 설정
    // GPIO port mode register
    GPIOA_MODER = (GPIOA_MODER & ~(0x3Fu << 10)) | (0x2Au << 10); // PA5~PA7 10: Alternate Function

    // 3. PA5~PA7의 주변장치 기능을 AF5로 선택
    // GPIO alternate function low regsiter
    GPIOA_AFRL = (GPIOA_AFRL & ~(0xFFFu << 20)) | (0x555u << 20); // PA5~PA7 AF5 (SCK, MISO, MOSI)

    // 4. SPI1 리셋 후 해제하여 초기 상태 확보
    // RCC APB2 peripheral reset register
    RCC_APB2RSTR |= 1u << 12;    // SPI1 RST 1: *keep* SPI1 reset
    RCC_APB2RSTR &= ~(1u << 12); // SPI1 RST 0: does not reset SPI1

    // 5. SPI1 통신 방식 설정
    // SPI control register 1
    // SSM(Software slave management): When the SSM bit is set, the NSS pin input is replaced with the value from the SSI bit
    // SSI(Internal slave select): Only when the SSM bit is set. The value of this bit is forced onto the NSS pin and the IO value of the NSS pin is ignored
    // BR(Baud rate control): When 111, f/256
    // MSTR(Master selection): When 1, Master configuration
    SPI1_CR1 = (1u << 9) | (1u << 8) | (7u << 3) | (1u << 2); // SSM: 1, SSI: 1, BR[2:0]: 111, MSTR: 1

    // 6. SPI1 활성화
    // SPE(SPI enable): When 1, peripheral enabled
    SPI1_CR1 |= 1u << 6; // SPE: 1
}

int spi_transfer(uint8_t tx)
{
    // 1. check if tx is empty (TXE = 1)
    if (!wait_flag(1u << 1, 1))
        return -1;

    // 2. write data -> SPI H/W generates clock
    SPI1_DR = tx;

    // 3. wait until rx is ready (RXNE = 1)
    if (!wait_flag(1u << 0, 1))
        return -1;

    // 4. read rx
    uint8_t rx = SPI1_DR;

    // 5. wait until communication is done (BSY = 0)
    if (!wait_flag(1u << 7, 0))
        return -1;
    return rx;
}

