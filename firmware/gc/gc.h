/*
 * gc.h - Garbage Collection (담당: 이규현)
 *
 * 이번 단계 범위: GC 시작 판단 + Greedy Victim 선택
 * 다음 단계(gc_collect: 유효 Page 이동 → remap → Erase)는 FTL↔GC 호출 방식(합의 6번) 확정 후.
 *
 * 함수 이름은 docs/ftl-gc-api.md 5.3 의 후보(gc_init / gc_should_run /
 * gc_select_victim / gc_collect)를 따른다.
 */
#ifndef SOLID_GC_H
#define SOLID_GC_H

#include <stdint.h>
#include <stdbool.h>

/* 오류 코드 (노션 FTL 공통 계약 4-2: ERR_GC_* 명명) */
typedef enum {
    GC_OK = 0,
    ERR_GC_NO_SPACE,          /* 회수할 Victim 없음 */
    ERR_GC_INVALID_ARG,
    ERR_GC_NOT_INITIALIZED
} gc_result_t;

/* GC 설정: 관리 영역 크기와 정책 값 */
typedef struct {
    uint8_t  chip_count;           /* 관리 대상 칩 수 (초기 8) */
    uint16_t blocks_per_chip;      /* 칩당 관리 Block 수 (초기 24) */
    uint16_t free_threshold;       /* Free Block 이 이 값 이하이면 GC 시작 (초기 2) */
} gc_config_t;

#define GC_DEFAULT_CHIPS           8U
#define GC_DEFAULT_BLOCKS_PER_CHIP 24U
#define GC_DEFAULT_FREE_THRESHOLD  2U

/* 선택된 Victim */
typedef struct {
    uint8_t  chip;
    uint16_t block;
    uint16_t valid_pages;     /* 옮겨야 할 Page 수 */
    uint16_t invalid_pages;   /* 회수될 Page 수 */
} gc_victim_t;

gc_config_t gc_default_config(void);

/* 설정을 저장한다. cfg 가 NULL 이면 기본값. 범위가 이상하면 ERR_GC_INVALID_ARG */
gc_result_t gc_init(const gc_config_t *cfg);

/* free_blocks 가 임계값 이하이면 true */
bool gc_should_run(uint16_t free_blocks);

/*
 * Greedy Victim 선택 (조회만 하고 아무것도 수정하지 않는다)
 *
 *   후보   : state == FTL_BLK_CLOSED 이고 invalid_pages > 0
 *            (FREE / OPEN / BAD / RESERVED 는 제외, 지워도 얻을 게 없는 블록 제외)
 *   1순위  : valid_pages 가 가장 적은 Block (옮길 양이 적고 얻는 공간이 큼)
 *   2순위  : erase_count 가 가장 적은 Block (마모 분산)
 *   3순위  : 번호가 가장 작은 Block (chip → block 순, 결과를 항상 같게)
 *
 *   조회에 실패한 Block 은 건너뛴다 (상태를 모르는 Block 을 지우면 안 되므로).
 *
 * 반환: GC_OK + *out 채움 / 후보가 없으면 ERR_GC_NO_SPACE
 */
gc_result_t gc_select_victim(gc_victim_t *out);

#endif /* SOLID_GC_H */
