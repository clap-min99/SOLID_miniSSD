#ifndef SOLID_FTL_ALLOC_H
#define SOLID_FTL_ALLOC_H

#include <stdbool.h>
#include <stdint.h>
#include "solid_types.h"
#include "address_mapper.h"
#include "ftl_gc.h"

/* Block 표와 Page 할당기 (FTL-02). FTL 내부용, GC는 ftl_gc.h만 include */

#define FTL_GC_RESERVED_BLOCKS 1U  /* 마지막 Free Block 1개는 GC 전용 */

/* 칩 0 ~ chip_count-1 의 관리 영역 준비: 공장 Bad Block은 BAD, 나머지는 Erase 후 FREE */
solid_err_t ftl_alloc_init(uint8_t chip_count);

/* 다음 빈 Page 하나를 예약. Host(for_gc = false)는 GC 예비 Block을 열지 못한다 */
solid_err_t ftl_alloc_page(bool for_gc, ppn_t *out);

/* Program 성공한 Page를 유효로 / 덮어써져 버려진 Page를 무효로 */
solid_err_t ftl_page_set_valid(ppn_t ppn);
solid_err_t ftl_page_set_invalid(ppn_t ppn);

/* ppn이 속한 Block의 Correctable ECC 횟수를 1 올리고 새 값을 돌려준다 (범위 밖이면 0) */
uint8_t ftl_block_note_ecc_corrected(ppn_t ppn);

/* 드라이버 결과(NAND_ERR_*)를 공통 코드(ERR_NAND_*)로 바꾼다 */
solid_err_t ftl_from_nand(nand_result_t r);

#endif /* SOLID_FTL_ALLOC_H */
