/*
 * 테스트: SPI1을 통한 NAND JEDEC ID 읽기 (NUCLEO-F411RE + W25N01GV)
 * 연결: Loopback 점퍼 제거 후 NAND 연결.
 *   PA5 -> SCK, PA7 -> MOSI(DI), PA6 <- MISO(DO), PB6 -> CS.
 *   NAND 전원 3.3V 및 공통 GND 연결, /WP와 /HOLD는 High 유지.
 * 출력: ST-LINK VCP 시리얼 터미널, 115200bps / 8N1 / 흐름 제어 없음.
 *
 * 빌드 (프로젝트 루트 D:/SOLID_miniSSD 기준):
 *   make -C tests/nand/1.jedec_check
 * 빌드 + 다운로드 + 실행:
 *   make -C tests/nand/1.jedec_check run
 * make가 멈추면 실행 파일을 직접 지정:
 *   & C:/msys64/ucrt64/bin/mingw32-make.exe -C tests/nand/1.jedec_check run
 *
 * 예상 결과: JEDEC ID: XX XX XX 형식으로 ID 3바이트가 반복 출력됨.
 *   실제 장착 칩 데이터시트의 Manufacturer ID + Device ID와 비교할 것.
 *   현재 코드는 ID를 출력하며, 예상 ID와 자동 비교하지 않음.
 *   SPI 대기 시간 초과 시 SPI TIMEOUT 출력 후 정지.
 * 터미널을 먼저 열고 실행. NAND 쓰기/삭제는 수행하지 않음.
 */

#include <stdint.h>
#include "uart.h"
#include "spi.h"
#include "stm32f411_regs.h"

volatile uint8_t jedec_id[3];

static void cs_init(void)
{
    RCC_AHB1ENR |= 1u << 1;
    (void)RCC_AHB1ENR;
    GPIOB_BSRR = 1u << 6;
    GPIOB_MODER = (GPIOB_MODER & ~(3u << 12)) | (1u << 12);
}

static int nand_read_id(void)
{
    // Figure 6: /CS LOW -> NAND 선택. ID를 모두 받을 때까지 LOW 유지
    GPIOB_BSRR = 1u << (6 + 16);

    // Figure 6: 9Fh 명령(8클록) -> 더미 8클록. 이때 받은 값은 버린다
    if (spi_transfer(0x9F) < 0 || spi_transfer(0x00) < 0)
        goto fail;

    // Figure 6: 더미 데이터를 보내 클록을 만들고 Manufacturer ID + Device ID 2바이트 수신.
    for (unsigned int i = 0; i < 3; ++i)
    {
        int value = spi_transfer(0x00);
        if (value < 0)
            goto fail;
        jedec_id[i] = (uint8_t)value;
    }

    // Figure 6: /CS HIGH -> ID 읽기 종료.
    GPIOB_BSRR = 1u << 6;
    return 1;

fail:
    GPIOB_BSRR = 1u << 6;
    return 0;
}

int main(void)
{
    uart_init();
    cs_init();
    spi_init();
    for (volatile unsigned int delay = 0; delay < 1000000; ++delay)
    {
    }

    for (;;)
    {
        if (!nand_read_id())
        {
            uart_puts("SPI TIMEOUT\r\n");
            for (;;)
            {
            }
        }
        uart_puts("JEDEC ID: ");
        for (unsigned int i = 0; i < 3; ++i)
        {
            uart_hex(jedec_id[i]);
            uart_puts(i == 2 ? "\r\n" : " ");
        }
        for (volatile unsigned int delay = 0; delay < 1000000; ++delay)
        {
        }
    }
}
