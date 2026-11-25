#ifndef SOLID_IPC_H
#define SOLID_IPC_H

/* /dev/solidctl 헤더의 각 필드는 32비트 리틀 엔디언으로 표현. */
#define SOLID_IPC_BLOCK_BYTES 2048U /* 논리블록 한 개의 크기(바이트) */
#define SOLID_IPC_MAX_BYTES 65536U /* 커널 요청 한 건의 최대 데이터 크기(바이트) */
#define SOLID_IPC_REQUEST_HEADER 16U /* 요청 헤더 id, op, lba, length */
#define SOLID_IPC_REPLY_HEADER 8U    /* 응답 헤더 id, status */

/* 브리지가 커널에서 받는 읽기·쓰기 명령 코드 */
#define SOLID_IPC_OP_READ 2U
#define SOLID_IPC_OP_WRITE 1U

/* 커널이 브리지 응답을 기다리는 시간(밀리초) */
#define SOLID_IPC_TIMEOUT_MS 20000U

#endif
