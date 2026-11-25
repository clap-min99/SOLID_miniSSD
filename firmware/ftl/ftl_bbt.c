#include "ftl_bbt.h"
#include "ftl_alloc.h"

/*
 * BBT Page 형식 (2,048B, 리틀 엔디언)
 *   [0]        0xFF        Main[0] 공장 마커 자리를 비워 둔다 → BBT Block도 공장 검사로 계속 찾을 수 있음
 *   [1..3]     "BBT"
 *   [4..7]     seq         저장할 때마다 +1. 부팅 때 CRC가 맞는 Page 중 가장 큰 seq를 쓴다
 *   [8..9]     count
 *   [10..]     count × {chip(1B), block(2B)}
 *   [2044..]   CRC-32 ([0..2043])
 */
#define HDR      10U
#define CRC_OFF  (NAND_PAGE_SIZE - 4U)

typedef struct {
    uint8_t  chip;
    uint16_t block;
} bbt_entry_t;

static bbt_entry_t entries[FTL_BBT_MAX_ENTRIES];
static uint16_t    count;
static uint16_t    bbt_blk[2];   /* BBT 전용 Block 번호 (칩 0) */
static uint8_t     cur;          /* 지금 이어 쓰는 쪽 */
static uint8_t     next_page;    /* cur에서 다음에 쓸 Page. 64면 다음 저장 때 반대쪽으로 넘어감 */
static uint32_t    seq;
static uint8_t     page[NAND_PAGE_SIZE];

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFU;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
        }
    }
    return ~c;
}

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void     put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* page[]가 올바른 BBT면 true */
static bool page_valid(void)
{
    uint16_t n = (uint16_t)(page[8] | page[9] << 8);
    return page[0] == 0xFF && page[1] == 'B' && page[2] == 'B' && page[3] == 'T' &&
           n <= FTL_BBT_MAX_ENTRIES && get32(page + CRC_OFF) == crc32(page, CRC_OFF);
}

static solid_err_t find_blocks(void)
{
    uint8_t got = 0;
    for (uint16_t i = 0; i < FTL_BBT_CANDIDATES && got < 2; i++) {
        uint16_t b = (uint16_t)(FTL_BBT_FIRST_CANDIDATE - i);
        bool bad = false;
        nand_result_t r = nand_is_factory_bad_block(0, b, &bad);
        if (r != NAND_OK) {
            return ftl_from_nand(r);
        }
        if (!bad) {
            bbt_blk[got++] = b;
        }
    }
    return got == 2 ? SOLID_OK : ERR_FTL_NO_SPACE;
}

solid_err_t ftl_bbt_load(bool *found)
{
    count = 0;
    seq = 0;
    *found = false;
    solid_err_t e = find_blocks();
    if (e != SOLID_OK) {
        return e;
    }

    int best_blk = -1, best_page = -1;
    uint8_t last[2] = {0, 0};   /* 각 Block에서 마지막으로 쓴 Page + 1 */
    for (uint8_t k = 0; k < 2; k++) {
        for (uint8_t p = 0; p < NAND_PAGES_PER_BLOCK; p++) {
            nand_ecc_t ecc;
            nand_addr_t a = { .chip = 0, .block = bbt_blk[k], .page = p };
            nand_result_t r = nand_read_page(a, page, &ecc);
            if (r == NAND_ERR_ECC_UNCORRECTABLE) {
                break;
            }
            if (r != NAND_OK) {
                return ftl_from_nand(r);
            }
            if (!page_valid()) {
                break;          /* 순서대로 쓰므로 첫 빈(또는 깨진) Page가 끝 */
            }
            last[k] = (uint8_t)(p + 1);
            uint32_t s = get32(page + 4);
            if (best_blk < 0 || s > seq) {
                seq = s;
                best_blk = k;
                best_page = p;
            }
        }
    }

    if (best_blk < 0) {         /* BBT 없음 → 처음 부팅. 첫 저장 때 bbt_blk[0]을 지우고 시작 */
        cur = 1;
        next_page = NAND_PAGES_PER_BLOCK;
        return SOLID_OK;
    }
    nand_ecc_t ecc;
    nand_addr_t a = { .chip = 0, .block = bbt_blk[best_blk], .page = (uint8_t)best_page };
    nand_result_t r = nand_read_page(a, page, &ecc);
    if (r != NAND_OK || !page_valid()) {
        return r != NAND_OK ? ftl_from_nand(r) : ERR_FTL_INTERNAL;
    }
    count = (uint16_t)(page[8] | page[9] << 8);
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *q = page + HDR + i * 3U;
        entries[i] = (bbt_entry_t){ .chip = q[0], .block = (uint16_t)(q[1] | q[2] << 8) };
    }
    cur = (uint8_t)best_blk;
    next_page = last[best_blk];
    *found = true;
    return SOLID_OK;
}

bool ftl_bbt_is_bad(uint8_t chip, uint16_t block)
{
    for (uint16_t i = 0; i < count; i++) {
        if (entries[i].chip == chip && entries[i].block == block) {
            return true;
        }
    }
    return false;
}

solid_err_t ftl_bbt_add(uint8_t chip, uint16_t block)
{
    if (ftl_bbt_is_bad(chip, block)) {
        return SOLID_OK;
    }
    if (count >= FTL_BBT_MAX_ENTRIES) {
        return ERR_FTL_INTERNAL;
    }
    entries[count++] = (bbt_entry_t){ .chip = chip, .block = block };
    return SOLID_OK;
}

uint16_t ftl_bbt_count(void)
{
    return count;
}

solid_err_t ftl_bbt_save(void)
{
    for (uint16_t i = 0; i < NAND_PAGE_SIZE; i++) {
        page[i] = 0xFF;
    }
    seq++;
    page[1] = 'B'; page[2] = 'B'; page[3] = 'T';
    put32(page + 4, seq);
    page[8] = (uint8_t)count;
    page[9] = (uint8_t)(count >> 8);
    for (uint16_t i = 0; i < count; i++) {
        uint8_t *q = page + HDR + i * 3U;
        q[0] = entries[i].chip;
        q[1] = (uint8_t)entries[i].block;
        q[2] = (uint8_t)(entries[i].block >> 8);
    }
    put32(page + CRC_OFF, crc32(page, CRC_OFF));

    /* Program 실패면 반대쪽 Block으로 넘어가 한 번 더. 최신 목록이 있는 Block은 넘어가기 전까지 안 지운다 */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (next_page >= NAND_PAGES_PER_BLOCK) {
            cur ^= 1U;
            nand_result_t r = nand_erase_block(0, bbt_blk[cur]);
            if (r != NAND_OK) {
                return ftl_from_nand(r);   /* ponytail: BBT Block 자체가 고장 나면 포기. 후보 Block으로 교체는 필요해지면 */
            }
            next_page = 0;
        }
        nand_addr_t a = { .chip = 0, .block = bbt_blk[cur], .page = next_page };
        nand_result_t r = nand_program_page(a, page);
        if (r == NAND_OK) {
            next_page++;
            return SOLID_OK;
        }
        if (r != NAND_ERR_PROGRAM_FAIL) {
            return ftl_from_nand(r);
        }
        next_page = NAND_PAGES_PER_BLOCK;
    }
    return ERR_NAND_PROGRAM_FAIL;
}
