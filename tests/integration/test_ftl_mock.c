/*
 * test_ftl_mock.c - 진짜 FTL + Mock NAND 통합 시뮬레이션
 *
 * 목적
 *   1) 진짜 FTL(ftl.c, ftl_alloc.c, ftl_map.c, address_mapper.c)이 NAND 규칙을 어기지 않는지
 *      (Mock NAND 위반 카운터 = 0)
 *   2) 무작위 덮어쓰기 후에도 모든 LPN이 마지막에 쓴 데이터를 돌려주는지
 *   3) FTL Block 표(valid/invalid/state)가 실제 매핑·Mock NAND 상태와 맞는지
 *   4) GC Victim 선택이 진짜 FTL 표에서도 Greedy 규칙대로 동작하는지
 *   5) WAF 계산 틀 (Host 쓰기 수 vs NAND Program 수)
 *
 * FTL-07(ftl_reserve_victim 등) 전이라 GC 회수(gc_collect)는 아직 붙이지 않는다.
 * GC 없이 무작위 덮어쓰기를 하면 언젠가 ERR_FTL_NO_SPACE 가 난다 → 그 지점을 기록.
 * FTL-07 이 들어오면 아래 TEMP 스텁 4개를 지우고 NO_SPACE 대신 gc_collect 를 부르면 된다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ftl.h"
#include "ftl_gc.h"
#include "ftl_map.h"
#include "address_mapper.h"
#include "gc.h"
#include "mock_nand.h"

/* ================================================================ TEMP: FTL-07 스텁
 * gc.c 를 링크하려면 필요. 이 테스트는 gc_select_victim(조회만)만 쓰므로 불리면 안 된다. */
static int stub_calls;
solid_err_t ftl_reserve_victim(uint8_t c, uint16_t b)   { (void)c; (void)b; stub_calls++; return ERR_FTL_NOT_SUPPORTED; }
solid_err_t ftl_unreserve_victim(uint8_t c, uint16_t b) { (void)c; (void)b; stub_calls++; return ERR_FTL_NOT_SUPPORTED; }
solid_err_t ftl_gc_move_one(uint8_t c, uint16_t b, bool *m) { (void)c; (void)b; (void)m; stub_calls++; return ERR_FTL_NOT_SUPPORTED; }
solid_err_t ftl_reclaim_block(uint8_t c, uint16_t b)    { (void)c; (void)b; stub_calls++; return ERR_FTL_NOT_SUPPORTED; }

/* ================================================================ 작은 테스트 틀 */
static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* 재현 가능한 난수 (xorshift32) */
static uint32_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

#define LPN_N FTL_LOGICAL_PAGE_COUNT
static uint32_t version[LPN_N];   /* 0 = 쓴 적 없음, 아니면 마지막으로 성공한 쓰기 번호 */
static uint8_t  wbuf[NAND_PAGE_SIZE], rbuf[NAND_PAGE_SIZE];

/* LPN·버전마다 다른 2KiB 패턴 (앞 8바이트에 LPN/버전을 그대로 넣어 디버그 쉽게) */
static void fill(uint8_t *p, uint16_t lpn, uint32_t ver)
{
    uint32_t s = (uint32_t)lpn * 2654435761u ^ ver * 40503u ^ 0x5A5A5A5Au;
    for (size_t i = 0; i < NAND_PAGE_SIZE; i++) {
        s = s * 1103515245u + 12345u;
        p[i] = (uint8_t)(s >> 16);
    }
    memcpy(p, &lpn, sizeof lpn);
    memcpy(p + 4, &ver, sizeof ver);
}

static uint32_t host_writes, next_ver;

static solid_err_t host_write(uint16_t lpn)
{
    uint32_t v = ++next_ver;
    fill(wbuf, lpn, v);
    solid_err_t e = ftl_write(lpn, wbuf);
    if (e == SOLID_OK) {
        version[lpn] = v;
        host_writes++;
    }
    return e;
}

