#ifndef SOLID_FTL_H
#define SOLID_FTL_H

#include <stdint.h>
#include "solid_types.h"

/* HIL이 부르는 FTL 진입점 (FTL-03~06). buf는 항상 2KiB(NAND_PAGE_SIZE) */

/* L2P와 Block 표를 초기화. chip_count = 실제 연결된 NAND 수 */
solid_err_t ftl_init(uint8_t chip_count);

/* 새 Page에 쓰고 매핑을 바꾼다 (Out-of-place). 실패하면 기존 매핑·데이터 유지 */
solid_err_t ftl_write(uint16_t lpn, const uint8_t *buf);

/* 쓴 적 없는 LPN은 ERR_FTL_UNMAPPED (팀 정책 확정 전 임시) */
solid_err_t ftl_read(uint16_t lpn, uint8_t *buf);

#endif /* SOLID_FTL_H */
