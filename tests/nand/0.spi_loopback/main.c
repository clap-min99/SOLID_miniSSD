/*
 * 테스트: SPI1 MOSI-MISO Loopback (NUCLEO-F411RE)
 * 연결: NAND 모듈을 분리하고 PA7(MOSI)과 PA6(MISO)을 점퍼로 연결.
 * 출력: ST-LINK VCP 시리얼 터미널, 115200bps / 8N1 / 흐름 제어 없음.
 *
 * 빌드 (프로젝트 루트 D:/SOLID_miniSSD 기준):
 *   make -C tests/nand/0.spi_loopback
 * 빌드 + 다운로드 + 실행:
 *   make -C tests/nand/0.spi_loopback run
 * make가 멈추면 실행 파일을 직접 지정:
 *   & C:/msys64/ucrt64/bin/mingw32-make.exe -C tests/nand/0.spi_loopback run
 *
 * 예상 결과:
 *   SPI LOOPBACK
 *   TX=00 RX=00 OK
 *   TX=FF RX=FF OK
 *   TX=55 RX=55 OK
 *   TX=AA RX=AA OK
 *   TX=12 RX=12 OK
 *   TX=34 RX=34 OK
 *   LOOPBACK PASS
 * 불일치: 해당 줄 FAIL 및 최종 LOOPBACK FAIL.
 * 대기 시간 초과: 해당 줄 TIMEOUT 및 최종 LOOPBACK FAIL.
 * 터미널을 먼저 열고 실행. 출력을 놓치면 보드 RESET 버튼으로 재실행.
 * Loopback 통과는 NAND 명령이나 CS 동작까지 검증한 결과는 아님.
 */

#include <stdint.h>
#include "uart.h"
#include "spi.h"

int main(void)
{
    const uint8_t data[] = {0x00, 0xFF, 0x55, 0xAA, 0x12, 0x34};
    unsigned int errors = 0;
    uart_init();
    spi_init();
    uart_puts("SPI LOOPBACK\r\n");

    for (unsigned int i = 0; i < sizeof(data); ++i)
    {
        int rx = spi_transfer(data[i]);
        uart_puts("TX=");
        uart_hex(data[i]);
        if (rx < 0)
        {
            uart_puts(" TIMEOUT\r\n");
            ++errors;
            break;
        }
        uart_puts(" RX=");
        uart_hex((uint8_t)rx);
        if ((uint8_t)rx != data[i])
        {
            uart_puts(" FAIL\r\n");
            ++errors;
        }
        else
            uart_puts(" OK\r\n");
    }
    uart_puts(errors ? "LOOPBACK FAIL\r\n" : "LOOPBACK PASS\r\n");
    for (;;)
    {
    }
}