/* 모든 LPN 을 읽어 기대값과 비교. 틀린 개수 반환 */
static uint32_t verify_all(void)
{
    uint32_t bad = 0;
    uint8_t exp[NAND_PAGE_SIZE];
    for (uint16_t lpn = 0; lpn < LPN_N; lpn++) {
        solid_err_t e = ftl_read(lpn, rbuf);
        if (version[lpn] == 0) {
            if (e != ERR_FTL_UNMAPPED) bad++;
            continue;
        }
        fill(exp, lpn, version[lpn]);
        if (e != SOLID_OK || memcmp(exp, rbuf, NAND_PAGE_SIZE) != 0) bad++;
    }
    return bad;
}

/* FTL Block 표 일관성:
 *  - 모든 Block: valid + invalid == write_pointer
 *  - valid 합계 == 매핑된 LPN 수
 *  - FREE 수 == ftl_get_free_block_count()
 *  - FTL write_pointer == Mock NAND next_page (실제로 쓴 위치와 장부가 같다)
 *  - OPEN Block 은 최대 1개 */
static void check_table(uint8_t chips, const char *tag)
{
    uint32_t valid_sum = 0, mapped = 0, free_n = 0, open_n = 0, wp_mismatch = 0, sum_mismatch = 0;
    for (uint8_t c = 0; c < chips; c++) {
        for (uint16_t b = FTL_FIRST_BLOCK; b < FTL_FIRST_BLOCK + FTL_ACTIVE_BLOCKS_PER_CHIP; b++) {
            ftl_block_info_t bi;
            if (ftl_get_block_info(c, b, &bi) != SOLID_OK) { sum_mismatch++; continue; }
            if (bi.valid_pages + bi.invalid_pages != bi.write_pointer) sum_mismatch++;
            if (bi.state != FTL_BLK_BAD && bi.write_pointer != mock_nand_next_page(c, b)) wp_mismatch++;
            valid_sum += bi.valid_pages;
            if (bi.state == FTL_BLK_FREE) free_n++;
            if (bi.state == FTL_BLK_OPEN) open_n++;
        }
    }
    for (uint16_t lpn = 0; lpn < LPN_N; lpn++) mapped += version[lpn] != 0;
    CHECK(sum_mismatch == 0, "[%s] valid+invalid != write_pointer: %u blocks", tag, (unsigned)sum_mismatch);
    CHECK(wp_mismatch == 0, "[%s] FTL write_pointer != Mock next_page: %u blocks", tag, (unsigned)wp_mismatch);
    CHECK(valid_sum == mapped, "[%s] valid 합계 %u != 매핑된 LPN %u", tag, (unsigned)valid_sum, (unsigned)mapped);
    CHECK(free_n == ftl_get_free_block_count(), "[%s] FREE %u != free_count %u", tag, (unsigned)free_n,
          (unsigned)ftl_get_free_block_count());
    CHECK(open_n <= 1, "[%s] OPEN Block %u개", tag, (unsigned)open_n);
}

/* 진짜 FTL 표에서 Greedy 정답을 직접 찾아 gc_select_victim 과 비교 */
static void check_victim(uint8_t chips, const char *tag)
{
    int best_c = -1, best_b = -1;
    ftl_block_info_t best = {0};
    for (uint8_t c = 0; c < chips; c++) {
        for (uint16_t b = FTL_FIRST_BLOCK; b < FTL_FIRST_BLOCK + FTL_ACTIVE_BLOCKS_PER_CHIP; b++) {
            ftl_block_info_t bi;
            ftl_get_block_info(c, b, &bi);
            if (bi.state != FTL_BLK_CLOSED || bi.invalid_pages == 0) continue;
            if (best_c < 0 || bi.valid_pages < best.valid_pages ||
                (bi.valid_pages == best.valid_pages && bi.erase_count < best.erase_count)) {
                best = bi; best_c = c; best_b = b;   /* (chip, block) 오름차순 순회라 동률이면 앞 것이 남음 */
            }
        }
    }
    gc_victim_t v;
    solid_err_t e = gc_select_victim(&v);
    if (best_c < 0) {
        CHECK(e != SOLID_OK, "[%s] 후보가 없는데 Victim 을 골랐다 (%u,%u)", tag, v.chip, v.block);
        return;
    }
    CHECK(e == SOLID_OK, "[%s] gc_select_victim 실패 0x%02X", tag, (unsigned)e);
    if (e != SOLID_OK) return;
    CHECK(v.chip == best_c && v.block == best_b, "[%s] Victim (%u,%u) != 정답 (%d,%d)", tag, v.chip, v.block,
          best_c, best_b);
    CHECK(v.valid_pages == best.valid_pages && v.invalid_pages == best.invalid_pages,
          "[%s] Victim valid/invalid %u/%u != 표 %u/%u", tag, v.valid_pages, v.invalid_pages,
          best.valid_pages, best.invalid_pages);
}

