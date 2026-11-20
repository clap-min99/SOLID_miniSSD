#include "../common/nand.h"
#include "spi.h"
#include "stm32f411_regs.h"

#include <stddef.h>

enum
{
    GPIO_PORT_A,
    GPIO_PORT_B,
    GPIO_PORT_C,
};

typedef struct
{
    uint8_t port;
    uint8_t pin;
} nand_cs_t;

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

    nand_cs_t cs = chip_selects[chip];
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
    NAND_CMD_SET_FEATURE = 0x1F,
    NAND_CMD_PAGE_READ = 0x13,
    NAND_CMD_READ_CACHE = 0x03,
    NAND_REG_CONFIGURATION = 0xB0,
    NAND_REG_STATUS = 0xC0,
    NAND_CONFIG_OTP_ENABLE = 1u << 6,
    NAND_STATUS_BUSY = 1u << 0,
    NAND_READY_POLL_LIMIT = 128,
    NAND_PARAMETER_PAGE_ROW = 0x01,
    NAND_PARAMETER_COPY_SIZE = 256,
    NAND_PARAMETER_COPY_COUNT = 3,
    NAND_PARAMETER_PAGE_SIZE_OFFSET = 80,
    NAND_PARAMETER_SPARE_SIZE_OFFSET = 84,
    NAND_PARAMETER_PAGES_PER_BLOCK_OFFSET = 92,
    NAND_PARAMETER_BLOCKS_PER_LUN_OFFSET = 96,
    NAND_PARAMETER_LUN_COUNT_OFFSET = 100,
    NAND_PARAMETER_CRC_OFFSET = 254,
};

/* SPI 명령 한 건 실행 */
// 명령 전송과 응답 수신 동안 같은 칩을 선택하고, 실패 시에도 CS를 해제
static nand_result_t nand_transfer(nand_cs_t cs, const uint8_t *command,
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

static nand_result_t nand_get_feature(nand_cs_t cs, uint8_t address, uint8_t *value)
{
    // MOSI 0Fh → 레지스터 주소 → 더미 00h
    // 더미 전송 중 MISO에서 레지스터 값 1B 수신
    const uint8_t command[] = {NAND_CMD_GET_FEATURE, address};
    return nand_transfer(cs, command, sizeof(command), value, 1);
}

static nand_result_t nand_wait_ready(nand_cs_t cs)
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

static nand_result_t nand_write_configuration(nand_cs_t cs, uint8_t configuration)
{
    // §8.2.4, Fig. 8
    // - MOSI 1Fh → B0h → 설정값
    // - B0h 재조회로 설정 적용 확인
    const uint8_t command[] = {NAND_CMD_SET_FEATURE, NAND_REG_CONFIGURATION, configuration};
    nand_result_t result = nand_transfer(cs, command, sizeof(command), NULL, 0);
    if (result != NAND_OK)
        return result;

    uint8_t actual;
    result = nand_get_feature(cs, NAND_REG_CONFIGURATION, &actual);
    if (result != NAND_OK)
        return result;
    return actual == configuration ? NAND_OK : NAND_ERR_IO;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)read_le16(data) | ((uint32_t)read_le16(data + 2) << 16);
}

// §8.2.27 Parameter Page 복사본 검증
// - 오프셋 0~3의 ONFI 서명 확인
// - 오프셋 0~253의 CRC-16 계산값과 254~255의 저장값 비교
static bool nand_parameter_copy_valid(const uint8_t *copy)
{
    if (copy[0] != 'O' || copy[1] != 'N' || copy[2] != 'F' || copy[3] != 'I')
        return false;

    uint16_t crc = 0x4F4E;
    for (size_t i = 0; i < NAND_PARAMETER_CRC_OFFSET; ++i)
    {
        crc ^= (uint16_t)copy[i] << 8;
        for (unsigned int bit = 0; bit < 8; ++bit)
        {
            uint16_t polynomial = (crc & 0x8000u) != 0 ? 0x8005u : 0u;
            crc = (uint16_t)((crc << 1) ^ polynomial);
        }
    }
    return crc == read_le16(copy + NAND_PARAMETER_CRC_OFFSET);
}

