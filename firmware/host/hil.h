#ifndef SOLID_HIL_H
#define SOLID_HIL_H

#include <stddef.h>

#include "../../include/protocol.h"

/* 블록 인덱스는 요청마다 0부터 시작한다. 콜백은 블록을 동기적으로
 * 제공하거나 소비하며 반환 후 page 포인터를 보관하지 않는다. */
typedef struct {
    void *context;
    SSD_Status (*get_write_block)(void *context, uint32_t index, uint8_t *page);
    SSD_Status (*put_read_block)(void *context, uint32_t index, const uint8_t *page);
} hil_block_io_t;

/* payload_size는 요청 데이터 길이다. READ 요청은 0, WRITE는 (nlb + 1) * 2 KiB다.
 * page는 재사용할 2 KiB 작업 버퍼다. WRITE 콜백은 검증된 데이터만 제공한다.
 * READ 완료 상태가 실패이면 전체 요청의 데이터는 유효하지 않다.
 * WRITE 도중 실패하면 앞서 성공한 블록의 쓰기는 유지된다. */
SSD_Completion hil_handle_command(SSD_Command command, size_t payload_size,
                                  uint8_t page[SSD_LBA_SIZE], const hil_block_io_t *io);

#endif /* SOLID_HIL_H */
