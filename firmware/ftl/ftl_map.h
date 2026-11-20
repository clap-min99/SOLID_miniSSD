#ifndef SOLID_FTL_MAP_H
#define SOLID_FTL_MAP_H

#include <stdbool.h>
#include "address_mapper.h"

#define FTL_LOGICAL_PAGE_COUNT 8192U /* 16MiB / 2KiB */

/* L2P table is private; only these calls touch it. */
void ftl_map_init(void);                      /* every entry -> INVALID_PPN */
bool ftl_map_get(uint16_t lpn, ppn_t *ppn);   /* false: lpn out of range */
bool ftl_map_set(uint16_t lpn, ppn_t ppn);    /* ppn: INVALID_PPN(매핑 해제) 또는 active PPN만 허용 */

#endif /* SOLID_FTL_MAP_H */
