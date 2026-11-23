#include "nand_internal.h"

enum
{
    NAND_CMD_WRITE_ENABLE = 0x06,
    NAND_CMD_PROGRAM_LOAD = 0x02,
    NAND_CMD_PROGRAM_EXECUTE = 0x10,
    NAND_CMD_READ_DATA = 0x03,
    NAND_CMD_PAGE_DATA_READ = 0x13,
    NAND_STATUS_WEL = 1u << 1,
    NAND_STATUS_P_FAIL = 1u << 3,
    NAND_STATUS_ECC_SHIFT = 4,
    NAND_STATUS_ECC_MASK = 0x03u << NAND_STATUS_ECC_SHIFT,
};

// 지정한 물리 페이지의 메인 영역에 2KiB를 기록
nand_result_t nand_program_page(nand_addr_t addr, const uint8_t *buffer)
{
    // 칩 선택 전에 물리 주소 범위와 입력 버퍼 확인
    if (addr.chip >= NAND_MAX_CHIPS ||
        addr.block >= NAND_BLOCKS_PER_CHIP ||
        addr.page >= NAND_PAGES_PER_BLOCK ||
        buffer == NULL)
        return NAND_ERR_INVALID_ARG;

    // 이전 NAND 작업이 끝날 때까지 대기
    nand_cs_t cs = nand_chip_select(addr.chip);
    nand_result_t result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    // Write Enable(06h) 전송 후 상태 레지스터의 WEL 확인
    const uint8_t write_enable = NAND_CMD_WRITE_ENABLE;
    result = nand_transfer(cs, &write_enable, sizeof(write_enable), NULL, 0);
    if (result != NAND_OK)
        return result;

    uint8_t status;
    result = nand_get_feature(cs, NAND_REG_STATUS, &status);
    if (result != NAND_OK)
        return result;
    if ((status & NAND_STATUS_WEL) == 0)
        return NAND_ERR_IO;

    // Program Data Load(02h)로 Column 0부터 데이터 2KiB 적재
    const uint8_t program_data_load[] = {NAND_CMD_PROGRAM_LOAD, 0x00, 0x00};
    result = nand_transfer_write(cs, program_data_load, sizeof(program_data_load),
                                 buffer, NAND_PAGE_SIZE);
    if (result != NAND_OK)
        return result;

    // 칩 전체에서 몇 번째 페이지인지 계산
    const uint16_t row = (uint16_t)(addr.block * NAND_PAGES_PER_BLOCK + addr.page);

    // Row를 상위, 하위 1바이트로 나눔
    const uint8_t program_execute[] = {
        NAND_CMD_PROGRAM_EXECUTE,
        0x00,
        (uint8_t)(row >> 8),
        (uint8_t)row,
    };
    result = nand_transfer(cs, program_execute, sizeof(program_execute), NULL, 0);
    if (result != NAND_OK)
        return result;

    // BUSY 해제 후 상태 레지스터에서 프로그램 결과 확인
    result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    result = nand_get_feature(cs, NAND_REG_STATUS, &status);
    if (result != NAND_OK)
        return result;
    if ((status & NAND_STATUS_P_FAIL) != 0)
        return NAND_ERR_PROGRAM_FAIL;

    return NAND_OK;
}

