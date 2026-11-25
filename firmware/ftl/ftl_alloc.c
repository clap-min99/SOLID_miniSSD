#include "ftl_alloc.h"
#include "ftl_bbt.h"

#define BLOCKS_MAX (NAND_MAX_CHIPS * FTL_ACTIVE_BLOCKS_PER_CHIP)  /* 192 */

typedef struct {
    uint64_t valid_map;      /* bit n = Page n 유효 */
    uint32_t erase_count;
    uint8_t  state;          /* ftl_block_state_t */
    uint8_t  write_pointer;
    uint8_t  valid_pages;
    uint8_t  ecc_corrected;  /* 지운 뒤 Correctable ECC 읽기 횟수 (남던 1바이트 사용) */
} block_t;                   /* 16B × 192 = 3KiB */

static block_t  blocks[BLOCKS_MAX];
static uint16_t block_count;  /* chip_count × 24, 0이면 초기화 전 */
static uint16_t free_count;
static int16_t  open_idx = -1;

solid_err_t ftl_from_nand(nand_result_t r)
{
    switch (r) {   /* default 없음: nand.h에 코드가 늘면 -Wswitch 경고로 알 수 있게 */
    case NAND_OK:                    return SOLID_OK;
    case NAND_ERR_INVALID_ARG:       return ERR_FTL_INTERNAL;  /* FTL이 잘못 호출한 버그 */
    case NAND_ERR_NOT_INITIALIZED:   return ERR_NAND_NOT_INITIALIZED;
    case NAND_ERR_NO_DEVICE:         return ERR_NAND_NO_DEVICE;
    case NAND_ERR_TIMEOUT:           return ERR_NAND_TIMEOUT;
    case NAND_ERR_IO:                return ERR_NAND_IO;
    case NAND_ERR_WRITE_PROTECTED:   return ERR_NAND_WRITE_PROTECTED;
    case NAND_ERR_PROGRAM_FAIL:      return ERR_NAND_PROGRAM_FAIL;
    case NAND_ERR_ERASE_FAIL:        return ERR_NAND_ERASE_FAIL;
    case NAND_ERR_ECC_UNCORRECTABLE: return ERR_NAND_ECC_UNCORRECTABLE;
    }
    return ERR_FTL_INTERNAL;
}

static ppn_t idx_to_ppn(uint16_t i, uint8_t page)
{
    return (ppn_t)((i / FTL_ACTIVE_BLOCKS_PER_CHIP) * FTL_PPN_PER_CHIP +
                   (i % FTL_ACTIVE_BLOCKS_PER_CHIP) * NAND_PAGES_PER_BLOCK + page);
}

static bool ppn_to_idx(ppn_t ppn, uint16_t *i, uint8_t *page)
{
    if (!ppn_is_active(ppn)) {
        return false;
    }
    uint16_t local = ppn % FTL_PPN_PER_CHIP;
    uint32_t idx = (ppn / FTL_PPN_PER_CHIP) * FTL_ACTIVE_BLOCKS_PER_CHIP + local / NAND_PAGES_PER_BLOCK;
    if (idx >= block_count) {   /* 연결 안 된 칩 */
        return false;
    }
    *i = (uint16_t)idx;
    *page = (uint8_t)(local % NAND_PAGES_PER_BLOCK);
    return true;
}