static void setup(uint8_t chips, uint32_t seed)
{
    mock_nand_config_t cfg = mock_nand_default_config();
    cfg.chip_count = chips;
    cfg.strict_sequential = true;   /* FTL 은 Block 안에서 반드시 0,1,2,... 순서로 써야 한다 */
    mock_nand_reset(&cfg);
    nand_init();
    memset(version, 0, sizeof version);
    host_writes = 0;
    next_ver = 0;
    rng = seed ? seed : 1;
    stub_calls = 0;
}

static void start_stats(void)
{
    mock_nand_clear_stats();
}

/* ================================================================ 시나리오 */

/* 1. 8192 LPN 을 순서대로 한 번씩 → 전부 읽기. WAF = 1.00 */
static void test_sequential_fill(void)
{
    printf("[1] 순차 쓰기 8192 LPN\n");
    setup(8, 1);
    CHECK(ftl_init(8) == SOLID_OK, "ftl_init");
    CHECK(ftl_get_free_block_count() == 192, "초기 FREE %u", (unsigned)ftl_get_free_block_count());
    start_stats();
    uint32_t err = 0;
    for (uint16_t lpn = 0; lpn < LPN_N; lpn++) err += host_write(lpn) != SOLID_OK;
    CHECK(err == 0, "쓰기 실패 %u", (unsigned)err);
    CHECK(verify_all() == 0, "읽기 불일치");
    mock_nand_stats_t st;
    mock_nand_get_stats(&st);
    CHECK(st.program_ok == host_writes, "NAND Program %u != Host 쓰기 %u", (unsigned)st.program_ok,
          (unsigned)host_writes);
    CHECK(mock_nand_violations() == 0, "NAND 규칙 위반 %u", (unsigned)mock_nand_violations());
    CHECK(ftl_get_free_block_count() == 192 - 128, "FREE %u (128 Block 사용 기대)",
          (unsigned)ftl_get_free_block_count());
    check_table(8, "seq");
    check_victim(8, "seq");   /* 덮어쓰기 없음 → invalid 0 → 후보 없음 */
    printf("    Host %u / NAND %u → WAF %.2f, 모의 시간 %.2f s\n", (unsigned)host_writes,
           (unsigned)st.program_ok, (double)st.program_ok / host_writes, st.sim_time_us / 1e6);
}

/* 2. 작업 영역(hot)만 무작위 덮어쓰기 → GC 없이 NO_SPACE 까지.
 *    중간중간 표·Victim·데이터 확인. NO_SPACE 이후에도 기존 데이터는 그대로여야 한다. */
