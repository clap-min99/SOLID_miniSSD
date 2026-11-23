#include "nand_internal.h"

enum
{
    NAND_CMD_WRITE_ENABLE = 0x06,
    NAND_CMD_PROGRAM_LOAD = 0x02,
    NAND_CMD_PROGRAM_EXECUTE = 0x10,
    NAND_STATUS_WEL = 1u << 1,
    NAND_STATUS_P_FAIL = 1u << 3,
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
        NAND_CMD_PROGRAM_EXECUTE, 0x00, (uint8_t)(row >> 8), (uint8_t)row,
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
