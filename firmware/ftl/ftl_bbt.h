#ifndef SOLID_FTL_BBT_H
#define SOLID_FTL_BBT_H

#include <stdbool.h>
#include <stdint.h>
#include "solid_types.h"

/*
 * 영속 Bad Block Table (FTL 내부용).
 * 칩 0의 정상 Block 2개(FTL_BBT_FIRST_CANDIDATE부터 아래로 찾음)를 번갈아 쓰고,
 * 불량 Block 목록은 2KiB Page 한 장에 저장한다. 새 목록은 다음 Page에 이어 쓰고
 * Block이 차면 다른 Block을 지우고 넘어간다 → 쓰는 도중 전원이 꺼져도 이전 목록이 남는다.
 */

#define FTL_BBT_FIRST_CANDIDATE 1023U   /* 칩 0 맨 끝 Block부터 */
#define FTL_BBT_CANDIDATES      8U      /* 1023 ~ 1016 중 공장 정상인 앞 2개 */
#define FTL_BBT_MAX_ENTRIES     192U    /* NAND_MAX_CHIPS × FTL_ACTIVE_BLOCKS_PER_CHIP */

/* NAND에서 BBT를 찾는다. *found = false면 처음 부팅(포맷 필요). 목록은 비운 상태로 시작 */
solid_err_t ftl_bbt_load(bool *found);

bool ftl_bbt_is_bad(uint8_t chip, uint16_t block);

/* 목록에 추가 (RAM만). 이미 있으면 무시. 가득 차면 ERR_FTL_INTERNAL */
solid_err_t ftl_bbt_add(uint8_t chip, uint16_t block);

/* 현재 목록을 NAND에 새 Page로 저장 */
solid_err_t ftl_bbt_save(void);

uint16_t ftl_bbt_count(void);

#endif /* SOLID_FTL_BBT_H */
