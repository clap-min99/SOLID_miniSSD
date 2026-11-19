/*
 * gc_ftl_if.h - GC 가 FTL 에게 요구하는 조회 인터페이스 (초안, 박수민님과 협의용)
 *
 * 원칙 (ftl-gc-api.md 2.2): GC 는 L2P / Page State / Block 통계를 직접 수정하지 않는다.
 * 이 파일의 함수는 전부 "조회 전용"이며, 구현은 FTL 이 한다.
 * 테스트에서는 tests/gc 의 가짜 FTL 이 구현한다.
 *
 * TBD: 이름·필드는 FTL 공통 계약 합의 후 solid_types.h / ftl.h 로 옮긴다.
 */
#ifndef SOLID_GC_FTL_IF_H
#define SOLID_GC_FTL_IF_H

#include <stdint.h>
#include <stdbool.h>

/* Block 상태 (ftl-gc-api.md 2.4 의 Free / Open / Closed / Bad / 예약 후보) */
typedef enum {
    FTL_BLK_FREE = 0,    /* Erase 됨, 아직 할당 안 함              → Victim 아님 */
    FTL_BLK_OPEN,        /* 지금 Page 를 채우는 중 (쓰기 중)       → Victim 아님 */
    FTL_BLK_CLOSED,      /* 다 채워짐, Valid/Invalid 섞여 있음     → Victim 후보 */
    FTL_BLK_BAD,         /* 공장/운영 중 불량                      → Victim·목적지 제외 */
    FTL_BLK_RESERVED     /* Metadata·GC 예비·이미 GC 진행 중       → Victim 아님 */
} ftl_block_state_t;

/* GC 가 Victim 을 고르는 데 필요한 Block 정보 */
typedef struct {
    ftl_block_state_t state;
    uint16_t valid_pages;     /* 현재 L2P 가 가리키는 Page 수 (Greedy 기준) */
    uint16_t invalid_pages;   /* 덮어써져서 쓰레기가 된 Page 수 (회수량) */
    uint32_t erase_count;     /* 동률일 때 마모 분산용 */
} ftl_block_info_t;

/* 조회 결과 */
typedef enum {
    FTL_QUERY_OK = 0,
    FTL_QUERY_ERR            /* 범위 밖, 초기화 전 등 */
} ftl_query_result_t;

/* chip(0~7) / block(관리 영역 안의 물리 Block 번호) 의 정보를 복사해서 돌려준다.
 * 수정 가능한 내부 포인터를 돌려주지 않는다. */
ftl_query_result_t ftl_get_block_info(uint8_t chip, uint16_t block, ftl_block_info_t *out);

#endif /* SOLID_GC_FTL_IF_H */
