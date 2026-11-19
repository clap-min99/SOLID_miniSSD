/*
 * mock_nand.h - Mock NAND 전용 제어 API (테스트에서만 사용)
 *
 * 데이터 동작(read/program/erase 등)은 firmware/common/nand.h 를 그대로 구현한다.
 * 이 헤더는 nand.h 에 없는 "테스트용 손잡이"만 제공한다.
 *   - 칩 개수 설정 / 전체 초기화
 *   - 공장 Bad Block 지정
 *   - 실패(Program/Erase Fail, Timeout, ECC) 주입
 *   - 통계(호출 횟수, Erase 횟수, 규칙 위반 횟수, 모의 시간) 조회
 *   - Spare 영역 엿보기/쓰기 (nand.h 에 Spare API 가 아직 없으므로 디버그용)
 *
 * 실제 펌웨어 코드(FTL/GC)는 이 헤더를 include 하면 안 된다.
 */
#ifndef SOLID_MOCK_NAND_H
#define SOLID_MOCK_NAND_H

#include <stdint.h>
#include <stdbool.h>
#include "nand.h"

/* ---------------------------------------------------------------- 설정 */

typedef struct {
    uint8_t chip_count;          /* 연결된 것으로 볼 칩 수 (1~NAND_MAX_CHIPS) */
    bool    strict_sequential;   /* true: 반드시 next_page 에만 Program 허용
                                    false: next_page 이상이면 허용(앞으로 건너뛰기 OK) */
} mock_nand_config_t;

/* 기본값: 8칩, 건너뛰기 허용 */
mock_nand_config_t mock_nand_default_config(void);

/* 모든 저장 내용·통계·주입 설정을 지우고 새 칩 상태(전부 0xFF)로 만든다.
 * nand_init() 전에 호출한다. 호출하지 않으면 nand_init()이 기본 설정으로 만든다. */
void mock_nand_reset(const mock_nand_config_t *cfg);

/* 할당한 메모리를 모두 해제한다 (테스트 종료 시) */
void mock_nand_destroy(void);

/* ---------------------------------------------------------------- Bad Block */

/* 공장 Bad Block 표시: Block 의 첫 Page Spare[0] 을 0x00 으로 기록한다.
 * nand_is_factory_bad_block() 이 true 를 돌려주고, Program/Erase 는 거부된다. */
void mock_nand_mark_factory_bad(uint8_t chip, uint16_t block);

/* ---------------------------------------------------------------- 실패 주입 */

typedef enum {
    MOCK_OP_READ = 0,
    MOCK_OP_PROGRAM,
    MOCK_OP_ERASE,
    MOCK_OP_COUNT
} mock_op_t;

#define MOCK_ANY_CHIP   0xFFU
#define MOCK_ANY_BLOCK  0xFFFFU

/* 다음 count 번의 op 호출(주소 조건이 맞을 때)을 result 로 실패시킨다.
 *   예) mock_nand_inject(MOCK_OP_PROGRAM, 0, 5, NAND_ERR_PROGRAM_FAIL, 1)
 *       → chip0 block5 에 대한 다음 Program 1번이 실패
 *   count = 0 이면 영구(해제 전까지 계속 실패)
 * read 에 NAND_OK 이외 값을 넣으면 그 오류를 반환한다. */
void mock_nand_inject(mock_op_t op, uint8_t chip, uint16_t block,
                      nand_result_t result, uint32_t count);

/* 다음 count 번의 Read 가 ECC 정정(NAND_OK + NAND_ECC_CORRECTED)으로 보고되게 한다 */
void mock_nand_inject_ecc_corrected(uint8_t chip, uint16_t block, uint32_t count);

/* 모든 주입 해제 */
void mock_nand_clear_injections(void);

/* ---------------------------------------------------------------- 통계 */

typedef struct {
    uint32_t read_calls;
    uint32_t program_calls;
    uint32_t erase_calls;

    uint32_t program_ok;         /* 실제로 Page 를 기록한 횟수 (WAF 계산용 NAND 쓰기 수) */
    uint32_t erase_ok;

    /* 규칙 위반: 정상 FTL/GC 라면 항상 0 이어야 한다 */
    uint32_t violation_reprogram;     /* 이미 쓴 Page 에 다시 Program */
    uint32_t violation_order;         /* Block 안에서 순서를 거슬러 Program */
    uint32_t violation_bad_block;     /* 공장 Bad Block 에 Program/Erase */
    uint32_t violation_bad_arg;       /* 범위 밖 주소, NULL 버퍼 등 */

    uint64_t sim_time_us;        /* 모의 소요 시간 (실측 W25N 값 기반) */
} mock_nand_stats_t;

void     mock_nand_get_stats(mock_nand_stats_t *out);
void     mock_nand_clear_stats(void);
uint32_t mock_nand_violations(void);          /* 위반 합계 */
uint32_t mock_nand_erase_count(uint8_t chip, uint16_t block);

/* 다음에 Program 할 수 있는 가장 낮은 Page 번호 (Erase 직후 0, 꽉 차면 64) */
uint8_t  mock_nand_next_page(uint8_t chip, uint16_t block);

/* Page 가 Erase 이후 Program 되었는지 */
bool     mock_nand_page_written(uint8_t chip, uint16_t block, uint8_t page);

/* ---------------------------------------------------------------- Spare (디버그) */

/* nand.h 에 Spare API 가 생기기 전까지 테스트에서만 쓰는 엿보기/쓰기.
 * poke 는 NAND 규칙(1→0 만 가능)을 따라 AND 로 기록된다. */
void mock_nand_peek_spare(nand_addr_t addr, uint8_t out[NAND_SPARE_SIZE]);
void mock_nand_poke_spare(nand_addr_t addr, const uint8_t in[NAND_SPARE_SIZE]);

/* 모의 시간 상수 (2026-11-18 W25N01GV bring-up 실측, SPI 3.1MHz) */
#define MOCK_T_READ_US     78U
#define MOCK_T_PROGRAM_US  5596U
#define MOCK_T_ERASE_US    3000U   /* 빈 블록 583us, 데이터 있는 블록은 미측정 → 데이터시트 범위(2~10ms) 내 임시값 */

#endif /* SOLID_MOCK_NAND_H */