solid_err_t ftl_alloc_init(uint8_t chip_count)
{
    if (chip_count == 0 || chip_count > NAND_MAX_CHIPS) {
        return ERR_FTL_INVALID_ARG;
    }
    block_count = 0;   /* 도중에 실패하면 초기화 전 상태로 남긴다 */
    free_count = 0;
    open_idx = -1;

    /* BBT가 있으면 그걸 믿고, 없으면(처음 부팅) 공장 마커를 검사해 BBT를 만든다.
     * 데이터를 쓴 Block은 Main[0]이 FF가 아니라서 공장 검사를 다시 하면 BAD로 오판된다 */
    bool have_bbt;
    solid_err_t e = ftl_bbt_load(&have_bbt);
    if (e != SOLID_OK) {
        return e;
    }
    bool dirty = !have_bbt;

    uint16_t n = (uint16_t)(chip_count * FTL_ACTIVE_BLOCKS_PER_CHIP);
    for (uint16_t i = 0; i < n; i++) {
        uint8_t  chip  = (uint8_t)(i / FTL_ACTIVE_BLOCKS_PER_CHIP);
        uint16_t block = (uint16_t)(FTL_FIRST_BLOCK + i % FTL_ACTIVE_BLOCKS_PER_CHIP);
        block_t *b = &blocks[i];
        bool bad = false;

        *b = (block_t){ .state = FTL_BLK_BAD };
        if (have_bbt) {
            bad = ftl_bbt_is_bad(chip, block);
        } else {
            nand_result_t r = nand_is_factory_bad_block(chip, block, &bad);
            if (r != NAND_OK) {
                return ftl_from_nand(r);
            }
            if (bad && (e = ftl_bbt_add(chip, block)) != SOLID_OK) {
                return e;
            }
        }
        if (bad) {
            continue;
        }
        /* ponytail: 체크포인트 전이라 부팅마다 관리 영역을 전부 Erase(포맷). 복원이 생기면 여기서 대신 복원 */
        nand_result_t r = nand_erase_block(chip, block);
        if (r == NAND_ERR_ERASE_FAIL || r == NAND_ERR_TIMEOUT) {
            if ((e = ftl_bbt_add(chip, block)) != SOLID_OK) {   /* 지워졌는지 모르는 Block은 BAD */
                return e;
            }
            dirty = true;
            continue;
        }
        if (r != NAND_OK) {
            return ftl_from_nand(r);
        }
        b->state = FTL_BLK_FREE;
        b->erase_count = 1;
        free_count++;
    }
    if (dirty && (e = ftl_bbt_save()) != SOLID_OK) {
        return e;
    }
    block_count = n;
    return SOLID_OK;
}

solid_err_t ftl_alloc_page(bool for_gc, ppn_t *out)
{
    if (out == NULL) {
        return ERR_FTL_INVALID_ARG;
    }
    if (block_count == 0) {
        return ERR_FTL_NOT_READY;
    }
    if (open_idx < 0) {
        uint16_t need = for_gc ? 1U : FTL_GC_RESERVED_BLOCKS + 1U;
        if (free_count < need) {
            return ERR_FTL_NO_SPACE;
        }
        /* 가장 덜 지워진 FREE Block, 같으면 번호가 작은 것 */
        int16_t pick = -1;
        for (uint16_t i = 0; i < block_count; i++) {
            if (blocks[i].state == FTL_BLK_FREE &&
                (pick < 0 || blocks[i].erase_count < blocks[pick].erase_count)) {
                pick = (int16_t)i;
            }
        }
        blocks[pick].state = FTL_BLK_OPEN;   /* free_count > 0 이므로 pick은 반드시 있음 */
        free_count--;
        open_idx = pick;
    }
    block_t *b = &blocks[open_idx];
    *out = idx_to_ppn((uint16_t)open_idx, b->write_pointer);
    if (++b->write_pointer == NAND_PAGES_PER_BLOCK) {
        b->state = FTL_BLK_CLOSED;
        open_idx = -1;
    }
    return SOLID_OK;
}

solid_err_t ftl_page_set_valid(ppn_t ppn)
{
    uint16_t i;
    uint8_t page;
    if (block_count == 0) {
        return ERR_FTL_NOT_READY;
    }
    if (!ppn_to_idx(ppn, &i, &page)) {
        return ERR_FTL_RANGE;
    }
    block_t *b = &blocks[i];
    if (page >= b->write_pointer || (b->valid_map >> page & 1U)) {
        return ERR_FTL_INTERNAL;   /* 할당 안 된 Page이거나 이미 유효 */
    }
    b->valid_map |= 1ULL << page;
    b->valid_pages++;
    return SOLID_OK;
}

solid_err_t ftl_page_set_invalid(ppn_t ppn)
{
    uint16_t i;
    uint8_t page;
    if (block_count == 0) {
        return ERR_FTL_NOT_READY;
    }
    if (!ppn_to_idx(ppn, &i, &page)) {
        return ERR_FTL_RANGE;
    }
    block_t *b = &blocks[i];
    if (!(b->valid_map >> page & 1U)) {
        return ERR_FTL_INTERNAL;   /* 유효하지 않은 Page를 무효화하려 함 */
    }
    b->valid_map &= ~(1ULL << page);
    b->valid_pages--;
    return SOLID_OK;
}

/* (chip, 칩 내 절대 block) → blocks[] 칸 */
static solid_err_t find_block(uint8_t chip, uint16_t block, block_t **out)
{
    if (block_count == 0) {
        return ERR_FTL_NOT_READY;
    }
    uint32_t local = (uint32_t)block - FTL_FIRST_BLOCK;   /* FIRST보다 작으면 wrap되어 큰 값 */
    uint32_t idx = (uint32_t)chip * FTL_ACTIVE_BLOCKS_PER_CHIP + local;
    if (local >= FTL_ACTIVE_BLOCKS_PER_CHIP || idx >= block_count) {
        return ERR_FTL_RANGE;
    }
    *out = &blocks[idx];
    return SOLID_OK;
}

