/*
 * gc.h - Garbage Collection (담당: 이규현)
 *
 * GC 는 "언제, 어떤 블록을 청소할지"를 정하고 순서를 조정한다.
 * 실제 데이터 이동·Mapping 수정·Erase 는 FTL 이 한다 (gc_ftl_if.h).
 *
 * 예비 블록 규칙 (11-20 박수민님과 합의안, FTL 할당기가 지킴):
 *   - GC 시작: Free ≤ 2 (gc_should_run)
 *   - 호스트 쓰기는 Free ≥ 2 일 때만 새 블록을 연다 → 마지막 Free 1개는 GC 전용
 *   - Victim 은 invalid ≥ 1 이라 valid ≤ 63 → 빈 블록 1개면 이동 공간 충분
 *   - 호스트와 GC 는 OPEN 블록 하나를 같이 쓴다 (분리는 나중에)
 *   - 호출 측(HIL/FTL)은 쓰기 전에 while (gc_should_run(free)) gc_collect(); 로 여유를 만든다.
 *     후보가 전부 skip/없음이면 ERR_GC_NO_SPACE → 호스트 쓰기 실패로 보고 (무한 루프 없음)
 *
 * 실패 처리:
 *   ERR_FTL_SRC_READ    → CLOSED 복귀 + Skip 목록 (같은 Victim 반복 방지)
 *   ERR_FTL_DST_PROGRAM → CLOSED 복귀, Skip 안 함 (목적지 문제, FTL 이 격리·재시도)
 *   ERR_FTL_NO_SPACE    → CLOSED 복귀, ERR_GC_NO_SPACE
 *   TBD: 원본 읽기 실패가 반복되는 블록을 영구 제외할지 (문서에만 기록)
 *
 * 함수 이름은 docs/ftl-gc-api.md 5.3 후보(gc_init / gc_should_run /
 * gc_select_victim / gc_collect)를 따른다.
 */
#ifndef SOLID_GC_H
#define SOLID_GC_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- 관리 영역 기본값
 * FTL 의 address_mapper.h (FTL_FIRST_BLOCK, FTL_ACTIVE_BLOCKS_PER_CHIP) 와 같은 값이어야 한다.
 * 그 헤더를 먼저 include 하면 그 값을 그대로 쓴다. */
#ifdef FTL_FIRST_BLOCK
#define GC_DEFAULT_FIRST_BLOCK      FTL_FIRST_BLOCK
#else
#define GC_DEFAULT_FIRST_BLOCK      0U
#endif

#ifdef FTL_ACTIVE_BLOCKS_PER_CHIP
#define GC_DEFAULT_BLOCKS_PER_CHIP  FTL_ACTIVE_BLOCKS_PER_CHIP
#else
#define GC_DEFAULT_BLOCKS_PER_CHIP  24U
#endif

#define GC_DEFAULT_CHIPS            8U
#define GC_DEFAULT_FREE_THRESHOLD   2U
#define GC_MAX_SKIP                 8U    /* 이동 실패한 Victim 을 다시 고르지 않도록 기억하는 수 */

/* ---------------------------------------------------------------- 타입 */

typedef enum {
    GC_OK = 0,
    ERR_GC_NO_SPACE,          /* 회수할 Victim 없음 */
    ERR_GC_INVALID_ARG,
    ERR_GC_NOT_INITIALIZED,
    ERR_GC_BUSY,              /* GC 실행 중에 GC 를 또 부름 (중첩 금지) */
    ERR_GC_FTL,               /* reserve / reclaim 이 예상과 다르게 거부됨 */
    ERR_GC_MOVE_FAIL,         /* 유효 Page 이동 실패 → Victim 은 CLOSED 로 복귀, Erase 안 함 */
    ERR_GC_ERASE_FAIL         /* Erase 실패 → FTL 이 BAD 처리 */
} gc_result_t;

typedef struct {
    uint8_t  chip_count;           /* 관리 칩 수 (초기 8) */
    uint16_t first_block;          /* 관리 영역 시작 Block (FTL_FIRST_BLOCK) */
    uint16_t blocks_per_chip;      /* 칩당 관리 Block 수 (FTL_ACTIVE_BLOCKS_PER_CHIP) */
    uint16_t free_threshold;       /* Free Block ≤ 이 값이면 GC 시작 (초기 2) */
} gc_config_t;

typedef struct {
    uint8_t  chip;
    uint16_t block;                /* 칩 안의 물리 Block 번호 */
    uint16_t valid_pages;          /* 옮겨야 할 Page 수 */
    uint16_t invalid_pages;        /* 회수될 Page 수 */
} gc_victim_t;

typedef struct {
    uint32_t collect_calls;
    uint32_t blocks_reclaimed;     /* Erase 성공해서 FREE 로 돌아간 블록 */
    uint32_t pages_moved;          /* GC 가 옮긴 Page (WAF 의 추가 쓰기 분) */
    uint32_t move_fail;
    uint32_t erase_fail;
} gc_stats_t;

/* GC 이벤트 로그 (팀 결정 ⑦: Victim, Valid 이동, Erase, Free Block 수) */
typedef enum {
    GC_EV_VICTIM = 0,     /* value = valid_pages */
    GC_EV_MOVED,          /* value = 옮긴 Page 수 */
    GC_EV_ERASED,         /* value = Erase 후 Free Block 수 */
    GC_EV_MOVE_FAIL,      /* value = ftl_result_t */
    GC_EV_ERASE_FAIL      /* value = ftl_result_t */
} gc_event_t;

typedef void (*gc_log_fn)(gc_event_t ev, uint8_t chip, uint16_t block, uint32_t value);

/* ---------------------------------------------------------------- API */

gc_config_t gc_default_config(void);

/* 설정 저장 + 통계·Skip 목록 초기화. cfg == NULL 이면 기본값 */
gc_result_t gc_init(const gc_config_t *cfg);

/* 이벤트 로그 콜백 (NULL 이면 로그 안 남김) */
void gc_set_logger(gc_log_fn fn);

/* free_blocks 가 임계값 이하이면 true */
bool gc_should_run(uint16_t free_blocks);

/*
 * Greedy Victim 선택 (조회만, 수정 없음)
 *   후보  : CLOSED 이고 invalid_pages > 0, 최근 이동 실패한 블록 제외
 *   1순위 : valid_pages 최소 / 2순위 : erase_count 최소 / 3순위 : chip → block 번호 최소
 */
gc_result_t gc_select_victim(gc_victim_t *out);

/*
 * Victim 1개 회수: 선택 → reserve → 유효 Page 전부 이동 → reclaim(Erase)
 *   out 이 NULL 이 아니면 처리한 Victim 을 돌려준다.
 *   이동 실패: unreserve(CLOSED 복귀) → ERR_GC_MOVE_FAIL (원본 읽기 실패만 Skip 목록에 추가)
 *   목적지 공간 없음: unreserve → ERR_GC_NO_SPACE
 *   Erase 실패: FTL 이 BAD 처리 → ERR_GC_ERASE_FAIL
 *   GC 실행 중 재호출: ERR_GC_BUSY
 */
gc_result_t gc_collect(gc_victim_t *out);

void gc_get_stats(gc_stats_t *out);

#endif /* SOLID_GC_H */
