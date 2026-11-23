#include <assert.h>
#include <stdio.h>
#include "ftl_alloc.h"

/* ---- 가짜 NAND: ftl_alloc이 쓰는 두 함수만 (Mock NAND가 develop에 들어오면 교체) ---- */
static bool          factory_bad[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];
static nand_result_t erase_res[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];
static nand_result_t bad_query_res = NAND_OK;
static int           erase_calls;

nand_result_t nand_is_factory_bad_block(uint8_t chip, uint16_t block, bool *is_bad)
{
    if (bad_query_res != NAND_OK) {
        return bad_query_res;
    }
    *is_bad = factory_bad[chip][block];
    return NAND_OK;
}

nand_result_t nand_erase_block(uint8_t chip, uint16_t block)
{
    erase_calls++;
    return erase_res[chip][block];
}

static ftl_block_info_t info(uint8_t chip, uint16_t block)
{
    ftl_block_info_t bi;
    assert(ftl_get_block_info(chip, block, &bi) == SOLID_OK);
    return bi;
}

static void test_before_init(void)
{
    ppn_t p;
    ftl_block_info_t bi;
    assert(ftl_alloc_page(false, &p) == ERR_FTL_NOT_READY);
    assert(ftl_get_block_info(0, 0, &bi) == ERR_FTL_NOT_READY);
    assert(ftl_page_set_valid(0) == ERR_FTL_NOT_READY);
    assert(ftl_alloc_init(0) == ERR_FTL_INVALID_ARG);
    assert(ftl_alloc_init(NAND_MAX_CHIPS + 1) == ERR_FTL_INVALID_ARG);
}

static void test_init_bad_blocks(void)
{
    factory_bad[0][3] = true;                /* 공장 불량 → Erase 안 함 */
    erase_res[1][5] = NAND_ERR_ERASE_FAIL;   /* Erase 실패 → BAD */
    erase_res[1][7] = NAND_ERR_TIMEOUT;      /* Erase 타임아웃 → BAD */
    erase_calls = 0;

    assert(ftl_alloc_init(2) == SOLID_OK);
    assert(erase_calls == 48 - 1);
    assert(ftl_get_free_block_count() == 48 - 3);
    assert(info(0, 3).state == FTL_BLK_BAD);
    assert(info(1, 5).state == FTL_BLK_BAD);
    assert(info(1, 7).state == FTL_BLK_BAD);
    assert(info(0, 0).state == FTL_BLK_FREE && info(0, 0).erase_count == 1);

    ftl_block_info_t bi;
    assert(ftl_get_block_info(2, 0, &bi) == ERR_FTL_RANGE);   /* 연결 안 된 칩 */
    assert(ftl_get_block_info(0, 24, &bi) == ERR_FTL_RANGE);  /* 활성 영역 밖 */
    assert(ftl_get_block_info(0, 0, NULL) == ERR_FTL_INVALID_ARG);
}

static void test_sequential_alloc_and_valid(void)
{
    ppn_t p;
    for (uint32_t i = 0; i < NAND_PAGES_PER_BLOCK; i++) {     /* Block (0,0)을 순서대로 채움 */
        assert(ftl_alloc_page(false, &p) == SOLID_OK && p == i);
    }
    assert(info(0, 0).state == FTL_BLK_CLOSED && info(0, 0).write_pointer == 64);
    assert(ftl_get_free_block_count() == 44);

    assert(ftl_alloc_page(false, &p) == SOLID_OK && p == 64);  /* 다음 Block (0,1) */
    assert(info(0, 1).state == FTL_BLK_OPEN);

    assert(ftl_page_set_valid(0) == SOLID_OK);
    assert(ftl_page_set_valid(0) == ERR_FTL_INTERNAL);        /* 이미 유효 */
    assert(ftl_page_set_valid(65) == ERR_FTL_INTERNAL);       /* 아직 할당 안 됨 */
    assert(info(0, 0).valid_pages == 1 && info(0, 0).invalid_pages == 63);

    assert(ftl_page_set_invalid(0) == SOLID_OK);
    assert(ftl_page_set_invalid(0) == ERR_FTL_INTERNAL);      /* 이미 무효 */
    assert(info(0, 0).valid_pages == 0 && info(0, 0).invalid_pages == 64);

    assert(ftl_page_set_valid(1536) == ERR_FTL_RANGE);        /* 활성 영역 밖 */
    assert(ftl_page_set_valid(2 * FTL_PPN_PER_CHIP) == ERR_FTL_RANGE);  /* 연결 안 된 칩 */
    assert(ftl_alloc_page(false, NULL) == ERR_FTL_INVALID_ARG);
}

static void test_gc_reserve(void)
{
    ppn_t p;
    uint32_t host = 0;
    while (ftl_alloc_page(false, &p) == SOLID_OK) {
        host++;
    }
    /* OPEN Block (0,1) 남은 63 Page + Free 43개 중 마지막 1개를 뺀 42 Block */
    assert(host == 63 + 42 * 64);
    assert(ftl_get_free_block_count() == 1);
    assert(ftl_alloc_page(false, &p) == ERR_FTL_NO_SPACE);   /* Host는 마지막 Block 못 씀 */

    for (uint32_t i = 0; i < NAND_PAGES_PER_BLOCK; i++) {     /* GC는 씀 */
        assert(ftl_alloc_page(true, &p) == SOLID_OK);
    }
    assert(ftl_get_free_block_count() == 0);
    assert(ftl_alloc_page(true, &p) == ERR_FTL_NO_SPACE);
}

static void test_nand_error(void)
{
    bad_query_res = NAND_ERR_IO;
    assert(ftl_alloc_init(1) == ERR_NAND_IO);
    ppn_t p;
    assert(ftl_alloc_page(false, &p) == ERR_FTL_NOT_READY);   /* 실패한 초기화는 사용 불가 */
    bad_query_res = NAND_OK;

    erase_res[0][0] = NAND_ERR_WRITE_PROTECTED;               /* 보호 해제 안 된 칩 */
    assert(ftl_alloc_init(1) == ERR_NAND_WRITE_PROTECTED);
    erase_res[0][0] = NAND_OK;
}

int main(void)
{
    test_before_init();
    test_init_bad_blocks();
    test_sequential_alloc_and_valid();
    test_gc_reserve();
    test_nand_error();
    puts("ftl alloc: all tests passed");
    return 0;
}
