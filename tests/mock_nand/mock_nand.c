/*
 * mock_nand.c - PC(gcc)에서 동작하는 가짜 W25N01GV
 *
 * firmware/common/nand.h 를 그대로 구현한다. FTL/GC 는 실제 드라이버와
 * 이 파일을 구분하지 않고 같은 nand_* 함수를 호출한다.
 *
 * 실제 NAND 제약을 그대로 강제한다 (어기면 에러 + 위반 카운터 증가):
 *   1. 이미 쓴 Page 에 다시 Program 할 수 없다          → violation_reprogram
 *   2. Block 안에서는 낮은 Page → 높은 Page 순서로만 쓴다 → violation_order
 *   3. Erase 는 Block 단위, 지우면 전부 0xFF               (erase_block)
 *   4. 공장 Bad Block 에는 Program/Erase 하지 않는다       → violation_bad_block
 *
 * 메모리: 8칩 × 1024 Block 전체(약 1.1GB)를 미리 잡지 않고,
 *         Block 을 처음 쓸 때만 malloc 한다. 안 쓴 Block 은 0xFF 로 읽힌다.
 */
#include <stdlib.h>
#include <string.h>
#include "mock_nand.h"

#define RAW_PAGE_SIZE   (NAND_PAGE_SIZE + NAND_SPARE_SIZE)   /* 2112B */
#define MAX_INJECTIONS  32U

typedef struct {
    uint8_t raw[NAND_PAGES_PER_BLOCK][RAW_PAGE_SIZE];  /* main 2048 + spare 64 */
    uint8_t written[NAND_PAGES_PER_BLOCK];             /* Erase 이후 Program 되었는가 */
    uint8_t next_page;                                 /* 다음에 쓸 수 있는 최소 Page */
} mock_block_t;

typedef struct {
    bool          used;
    mock_op_t     op;
    uint8_t       chip;
    uint16_t      block;
    nand_result_t result;
    bool          ecc_corrected;   /* read 전용: NAND_OK + CORRECTED */
    uint32_t      remaining;       /* 0 = 영구 */
} injection_t;

static struct {
    bool               configured;
    bool               initialized;
    mock_nand_config_t cfg;
    mock_block_t      *blk[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];
    uint32_t           erase_count[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];
    injection_t        inj[MAX_INJECTIONS];
    mock_nand_stats_t  st;
    nand_status_t      last_status[NAND_MAX_CHIPS];
} M;

/* ================================================================ 내부 유틸 */

static void free_all_blocks(void)
{
    for (uint32_t c = 0; c < NAND_MAX_CHIPS; c++) {
        for (uint32_t b = 0; b < NAND_BLOCKS_PER_CHIP; b++) {
            free(M.blk[c][b]);
            M.blk[c][b] = NULL;
        }
    }
}

/* 처음 접근하는 Block 이면 0xFF 로 채운 새 Block 을 만든다 */
static mock_block_t *get_block(uint8_t chip, uint16_t block)
{
    mock_block_t *b = M.blk[chip][block];
    if (b == NULL) {
        b = (mock_block_t *)malloc(sizeof(*b));
        if (b == NULL) {
            return NULL;
        }
        memset(b->raw, 0xFF, sizeof(b->raw));
        memset(b->written, 0, sizeof(b->written));
        b->next_page = 0;
        M.blk[chip][block] = b;
    }
    return b;
}

static bool is_factory_bad(uint8_t chip, uint16_t block)
{
    const mock_block_t *b = M.blk[chip][block];
    return (b != NULL) && (b->raw[0][NAND_PAGE_SIZE] != 0xFF);
}

/* 칩 번호 검사: 범위 밖이면 INVALID_ARG, 설정된 칩 수 밖이면 NO_DEVICE */
static nand_result_t check_chip(uint8_t chip)
{
    if (!M.initialized) {
        return NAND_ERR_NOT_INITIALIZED;
    }
    if (chip >= NAND_MAX_CHIPS) {
        M.st.violation_bad_arg++;
        return NAND_ERR_INVALID_ARG;
    }
    if (chip >= M.cfg.chip_count) {
        return NAND_ERR_NO_DEVICE;
    }
    return NAND_OK;
}

static nand_result_t check_addr(nand_addr_t a)
{
    nand_result_t r = check_chip(a.chip);
    if (r != NAND_OK) {
        return r;
    }
    if (a.block >= NAND_BLOCKS_PER_CHIP || a.page >= NAND_PAGES_PER_BLOCK) {
        M.st.violation_bad_arg++;
        return NAND_ERR_INVALID_ARG;
    }
    return NAND_OK;
}