static void test_random_overwrite_until_full(uint16_t hot_lpns, uint32_t seed)
{
    printf("[2] 무작위 덮어쓰기 (LPN 0~%u, seed %u) → GC 없이 공간 부족까지\n", hot_lpns - 1, (unsigned)seed);
    setup(8, seed);
    CHECK(ftl_init(8) == SOLID_OK, "ftl_init");
    start_stats();
    solid_err_t last = SOLID_OK;
    uint32_t n = 0;
    for (;;) {
        uint16_t lpn = (uint16_t)(rnd() % hot_lpns);
        last = host_write(lpn);
        if (last != SOLID_OK) break;
        if (++n % 2000 == 0) {
            char tag[32];
            snprintf(tag, sizeof tag, "rand@%u", (unsigned)n);
            check_table(8, tag);
            check_victim(8, tag);
        }
    }
    CHECK(last == ERR_FTL_NO_SPACE, "멈춘 이유 0x%02X (NO_SPACE 기대)", (unsigned)last);

    /* Host 는 Free ≥ 2 일 때만 새 Block → 마지막 1개는 GC 전용으로 남아 있어야 한다 */
    CHECK(ftl_get_free_block_count() == 1, "남은 FREE %u (GC 예비 1 기대)", (unsigned)ftl_get_free_block_count());
    CHECK(host_writes == 191u * NAND_PAGES_PER_BLOCK, "Host 쓰기 %u (191×64 기대)", (unsigned)host_writes);
    CHECK(verify_all() == 0, "공간 부족 후 읽기 불일치");
    check_table(8, "full");
    check_victim(8, "full");

    /* 실패한 쓰기는 기존 데이터를 건드리지 않아야 한다 */
    uint16_t lpn = 0;
    uint32_t before = version[lpn];
    CHECK(host_write(lpn) == ERR_FTL_NO_SPACE, "꽉 찬 뒤 쓰기");
    CHECK(version[lpn] == before && verify_all() == 0, "실패한 쓰기가 데이터를 바꿈");

    mock_nand_stats_t st;
    mock_nand_get_stats(&st);
    CHECK(mock_nand_violations() == 0, "NAND 규칙 위반 %u", (unsigned)mock_nand_violations());
    CHECK(stub_calls == 0, "FTL-07 스텁이 불림");

    /* 지금 GC 를 돌린다면: 회수 이득이 가장 큰 Block */
    gc_victim_t v;
    if (gc_select_victim(&v) == SOLID_OK) {
        printf("    Host %u 쓰기 후 공간 부족. 지금 Victim = chip %u block %u (valid %u / invalid %u)\n",
               (unsigned)host_writes, v.chip, v.block, v.valid_pages, v.invalid_pages);
    }
    printf("    NAND Program %u, 위반 %u, 모의 시간 %.1f s\n", (unsigned)st.program_ok,
           (unsigned)mock_nand_violations(), st.sim_time_us / 1e6);
}

/* 3. Program 실패 주입 → FTL 이 다음 Page 로 1번 재시도. 두 번 연속이면 실패 + 기존 데이터 유지 */
static void test_program_fail(void)
{
    printf("[3] Program 실패 주입\n");
    setup(8, 3);
    CHECK(ftl_init(8) == SOLID_OK, "ftl_init");
    for (uint16_t lpn = 0; lpn < 10; lpn++) host_write(lpn);

    /* 한 번 실패 → 재시도 성공 */
    mock_nand_inject(MOCK_OP_PROGRAM, MOCK_ANY_CHIP, MOCK_ANY_BLOCK, NAND_ERR_PROGRAM_FAIL, 1);
    CHECK(host_write(5) == SOLID_OK, "1회 실패 후 재시도 성공 기대");
    CHECK(verify_all() == 0, "재시도 후 읽기 불일치");
    check_table(8, "pf1");

    /* 두 번 연속 실패 → ERR_NAND_PROGRAM_FAIL, LPN 5 는 직전 데이터 그대로 */
    mock_nand_inject(MOCK_OP_PROGRAM, MOCK_ANY_CHIP, MOCK_ANY_BLOCK, NAND_ERR_PROGRAM_FAIL, 2);
    solid_err_t e = host_write(5);
    CHECK(e == ERR_NAND_PROGRAM_FAIL, "2회 실패 → 0x%02X", (unsigned)e);
    CHECK(verify_all() == 0, "실패 후 기존 데이터가 바뀜");
    check_table(8, "pf2");

    /* 실패한 Page 들은 invalid 로 남아 GC 대상이 된다 (write_pointer 는 전진) */
    ftl_block_info_t bi;
    ftl_get_block_info(0, 0, &bi);
    CHECK(bi.write_pointer == 14 && bi.invalid_pages == 4, "chip0 block0 wp %u invalid %u (14/4 기대)",
          bi.write_pointer, bi.invalid_pages);
    CHECK(mock_nand_violations() == 0, "NAND 규칙 위반 %u", (unsigned)mock_nand_violations());
}

