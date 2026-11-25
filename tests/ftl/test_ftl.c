#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ftl.h"
#include "ftl_alloc.h"
#include "ftl_bbt.h"
#include "ftl_map.h"

/* ---- 가짜 NAND: 칩 1개 관리 영역(24 Block) + BBT 후보 Block을 메모리에 둔다 ---- */
#define FAKE_BLOCKS (FTL_ACTIVE_BLOCKS_PER_CHIP + FTL_BBT_CANDIDATES)
/* 관리 영역은 그대로, BBT 후보(1023, 1022, ...)는 뒤 칸에 */
static uint16_t slot(uint16_t block)
{
    if (block < FTL_ACTIVE_BLOCKS_PER_CHIP) {
        return block;
    }
    assert(block > FTL_BBT_FIRST_CANDIDATE - FTL_BBT_CANDIDATES && block <= FTL_BBT_FIRST_CANDIDATE);
    return (uint16_t)(FTL_ACTIVE_BLOCKS_PER_CHIP + FTL_BBT_FIRST_CANDIDATE - block);
}
static uint8_t       store[FAKE_BLOCKS][NAND_PAGES_PER_BLOCK][NAND_PAGE_SIZE];
static bool          written[FAKE_BLOCKS][NAND_PAGES_PER_BLOCK];
static int           program_fails;                 /* 다음 N번 Program을 PROGRAM_FAIL로 */
static nand_result_t program_res = NAND_OK;         /* PROGRAM_FAIL 외 결과 주입 */
static nand_ecc_t    read_ecc = NAND_ECC_NO_ERROR;
static int           program_calls;

nand_result_t nand_is_factory_bad_block(uint8_t chip, uint16_t block, bool *is_bad)
{
    (void)chip; (void)block;
    *is_bad = false;
    return NAND_OK;
}

nand_result_t nand_erase_block(uint8_t chip, uint16_t block)
{
    (void)chip;
    block = slot(block);
    memset(store[block], 0xFF, sizeof store[block]);
    memset(written[block], 0, sizeof written[block]);
    return NAND_OK;
}

nand_result_t nand_program_page(nand_addr_t a, const uint8_t *buf)
{
    program_calls++;
    assert(a.chip == 0);
    a.block = slot(a.block);
    assert(!written[a.block][a.page]);   /* 같은 Page 두 번 쓰기 = FTL 버그 */
    written[a.block][a.page] = true;     /* 실패해도 그 Page는 다시 못 씀 */
    if (program_fails > 0) {
        program_fails--;
        return NAND_ERR_PROGRAM_FAIL;
    }
    if (program_res != NAND_OK) {
        return program_res;
    }
    memcpy(store[a.block][a.page], buf, NAND_PAGE_SIZE);
    return NAND_OK;
}

nand_result_t nand_read_page(nand_addr_t a, uint8_t *buf, nand_ecc_t *ecc)
{
    assert(a.chip == 0);
    a.block = slot(a.block);
    memcpy(buf, store[a.block][a.page], NAND_PAGE_SIZE);
    *ecc = read_ecc;
    return NAND_OK;
}

static uint8_t w[NAND_PAGE_SIZE], r[NAND_PAGE_SIZE];

static void fill(uint8_t v) { memset(w, v, sizeof w); }

static bool read_is(uint16_t lpn, uint8_t v)
{
    fill(v);
    return ftl_read(lpn, r) == SOLID_OK && memcmp(r, w, sizeof r) == 0;
}

static ppn_t ppn_of(uint16_t lpn)
{
    ppn_t p;
    assert(ftl_map_get(lpn, &p));
    return p;
}

static ftl_block_info_t info0(void)
{
    ftl_block_info_t bi;
    assert(ftl_get_block_info(0, 0, &bi) == SOLID_OK);
    return bi;
}