/* 주입된 실패가 있으면 하나 소비하고 그 결과를 돌려준다 */
static bool take_injection(mock_op_t op, uint8_t chip, uint16_t block,
                           nand_result_t *res, bool *ecc_corrected)
{
    for (uint32_t i = 0; i < MAX_INJECTIONS; i++) {
        injection_t *j = &M.inj[i];
        if (!j->used || j->op != op) {
            continue;
        }
        if (j->chip != MOCK_ANY_CHIP && j->chip != chip) {
            continue;
        }
        if (j->block != MOCK_ANY_BLOCK && j->block != block) {
            continue;
        }
        *res = j->result;
        if (ecc_corrected != NULL) {
            *ecc_corrected = j->ecc_corrected;
        }
        if (j->remaining > 0 && --j->remaining == 0) {
            j->used = false;
        }
        return true;
    }
    return false;
}

static void add_injection(const injection_t *src)
{
    for (uint32_t i = 0; i < MAX_INJECTIONS; i++) {
        if (!M.inj[i].used) {
            M.inj[i] = *src;
            M.inj[i].used = true;
            return;
        }
    }
    /* 슬롯이 가득 차면 무시 (테스트에서 32개 이상 걸 일은 없음) */
}

/* ================================================================ nand.h 구현 */

nand_result_t nand_init(void)
{
    if (!M.configured) {
        mock_nand_config_t cfg = mock_nand_default_config();
        mock_nand_reset(&cfg);
    }
    M.initialized = true;
    return NAND_OK;
}

nand_result_t nand_probe(uint8_t chip)
{
    return check_chip(chip);
}

nand_result_t nand_get_geometry(uint8_t chip, nand_geometry_t *geometry)
{
    nand_result_t r = check_chip(chip);
    if (r != NAND_OK) {
        return r;
    }
    if (geometry == NULL) {
        M.st.violation_bad_arg++;
        return NAND_ERR_INVALID_ARG;
    }
    geometry->page_size       = NAND_PAGE_SIZE;
    geometry->spare_size      = NAND_SPARE_SIZE;
    geometry->pages_per_block = NAND_PAGES_PER_BLOCK;
    geometry->blocks_per_chip = NAND_BLOCKS_PER_CHIP;
    return NAND_OK;
}

nand_result_t nand_read_page(nand_addr_t addr, uint8_t *buffer, nand_ecc_t *ecc)
{
    M.st.read_calls++;
    if (ecc != NULL) {
        *ecc = NAND_ECC_UNKNOWN;
    }

    nand_result_t r = check_addr(addr);
    if (r != NAND_OK) {
        return r;
    }
    if (buffer == NULL) {
        M.st.violation_bad_arg++;
        return NAND_ERR_INVALID_ARG;
    }

    M.st.sim_time_us += MOCK_T_READ_US;
    nand_status_t *s = &M.last_status[addr.chip];

    nand_result_t inj = NAND_OK;
    bool corrected = false;
    if (take_injection(MOCK_OP_READ, addr.chip, addr.block, &inj, &corrected)) {
        if (inj != NAND_OK) {
            /* 실패 시 버퍼는 쓰레기로 채워서, 호출자가 잘못 쓰면 바로 티가 나게 한다 */
            memset(buffer, 0x5A, NAND_PAGE_SIZE);
            s->ecc = (inj == NAND_ERR_ECC_UNCORRECTABLE) ? NAND_ECC_UNCORRECTABLE : NAND_ECC_UNKNOWN;
            if (ecc != NULL) {
                *ecc = s->ecc;
            }
            return inj;
        }
    }

    const mock_block_t *b = M.blk[addr.chip][addr.block];
    if (b == NULL) {
        memset(buffer, 0xFF, NAND_PAGE_SIZE);
    } else {
        memcpy(buffer, b->raw[addr.page], NAND_PAGE_SIZE);
    }

    s->ecc = corrected ? NAND_ECC_CORRECTED : NAND_ECC_NO_ERROR;
    if (ecc != NULL) {
        *ecc = s->ecc;
    }
    return NAND_OK;
}

