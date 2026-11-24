#include "ftl.h"
#include "ftl_alloc.h"
#include "ftl_map.h"
#include "gc.h"

#define FTL_PROGRAM_RETRY 1U   /* Program 실패 시 다른 Page로 다시 쓰는 횟수 */

/* 한 Block에서 Correctable ECC가 이 횟수 이상 나오면 읽은 Page를 새 Page로 옮긴다 (read reclaim).
 * W25N01GV는 고친 비트 수를 알려주지 않아서 횟수로 판단. 실제 칩 상태를 보고 조정할 값 */
#define FTL_ECC_RELOCATE_THRESHOLD 3U

static bool ready;
static uint32_t ecc_corrected;   /* 칩이 비트 오류를 고쳐서 돌려준 읽기 횟수 */
static uint8_t gc_buf[NAND_PAGE_SIZE];   /* GC 이동 전용 (HIL 버퍼와 분리, 11-20 합의) */

uint32_t ftl_get_ecc_corrected_count(void)
{
    return ecc_corrected;
}

solid_err_t ftl_init(uint8_t chip_count)
{
    ready = false;
    ecc_corrected = 0;
    ftl_map_init();
    solid_err_t e = ftl_alloc_init(chip_count);
    if (e != SOLID_OK) {
        return e;
    }
    gc_config_t cfg = gc_default_config();
    cfg.chip_count = chip_count;
    if (gc_init(&cfg) != SOLID_OK) {
        return ERR_FTL_INTERNAL;
    }
    ready = true;
    return SOLID_OK;
}

/* 새 Page 하나에 buf를 Program하고, 성공하면 lpn을 그쪽으로 옮긴다 (old는 무효).
 * Program 실패는 NAND_ERR_PROGRAM_FAIL 그대로 돌려줘서 호출 측이 재시도 여부를 정한다 */
static solid_err_t program_and_remap(uint16_t lpn, ppn_t old, const uint8_t *buf, bool for_gc,
                                     nand_result_t *nand_res)
{
    ppn_t ppn;
    nand_addr_t addr;
    *nand_res = NAND_OK;
    solid_err_t e = ftl_alloc_page(for_gc, &ppn);
    if (e != SOLID_OK) {
        return e;
    }
    if (!ppn_to_addr(ppn, &addr)) {
        return ERR_FTL_INTERNAL;
    }
    *nand_res = nand_program_page(addr, buf);
    if (*nand_res != NAND_OK) {
        return ftl_from_nand(*nand_res);   /* 이 Page는 버림 (쓴 것도 유효도 아님 → 무효로 셈) */
    }
    if ((e = ftl_page_set_valid(ppn)) != SOLID_OK) {
        return e;
    }
    if (old != INVALID_PPN && (e = ftl_page_set_invalid(old)) != SOLID_OK) {
        return e;
    }
    return ftl_map_set(lpn, ppn) ? SOLID_OK : ERR_FTL_INTERNAL;
}

/* Free가 기준 이하이면 GC를 돌려 빈 Block을 만든다.
 * 회수할 게 없거나 이동·Erase 실패는 넘어가고(할당이 NO_SPACE로 판단), 장치 고장·버그만 돌려준다 */
static solid_err_t make_room(void)
{
    /* 성공한 회수마다 무효 Page가 줄어서 끝나지만, 실패가 반복될 때를 위한 상한 */
    for (unsigned n = 0; n < NAND_MAX_CHIPS * FTL_ACTIVE_BLOCKS_PER_CHIP; n++) {
        if (!gc_should_run(ftl_get_free_block_count())) {
            return SOLID_OK;
        }
        solid_err_t e = gc_collect(NULL);
        switch (e) {
        case SOLID_OK:
        case ERR_FTL_SRC_READ:       /* GC가 Skip 목록에 넣음 → 다음 Victim */
        case ERR_FTL_DST_PROGRAM:
        case ERR_FTL_VALID_REMAINS:
        case ERR_NAND_ERASE_FAIL:    /* FTL이 BAD 처리 */
            continue;
        case ERR_GC_NO_SPACE:        /* 회수할 Victim 없음 */
        case ERR_FTL_NO_SPACE:       /* 옮길 자리 없음 */
            return SOLID_OK;
        default:                     /* NAND 장치 오류·버그 → 그대로 위로 */
            return e;
        }
    }
    return SOLID_OK;
}

