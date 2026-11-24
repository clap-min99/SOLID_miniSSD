#ifndef SOLID_FTL_GC_H
#define SOLID_FTL_GC_H

#include <stdbool.h>
#include <stdint.h>
#include "solid_types.h"

/*
 * GC가 include하는 FTL 공개 헤더 (Block 표 조회 + GC용 함수).
 * 할당기 내부 함수는 ftl_alloc.h에만 있다.
 * 상태 전이·예비 Block 규칙은 docs/ftl-gc-api.md 5.2.1 (FTL↔GC 합의).
 */
typedef enum {
    FTL_BLK_FREE = 0,  /* Erase 됨, 할당 대기 */
    FTL_BLK_OPEN,      /* 지금 채우는 중 (전체에서 1개) */
    FTL_BLK_CLOSED,    /* 다 채워짐, Victim 후보 */
    FTL_BLK_BAD,       /* 공장 불량 / Erase 실패 */
    FTL_BLK_RESERVED   /* GC Victim으로 잡힘 */
} ftl_block_state_t;

typedef struct {
    ftl_block_state_t state;
    uint8_t  write_pointer;  /* 다음에 쓸 Page (0~64) */
    uint16_t valid_pages;
    uint16_t invalid_pages;  /* write_pointer - valid_pages */
    uint32_t erase_count;
} ftl_block_info_t;

solid_err_t ftl_get_block_info(uint8_t chip, uint16_t block, ftl_block_info_t *out);
uint16_t    ftl_get_free_block_count(void);

/* CLOSED → RESERVED. 이후 할당·Victim 선택에서 빠진다. 아니면 ERR_FTL_BAD_STATE */
solid_err_t ftl_reserve_victim(uint8_t chip, uint16_t block);

/* RESERVED → CLOSED. 이동 실패·회수 거부 시 GC가 되돌린다 */
solid_err_t ftl_unreserve_victim(uint8_t chip, uint16_t block);

/* Victim(RESERVED)의 유효 Page 하나를 새 Page로 옮기고 매핑을 바꾼다.
 *   *moved = true : 1 Page 옮김 / false : 남은 유효 Page 없음 → reclaim으로
 *   실패: ERR_FTL_SRC_READ(원본 ECC 실패) / ERR_FTL_DST_PROGRAM / ERR_FTL_NO_SPACE (기존 매핑 유지) */
solid_err_t ftl_gc_move_one(uint8_t chip, uint16_t block, bool *moved);

/* RESERVED이고 유효 Page 0인 Block을 Erase.
 *   성공 → FREE / Erase 실패·타임아웃 → BAD + ERR_NAND_ERASE_FAIL / 유효 Page 남음 → ERR_FTL_VALID_REMAINS(상태 유지) */
solid_err_t ftl_reclaim_block(uint8_t chip, uint16_t block);

#endif /* SOLID_FTL_GC_H */
