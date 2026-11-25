#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include "../firmware/common/solid_types.h"

// 프로토콜 기본 설정
#define SSD_LBA_SIZE    2048U // 논리블록 하나 크기
#define SSD_MAX_BLOCKS  64 // 하나의 명령에서 처리할 수 있는 최대 논리 블록 수
#define SSD_MAX_DATA_SIZE (SSD_LBA_SIZE * SSD_MAX_BLOCKS) // 하나의 명령에서 처리할 수 있는 최대 데이터 크기

#define SSD_CMD_HEADER_SIZE 13U // 명령 헤더 크기
#define SSD_CPL_HEADER_SIZE 4U // 완료 응답 헤더 크기

// 명령 종류
typedef enum {
    SSD_OP_WRITE = 0x01,
    SSD_OP_READ = 0x02
} SSD_Op;

// 명령 처리 결과 및 오류 코드
typedef solid_err_t SSD_Status;

// 명령 헤더
typedef struct {
    uint8_t op;
    uint16_t cid;
    uint64_t slba;
    uint16_t nlb;
} SSD_Command;

// 완료 응답 헤더
typedef struct {
    uint16_t cid;
    uint16_t status;
} SSD_Completion;

// 보조 인라인 함수
// nlb 카운트 가져오기
static inline uint32_t GetNlbCount(uint16_t nlb) {
    return (uint32_t)nlb + 1U;
}

// 명령에서 처리하는 전체 데이터 크기 계산
static inline uint32_t GetDataSize(uint16_t nlb) {
    return GetNlbCount(nlb) * SSD_LBA_SIZE;
}

// 요청된 논리 블록 개수가 최대 허용 범위 이내인지
// 반환값 1 = 유효한 요청, 0 = 잘못된 요청
static inline uint8_t IsValidNLB(uint16_t nlb) {
    return GetNlbCount(nlb) <= SSD_MAX_BLOCKS;
}

#endif
