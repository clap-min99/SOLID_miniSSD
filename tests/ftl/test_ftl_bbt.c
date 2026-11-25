/* 영속 BBT: 재부팅(ftl_alloc_init 다시 호출) 뒤에도 불량 Block을 기억하는지. Mock NAND 사용 */
#include <assert.h>
#include <stdio.h>
#include "ftl_alloc.h"
#include "ftl_bbt.h"
#include "mock_nand.h"

static ftl_block_info_t info(uint16_t block)
{
    ftl_block_info_t bi;
    assert(ftl_get_block_info(0, block, &bi) == SOLID_OK);
    return bi;
}

static void fresh_chip(void)
{
    mock_nand_config_t c = mock_nand_default_config();
    c.chip_count = 1;
    mock_nand_reset(&c);
    assert(nand_init() == NAND_OK);
}

/* 처음 부팅: 공장 불량을 찾아 BBT에 저장. 이후 부팅: 공장 검사 없이 BBT만 믿는다 */
static void test_first_and_next_boot(void)
{
    fresh_chip();
    mock_nand_mark_factory_bad(0, 3);
    assert(ftl_alloc_init(1) == SOLID_OK);
    assert(info(3).state == FTL_BLK_BAD);
    assert(ftl_bbt_count() == 1);
    assert(ftl_get_free_block_count() == 23);

    /* 다음 부팅: BBT에서 읽음. 공장 검사를 다시 안 하는 건 test_erase_fail_persists에서
     * (공장 검사로는 정상인 Block 0이 BBT 때문에 BAD로 남는지)로 확인 */
    assert(ftl_alloc_init(1) == SOLID_OK);
    assert(info(3).state == FTL_BLK_BAD);
    assert(ftl_bbt_count() == 1);
    assert(ftl_get_free_block_count() == 23);
}

/* 부팅 중 Erase 실패와 GC 회수 중 Erase 실패가 둘 다 다음 부팅까지 남는다 */
static void test_erase_fail_persists(void)
{
    fresh_chip();
    mock_nand_inject(MOCK_OP_ERASE, 0, 7, NAND_ERR_ERASE_FAIL, 1);
    assert(ftl_alloc_init(1) == SOLID_OK);
    assert(info(7).state == FTL_BLK_BAD);

    /* Block 0을 채워 CLOSED → Victim → 회수 중 Erase 실패 */
    ppn_t p;
    for (unsigned i = 0; i < NAND_PAGES_PER_BLOCK; i++) {
        assert(ftl_alloc_page(false, &p) == SOLID_OK);
        assert(ftl_page_set_valid(p) == SOLID_OK);
        assert(ftl_page_set_invalid(p) == SOLID_OK);
    }
    assert(ftl_alloc_page(false, &p) == SOLID_OK);   /* Block 0이 CLOSED가 되고 다음 Block이 열림 */
    assert(info(0).state == FTL_BLK_CLOSED);
    assert(ftl_reserve_victim(0, 0) == SOLID_OK);
    mock_nand_inject(MOCK_OP_ERASE, 0, 0, NAND_ERR_ERASE_FAIL, 1);
    assert(ftl_reclaim_block(0, 0) == ERR_NAND_ERASE_FAIL);
    assert(info(0).state == FTL_BLK_BAD);

    mock_nand_clear_injections();                  /* 재부팅: 이제 Erase는 성공하지만 */
    assert(ftl_alloc_init(1) == SOLID_OK);
    assert(info(0).state == FTL_BLK_BAD);          /* BBT가 기억해서 다시 안 쓴다 */
    assert(info(7).state == FTL_BLK_BAD);
    assert(ftl_bbt_count() == 2);
    assert(mock_nand_violations() == 0);
}

/* 64번 넘게 저장하면 반대쪽 Block으로 넘어가고, 그래도 최신 목록을 읽는다 */
static void test_wraps_between_blocks(void)
{
    fresh_chip();
    assert(ftl_alloc_init(1) == SOLID_OK);         /* 저장 1번 */
    for (uint16_t i = 0; i < 150; i++) {           /* 저장 150번 → 1023 → 1022 → 1023 */
        assert(ftl_bbt_add(0, (uint16_t)(i % 20 == 0 ? 10 + i / 20 : 10)) == SOLID_OK);
        assert(ftl_bbt_save() == SOLID_OK);
    }
    uint16_t n = ftl_bbt_count();
    bool found;
    assert(ftl_bbt_load(&found) == SOLID_OK && found);
    assert(ftl_bbt_count() == n);
    assert(ftl_bbt_is_bad(0, 10) && ftl_bbt_is_bad(0, 17));
}

/* BBT Block 하나가 공장 불량이면 그다음 후보를 쓴다 */
static void test_skips_bad_candidate(void)
{
    fresh_chip();
    mock_nand_mark_factory_bad(0, FTL_BBT_FIRST_CANDIDATE);
    mock_nand_mark_factory_bad(0, 3);
    assert(ftl_alloc_init(1) == SOLID_OK);
    assert(ftl_alloc_init(1) == SOLID_OK);
    assert(info(3).state == FTL_BLK_BAD && ftl_bbt_count() == 1);
}

int main(void)
{
    test_first_and_next_boot();
    test_erase_fail_persists();
    test_wraps_between_blocks();
    test_skips_bad_candidate();
    mock_nand_destroy();
    puts("ftl bbt: all tests passed");
    return 0;
}