solid_err_t ftl_write(uint16_t lpn, const uint8_t *buf)
{
    ppn_t old;
    if (!ready) {
        return ERR_FTL_NOT_READY;
    }
    if (buf == NULL) {
        return ERR_FTL_INVALID_ARG;
    }
    if (lpn >= FTL_LOGICAL_PAGE_COUNT) {
        return ERR_FTL_RANGE;
    }
    solid_err_t e = make_room();
    if (e != SOLID_OK) {
        return e;
    }
    (void)ftl_map_get(lpn, &old);   /* GC 뒤에 읽는다: GC가 이 LPN을 옮겼을 수 있음 */
    for (unsigned attempt = 0; attempt <= FTL_PROGRAM_RETRY; attempt++) {
        nand_result_t r;
        e = program_and_remap(lpn, old, buf, false, &r);
        if (r != NAND_ERR_PROGRAM_FAIL) {
            return e;   /* 성공, 공간 부족, TIMEOUT·IO 등은 재시도하지 않음 */
        }
    }
    return ERR_NAND_PROGRAM_FAIL;
}

solid_err_t ftl_gc_move_one(uint8_t chip, uint16_t block, bool *moved)
{
    ppn_t src;
    nand_addr_t addr;
    nand_ecc_t ecc = NAND_ECC_UNKNOWN;
    if (moved == NULL) {
        return ERR_FTL_INVALID_ARG;
    }
    *moved = false;
    if (!ready) {
        return ERR_FTL_NOT_READY;
    }
    solid_err_t e = ftl_victim_next_valid(chip, block, &src);
    if (e != SOLID_OK || src == INVALID_PPN) {
        return e;   /* 남은 유효 Page 없음이면 SOLID_OK + moved=false */
    }

    /* PPN → LPN: L2P를 훑는다. 매번 찾으니 옛 목록으로 잘못 덮어쓸 일이 없다.
     * ponytail: 이동마다 8,192칸 스캔. GC가 느리면 Victim마다 1회 스캔한 목록으로 바꿀 것 */
    uint16_t lpn = 0;
    ppn_t p;
    while (ftl_map_get(lpn, &p) && p != src) {
        lpn++;
    }
    if (p != src) {
        return ERR_FTL_INTERNAL;   /* 유효 표시인데 가리키는 LPN이 없음 */
    }

    if (!ppn_to_addr(src, &addr)) {
        return ERR_FTL_INTERNAL;
    }
    nand_result_t r = nand_read_page(addr, gc_buf, &ecc);
    if (r == NAND_ERR_ECC_UNCORRECTABLE || (r == NAND_OK && ecc == NAND_ECC_UNCORRECTABLE)) {
        return ERR_FTL_SRC_READ;
    }
    if (r != NAND_OK) {
        return ftl_from_nand(r);   /* SPI·타임아웃은 장치 문제로 그대로 */
    }
    if (ecc == NAND_ECC_UNKNOWN) {
        return ERR_FTL_SRC_READ;
    }

    e = program_and_remap(lpn, src, gc_buf, true, &r);
    if (r == NAND_ERR_PROGRAM_FAIL) {
        return ERR_FTL_DST_PROGRAM;
    }
    if (e == SOLID_OK) {
        *moved = true;
    }
    return e;
}

solid_err_t ftl_read(uint16_t lpn, uint8_t *buf)
{
    ppn_t ppn;
    nand_addr_t addr;
    nand_ecc_t ecc = NAND_ECC_UNKNOWN;
    if (!ready) {
        return ERR_FTL_NOT_READY;
    }
    if (buf == NULL) {
        return ERR_FTL_INVALID_ARG;
    }
    if (!ftl_map_get(lpn, &ppn)) {
        return ERR_FTL_RANGE;
    }
    if (ppn == INVALID_PPN) {
        return ERR_FTL_UNMAPPED;
    }
    if (!ppn_to_addr(ppn, &addr)) {
        return ERR_FTL_INTERNAL;
    }

    nand_result_t r = nand_read_page(addr, buf, &ecc);
    if (r != NAND_OK) {
        return ftl_from_nand(r);
    }
    switch (ecc) {
    case NAND_ECC_NO_ERROR:       return SOLID_OK;
    case NAND_ECC_CORRECTED:      /* 정정됨 = 데이터 정상. 횟수를 세고, Block이 기준을 넘으면 옮겨 쓴다 */
        ecc_corrected++;
        if (ftl_block_note_ecc_corrected(ppn) >= FTL_ECC_RELOCATE_THRESHOLD) {
            (void)ftl_write(lpn, buf);   /* read reclaim: 실패해도 데이터는 이미 정상이라 읽기는 성공 */
        }
        return SOLID_OK;
    case NAND_ECC_UNCORRECTABLE:  return ERR_NAND_ECC_UNCORRECTABLE;
    case NAND_ECC_UNKNOWN:        return ERR_NAND_IO;  /* ECC 결과를 모름 → 데이터를 믿을 수 없음 */
    }
    return ERR_FTL_INTERNAL;
}