// 지정한 물리 페이지의 메인 영역에서 2KiB를 읽고 ECC 결과를 전달
nand_result_t nand_read_page(nand_addr_t addr, uint8_t *buffer, nand_ecc_t *ecc)
{
    if (addr.chip >= NAND_MAX_CHIPS ||
        addr.block >= NAND_BLOCKS_PER_CHIP ||
        addr.page >= NAND_PAGES_PER_BLOCK ||
        buffer == NULL || ecc == NULL)
        return NAND_ERR_INVALID_ARG;

    // TODO: 칩 선택 후 이전 NAND 작업의 BUSY 해제 대기
    nand_cs_t cs = nand_chip_select(addr.chip);
    nand_result_t result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    // TODO: Row 주소를 계산하고 Page Data Read(13h)로 페이지를 내부 버퍼에 로드
    const uint16_t row = (uint16_t)(addr.block * NAND_PAGES_PER_BLOCK + addr.page);
    const uint8_t page_data_read[] = {NAND_CMD_PAGE_DATA_READ, 0x00, (uint8_t)(row >> 8),
                                      (uint8_t)row};
    result = nand_transfer_write(cs, page_data_read, sizeof(page_data_read),
                                 NULL, 0);
    if (result != NAND_OK)
        return result;

    // BUSY가 풀린 뒤
    result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    // C0h의 ECC 상태 비트 [5:4]를 읽음
    uint8_t status;
    result = nand_get_feature(cs, NAND_REG_STATUS, &status);
    if (result != NAND_OK)
        return result;

    switch ((status & NAND_STATUS_ECC_MASK) >> NAND_STATUS_ECC_SHIFT)
    {
    case 0:
        *ecc = NAND_ECC_NO_ERROR;
        break;
    case 1:
        *ecc = NAND_ECC_CORRECTED;
        break;
    default:
        *ecc = NAND_ECC_UNCORRECTABLE;
        return NAND_ERR_ECC_UNCORRECTABLE;
    }

    // TODO: 버퍼 읽기 모드에서 Read Data(03h)로 Column 0부터 2KiB 수신
    const uint8_t read_data[] = {
        NAND_CMD_READ_DATA,
        0x00,
        0x00,
        0x00,
    };

    return nand_transfer(cs, read_data, sizeof(read_data), buffer, NAND_PAGE_SIZE);
}

// 물리 블록 하나의 첫 페이지에서 공장 배드 블록 마커를 검사
nand_result_t nand_is_factory_bad_block(uint8_t chip, uint16_t block, bool *is_bad)
{
    if (chip >= NAND_MAX_CHIPS || block >= NAND_BLOCKS_PER_CHIP || is_bad == NULL)
        return NAND_ERR_INVALID_ARG;

    // 칩 선택 후 이전 NAND 작업이 끝날 때까지 대기
    nand_cs_t cs = nand_chip_select(chip);
    nand_result_t result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    // 블록의 첫 페이지를 13h로 내부 버퍼에 로드
    const uint16_t row = (uint16_t)(block * NAND_PAGES_PER_BLOCK);
    const uint8_t page_data_read[] = {
        NAND_CMD_PAGE_DATA_READ,
        0x00,
        (uint8_t)(row >> 8),
        (uint8_t)row,
    };

    result = nand_transfer(cs, page_data_read, sizeof(page_data_read), NULL, 0);
    if (result != NAND_OK)
        return result;

    // 첫 페이지가 내부 버퍼에 올라올 때까지 대기
    result = nand_wait_ready(cs);
    if (result != NAND_OK)
        return result;

    // 메인 영역의 첫 바이트로 배드 블록이 확인되면 검사 종료
    const uint8_t read_main_marker[] = {NAND_CMD_READ_DATA, 0x00, 0x00, 0x00};
    uint8_t marker;
    result = nand_transfer(cs, read_main_marker, sizeof(read_main_marker), &marker, 1);
    if (result != NAND_OK)
        return result;
    if (marker != 0xFFu)
    {
        *is_bad = true;
        return NAND_OK;
    }

    // 스페어 영역은 메인 영역 바로 뒤의 Column 0x0800에서 시작
    const uint8_t read_spare_marker[] = {
        NAND_CMD_READ_DATA,
        (uint8_t)(NAND_PAGE_SIZE >> 8),
        (uint8_t)NAND_PAGE_SIZE,
        0x00,
    };
    result = nand_transfer(cs, read_spare_marker, sizeof(read_spare_marker), &marker, 1);
    if (result != NAND_OK)
        return result;

    *is_bad = marker != 0xFFu;
    return NAND_OK;
}
