/*
 * gc_ftl_if.h - GC 가 보는 FTL 경계 (GC 쪽 얇은 래퍼)
 *
 * 공식 정의는 FTL 쪽에 있다 (11-23 박수민님과 합의):
 *   - firmware/ftl/ftl_gc.h     : Block 상태·정보 타입, ftl_get_block_info, ftl_get_free_block_count
 *   - firmware/common/solid_types.h : solid_err_t (SOLID_OK, ERR_FTL_*, ERR_GC_*, ERR_NAND_*)
 *   - firmware/ftl/address_mapper.h : FTL_FIRST_BLOCK, FTL_ACTIVE_BLOCKS_PER_CHIP
 * 이 파일은 그것들을 include 하고, FTL-07 에서 ftl_gc.h 로 들어올 함수만 임시로 선언한다.
 * 상태 전이·예비 Block·오류 처리 규칙: docs/ftl-gc-api.md 5.2.1, docs/ftl-agreement.md 4-2
 */
#ifndef SOLID_GC_FTL_IF_H
#define SOLID_GC_FTL_IF_H

#include <stdbool.h>
#include <stdint.h>
#include "solid_types.h"
#include "ftl_gc.h"
#include "address_mapper.h"

/* ---------------------------------------------------------------- FTL-07 예정 (임시 선언)
 * ftl_gc.h 에 같은 원형이 추가되면 이 블록은 지운다. (같은 원형의 중복 선언은 C 에서 허용) */

/* CLOSED → RESERVED. 이후 새 할당·Victim 선택에서 빠진다 */
solid_err_t ftl_reserve_victim(uint8_t chip, uint16_t block);

/* RESERVED → CLOSED. 이동 실패·회수 거부 시 GC 가 되돌린다 */
solid_err_t ftl_unreserve_victim(uint8_t chip, uint16_t block);

/* Victim 의 다음 유효 Page 하나를 옮긴다. RESERVED 일 때만.
 *   *moved = true : 1 Page 옮김 / false : 남은 유효 Page 없음 → reclaim 으로
 *   실패: ERR_FTL_SRC_READ / ERR_FTL_DST_PROGRAM / ERR_FTL_NO_SPACE (기존 Mapping 유지) */
solid_err_t ftl_gc_move_one(uint8_t chip, uint16_t block, bool *moved);

/* RESERVED 이고 valid == 0 인 Block 을 Erase.
 *   성공 → FREE / Erase 실패 → BAD + ERR_NAND_ERASE_FAIL / 유효 Page 남음 → ERR_FTL_VALID_REMAINS(상태 유지) */
solid_err_t ftl_reclaim_block(uint8_t chip, uint16_t block);

#endif /* SOLID_GC_FTL_IF_H */
