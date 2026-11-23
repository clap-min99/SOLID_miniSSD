#include "nand_internal.h"

enum
{

    NAND_CMD_PAGE_READ = 0x13,
    NAND_CMD_READ_CACHE = 0x03,
    NAND_CONFIG_OTP_ENABLE = 1u << 6,
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

// NAND 설정 레지스터 B0h에 값을 쓰고 재조회로 확인
nand_result_t nand_write_configuration(nand_cs_t cs, uint8_t configuration)
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

// Little-endian 바이트 2개를 16비트 정수로 변환
static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

// Little-endian 바이트 4개를 32비트 정수로 변환
static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)read_le16(data) | ((uint32_t)read_le16(data + 2) << 16);
}

// Parameter Page 복사본의 ONFI 서명과 CRC를 검증 (§8.2.27)
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

// OTP 영역의 Parameter Page를 읽어 물리 사양을 해석
static nand_result_t nand_read_geometry(nand_cs_t cs, nand_geometry_t *geometry)
{
    // MOSI 13h → 더미 00h → Row 00h 01h
    // OTP 영역의 Parameter Page를 NAND 내부 캐시로 로드
    const uint8_t page_read[] = {NAND_CMD_PAGE_READ, 0x00, 0x00, NAND_PARAMETER_PAGE_ROW};
    nand_result_t result = nand_transfer(cs, page_read, sizeof(page_read), NULL, 0);
    if (result != NAND_OK)
        return result;

    // BUSY 해제 후 캐시 읽기 가능
    result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    uint8_t copy[NAND_PARAMETER_COPY_SIZE];
    // 256B 복사본 3개, 시작 Column 0000h / 0100h / 0200h
    for (unsigned int index = 0; index < NAND_PARAMETER_COPY_COUNT; ++index)
    {
        uint16_t column = (uint16_t)(index * NAND_PARAMETER_COPY_SIZE);

        // MOSI 03h → Column 상위/하위 바이트 → 더미 00h
        // OTP 접근 시 BUF 설정과 관계없이 위 형식 적용
        const uint8_t read_cache[] = {
            NAND_CMD_READ_CACHE, (uint8_t)(column >> 8), (uint8_t)column, 0x00,
        };
        result = nand_transfer(cs, read_cache, sizeof(read_cache), copy, sizeof(copy));
        if (result != NAND_OK)
            return result;
        if (!nand_parameter_copy_valid(copy))
            continue;

        // 고정 오프셋에서 Little-endian 값 해석
        // 반환 전 API 필드의 16비트 범위 검사
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
    nand_cs_t cs = nand_chip_select(chip);
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

    // 사양 저장
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
