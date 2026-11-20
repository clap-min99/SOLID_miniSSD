/*
 * gc.c - Garbage Collection (담당: 이규현)
 */
#include <stddef.h>
#include "gc.h"
#include "gc_ftl_if.h"
#include "nand.h"      /* geometry 상수만 사용. nand_* 함수는 호출하지 않음 */

typedef struct {
    uint8_t  chip;
    uint16_t block;
} blk_id_t;

static struct {
    bool        initialized;
    bool        busy;              /* gc_collect 실행 중 (중첩 금지) */
    gc_config_t cfg;
    gc_stats_t  st;
    gc_log_fn   log;
    blk_id_t    skip[GC_MAX_SKIP]; /* 최근 이동 실패한 Victim (링 버퍼) */
    uint8_t     skip_count;
    uint8_t     skip_next;
} G;

/* ================================================================ 내부 유틸 */

static void log_ev(gc_event_t ev, uint8_t chip, uint16_t block, uint32_t value)
{
    if (G.log != NULL) {
        G.log(ev, chip, block, value);
    }
}

static bool is_skipped(uint8_t chip, uint16_t block)
{
    for (uint8_t i = 0; i < G.skip_count; i++) {
        if (G.skip[i].chip == chip && G.skip[i].block == block) {
            return true;
        }
    }
    return false;
}

static void add_skip(uint8_t chip, uint16_t block)
{
    if (is_skipped(chip, block)) {
        return;
    }
    G.skip[G.skip_next].chip = chip;
    G.skip[G.skip_next].block = block;
    G.skip_next = (uint8_t)((G.skip_next + 1U) % GC_MAX_SKIP);
    if (G.skip_count < GC_MAX_SKIP) {
        G.skip_count++;
    }
}

/* a 가 b 보다 Victim 으로 더 좋은가 (같으면 false → 먼저 찾은 작은 번호가 유지됨) */
static bool better(const ftl_block_info_t *a, const ftl_block_info_t *b)
{
    if (a->valid_pages != b->valid_pages) {
        return a->valid_pages < b->valid_pages;
    }
    return a->erase_count < b->erase_count;
}

/* ================================================================ API */

gc_config_t gc_default_config(void)
{
    gc_config_t c;
    c.chip_count      = GC_DEFAULT_CHIPS;
    c.first_block     = GC_DEFAULT_FIRST_BLOCK;
    c.blocks_per_chip = GC_DEFAULT_BLOCKS_PER_CHIP;
    c.free_threshold  = GC_DEFAULT_FREE_THRESHOLD;
    return c;
}

gc_result_t gc_init(const gc_config_t *cfg)
{
    gc_config_t c = (cfg != NULL) ? *cfg : gc_default_config();

    G.initialized = false;
    if (c.chip_count == 0 || c.chip_count > NAND_MAX_CHIPS ||
        c.blocks_per_chip == 0 ||
        (uint32_t)c.first_block + c.blocks_per_chip > NAND_BLOCKS_PER_CHIP) {
        return ERR_GC_INVALID_ARG;
    }
    G.cfg = c;
    G.busy = false;
    G.skip_count = 0;
    G.skip_next = 0;
    G.st = (gc_stats_t){ 0 };
    G.initialized = true;
    return GC_OK;
}

void gc_set_logger(gc_log_fn fn)
{
    G.log = fn;
}

