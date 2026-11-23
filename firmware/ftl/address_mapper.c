#include "address_mapper.h"

// valid - active 
bool ppn_is_valid(ppn_t ppn)
{
    return ppn < FTL_PPN_COUNT; /* INVALID_PPN(0xFFFF)은 항상 범위 밖 */
}

bool ppn_is_active(ppn_t ppn)
{
    return ppn_is_valid(ppn) &&
           (ppn % FTL_PPN_PER_CHIP) / NAND_PAGES_PER_BLOCK < FTL_ACTIVE_BLOCKS_PER_CHIP;
}

// 물리 주소 -> chip, block, page
bool ppn_to_addr(ppn_t ppn, nand_addr_t *addr)
{
    // ppn이 32768 이상이거나 INVALID 이면 변환하지 않고 거절.
    // 미매핑 표시를 실제 주소로 착각해 NAND에 접근 하는 사고 방지
    if (addr == NULL || !ppn_is_valid(ppn)) {
        return false;
    }
    
    // 칩당 PPN 4096 고정 슬롯 채택 
    uint16_t local = ppn % FTL_PPN_PER_CHIP;            // 칩 안에서 몇 번째 칸인지
    addr->chip  = (uint8_t)(ppn / FTL_PPN_PER_CHIP);    // 칩 선택: 4096으로 나눈 몫
    addr->block = (uint16_t)(FTL_FIRST_BLOCK + local / NAND_PAGES_PER_BLOCK);   // local/64
    addr->page  = (uint8_t)(local % NAND_PAGES_PER_BLOCK);                      // local%64
    return true;
}

bool addr_to_ppn(const nand_addr_t *addr, ppn_t *ppn)
{
    if (addr == NULL || ppn == NULL ||
        addr->chip >= NAND_MAX_CHIPS ||
        (uint32_t)addr->block - FTL_FIRST_BLOCK >= FTL_RESERVED_BLOCKS_PER_CHIP || /* FIRST_BLOCK보다 작으면 unsigned 뺄셈이 wrap되어 큰 값이 됨 */
        addr->page >= NAND_PAGES_PER_BLOCK) {
        return false;
    }
    *ppn = (ppn_t)(addr->chip * FTL_PPN_PER_CHIP +
                   (addr->block - FTL_FIRST_BLOCK) * NAND_PAGES_PER_BLOCK +
                   addr->page);
    return true;
}
