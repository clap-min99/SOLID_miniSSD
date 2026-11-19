/*
 * gc.c - Garbage Collection (담당: 이규현)
 */
#include <stddef.h>
#include "gc.h"
#include "gc_ftl_if.h"
#include "nand.h"

static struct {
    bool        initialized;
    gc_config_t cfg;
} G;

gc_config_t gc_default_config(void)
{
    gc_config_t c;
    c.chip_count      = GC_DEFAULT_CHIPS;
    c.blocks_per_chip = GC_DEFAULT_BLOCKS_PER_CHIP;
    c.free_threshold  = GC_DEFAULT_FREE_THRESHOLD;
    return c;
}

gc_result_t gc_init(const gc_config_t *cfg)
{
    gc_config_t c = (cfg != NULL) ? *cfg : gc_default_config();

    if (c.chip_count == 0 || c.chip_count > NAND_MAX_CHIPS ||
        c.blocks_per_chip == 0 || c.blocks_per_chip > NAND_BLOCKS_PER_CHIP) {
        G.initialized = false;
        return ERR_GC_INVALID_ARG;
    }
    G.cfg = c;
    G.initialized = true;
    return GC_OK;
}

bool gc_should_run(uint16_t free_blocks)
{
    if (!G.initialized) {
        return false;
    }
    return free_blocks <= G.cfg.free_threshold;
}

/* a 가 b 보다 Victim 으로 더 좋은가 (같으면 false → 먼저 찾은 작은 번호가 유지됨) */
static bool better(const ftl_block_info_t *a, const ftl_block_info_t *b)
{
    if (a->valid_pages != b->valid_pages) {
        return a->valid_pages < b->valid_pages;
    }
    return a->erase_count < b->erase_count;
}

gc_result_t gc_select_victim(gc_victim_t *out)
{
    if (!G.initialized) {
        return ERR_GC_NOT_INITIALIZED;
    }
    if (out == NULL) {
        return ERR_GC_INVALID_ARG;
    }

    bool found = false;
    ftl_block_info_t best = { 0 };
    gc_victim_t pick = { 0 };

    for (uint8_t c = 0; c < G.cfg.chip_count; c++) {
        for (uint16_t b = 0; b < G.cfg.blocks_per_chip; b++) {
            ftl_block_info_t info;
            if (ftl_get_block_info(c, b, &info) != FTL_QUERY_OK) {
                continue;
            }
            if (info.state != FTL_BLK_CLOSED || info.invalid_pages == 0) {
                continue;
            }
            if (!found || better(&info, &best)) {
                found = true;
                best = info;
                pick.chip = c;
                pick.block = b;
                pick.valid_pages = info.valid_pages;
                pick.invalid_pages = info.invalid_pages;
            }
        }
    }

    if (!found) {
        return ERR_GC_NO_SPACE;
    }
    *out = pick;
    return GC_OK;
}
