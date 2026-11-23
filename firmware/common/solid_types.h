#ifndef SOLID_TYPES_H
#define SOLID_TYPES_H

/*
 * SOLID 공통 오류 코드 (FTL 공통 계약 4-2)
 *
 * - FTL / GC / HIL은 이 값만 주고받는다.
 * - NAND 드라이버는 nand.h의 nand_result_t(NAND_ERR_*)를 그대로 쓰고,
 *   FTL이 상위로 올릴 때 ERR_NAND_*로 바꾼다 (docs/ftl-agreement.md 4-2 변환표).
 * - 값은 로그·Host 전달에 쓰이므로 한번 정하면 바꾸지 않는다. 새 코드는 각 구간 끝에 추가.
 *   0x00 성공 / 0x10~ FTL / 0x20~ GC / 0x30~ NAND
 */
typedef enum {
    SOLID_OK                  = 0x00,

    /* FTL */
    ERR_FTL_INVALID_ARG       = 0x10, /* NULL 포인터 등 잘못된 인자 (호출 측 버그) */
    ERR_FTL_RANGE             = 0x11, /* LPN / chip / block 범위 밖 */
    ERR_FTL_NOT_READY         = 0x12, /* ftl_init 완료 전 */
    ERR_FTL_UNMAPPED          = 0x13, /* 쓴 적 없는 LPN 읽기 (UNMAPPED READ 정책에 따라 사용) */
    ERR_FTL_NO_SPACE          = 0x14, /* GC를 해도 빈 Page 없음 */
    ERR_FTL_BAD_STATE         = 0x15, /* Block 상태가 요청과 안 맞음 (예: RESERVED 아닌 Block 이동) */
    ERR_FTL_SRC_READ          = 0x16, /* GC 이동: 원본 Page 읽기 실패 */
    ERR_FTL_DST_PROGRAM       = 0x17, /* GC 이동: 목적지 Program 실패 */
    ERR_FTL_VALID_REMAINS     = 0x18, /* 유효 Page가 남아 Erase 거부 */
    ERR_FTL_NOT_SUPPORTED     = 0x19, /* 미구현 기능 (예: 영속화 전 FLUSH) */
    ERR_FTL_FLUSH_FAIL        = 0x1A, /* 메타데이터 저장 실패 */
    ERR_FTL_NO_META           = 0x1B, /* 저장된 메타데이터 없음 (새 장치, Format 필요) */
    ERR_FTL_META_CORRUPT      = 0x1C, /* 메타데이터 손상 또는 구성 불일치 */
    ERR_FTL_INTERNAL          = 0x1F, /* 내부 불변식 깨짐 (버그) */

    /* GC */
    ERR_GC_INVALID_ARG        = 0x20,
    ERR_GC_NOT_INITIALIZED    = 0x21,
    ERR_GC_NO_SPACE           = 0x22, /* 회수할 Victim 없음 (후보 전부 skip 포함) */
    ERR_GC_REENTRANT          = 0x23, /* GC 실행 중 GC 재호출 (중첩 금지) */

    /* NAND (nand_result_t를 상위로 올릴 때) */
    ERR_NAND_NOT_READY        = 0x30, /* NAND_ERR_NOT_INITIALIZED */
    ERR_NAND_NO_DEVICE        = 0x31, /* NAND_ERR_NO_DEVICE: 칩 응답 없음 / ID 불일치 */
    ERR_NAND_IO               = 0x32, /* NAND_ERR_IO: SPI 통신 오류 */
    ERR_NAND_TIMEOUT          = 0x33, /* NAND_ERR_TIMEOUT: BUSY 해제 안 됨 */
    ERR_NAND_WRITE_PROTECTED  = 0x34, /* NAND_ERR_WRITE_PROTECTED: 보호 해제 안 됨 (전원 후 SR1=7C) */
    ERR_NAND_PROGRAM_FAIL     = 0x35, /* NAND_ERR_PROGRAM_FAIL */
    ERR_NAND_ERASE_FAIL       = 0x36, /* NAND_ERR_ERASE_FAIL: 해당 Block은 BAD 처리됨 */
    ERR_NAND_ECC              = 0x37, /* NAND_ERR_ECC_UNCORRECTABLE */
    ERR_NAND_BAD_BLOCK        = 0x38  /* Bad Block 접근 */
} solid_err_t;

#endif /* SOLID_TYPES_H */