/* 4. Correctable ECC 가 한 Block 에서 3번 → 읽은 Page 를 새 Page 로 옮김 (read reclaim) */
static void test_ecc_read_reclaim(void)
{
    printf("[4] ECC 정정 3회 → read reclaim\n");
    setup(8, 4);
    CHECK(ftl_init(8) == SOLID_OK, "ftl_init");
    for (uint16_t lpn = 0; lpn < 64; lpn++) host_write(lpn);   /* chip0 block0 꽉 참 */
    start_stats();
    mock_nand_inject_ecc_corrected(0, 0, 3);
    for (uint16_t lpn = 0; lpn < 3; lpn++) {
        CHECK(ftl_read(lpn, rbuf) == SOLID_OK, "정정 읽기는 성공이어야 함");
    }
    mock_nand_stats_t st;
    mock_nand_get_stats(&st);
    CHECK(st.program_ok == 1, "옮겨 쓴 Page %u (1 기대)", (unsigned)st.program_ok);
    CHECK(ftl_get_ecc_corrected_count() == 3, "ECC 정정 횟수 %u", (unsigned)ftl_get_ecc_corrected_count());
    CHECK(verify_all() == 0, "read reclaim 후 읽기 불일치");
    ftl_block_info_t bi;
    ftl_get_block_info(0, 0, &bi);
    CHECK(bi.valid_pages == 63 && bi.invalid_pages == 1, "block0 valid %u invalid %u (63/1 기대)",
          bi.valid_pages, bi.invalid_pages);
    check_table(8, "ecc");
    CHECK(mock_nand_violations() == 0, "NAND 규칙 위반 %u", (unsigned)mock_nand_violations());
}

/* 5. 공장 Bad Block + 칩 2개만 연결 */
static void test_bad_block_and_two_chips(void)
{
    printf("[5] 공장 Bad Block 2개 + 칩 2개\n");
    setup(2, 5);
    mock_nand_mark_factory_bad(0, 3);
    mock_nand_mark_factory_bad(1, 0);
    CHECK(ftl_init(2) == SOLID_OK, "ftl_init");
    CHECK(ftl_get_free_block_count() == 46, "FREE %u (48-2 기대)", (unsigned)ftl_get_free_block_count());
    ftl_block_info_t bi;
    CHECK(ftl_get_block_info(0, 3, &bi) == SOLID_OK && bi.state == FTL_BLK_BAD, "chip0 block3 BAD");
    CHECK(ftl_get_block_info(2, 0, &bi) == ERR_FTL_RANGE, "연결 안 된 칩은 RANGE");

    start_stats();
    solid_err_t e = SOLID_OK;
    while ((e = host_write((uint16_t)(rnd() % 1000))) == SOLID_OK) {}
    CHECK(e == ERR_FTL_NO_SPACE, "멈춘 이유 0x%02X", (unsigned)e);
    CHECK(host_writes == 45u * NAND_PAGES_PER_BLOCK, "Host 쓰기 %u (45×64 기대)", (unsigned)host_writes);
    CHECK(verify_all() == 0, "읽기 불일치");
    check_table(2, "2chip");
    CHECK(mock_nand_violations() == 0, "NAND 규칙 위반 %u (Bad Block 접근 포함)", (unsigned)mock_nand_violations());
}

int main(void)
{
    gc_config_t gcfg = gc_default_config();
    gcfg.chip_count = 8;
    gc_init(&gcfg);

    test_sequential_fill();
    test_random_overwrite_until_full(1024, 7);
    test_random_overwrite_until_full(8192, 11);
    test_program_fail();
    test_ecc_read_reclaim();

    gcfg.chip_count = 2;
    gc_init(&gcfg);
    test_bad_block_and_two_chips();

    mock_nand_destroy();
    printf("checks: %d passed, %d failed\n", checks - fails, fails);
    printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
    return fails ? 1 : 0;
}
