/*
 * fake_ftl.h - GC 테스트용 가짜 FTL (gc_ftl_if.h 구현)
 *
 * 진짜 FTL(박수민)이 나오기 전까지 블록 표를 배열로 흉내 낸다.
 * 테스트가 표를 직접 채우고, 실패를 주입하고, 호출 순서를 확인할 수 있다.
 */
#ifndef SOLID_FAKE_FTL_H
#define SOLID_FAKE_FTL_H

#include <stdint.h>
#include <stdbool.h>
#include "gc_ftl_if.h"
#include "nand.h"

extern ftl_block_info_t fake_tbl[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];
extern bool             fake_query_fail[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];

/* 조회 통계 */
extern uint32_t fake_info_calls;
extern uint16_t fake_max_block_seen;
extern uint8_t  fake_max_chip_seen;

/* 호출 순서 기록: R=reserve U=unreserve M=move(1 Page) E=reclaim */
extern char     fake_trace[256];

/* 실패 주입 */
extern int          fake_move_fail_at;     /* n 번째 move 호출에서 실패 (-1 = 없음) */
extern ftl_result_t fake_move_fail_code;
extern bool         fake_erase_fail;       /* 다음 reclaim 의 Erase 실패 */
extern bool         fake_reclaim_force_valid_remains;
extern bool         fake_reserve_reject;

/* move 도중 불리는 훅 (중첩 GC 테스트용) */
extern void (*fake_on_move)(void);

void fake_ftl_reset(void);
void fake_ftl_reset_move_counter(void);   /* move 실패 주입을 다시 1 번째 호출 기준으로 */

/* 다 채워진(CLOSED) Block: valid + invalid = 64 */
void fake_set_closed(uint8_t c, uint16_t b, uint16_t valid, uint32_t erase);
void fake_set_state(uint8_t c, uint16_t b, ftl_block_state_t s, uint16_t valid, uint16_t invalid);
/* 관리 영역 밖까지 전부 CLOSED + valid 64 (회수 이득 0) */
void fake_fill_all_full_valid(void);

#endif /* SOLID_FAKE_FTL_H */