nand_result_t nand_program_page(nand_addr_t addr, const uint8_t *buffer)
{
    M.st.program_calls++;

    nand_result_t r = check_addr(addr);
    if (r != NAND_OK) {
        return r;
    }
    if (buffer == NULL) {
        M.st.violation_bad_arg++;
        return NAND_ERR_INVALID_ARG;
    }
    if (is_factory_bad(addr.chip, addr.block)) {
        M.st.violation_bad_block++;
        return NAND_ERR_INVALID_ARG;
    }

    mock_block_t *b = get_block(addr.chip, addr.block);
    if (b == NULL) {
        return NAND_ERR_IO;
    }
    if (b->written[addr.page]) {
        M.st.violation_reprogram++;
        return NAND_ERR_INVALID_ARG;
    }
    if (addr.page < b->next_page ||
        (M.cfg.strict_sequential && addr.page != b->next_page)) {
        M.st.violation_order++;
        return NAND_ERR_INVALID_ARG;
    }

    M.st.sim_time_us += MOCK_T_PROGRAM_US;
    nand_status_t *s = &M.last_status[addr.chip];

    /* 여기부터는 칩이 실제로 Program 을 시도한 것으로 본다: Page 는 소모된다 */
    b->written[addr.page] = 1;
    b->next_page = (uint8_t)(addr.page + 1U);

    nand_result_t inj = NAND_OK;
    if (take_injection(MOCK_OP_PROGRAM, addr.chip, addr.block, &inj, NULL) && inj != NAND_OK) {
        /* 실패한 Page 는 반쯤 쓰인 상태로 남긴다 (재사용 금지 확인용) */
        memset(b->raw[addr.page], 0x00, NAND_PAGE_SIZE / 2U);
        s->program_failed = true;
        return inj;
    }

    /* NAND 는 1→0 만 가능: AND 로 기록 */
    for (uint32_t i = 0; i < NAND_PAGE_SIZE; i++) {
        b->raw[addr.page][i] &= buffer[i];
    }
    s->program_failed = false;
    M.st.program_ok++;
    return NAND_OK;
}

nand_result_t nand_erase_block(uint8_t chip, uint16_t block)
{
    M.st.erase_calls++;

    nand_result_t r = check_chip(chip);
    if (r != NAND_OK) {
        return r;
    }
    if (block >= NAND_BLOCKS_PER_CHIP) {
        M.st.violation_bad_arg++;
        return NAND_ERR_INVALID_ARG;
    }
    if (is_factory_bad(chip, block)) {
        /* 실제 칩은 지워지면서 공장 Marker 를 잃는다 → 절대 하면 안 됨 */
        M.st.violation_bad_block++;
        return NAND_ERR_INVALID_ARG;
    }

    M.st.sim_time_us += MOCK_T_ERASE_US;
    M.erase_count[chip][block]++;
    nand_status_t *s = &M.last_status[chip];

    nand_result_t inj = NAND_OK;
    if (take_injection(MOCK_OP_ERASE, chip, block, &inj, NULL) && inj != NAND_OK) {
        s->erase_failed = true;
        return inj;   /* 내용은 그대로 둔다: 지워졌다고 믿으면 안 된다 */
    }

    mock_block_t *b = M.blk[chip][block];
    if (b != NULL) {
        memset(b->raw, 0xFF, sizeof(b->raw));
        memset(b->written, 0, sizeof(b->written));
        b->next_page = 0;
    }
    s->erase_failed = false;
    M.st.erase_ok++;
    return NAND_OK;
}

nand_result_t nand_is_factory_bad_block(uint8_t chip, uint16_t block, bool *is_bad)
{
    nand_result_t r = check_chip(chip);
    if (r != NAND_OK) {
        return r;
    }
    if (block >= NAND_BLOCKS_PER_CHIP || is_bad == NULL) {
        M.st.violation_bad_arg++;
        return NAND_ERR_INVALID_ARG;
    }
    M.st.sim_time_us += MOCK_T_READ_US;
    *is_bad = is_factory_bad(chip, block);
    return NAND_OK;
}

nand_result_t nand_get_status(uint8_t chip, nand_status_t *status)
{
    nand_result_t r = check_chip(chip);
    if (r != NAND_OK) {
        return r;
    }
    if (status == NULL) {
        M.st.violation_bad_arg++;
        return NAND_ERR_INVALID_ARG;
    }
    *status = M.last_status[chip];
    return NAND_OK;
}

/* ================================================================ mock 제어 API */

mock_nand_config_t mock_nand_default_config(void)
{
    mock_nand_config_t c;
    c.chip_count = NAND_MAX_CHIPS;
    c.strict_sequential = false;
    return c;
}