int main(void)
{
    /* 초기화 전 */
    assert(ftl_read(0, r) == ERR_FTL_NOT_READY);
    assert(ftl_write(0, w) == ERR_FTL_NOT_READY);
    assert(ftl_init(1) == SOLID_OK);

    /* 입력 검사, 쓴 적 없는 LPN */
    assert(ftl_read(5, r) == ERR_FTL_UNMAPPED);
    assert(ftl_read(FTL_LOGICAL_PAGE_COUNT, r) == ERR_FTL_RANGE);
    assert(ftl_write(FTL_LOGICAL_PAGE_COUNT, w) == ERR_FTL_RANGE);
    assert(ftl_read(5, NULL) == ERR_FTL_INVALID_ARG);
    assert(ftl_write(5, NULL) == ERR_FTL_INVALID_ARG);

    /* FTL-03/04: 쓰고 그대로 읽기 */
    fill(0xA1);
    assert(ftl_write(5, w) == SOLID_OK);
    assert(read_is(5, 0xA1) && ppn_of(5) == 0);

    /* FTL-05/06: 덮어쓰기 → 새 PPN, 옛 Page 무효 */
    fill(0xB2);
    assert(ftl_write(5, w) == SOLID_OK);
    assert(read_is(5, 0xB2) && ppn_of(5) == 1);
    assert(info0().valid_pages == 1 && info0().invalid_pages == 1);

    /* Program 1번 실패 → 다음 Page로 재시도해서 성공, 실패한 Page는 무효로 남음 */
    program_fails = 1;
    fill(0xC3);
    assert(ftl_write(5, w) == SOLID_OK);
    assert(read_is(5, 0xC3) && ppn_of(5) == 3);
    assert(info0().valid_pages == 1 && info0().invalid_pages == 3);

    /* 2번 연속 실패 → 오류, 기존 데이터·매핑 그대로 */
    program_fails = 2;
    fill(0xD4);
    assert(ftl_write(5, w) == ERR_NAND_PROGRAM_FAIL);
    assert(read_is(5, 0xC3) && ppn_of(5) == 3);

    /* 타임아웃은 재시도하지 않음 */
    program_res = NAND_ERR_TIMEOUT;
    program_calls = 0;
    assert(ftl_write(5, w) == ERR_NAND_TIMEOUT && program_calls == 1);
    assert(read_is(5, 0xC3));
    program_res = NAND_OK;

    /* ECC */
    /* Correctable ECC: 성공 + 횟수 기록, 같은 Block에서 3번째면 새 Page로 옮김 */
    assert(ftl_get_ecc_corrected_count() == 0);
    ppn_t before = ppn_of(5);
    read_ecc = NAND_ECC_CORRECTED;
    assert(read_is(5, 0xC3) && ppn_of(5) == before);  /* 1번째: 그대로 */
    assert(read_is(5, 0xC3) && ppn_of(5) == before);  /* 2번째: 그대로 */
    assert(read_is(5, 0xC3) && ppn_of(5) != before);  /* 3번째: 옮겨짐 */
    assert(ftl_get_ecc_corrected_count() == 3);
    read_ecc = NAND_ECC_NO_ERROR;
    assert(read_is(5, 0xC3));                          /* 옮긴 곳에서도 같은 데이터 */
    read_ecc = NAND_ECC_CORRECTED;
    read_ecc = NAND_ECC_UNCORRECTABLE;
    assert(ftl_read(5, r) == ERR_NAND_ECC_UNCORRECTABLE);
    read_ecc = NAND_ECC_UNKNOWN;
    assert(ftl_read(5, r) == ERR_NAND_IO);
    read_ecc = NAND_ECC_NO_ERROR;

    /* 공간이 차면 ERR_FTL_NO_SPACE (GC 연결 전), 그래도 이미 쓴 데이터는 읽힘 */
    uint16_t lpn = 100;
    solid_err_t e;
    do {
        fill((uint8_t)lpn);
        e = ftl_write(lpn++, w);
    } while (e == SOLID_OK);
    assert(e == ERR_FTL_NO_SPACE && ftl_get_free_block_count() == 1);
    assert(read_is(100, 100) && read_is((uint16_t)(lpn - 2), (uint8_t)(lpn - 2)));

    puts("ftl write/read: all tests passed");
    return 0;
}
