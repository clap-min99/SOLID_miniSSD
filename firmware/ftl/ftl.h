#ifndef SOLID_FTL_H
#define SOLID_FTL_H

#include <stdint.h>
#include "solid_types.h"

#define FTL_LOGICAL_PAGE_COUNT 8192U /* 16MiB / 2KiB */

/* HIL이 부르는 FTL 진입점 (FTL-03~06). buf는 항상 2KiB(NAND_PAGE_SIZE) */

/* L2P와 Block 표를 초기화. chip_count = 실제 연결된 NAND 수 */
solid_err_t ftl_init(uint8_t chip_count);

/* 새 Page에 쓰고 매핑을 바꾼다 (Out-of-place). 실패하면 기존 매핑·데이터 유지 */
solid_err_t ftl_write(uint16_t lpn, const uint8_t *buf);

/* 쓴 적 없는 LPN은 ERR_FTL_UNMAPPED (팀 정책 확정 전 임시) */
solid_err_t ftl_read(uint16_t lpn, uint8_t *buf);

/* 읽기에서 Correctable ECC(칩이 1~4비트를 고침)가 나온 전체 횟수.
 * 데이터는 정상이라 성공 처리. 같은 Block에서 기준 횟수 이상이면 그 Page를 새 Page로 옮긴다 */
uint32_t ftl_get_ecc_corrected_count(void);

#endif /* SOLID_FTL_H */