void mock_nand_reset(const mock_nand_config_t *cfg)
{
    free_all_blocks();
    memset(M.erase_count, 0, sizeof(M.erase_count));
    memset(M.inj, 0, sizeof(M.inj));
    memset(&M.st, 0, sizeof(M.st));
    memset(M.last_status, 0, sizeof(M.last_status));

    M.cfg = (cfg != NULL) ? *cfg : mock_nand_default_config();
    if (M.cfg.chip_count == 0 || M.cfg.chip_count > NAND_MAX_CHIPS) {
        M.cfg.chip_count = NAND_MAX_CHIPS;
    }
    M.configured  = true;
    M.initialized = false;   /* nand_init() 을 다시 불러야 사용 가능 */
}

void mock_nand_destroy(void)
{
    free_all_blocks();
    M.configured  = false;
    M.initialized = false;
}

void mock_nand_mark_factory_bad(uint8_t chip, uint16_t block)
{
    if (chip >= NAND_MAX_CHIPS || block >= NAND_BLOCKS_PER_CHIP) {
        return;
    }
    mock_block_t *b = get_block(chip, block);
    if (b != NULL) {
        b->raw[0][NAND_PAGE_SIZE] = 0x00;   /* Page 0 Spare 첫 바이트 = 불량 표시 */
    }
}

void mock_nand_inject(mock_op_t op, uint8_t chip, uint16_t block,
                      nand_result_t result, uint32_t count)
{
    injection_t j = { 0 };
    j.op = op;
    j.chip = chip;
    j.block = block;
    j.result = result;
    j.remaining = count;
    add_injection(&j);
}

void mock_nand_inject_ecc_corrected(uint8_t chip, uint16_t block, uint32_t count)
{
    injection_t j = { 0 };
    j.op = MOCK_OP_READ;
    j.chip = chip;
    j.block = block;
    j.result = NAND_OK;
    j.ecc_corrected = true;
    j.remaining = count;
    add_injection(&j);
}

void mock_nand_clear_injections(void)
{
    memset(M.inj, 0, sizeof(M.inj));
}

void mock_nand_get_stats(mock_nand_stats_t *out)
{
    if (out != NULL) {
        *out = M.st;
    }
}

void mock_nand_clear_stats(void)
{
    memset(&M.st, 0, sizeof(M.st));
}

uint32_t mock_nand_violations(void)
{
    return M.st.violation_reprogram + M.st.violation_order +
           M.st.violation_bad_block + M.st.violation_bad_arg;
}

uint32_t mock_nand_erase_count(uint8_t chip, uint16_t block)
{
    if (chip >= NAND_MAX_CHIPS || block >= NAND_BLOCKS_PER_CHIP) {
        return 0;
    }
    return M.erase_count[chip][block];
}

uint8_t mock_nand_next_page(uint8_t chip, uint16_t block)
{
    if (chip >= NAND_MAX_CHIPS || block >= NAND_BLOCKS_PER_CHIP || M.blk[chip][block] == NULL) {
        return 0;
    }
    return M.blk[chip][block]->next_page;
}

bool mock_nand_page_written(uint8_t chip, uint16_t block, uint8_t page)
{
    if (chip >= NAND_MAX_CHIPS || block >= NAND_BLOCKS_PER_CHIP ||
        page >= NAND_PAGES_PER_BLOCK || M.blk[chip][block] == NULL) {
        return false;
    }
    return M.blk[chip][block]->written[page] != 0;
}

void mock_nand_peek_spare(nand_addr_t a, uint8_t out[NAND_SPARE_SIZE])
{
    if (a.chip >= NAND_MAX_CHIPS || a.block >= NAND_BLOCKS_PER_CHIP || a.page >= NAND_PAGES_PER_BLOCK) {
        return;
    }
    const mock_block_t *b = M.blk[a.chip][a.block];
    if (b == NULL) {
        memset(out, 0xFF, NAND_SPARE_SIZE);
    } else {
        memcpy(out, &b->raw[a.page][NAND_PAGE_SIZE], NAND_SPARE_SIZE);
    }
}

void mock_nand_poke_spare(nand_addr_t a, const uint8_t in[NAND_SPARE_SIZE])
{
    if (a.chip >= NAND_MAX_CHIPS || a.block >= NAND_BLOCKS_PER_CHIP || a.page >= NAND_PAGES_PER_BLOCK) {
        return;
    }
    mock_block_t *b = get_block(a.chip, a.block);
    if (b == NULL) {
        return;
    }
    for (uint32_t i = 0; i < NAND_SPARE_SIZE; i++) {
        b->raw[a.page][NAND_PAGE_SIZE + i] &= in[i];
    }
}
