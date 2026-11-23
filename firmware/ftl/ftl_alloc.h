#ifndef SOLID_FTL_ALLOC_H
#define SOLID_FTL_ALLOC_H

#include <stdbool.h>
#include <stdint.h>
#include "solid_types.h"
#include "address_mapper.h"

/*
 * Block 표와 Page 할당기 (FTL-02)
 * 상태 전이·예비 Block 규칙은 docs/ftl-gc-api.md 5.2.1 (FTL↔GC 합의).
 * ftl_block_state_t / ftl_block_info_t 는 GC의 gc_ftl_if.h v0.2와 필드가 같다.
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

#define FTL_GC_RESERVED_BLOCKS 1U  /* 마지막 Free Block 1개는 GC 전용 */

/* 칩 0 ~ chip_count-1 의 관리 영역 준비: 공장 Bad Block은 BAD, 나머지는 Erase 후 FREE */
solid_err_t ftl_alloc_init(uint8_t chip_count);

/* 다음 빈 Page 하나를 예약. Host(for_gc = false)는 GC 예비 Block을 열지 못한다 */
solid_err_t ftl_alloc_page(bool for_gc, ppn_t *out);

/* Program 성공한 Page를 유효로 / 덮어써져 버려진 Page를 무효로 */
solid_err_t ftl_page_set_valid(ppn_t ppn);
solid_err_t ftl_page_set_invalid(ppn_t ppn);

/* GC 조회 (gc_ftl_if.h v0.2와 같은 이름) */
solid_err_t ftl_get_block_info(uint8_t chip, uint16_t block, ftl_block_info_t *out);
uint16_t    ftl_get_free_block_count(void);

/* FTL 내부용: 드라이버 결과(NAND_ERR_*)를 공통 코드(ERR_NAND_*)로 바꾼다 */
solid_err_t ftl_from_nand(nand_result_t r);

#endif /* SOLID_FTL_ALLOC_H */
