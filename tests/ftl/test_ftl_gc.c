/* FTL-07: FTL + GC + Mock NAND 통합 테스트 (칩 1개 = 24 Block = 1,536 Page) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ftl.h"
#include "ftl_alloc.h"
#include "ftl_map.h"
#include "gc.h"
#include "mock_nand.h"

#define WORKSET 600U   /* 겹쳐 쓰는 LPN 수 (물리 1,536 Page의 약 40%) */

static uint8_t w[NAND_PAGE_SIZE], r[NAND_PAGE_SIZE];
static uint8_t gen[WORKSET];   /* LPN별 마지막으로 쓴 세대 */

static void pattern(uint16_t lpn, uint8_t g)
{
    for (unsigned i = 0; i < NAND_PAGE_SIZE; i++) {
        w[i] = (uint8_t)(lpn * 7U + g * 13U + i);
    }
}

static void write_ok(uint16_t lpn, uint8_t g)
{
    pattern(lpn, g);
    assert(ftl_write(lpn, w) == SOLID_OK);
    gen[lpn] = g;
}

/* LPN WORKSET개를 무작위 순서로 덮어씀 → Block마다 유효·무효가 섞여 GC가 Page를 옮기게 됨 */
static void random_round(uint8_t g)
{
    static uint32_t seed = 1;
    for (uint16_t i = 0; i < WORKSET; i++) {
        seed = seed * 1103515245U + 12345U;
        write_ok((uint16_t)((seed >> 16) % WORKSET), g);
    }
}

static void verify_all(void)
{
    for (uint16_t l = 0; l < WORKSET; l++) {
        pattern(l, gen[l]);
        assert(ftl_read(l, r) == SOLID_OK);
        assert(memcmp(r, w, sizeof r) == 0);
    }
}

static void setup(void)
{
    mock_nand_config_t c = mock_nand_default_config();
    c.chip_count = 1;
    c.strict_sequential = true;   /* Block 안 순차 Program 규칙까지 검사 */
    mock_nand_reset(&c);
    assert(nand_init() == NAND_OK);
    assert(ftl_init(1) == SOLID_OK);
    memset(gen, 0, sizeof gen);
}

int main(void)
{
    gc_stats_t st;

    /* 1. 같은 LPN들을 무작위 순서로 계속 덮어써서 GC가 유효 Page를 옮기게 한다. 데이터·매핑 유지 */
    setup();
    for (uint16_t l = 0; l < WORKSET; l++) {
        write_ok(l, 1);
    }
    for (uint8_t g = 2; g <= 11; g++) {
        random_round(g);
        verify_all();
    }
    gc_get_stats(&st);
    assert(st.blocks_reclaimed > 0 && st.pages_moved > 0);
    assert(mock_nand_violations() == 0);
    assert(ftl_get_free_block_count() >= 1);
    printf("  random overwrite: GC %u회, Block 회수 %u, Page 이동 %u\n",
           (unsigned)st.collect_calls, (unsigned)st.blocks_reclaimed, (unsigned)st.pages_moved);

    /* 2. Erase 실패 → 그 Block은 BAD, 나머지로 계속 동작 */
    mock_nand_inject(MOCK_OP_ERASE, MOCK_ANY_CHIP, MOCK_ANY_BLOCK, NAND_ERR_ERASE_FAIL, 1);
    random_round(12);
    mock_nand_clear_injections();
    unsigned bad = 0;
    for (uint16_t b = 0; b < FTL_ACTIVE_BLOCKS_PER_CHIP; b++) {
        ftl_block_info_t bi;
        assert(ftl_get_block_info(0, b, &bi) == SOLID_OK);
        bad += (bi.state == FTL_BLK_BAD);
        assert(bi.state != FTL_BLK_RESERVED);   /* GC가 끝나면 RESERVED가 남으면 안 됨 */
    }
    assert(bad == 1);
    random_round(13);
    verify_all();

    /* 3. GC 원본 읽기 정정 불가 → 그 Victim은 건너뛰고 쓰기는 계속 성공 */
    mock_nand_inject(MOCK_OP_READ, MOCK_ANY_CHIP, MOCK_ANY_BLOCK, NAND_ERR_ECC_UNCORRECTABLE, 1);
    random_round(14);
    mock_nand_clear_injections();
    gc_get_stats(&st);
    assert(st.move_fail >= 1);
    verify_all();
    assert(mock_nand_violations() == 0);

    /* 4. 직접 호출: 상태 규칙 */
    uint16_t cb = 0;
    ftl_block_info_t bi;
    for (;; cb++) {   /* 유효 Page가 있는 CLOSED Block 하나 */
        assert(cb < FTL_ACTIVE_BLOCKS_PER_CHIP);
        assert(ftl_get_block_info(0, cb, &bi) == SOLID_OK);
        if (bi.state == FTL_BLK_CLOSED && bi.valid_pages > 0) break;
    }
    bool moved;
    assert(ftl_gc_move_one(0, cb, &moved) == ERR_FTL_BAD_STATE);    /* RESERVED 전 */
    assert(ftl_reclaim_block(0, cb) == ERR_FTL_BAD_STATE);
    assert(ftl_reserve_victim(0, cb) == SOLID_OK);
    assert(ftl_reserve_victim(0, cb) == ERR_FTL_BAD_STATE);
    assert(ftl_reclaim_block(0, cb) == ERR_FTL_VALID_REMAINS);      /* 상태 유지 */
    assert(ftl_unreserve_victim(0, cb) == SOLID_OK);
    assert(ftl_unreserve_victim(0, cb) == ERR_FTL_BAD_STATE);
    assert(ftl_reserve_victim(0, FTL_ACTIVE_BLOCKS_PER_CHIP) == ERR_FTL_RANGE);
    assert(ftl_gc_move_one(0, cb, NULL) == ERR_FTL_INVALID_ARG);
    verify_all();

    /* 5. 서로 다른 LPN으로 꽉 채우면 결국 NO_SPACE, 이미 쓴 데이터는 그대로 읽힘 */
    setup();
    uint16_t lpn = 0;
    solid_err_t e;
    do {
        pattern(lpn, 1);
        e = ftl_write(lpn, w);
        if (e == SOLID_OK && lpn < WORKSET) gen[lpn] = 1;
        lpn++;
    } while (e == SOLID_OK && lpn < FTL_LOGICAL_PAGE_COUNT);
    assert(e == ERR_FTL_NO_SPACE);
    verify_all();
    assert(mock_nand_violations() == 0);

    mock_nand_destroy();
    puts("ftl + gc + mock nand: all tests passed");
    return 0;
}
