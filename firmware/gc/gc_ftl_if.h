/*
 * gc_ftl_if.h - GC 가 호출하는 FTL 함수 (구현은 FTL, 담당: 박수민)
 *
 * 2026-11-20 박수민님 제안 반영 (v0.2)
 *   1. FTL 이 블록 표를 항상 최신으로 유지 → 조회 O(1)
 *   2. Free Block 수 = 전역 카운터 (FREE 상태만 셈, OPEN 제외)
 *   3. 상태 전이 API: Victim 은 RESERVED 로 묶고, 실패 시 CLOSED/BAD 로 되돌림
 *   4. 유효 Page 이동과 Erase 는 FTL 이 처리 → GC 는 nand.h 를 호출하지 않음
 *   5. GC 인터페이스에서는 PPN 을 쓰지 않음 (chip, block 만 사용)
 *   6. block 은 칩 안의 물리 Block 번호, 관리 영역은 FTL_FIRST_BLOCK(0)부터 24개
 *
 * 블록 상태 전이 (FTL 이 관리, GC 는 요청만):
 *
 *   FREE ──할당──▶ OPEN ──다 참──▶ CLOSED ──ftl_reserve_victim──▶ RESERVED
 *   RESERVED ──ftl_reclaim_block (Erase 성공)──▶ FREE
 *   RESERVED ──ftl_reclaim_block (Erase 실패)──▶ BAD
 *   RESERVED ──ftl_unreserve_victim (이동 실패, Erase 안 함)──▶ CLOSED
 *
 * TBD: 이름·반환형은 합의 후 ftl.h / solid_types.h 로 옮긴다.
 */
#ifndef SOLID_GC_FTL_IF_H
#define SOLID_GC_FTL_IF_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- 타입 */

typedef enum {
    FTL_BLK_FREE = 0,    /* Erase 됨, 할당 대기                    → Victim 아님 */
    FTL_BLK_OPEN,        /* 지금 Page 를 채우는 중 (한 번에 1개)   → Victim 아님 */
    FTL_BLK_CLOSED,      /* 다 채워짐                              → Victim 후보 */
    FTL_BLK_BAD,         /* 공장/운영 중 불량                      → Victim·목적지 제외 */
    FTL_BLK_RESERVED     /* GC Victim 으로 잡힘 / Metadata·예비     → Victim 아님 */
} ftl_block_state_t;

/* FTL 블록 표 한 칸의 복사본 */
typedef struct {
    ftl_block_state_t state;
    uint8_t  write_pointer;   /* 다음에 쓸 Page (0~64) */
    uint16_t valid_pages;     /* 현재 L2P 가 가리키는 Page 수 (Greedy 기준) */
    uint16_t invalid_pages;   /* write_pointer - valid_pages (FTL 이 계산해서 채움) */
    uint32_t erase_count;     /* 동률일 때 마모 분산용 */
} ftl_block_info_t;

/* FTL 결과 (노션 FTL 공통 계약 4-2 의 ERR_FTL_* 명명, 값은 합의 대상) */
typedef enum {
    FTL_OK = 0,
    ERR_FTL_INVALID_ARG,      /* 범위 밖 chip/block, NULL */
    ERR_FTL_BAD_STATE,        /* 지금 상태에서 할 수 없는 전이 (예: CLOSED 아닌 블록 reserve) */
    ERR_FTL_NO_SPACE,         /* 이동할 목적지 Page 없음 (예비 블록까지 소진) */
    ERR_FTL_SRC_READ,         /* Victim 원본 Page 읽기 실패 (ECC 정정 불가 등) → Victim 문제 */
    ERR_FTL_DST_PROGRAM,      /* 목적지 Program 실패, FTL 내부 재시도까지 실패 → Victim 문제 아님 */
    ERR_FTL_NAND_ERASE,       /* Erase 실패 → FTL 이 BAD 처리 완료 */
    ERR_FTL_VALID_REMAINS     /* 유효 Page 가 남은 블록을 Erase 하려 함 → 거부 */
} ftl_result_t;

/* ---------------------------------------------------------------- 조회 */

/* chip / block 의 블록 표 한 칸을 복사해서 돌려준다 */
ftl_result_t ftl_get_block_info(uint8_t chip, uint16_t block, ftl_block_info_t *out);

/* FREE 상태 블록 수 (OPEN 은 세지 않음). gc_should_run() 입력 */
uint16_t ftl_get_free_block_count(void);

/* ---------------------------------------------------------------- GC 전용 상태 전이 */

/* CLOSED → RESERVED. 이후 새 할당·Victim 선택에서 빠진다 */
ftl_result_t ftl_reserve_victim(uint8_t chip, uint16_t block);

/* RESERVED → CLOSED. 이동 도중 실패해서 Erase 하지 않을 때 */
ftl_result_t ftl_unreserve_victim(uint8_t chip, uint16_t block);

/*
 * Victim 의 다음 유효 Page 하나를 다른 블록으로 옮긴다 (11-20 박수민님과 합의)
 *   FTL 내부: Victim 마다 L2P 를 한 번 훑어 (LPN, PPN) 목록 작성 → 옮기기 직전
 *             "이 LPN 이 아직 이 PPN 을 가리키는지" 재확인(호스트가 덮어썼으면 건너뜀) →
 *             목적지 할당 → Read → Program → remap → 옛 Page invalid
 *   호출 조건: Victim 이 RESERVED 일 때만. 아니면 ERR_FTL_BAD_STATE
 *   *moved = true  : 1 Page 옮김
 *   *moved = false : 더 옮길 유효 Page 없음 (FTL_OK 와 함께) → GC 는 reclaim 으로 넘어감
 *   실패: ERR_FTL_SRC_READ / ERR_FTL_DST_PROGRAM / ERR_FTL_NO_SPACE
 *         어느 경우든 그 LPN 의 기존 Mapping 은 그대로 유지한다.
 *         DST_PROGRAM 은 FTL 이 목적지를 격리하고 다른 곳으로 재시도한 뒤에도 실패한 경우만.
 */
ftl_result_t ftl_gc_move_one(uint8_t chip, uint16_t block, bool *moved);

/*
 * RESERVED 이고 valid == 0 인 블록을 Erase 한다.
 *   성공: erase_count++, FREE 로 반환, Free 카운터++
 *   Erase 실패: BAD 로 바꾸고 ERR_FTL_NAND_ERASE
 *   유효 Page 남음: 상태를 바꾸지 않고(RESERVED 유지) ERR_FTL_VALID_REMAINS
 *                  → CLOSED 로 되돌릴지는 GC 가 정한다 (GC 는 unreserve + skip)
 */
ftl_result_t ftl_reclaim_block(uint8_t chip, uint16_t block);

#endif /* SOLID_GC_FTL_IF_H */