solid_err_t ftl_get_block_info(uint8_t chip, uint16_t block, ftl_block_info_t *out)
{
    block_t *b;
    if (out == NULL) {
        return ERR_FTL_INVALID_ARG;
    }
    solid_err_t e = find_block(chip, block, &b);
    if (e != SOLID_OK) {
        return e;
    }
    out->state = (ftl_block_state_t)b->state;
    out->write_pointer = b->write_pointer;
    out->valid_pages = b->valid_pages;
    out->invalid_pages = (uint16_t)(b->write_pointer - b->valid_pages);
    out->erase_count = b->erase_count;
    return SOLID_OK;
}

uint16_t ftl_get_free_block_count(void)
{
    return free_count;
}

/* from 상태인 Block을 to로. 아니면 ERR_FTL_BAD_STATE */
static solid_err_t move_state(uint8_t chip, uint16_t block, uint8_t from, uint8_t to)
{
    block_t *b;
    solid_err_t e = find_block(chip, block, &b);
    if (e != SOLID_OK) {
        return e;
    }
    if (b->state != from) {
        return ERR_FTL_BAD_STATE;
    }
    b->state = to;
    return SOLID_OK;
}

solid_err_t ftl_reserve_victim(uint8_t chip, uint16_t block)
{
    return move_state(chip, block, FTL_BLK_CLOSED, FTL_BLK_RESERVED);
}

solid_err_t ftl_unreserve_victim(uint8_t chip, uint16_t block)
{
    return move_state(chip, block, FTL_BLK_RESERVED, FTL_BLK_CLOSED);
}

solid_err_t ftl_victim_next_valid(uint8_t chip, uint16_t block, ppn_t *ppn)
{
    block_t *b;
    solid_err_t e = find_block(chip, block, &b);
    if (e != SOLID_OK) {
        return e;
    }
    if (b->state != FTL_BLK_RESERVED) {
        return ERR_FTL_BAD_STATE;
    }
    *ppn = INVALID_PPN;
    for (uint8_t p = 0; p < NAND_PAGES_PER_BLOCK; p++) {
        if (b->valid_map >> p & 1U) {
            *ppn = idx_to_ppn((uint16_t)(b - blocks), p);
            break;
        }
    }
    return SOLID_OK;
}

solid_err_t ftl_reclaim_block(uint8_t chip, uint16_t block)
{
    block_t *b;
    solid_err_t e = find_block(chip, block, &b);
    if (e != SOLID_OK) {
        return e;
    }
    if (b->state != FTL_BLK_RESERVED) {
        return ERR_FTL_BAD_STATE;
    }
    if (b->valid_pages != 0) {
        return ERR_FTL_VALID_REMAINS;
    }
    nand_result_t r = nand_erase_block(chip, block);
    if (r == NAND_ERR_ERASE_FAIL || r == NAND_ERR_TIMEOUT) {
        b->state = FTL_BLK_BAD;          /* 지워졌는지 모름 → 다시 안 씀 */
        /* 재부팅 후에도 빼도록 BBT에 기록. 실패해도 GC 계약대로 ERASE_FAIL을 돌려준다
         * ponytail: BBT 저장 실패는 이번 부팅 동안만 BAD로 남음. 알려야 하면 별도 카운터 추가 */
        if (ftl_bbt_add(chip, block) == SOLID_OK) {
            (void)ftl_bbt_save();
        }
        return ERR_NAND_ERASE_FAIL;
    }
    if (r != NAND_OK) {
        return ftl_from_nand(r);         /* 상태 유지(RESERVED), GC가 CLOSED로 되돌림 */
    }
    uint32_t erases = b->erase_count + 1U;
    *b = (block_t){ .state = FTL_BLK_FREE, .erase_count = erases };   /* 비트맵·wp·ECC 횟수 0 */
    free_count++;
    return SOLID_OK;
}

uint8_t ftl_block_note_ecc_corrected(ppn_t ppn)
{
    uint16_t i;
    uint8_t page;
    if (block_count == 0 || !ppn_to_idx(ppn, &i, &page)) {
        return 0;
    }
    if (blocks[i].ecc_corrected < UINT8_MAX) {
        blocks[i].ecc_corrected++;
    }
    return blocks[i].ecc_corrected;
}
