#ifndef SOLID_ADDRESS_MAPPER_H
#define SOLID_ADDRESS_MAPPER_H

#include <stdbool.h>
#include <stdint.h>
#include "nand.h"

typedef uint16_t ppn_t;
#define INVALID_PPN UINT16_MAX

/*
 * Chip-major 고정 슬롯: 칩마다 PPN 4096개(64 Block)를 예약한다.
 * 처음에는 앞 24 Block만 활성(할당 가능)이고, 나중에 활성 영역을
 * 늘려도 기존 PPN이 가리키는 주소는 바뀌지 않는다.
 *
 * ponytail: 관리 영역이 각 칩의 Block 0부터 시작한다고 가정.
 *           팀에서 확정되면 FTL_FIRST_BLOCK을 수정할 것.
 */
#define FTL_FIRST_BLOCK             0U
#define FTL_RESERVED_BLOCKS_PER_CHIP 64U
#define FTL_ACTIVE_BLOCKS_PER_CHIP   24U
#define FTL_PPN_PER_CHIP  (FTL_RESERVED_BLOCKS_PER_CHIP * NAND_PAGES_PER_BLOCK)
#define FTL_PPN_COUNT     (NAND_MAX_CHIPS * FTL_PPN_PER_CHIP)

/* 순수 주소 계산. Bad Block 여부는 보지 않는다. */
bool ppn_to_addr(ppn_t ppn, nand_addr_t *addr);
bool addr_to_ppn(const nand_addr_t *addr, ppn_t *ppn);

/* 예약된 유효 주소와 현재 할당 가능한 주소는 서로 다르다. */
bool ppn_is_valid(ppn_t ppn);
bool ppn_is_active(ppn_t ppn);

#endif /* SOLID_ADDRESS_MAPPER_H */
