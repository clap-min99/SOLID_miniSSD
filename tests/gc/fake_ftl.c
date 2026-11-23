/*
 * fake_ftl.c - GC 테스트용 가짜 FTL
 */
#include <string.h>
#include "fake_ftl.h"

ftl_block_info_t fake_tbl[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];
bool             fake_query_fail[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];
uint32_t         fake_info_calls;
uint16_t         fake_max_block_seen;
uint8_t          fake_max_chip_seen;
char             fake_trace[256];

int          fake_move_fail_at;
solid_err_t fake_move_fail_code;
bool         fake_erase_fail;
bool         fake_reclaim_force_valid_remains;
bool         fake_reserve_reject;
void       (*fake_on_move)(void);

static int move_calls;

static void trace(char c)
{
    size_t n = strlen(fake_trace);
    if (n + 1 < sizeof(fake_trace)) {
        fake_trace[n] = c;
        fake_trace[n + 1] = '\0';
    }
}

static bool bad_addr(uint8_t chip, uint16_t block)
{
    return chip >= NAND_MAX_CHIPS || block >= NAND_BLOCKS_PER_CHIP;
}

void fake_ftl_reset(void)
{
    memset(fake_tbl, 0, sizeof(fake_tbl));          /* 전부 FREE */
    memset(fake_query_fail, 0, sizeof(fake_query_fail));
    fake_info_calls = 0;
    fake_max_block_seen = 0;
    fake_max_chip_seen = 0;
    fake_trace[0] = '\0';
    fake_move_fail_at = -1;
    fake_move_fail_code = ERR_FTL_SRC_READ;
    fake_erase_fail = false;
    fake_reclaim_force_valid_remains = false;
    fake_reserve_reject = false;
    fake_on_move = NULL;
    move_calls = 0;
}

void fake_ftl_reset_move_counter(void)
{
    move_calls = 0;
}

void fake_set_closed(uint8_t c, uint16_t b, uint16_t valid, uint32_t erase)
{
    fake_tbl[c][b].state = FTL_BLK_CLOSED;
    fake_tbl[c][b].write_pointer = NAND_PAGES_PER_BLOCK;
    fake_tbl[c][b].valid_pages = valid;
    fake_tbl[c][b].invalid_pages = (uint16_t)(NAND_PAGES_PER_BLOCK - valid);
    fake_tbl[c][b].erase_count = erase;
}

void fake_set_state(uint8_t c, uint16_t b, ftl_block_state_t s, uint16_t valid, uint16_t invalid)
{
    fake_tbl[c][b].state = s;
    fake_tbl[c][b].write_pointer = (uint8_t)(valid + invalid);
    fake_tbl[c][b].valid_pages = valid;
    fake_tbl[c][b].invalid_pages = invalid;
    fake_tbl[c][b].erase_count = 0;
}

void fake_fill_all_full_valid(void)
{
    for (uint8_t c = 0; c < NAND_MAX_CHIPS; c++)
        for (uint16_t b = 0; b < NAND_BLOCKS_PER_CHIP; b++)
            fake_set_closed(c, b, 64, 0);
}

/* ================================================================ gc_ftl_if.h 구현 */

solid_err_t ftl_get_block_info(uint8_t chip, uint16_t block, ftl_block_info_t *out)
{
    fake_info_calls++;
    if (chip > fake_max_chip_seen)   fake_max_chip_seen = chip;
    if (block > fake_max_block_seen) fake_max_block_seen = block;

    if (out == NULL) return ERR_FTL_INVALID_ARG;
    if (bad_addr(chip, block) || fake_query_fail[chip][block]) {
        return ERR_FTL_RANGE;
    }
    *out = fake_tbl[chip][block];   /* 복사본만 준다 */
    return SOLID_OK;
}

uint16_t ftl_get_free_block_count(void)
{
    uint16_t n = 0;
    for (uint8_t c = 0; c < NAND_MAX_CHIPS; c++)
        for (uint16_t b = 0; b < 24U; b++)
            if (fake_tbl[c][b].state == FTL_BLK_FREE) n++;
    return n;
}

solid_err_t ftl_reserve_victim(uint8_t chip, uint16_t block)
{
    trace('R');
    if (bad_addr(chip, block)) return ERR_FTL_RANGE;
    if (fake_reserve_reject || fake_tbl[chip][block].state != FTL_BLK_CLOSED) {
        return ERR_FTL_BAD_STATE;
    }
    fake_tbl[chip][block].state = FTL_BLK_RESERVED;
    return SOLID_OK;
}

solid_err_t ftl_unreserve_victim(uint8_t chip, uint16_t block)
{
    trace('U');
    if (bad_addr(chip, block)) return ERR_FTL_RANGE;
    if (fake_tbl[chip][block].state != FTL_BLK_RESERVED) return ERR_FTL_BAD_STATE;
    fake_tbl[chip][block].state = FTL_BLK_CLOSED;
    return SOLID_OK;
}

solid_err_t ftl_gc_move_one(uint8_t chip, uint16_t block, bool *moved)
{
    if (moved == NULL) return ERR_FTL_INVALID_ARG;
    if (bad_addr(chip, block)) return ERR_FTL_RANGE;
    *moved = false;
    if (fake_on_move != NULL) {
        fake_on_move();
    }
    if (fake_tbl[chip][block].state != FTL_BLK_RESERVED) return ERR_FTL_BAD_STATE;

    move_calls++;
    if (fake_move_fail_at >= 0 && move_calls == fake_move_fail_at) {
        trace('X');
        return fake_move_fail_code;    /* 기존 Mapping 유지: valid 수 그대로 */
    }
    if (fake_tbl[chip][block].valid_pages == 0) {
        return SOLID_OK;                 /* 더 옮길 것 없음 */
    }
    trace('M');
    fake_tbl[chip][block].valid_pages--;
    fake_tbl[chip][block].invalid_pages++;
    *moved = true;
    return SOLID_OK;
}

solid_err_t ftl_reclaim_block(uint8_t chip, uint16_t block)
{
    trace('E');
    if (bad_addr(chip, block)) return ERR_FTL_RANGE;
    ftl_block_info_t *t = &fake_tbl[chip][block];
    if (t->state != FTL_BLK_RESERVED) return ERR_FTL_BAD_STATE;
    if (t->valid_pages > 0 || fake_reclaim_force_valid_remains) return ERR_FTL_VALID_REMAINS;

    t->erase_count++;
    if (fake_erase_fail) {
        fake_erase_fail = false;
        t->state = FTL_BLK_BAD;
        return ERR_NAND_ERASE_FAIL;
    }
    t->state = FTL_BLK_FREE;
    t->write_pointer = 0;
    t->valid_pages = 0;
    t->invalid_pages = 0;
    return SOLID_OK;
}
