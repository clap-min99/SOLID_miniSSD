#include "hil.h"

#include "../common/nand.h"
#include "../ftl/ftl.h"

_Static_assert(SSD_LBA_SIZE == NAND_PAGE_SIZE, "HIL and FTL block sizes must match");
_Static_assert(FTL_LOGICAL_PAGE_COUNT - 1U <= UINT16_MAX, "FTL LPN must fit in uint16_t");

/* Host에 공개하기로 합의한 FTL/NAND 결과를 그대로 전달한다. */
static SSD_Status hil_host_status(solid_err_t result)
{
    switch (result) {
    case SOLID_OK:
    case ERR_FTL_INVALID_ARG:
    case ERR_FTL_RANGE:
    case ERR_FTL_NOT_READY:
    case ERR_FTL_UNMAPPED:
    case ERR_FTL_NO_SPACE:
    case ERR_FTL_NOT_SUPPORTED:
    case ERR_FTL_FLUSH_FAIL:
    case ERR_FTL_NO_META:
    case ERR_FTL_META_CORRUPT:
    case ERR_FTL_INTERNAL:
    case ERR_NAND_NOT_INITIALIZED:
    case ERR_NAND_NO_DEVICE:
    case ERR_NAND_IO:
    case ERR_NAND_TIMEOUT:
    case ERR_NAND_WRITE_PROTECTED:
    case ERR_NAND_PROGRAM_FAIL:
    case ERR_NAND_ECC_UNCORRECTABLE:
    case ERR_HIL_BAD_COMMAND:
    case ERR_HIL_BAD_LENGTH:
    case ERR_HIL_BAD_VERSION:
    case ERR_HIL_BAD_FIELD:
        return result;
    default:
        return ERR_FTL_INTERNAL;
    }
}

SSD_Completion hil_handle_command(SSD_Command command, size_t payload_size,
                                  uint8_t page[SSD_LBA_SIZE], const hil_block_io_t *io)
{
    SSD_Completion completion = { .cid = command.cid };
    const bool write = command.op == SSD_OP_WRITE;
    const uint32_t block_count = GetNlbCount(command.nlb);

    /* 전체 요청의 필드, 길이와 주소 범위를 FTL 호출 전에 검사한다. */
    if (!write && command.op != SSD_OP_READ) {
        completion.status = ERR_HIL_BAD_COMMAND;
        return completion;
    }
    if (!IsValidNLB(command.nlb) || page == NULL || io == NULL ||
        (write ? io->get_write_block == NULL : io->put_read_block == NULL)) {
        completion.status = ERR_HIL_BAD_FIELD;
        return completion;
    }
    if (payload_size != (write ? GetDataSize(command.nlb) : 0U)) {
        completion.status = ERR_HIL_BAD_LENGTH;
        return completion;
    }
    if (command.slba >= FTL_LOGICAL_PAGE_COUNT ||
        block_count > FTL_LOGICAL_PAGE_COUNT - command.slba) {
        completion.status = ERR_FTL_RANGE;
        return completion;
    }

    /* 요청 하나를 연속 LPN의 단일 페이지 연산으로 나눈다. */
    for (uint32_t index = 0; index < block_count; ++index) {
        const uint16_t lpn = (uint16_t)(command.slba + index);
        solid_err_t result;
        if (write) {
            result = io->get_write_block(io->context, index, page);
            if (result == SOLID_OK)
                result = ftl_write(lpn, page);
        } else {
            result = ftl_read(lpn, page);
            if (result == SOLID_OK)
                result = io->put_read_block(io->context, index, page);
        }
        if (result != SOLID_OK) {
            completion.status = hil_host_status(result);
            return completion;
        }
    }
    return completion;
}
