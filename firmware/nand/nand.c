#include "nand_internal.h"
#include "spi.h"
#include "stm32f411_regs.h"

enum
{
    GPIO_PORT_A,
    GPIO_PORT_B,
    GPIO_PORT_C,
};

// Nucleo-F411RE header wiring
static const nand_cs_t chip_selects[] = {
    {GPIO_PORT_B, 6},  // Chip 0, D10
    {GPIO_PORT_C, 7},  // Chip 1, D9
    {GPIO_PORT_A, 9},  // Chip 2, D8
    {GPIO_PORT_A, 8},  // Chip 3, D7
    {GPIO_PORT_B, 10}, // Chip 4, D6
    {GPIO_PORT_B, 4},  // Chip 5, D5
    {GPIO_PORT_B, 5},  // Chip 6, D4
    {GPIO_PORT_A, 10}, // Chip 7, D2
};

// NAND 칩 개수와 CS 핀 테이블의 항목 수가 같은지 검사
_Static_assert(sizeof(chip_selects) / sizeof(chip_selects[0]) == NAND_MAX_CHIPS,
               "Each NAND chip index needs a CS pin");

static bool nand_initialized;
static const uint8_t supported_jedec_ids[][3] = {
    {0xEFu, 0xAAu, 0x21u}, // W25N01GV
};

// NAND 칩 8개의 CS 핀을 비활성 상태(HIGH)의 GPIO 출력으로 초기화
static void cs_init(void)
{
    uint32_t gpio_clocks = 0;
    for (unsigned int chip = 0; chip < NAND_MAX_CHIPS; ++chip)
        gpio_clocks |= 1u << chip_selects[chip].port;

    RCC_AHB1ENR |= gpio_clocks;
    (void)RCC_AHB1ENR;

    for (unsigned int chip = 0; chip < NAND_MAX_CHIPS; ++chip)
    {
        nand_cs_t cs = chip_selects[chip];
        unsigned int mode_shift = 2u * cs.pin;

        // GPIO 출력으로 바꾸기 전 CS HIGH 유지
        GPIO_BSRR(cs.port) = 1u << cs.pin;
        GPIO_OTYPER(cs.port) &= ~(1u << cs.pin);
        GPIO_MODER(cs.port) = (GPIO_MODER(cs.port) & ~(3u << mode_shift)) |
                              (1u << mode_shift);
    }
}

// 범위를 확인한 칩 번호에 해당하는 CS 핀을 반환
nand_cs_t nand_chip_select(uint8_t chip)
{
    return chip_selects[chip];
}

// NAND 통신에 사용할 CS 핀과 SPI를 초기화
nand_result_t nand_init(void)
{
    cs_init();
    spi_init();
    nand_initialized = true;
    return NAND_OK;
}

// 지정한 NAND 칩의 JEDEC ID를 읽어 지원 모델인지 확인
// 오류 처리 순서: 인자 검사 -> 초기화 검사 -> SPI 전송 검사 -> JEDEC ID 검사
nand_result_t nand_probe(
    uint8_t chip)
{
    uint8_t jedec_id[3];

    // 1. 칩 번호 범위 확인 (0~7, nand 8개)
    if (chip >= NAND_MAX_CHIPS)
        return NAND_ERR_INVALID_ARG;

    // 2. nand 초기화 확인
    if (!nand_initialized)
        return NAND_ERR_NOT_INITIALIZED;

    nand_cs_t cs = nand_chip_select(chip);
    GPIO_BSRR(cs.port) = 1u << (cs.pin + 16u);

    // 3-1. 0x9F 명령 전송 & 더미 클록 8 전송
    if (spi_transfer(0x9F) < 0 || spi_transfer(0x00) < 0)
        goto timeout;

    // 3-2. JEDEC 3바이트 수신
    for (unsigned int i = 0; i < sizeof(jedec_id); ++i)
    {
        int value = spi_transfer(0x00);
        if (value < 0)
            goto timeout;
        jedec_id[i] = (uint8_t)value;
    }

    GPIO_BSRR(cs.port) = 1u << cs.pin;

    // 4. 실제 JEDEC ID와 예상 ID 비교
    for (unsigned int i = 0;
         i < sizeof(supported_jedec_ids) / sizeof(supported_jedec_ids[0]);
         ++i)
    {
        const uint8_t *expected = supported_jedec_ids[i];
        if (jedec_id[0] == expected[0] &&
            jedec_id[1] == expected[1] &&
            jedec_id[2] == expected[2])
            return NAND_OK;
    }

    // 읽은 ID가 지원 목록에 없음
    return NAND_ERR_NO_DEVICE;

timeout:
    GPIO_BSRR(cs.port) = 1u << cs.pin;
    return NAND_ERR_TIMEOUT;
}

enum
{
    NAND_CMD_GET_FEATURE = 0x0F,
    NAND_REG_STATUS = 0xC0,
    NAND_STATUS_BUSY = 1u << 0,
    NAND_READY_POLL_LIMIT = 128,
};

// CS를 LOW로 유지하며 명령 바이트를 보내고 지정한 길이만큼 응답을 수신
nand_result_t nand_transfer(nand_cs_t cs, const uint8_t *command,
                            size_t command_size, uint8_t *data, size_t data_size)
{
    // CS LOW
    GPIO_BSRR(cs.port) = 1u << (cs.pin + 16u);

    // 명령 전송
    for (size_t i = 0; i < command_size; ++i)
    {
        if (spi_transfer(command[i]) < 0)
            goto timeout;
    }

    // 필요한 만큼 응답 수신
    for (size_t i = 0; i < data_size; ++i)
    {
        int value = spi_transfer(0x00);
        if (value < 0)
            goto timeout;
        data[i] = (uint8_t)value;
    }

    GPIO_BSRR(cs.port) = 1u << cs.pin;
    return NAND_OK;

timeout:
    GPIO_BSRR(cs.port) = 1u << cs.pin;
    return NAND_ERR_TIMEOUT;
}

// 지정한 NAND 레지스터에서 값 1바이트를 읽음
nand_result_t nand_get_feature(nand_cs_t cs, uint8_t address, uint8_t *value)
{
    // MOSI 0Fh → 레지스터 주소 → 더미 00h
    // 더미 전송 중 MISO에서 레지스터 값 1B 수신
    const uint8_t command[] = {NAND_CMD_GET_FEATURE, address};
    return nand_transfer(cs, command, sizeof(command), value, 1);
}

// NAND 상태 레지스터를 반복 조회해 BUSY 해제를 기다림
nand_result_t nand_wait_ready(nand_cs_t cs)
{
    // 상태 레지스터 C0h 조회, BUSY(bit 0) = 0까지 대기
    for (unsigned int attempt = 0; attempt < NAND_READY_POLL_LIMIT; ++attempt)
    {
        uint8_t status;
        nand_result_t result = nand_get_feature(cs, NAND_REG_STATUS, &status);
        if (result != NAND_OK)
            return result;
        if ((status & NAND_STATUS_BUSY) == 0)
            return NAND_OK;
    }
    return NAND_ERR_TIMEOUT;
}
