#include "ftl.h"
#include "ftl_alloc.h"
#include "ftl_map.h"

#define FTL_PROGRAM_RETRY 1U   /* Program 실패 시 다른 Page로 다시 쓰는 횟수 */

static bool ready;

solid_err_t ftl_init(uint8_t chip_count)
{
    ready = false;
    ftl_map_init();
    solid_err_t e = ftl_alloc_init(chip_count);
    ready = (e == SOLID_OK);
    return e;
}

solid_err_t ftl_write(uint16_t lpn, const uint8_t *buf)
{
    ppn_t old;
    if (!ready) {
        return ERR_FTL_NOT_READY;
    }
    if (buf == NULL) {
        return ERR_FTL_INVALID_ARG;
    }
    if (!ftl_map_get(lpn, &old)) {
        return ERR_FTL_RANGE;
    }

    for (unsigned attempt = 0; attempt <= FTL_PROGRAM_RETRY; attempt++) {
        ppn_t ppn;
        nand_addr_t addr;
        /* ponytail: 공간 부족이면 지금은 그대로 실패. GC 연결은 FTL-07 */
        solid_err_t e = ftl_alloc_page(false, &ppn);
        if (e != SOLID_OK) {
            return e;
        }
        if (!ppn_to_addr(ppn, &addr)) {
            return ERR_FTL_INTERNAL;
        }

        nand_result_t r = nand_program_page(addr, buf);
        if (r == NAND_ERR_PROGRAM_FAIL) {
            continue;   /* 이 Page는 버리고(무효로 남음) 다음 Page로 재시도 */
        }
        if (r != NAND_OK) {
            return ftl_from_nand(r);   /* TIMEOUT·IO 등은 재시도하지 않음 */
        }

        /* Program 성공 → 새 Page 유효, 옛 Page 무효, 매핑 교체 */
        if ((e = ftl_page_set_valid(ppn)) != SOLID_OK) {
            return e;
        }
        if (old != INVALID_PPN && (e = ftl_page_set_invalid(old)) != SOLID_OK) {
            return e;
        }
        return ftl_map_set(lpn, ppn) ? SOLID_OK : ERR_FTL_INTERNAL;
    }
    return ERR_NAND_PROGRAM_FAIL;
}

solid_err_t ftl_read(uint16_t lpn, uint8_t *buf)
{
    ppn_t ppn;
    nand_addr_t addr;
    nand_ecc_t ecc = NAND_ECC_UNKNOWN;
    if (!ready) {
        return ERR_FTL_NOT_READY;
    }
    if (buf == NULL) {
        return ERR_FTL_INVALID_ARG;
    }
    if (!ftl_map_get(lpn, &ppn)) {
        return ERR_FTL_RANGE;
    }
    if (ppn == INVALID_PPN) {
        return ERR_FTL_UNMAPPED;
    }
    if (!ppn_to_addr(ppn, &addr)) {
        return ERR_FTL_INTERNAL;
    }

    nand_result_t r = nand_read_page(addr, buf, &ecc);
    if (r != NAND_OK) {
        return ftl_from_nand(r);
    }
    switch (ecc) {
    case NAND_ECC_NO_ERROR:
    case NAND_ECC_CORRECTED:      return SOLID_OK;   /* 정정됨 = 데이터 정상 (처리 정책은 팀 미정) */
    case NAND_ECC_UNCORRECTABLE:  return ERR_NAND_ECC_UNCORRECTABLE;
    case NAND_ECC_UNKNOWN:        return ERR_NAND_IO;  /* ECC 결과를 모름 → 데이터를 믿을 수 없음 */
    }
    return ERR_FTL_INTERNAL;
}
