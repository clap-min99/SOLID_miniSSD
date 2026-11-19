#include "ftl_map.h"

static ppn_t l2p[FTL_LOGICAL_PAGE_COUNT]; /* 16KiB */

// 전체 논리 주소를 '없음'으로 초기화
void ftl_map_init(void)
{
    for (uint32_t i = 0; i < FTL_LOGICAL_PAGE_COUNT; i++) {
        l2p[i] = INVALID_PPN; /* not memset(0): 0 is a real PPN */
    }
}


bool ftl_map_get(uint16_t lpn, ppn_t *ppn)
{
    if (ppn == NULL || lpn >= FTL_LOGICAL_PAGE_COUNT) {
        return false;
    }
    *ppn = l2p[lpn];
    return true;
}

bool ftl_map_set(uint16_t lpn, ppn_t ppn)
{
    if (lpn >= FTL_LOGICAL_PAGE_COUNT || (ppn != INVALID_PPN && !ppn_is_active(ppn))) {
        return false;
    }
    l2p[lpn] = ppn;
    return true;
}