bool gc_should_run(uint16_t free_blocks)
{
    if (!G.initialized) {
        return false;
    }
    return free_blocks <= G.cfg.free_threshold;
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
    const uint16_t first = G.cfg.first_block;
    const uint16_t last  = (uint16_t)(G.cfg.first_block + G.cfg.blocks_per_chip);

    for (uint8_t c = 0; c < G.cfg.chip_count; c++) {
        for (uint16_t b = first; b < last; b++) {
            ftl_block_info_t info;
            if (ftl_get_block_info(c, b, &info) != FTL_OK) {
                continue;               /* 상태를 모르는 블록은 지우면 안 됨 */
            }
            if (info.state != FTL_BLK_CLOSED || info.invalid_pages == 0) {
                continue;
            }
            if (is_skipped(c, b)) {
                continue;               /* 최근 이동 실패 → 같은 실패 반복 방지 */
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

gc_result_t gc_collect(gc_victim_t *out)
{
    if (!G.initialized) {
        return ERR_GC_NOT_INITIALIZED;
    }
    if (G.busy) {
        return ERR_GC_BUSY;
    }
    G.busy = true;
    G.st.collect_calls++;

    gc_result_t res;
    gc_victim_t v;

    res = gc_select_victim(&v);
    if (res != GC_OK) {
        goto done;
    }
    if (out != NULL) {
        *out = v;
    }
    log_ev(GC_EV_VICTIM, v.chip, v.block, v.valid_pages);

    if (ftl_reserve_victim(v.chip, v.block) != FTL_OK) {
        res = ERR_GC_FTL;
        goto done;
    }

    /* 유효 Page 를 하나씩 옮긴다. 블록당 최대 64 Page 이므로 그 이상 돌면 FTL 이상 */
    uint32_t moved_here = 0;
    for (;;) {
        bool moved = false;
        ftl_result_t fr = ftl_gc_move_one(v.chip, v.block, &moved);
        if (fr != FTL_OK) {
            G.st.move_fail++;
            log_ev(GC_EV_MOVE_FAIL, v.chip, v.block, (uint32_t)fr);
            (void)ftl_unreserve_victim(v.chip, v.block);   /* CLOSED 복귀, Erase 안 함 */
            switch (fr) {
            case ERR_FTL_SRC_READ:
                add_skip(v.chip, v.block);  /* Victim 자체 문제 → 다음엔 다른 블록 */
                res = ERR_GC_MOVE_FAIL;
                break;
            case ERR_FTL_DST_PROGRAM:
                res = ERR_GC_MOVE_FAIL;     /* 목적지 문제(FTL 이 격리) → Victim 은 다시 후보 */
                break;
            case ERR_FTL_NO_SPACE:
                res = ERR_GC_NO_SPACE;      /* 예비 블록까지 소진 → 호스트 쓰기 실패로 올라감 */
                break;
            default:
                res = ERR_GC_FTL;           /* BAD_STATE 등 계약 위반 */
                break;
            }
            goto done;
        }
        if (!moved) {
            break;
        }
        moved_here++;
        G.st.pages_moved++;
        if (moved_here > NAND_PAGES_PER_BLOCK) {
            (void)ftl_unreserve_victim(v.chip, v.block);
            add_skip(v.chip, v.block);
            res = ERR_GC_FTL;
            goto done;
        }
    }
    log_ev(GC_EV_MOVED, v.chip, v.block, moved_here);

    ftl_result_t er = ftl_reclaim_block(v.chip, v.block);
    if (er == FTL_OK) {
        G.st.blocks_reclaimed++;
        log_ev(GC_EV_ERASED, v.chip, v.block, ftl_get_free_block_count());
        res = GC_OK;
    } else if (er == ERR_FTL_NAND_ERASE) {
        G.st.erase_fail++;                                /* FTL 이 BAD 처리 */
        log_ev(GC_EV_ERASE_FAIL, v.chip, v.block, (uint32_t)er);
        res = ERR_GC_ERASE_FAIL;
    } else {
        /* 유효 Page 가 남았다는 등 예상 밖 거부 → 지우지 말고 되돌린다 */
        (void)ftl_unreserve_victim(v.chip, v.block);
        add_skip(v.chip, v.block);
        res = ERR_GC_FTL;
    }

done:
    G.busy = false;
    return res;
}

void gc_get_stats(gc_stats_t *out)
{
    if (out != NULL) {
        *out = G.st;
    }
}