static nand_result_t nand_read_geometry(nand_cs_t cs, nand_geometry_t *geometry)
{
    // §8.2.14, Fig. 18 및 §8.2.26
    // - MOSI 13h → 더미 00h → Row 00h 01h
    // - OTP 영역의 Parameter Page를 NAND 내부 캐시로 로드
    const uint8_t page_read[] = {NAND_CMD_PAGE_READ, 0x00, 0x00, NAND_PARAMETER_PAGE_ROW};
    nand_result_t result = nand_transfer(cs, page_read, sizeof(page_read), NULL, 0);
    if (result != NAND_OK)
        return result;

    // - BUSY 해제 후 캐시 읽기 가능
    result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    uint8_t copy[NAND_PARAMETER_COPY_SIZE];
    // - 256B 복사본 3개, 시작 Column 0000h / 0100h / 0200h
    for (unsigned int index = 0; index < NAND_PARAMETER_COPY_COUNT; ++index)
    {
        uint16_t column = (uint16_t)(index * NAND_PARAMETER_COPY_SIZE);
        // §8.2.15, Fig. 19a 및 §8.2.26
        // - MOSI 03h → Column 상위/하위 바이트 → 더미 00h
        // - OTP 접근 시 BUF 설정과 관계없이 위 형식 적용
        const uint8_t read_cache[] = {
            NAND_CMD_READ_CACHE, (uint8_t)(column >> 8), (uint8_t)column, 0x00,
        };
        result = nand_transfer(cs, read_cache, sizeof(read_cache), copy, sizeof(copy));
        if (result != NAND_OK)
            return result;
        if (!nand_parameter_copy_valid(copy))
            continue;

        // §8.2.27
        // - 고정 오프셋에서 Little-endian 값 해석
        // - 반환 전 API 필드의 16비트 범위 검사
        uint32_t page_size = read_le32(copy + NAND_PARAMETER_PAGE_SIZE_OFFSET);
        uint16_t spare_size = read_le16(copy + NAND_PARAMETER_SPARE_SIZE_OFFSET);
        uint32_t pages_per_block = read_le32(copy + NAND_PARAMETER_PAGES_PER_BLOCK_OFFSET);
        uint32_t blocks_per_lun = read_le32(copy + NAND_PARAMETER_BLOCKS_PER_LUN_OFFSET);
        uint32_t lun_count = copy[NAND_PARAMETER_LUN_COUNT_OFFSET];
        if (page_size == 0 || page_size > UINT16_MAX || spare_size == 0 ||
            pages_per_block == 0 || pages_per_block > UINT16_MAX ||
            blocks_per_lun == 0 || lun_count == 0 || blocks_per_lun > UINT16_MAX / lun_count)
            continue;

        *geometry = (nand_geometry_t){
            .page_size = (uint16_t)page_size,
            .spare_size = spare_size,
            .pages_per_block = (uint16_t)pages_per_block,
            .blocks_per_chip = (uint16_t)(blocks_per_lun * lun_count),
        };
        return NAND_OK;
    }
    return NAND_ERR_IO;
}

// 칩의 Parameter Page에서 물리 구성을 읽고 기존 설정을 복원
nand_result_t nand_get_geometry(
    uint8_t chip,
    nand_geometry_t *geometry
)
{
    if (geometry == NULL)
        return NAND_ERR_INVALID_ARG;

    // JEDEC ID 반환
    nand_result_t result = nand_probe(chip);
    if (result != NAND_OK)
        return result;

    // BUSY 확인
    nand_cs_t cs = chip_selects[chip];
    result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    // OTP 접근 후 복원을 위해 기존 B0h 설정값을 configuration에 저장
    uint8_t configuration;
    result = nand_get_feature(cs, NAND_REG_CONFIGURATION, &configuration);
    if (result != NAND_OK)
        return result;

    // OTP-E(bit 6) 설정, 나머지 설정 비트 유지
    bool restore_configuration = (configuration & NAND_CONFIG_OTP_ENABLE) == 0;
    if (restore_configuration)
        result = nand_write_configuration(cs, configuration | NAND_CONFIG_OTP_ENABLE);

    //
    nand_geometry_t detected;
    if (result == NAND_OK)
        result = nand_read_geometry(cs, &detected);

    // 읽기 또는 모드 전환 실패 시에도 기존 설정 복원 시도
    if (restore_configuration)
    {
        nand_result_t restore_result = nand_wait_ready(cs);
        if (restore_result == NAND_OK)
            restore_result = nand_write_configuration(cs, configuration);
        if (restore_result != NAND_OK)
            return restore_result;
    }

    if (result == NAND_OK)
        *geometry = detected;
    return result;
}
