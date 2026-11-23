/*
 * test_mock_nand.c - Mock NAND 기본 동작 검증
 *
 * 오늘 완료 기준 ①: "Mock NAND 로 기본 Page 동작을 검증"
 * 빌드/실행 방법은 tests/mock_nand/README.md 참고.
 */
#include <stdio.h>
#include <string.h>
#include "mock_nand.h"

/* ---------------------------------------------------------------- 미니 테스트 도구 */
static int g_fail = 0, g_pass = 0;
static const char *g_case = "";

#define CHECK(cond) do {                                                     \
        if (cond) { g_pass++; }                                              \
        else { g_fail++; printf("  [FAIL] %s:%d  %s  (%s)\n",               \
                                __FILE__, __LINE__, #cond, g_case); }        \
    } while (0)

#define CHECK_EQ(a, b) do {                                                  \
        long _a = (long)(a), _b = (long)(b);                                 \
        if (_a == _b) { g_pass++; }                                          \
        else { g_fail++; printf("  [FAIL] %s:%d  %s == %s  (got %ld, want %ld)  (%s)\n", \
                                __FILE__, __LINE__, #a, #b, _a, _b, g_case); } \
    } while (0)

#define RUN(fn) do {                                                         \
        int before = g_fail; g_case = #fn;                                   \
        setup(); fn();                                                       \
        printf("%s %s\n", (g_fail == before) ? "[ OK ]" : "[FAIL]", #fn);    \
    } while (0)

static uint8_t wbuf[NAND_PAGE_SIZE], rbuf[NAND_PAGE_SIZE];

static nand_addr_t A(uint8_t c, uint16_t b, uint8_t p)
{
    nand_addr_t a = { c, b, p };
    return a;
}

static void fill(uint8_t *buf, uint32_t seed)
{
    for (uint32_t i = 0; i < NAND_PAGE_SIZE; i++) {
        buf[i] = (uint8_t)((i * 31U + seed * 17U) ^ (seed >> 3));
    }
}

static void setup(void)
{
    mock_nand_config_t cfg = mock_nand_default_config();
    mock_nand_reset(&cfg);
    nand_init();
}

/* ================================================================ 테스트 */

/* 초기화 전 호출은 거부 */
static void test_not_initialized(void)
{
    mock_nand_reset(NULL);                       /* reset 후 init 안 함 */
    CHECK_EQ(nand_read_page(A(0, 0, 0), rbuf, NULL), NAND_ERR_NOT_INITIALIZED);
    nand_init();
    CHECK_EQ(nand_read_page(A(0, 0, 0), rbuf, NULL), NAND_OK);
}

/* probe / geometry: 8칩 연결, 사양 값 */
static void test_probe_geometry(void)
{
    nand_geometry_t g;
    for (uint8_t c = 0; c < 8; c++) {
        CHECK_EQ(nand_probe(c), NAND_OK);
    }
    CHECK_EQ(nand_get_geometry(3, &g), NAND_OK);
    CHECK_EQ(g.page_size, 2048);
    CHECK_EQ(g.spare_size, 64);
    CHECK_EQ(g.pages_per_block, 64);
    CHECK_EQ(g.blocks_per_chip, 1024);

    /* 칩 2개만 연결된 구성 */
    mock_nand_config_t cfg = mock_nand_default_config();
    cfg.chip_count = 2;
    mock_nand_reset(&cfg);
    nand_init();
    CHECK_EQ(nand_probe(1), NAND_OK);
    CHECK_EQ(nand_probe(2), NAND_ERR_NO_DEVICE);
}

/* 새 칩은 전부 0xFF */
static void test_fresh_reads_ff(void)
{
    nand_ecc_t ecc;
    CHECK_EQ(nand_read_page(A(7, 1023, 63), rbuf, &ecc), NAND_OK);
    CHECK_EQ(ecc, NAND_ECC_NO_ERROR);
    int all_ff = 1;
    for (uint32_t i = 0; i < NAND_PAGE_SIZE; i++) {
        if (rbuf[i] != 0xFF) { all_ff = 0; break; }
    }
    CHECK(all_ff);
}

/* 쓰고 읽으면 같다 */
static void test_program_read_match(void)
{
    fill(wbuf, 1);
    CHECK_EQ(nand_program_page(A(0, 5, 0), wbuf), NAND_OK);
    CHECK_EQ(nand_read_page(A(0, 5, 0), rbuf, NULL), NAND_OK);
    CHECK(memcmp(wbuf, rbuf, NAND_PAGE_SIZE) == 0);
    CHECK(mock_nand_page_written(0, 5, 0));
    CHECK_EQ(mock_nand_next_page(0, 5), 1);
    CHECK_EQ(mock_nand_violations(), 0);
}

/* 칩·블록이 달라도 서로 간섭하지 않는다 */
static void test_isolation_between_chips(void)
{
    for (uint8_t c = 0; c < 8; c++) {
        fill(wbuf, 100U + c);
        CHECK_EQ(nand_program_page(A(c, 10, 0), wbuf), NAND_OK);
    }
    for (uint8_t c = 0; c < 8; c++) {
        fill(wbuf, 100U + c);
        nand_read_page(A(c, 10, 0), rbuf, NULL);
        CHECK(memcmp(wbuf, rbuf, NAND_PAGE_SIZE) == 0);
    }
}

/* 블록 64페이지를 순서대로 꽉 채우고 전부 일치 */
static void test_fill_whole_block(void)
{
    for (uint8_t p = 0; p < NAND_PAGES_PER_BLOCK; p++) {
        fill(wbuf, p);
        CHECK_EQ(nand_program_page(A(1, 2, p), wbuf), NAND_OK);
    }
    CHECK_EQ(mock_nand_next_page(1, 2), 64);
    for (uint8_t p = 0; p < NAND_PAGES_PER_BLOCK; p++) {
        fill(wbuf, p);
        nand_read_page(A(1, 2, p), rbuf, NULL);
        CHECK(memcmp(wbuf, rbuf, NAND_PAGE_SIZE) == 0);
    }
}

/* 규칙 1: 같은 페이지 재프로그램 금지 (덮어쓰기 불가) */
static void test_reprogram_rejected(void)
{
    fill(wbuf, 1);
    nand_program_page(A(0, 0, 0), wbuf);
    fill(wbuf, 2);
    CHECK_EQ(nand_program_page(A(0, 0, 0), wbuf), NAND_ERR_INVALID_ARG);

    mock_nand_stats_t st;
    mock_nand_get_stats(&st);
    CHECK_EQ(st.violation_reprogram, 1);

    /* 원래 데이터가 그대로 남아 있어야 함 */
    fill(wbuf, 1);
    nand_read_page(A(0, 0, 0), rbuf, NULL);
    CHECK(memcmp(wbuf, rbuf, NAND_PAGE_SIZE) == 0);
}

/* 규칙 2: 블록 안에서 순서 거슬러 쓰기 금지 */
static void test_out_of_order_rejected(void)
{
    fill(wbuf, 1);
    CHECK_EQ(nand_program_page(A(0, 3, 5), wbuf), NAND_OK);   /* 앞으로 건너뛰기는 허용 */
    CHECK_EQ(nand_program_page(A(0, 3, 2), wbuf), NAND_ERR_INVALID_ARG);

    mock_nand_stats_t st;
    mock_nand_get_stats(&st);
    CHECK_EQ(st.violation_order, 1);
    CHECK(!mock_nand_page_written(0, 3, 2));
}

/* strict 모드: 건너뛰기도 금지 */
static void test_strict_sequential(void)
{
    mock_nand_config_t cfg = mock_nand_default_config();
    cfg.strict_sequential = true;
    mock_nand_reset(&cfg);
    nand_init();

    fill(wbuf, 1);
    CHECK_EQ(nand_program_page(A(0, 3, 0), wbuf), NAND_OK);
    CHECK_EQ(nand_program_page(A(0, 3, 2), wbuf), NAND_ERR_INVALID_ARG);
    CHECK_EQ(nand_program_page(A(0, 3, 1), wbuf), NAND_OK);
}

/* 규칙 3: Erase 하면 블록 전체 0xFF, 다시 0페이지부터 쓸 수 있음 */
static void test_erase_resets_block(void)
{
    for (uint8_t p = 0; p < 4; p++) {
        fill(wbuf, p);
        nand_program_page(A(2, 7, p), wbuf);
    }
    /* 옆 블록은 지워지면 안 됨 */
    fill(wbuf, 99);
    nand_program_page(A(2, 8, 0), wbuf);

    CHECK_EQ(nand_erase_block(2, 7), NAND_OK);
    CHECK_EQ(mock_nand_erase_count(2, 7), 1);
    CHECK_EQ(mock_nand_next_page(2, 7), 0);

    nand_read_page(A(2, 7, 0), rbuf, NULL);
    CHECK(rbuf[0] == 0xFF && rbuf[NAND_PAGE_SIZE - 1] == 0xFF);

    fill(wbuf, 42);
    CHECK_EQ(nand_program_page(A(2, 7, 0), wbuf), NAND_OK);   /* 재사용 가능 */

    fill(wbuf, 99);
    nand_read_page(A(2, 8, 0), rbuf, NULL);
    CHECK(memcmp(wbuf, rbuf, NAND_PAGE_SIZE) == 0);
}

/* NAND 비트 특성: 0→1 은 Erase 로만 가능 (어제 F0 위 0F → 00 실험 재현) */
static void test_bits_only_go_down(void)
{
    nand_addr_t a = A(0, 20, 0);
    uint8_t s1[NAND_SPARE_SIZE], s2[NAND_SPARE_SIZE], out[NAND_SPARE_SIZE];
    memset(s1, 0xF0, sizeof(s1));
    memset(s2, 0x0F, sizeof(s2));
    a.page = 1;                       /* Page 0 Spare[0] 은 Bad Block Marker 자리라 피함 */
    mock_nand_poke_spare(a, s1);
    mock_nand_poke_spare(a, s2);
    mock_nand_peek_spare(a, out);
    CHECK_EQ(out[0], 0x00);
}

/* 규칙 4: 공장 Bad Block 은 감지되고, Program/Erase 거부 */
static void test_factory_bad_block(void)
{
    bool bad = true;
    CHECK_EQ(nand_is_factory_bad_block(0, 11, &bad), NAND_OK);
    CHECK(!bad);

    mock_nand_mark_factory_bad(0, 11);
    CHECK_EQ(nand_is_factory_bad_block(0, 11, &bad), NAND_OK);
    CHECK(bad);

    fill(wbuf, 1);
    CHECK_EQ(nand_program_page(A(0, 11, 0), wbuf), NAND_ERR_INVALID_ARG);
    CHECK_EQ(nand_erase_block(0, 11), NAND_ERR_INVALID_ARG);

    /* Erase 가 거부됐으므로 Marker 가 그대로 남아야 함 */
    CHECK_EQ(nand_is_factory_bad_block(0, 11, &bad), NAND_OK);
    CHECK(bad);

    mock_nand_stats_t st;
    mock_nand_get_stats(&st);
    CHECK_EQ(st.violation_bad_block, 2);

    /* 정상 블록에 Page 0 을 써도 Bad 로 오인하지 않음 */
    nand_program_page(A(0, 12, 0), wbuf);
    CHECK_EQ(nand_is_factory_bad_block(0, 12, &bad), NAND_OK);
    CHECK(!bad);
}

/* 범위 밖 주소 / NULL 버퍼 */
static void test_bad_arguments(void)
{
    fill(wbuf, 1);
    CHECK_EQ(nand_program_page(A(0, 1024, 0), wbuf), NAND_ERR_INVALID_ARG);
    CHECK_EQ(nand_program_page(A(0, 0, 64), wbuf), NAND_ERR_INVALID_ARG);
    CHECK_EQ(nand_program_page(A(8, 0, 0), wbuf), NAND_ERR_INVALID_ARG);
    CHECK_EQ(nand_read_page(A(0, 0, 0), NULL, NULL), NAND_ERR_INVALID_ARG);
    CHECK_EQ(nand_erase_block(0, 1024), NAND_ERR_INVALID_ARG);
}

/* 실패 주입: Program Fail → 오류 반환 + 그 Page 는 소모됨(재사용 금지) */
static void test_inject_program_fail(void)
{
    mock_nand_inject(MOCK_OP_PROGRAM, 0, 30, NAND_ERR_PROGRAM_FAIL, 1);
    fill(wbuf, 1);
    CHECK_EQ(nand_program_page(A(0, 30, 0), wbuf), NAND_ERR_PROGRAM_FAIL);
    CHECK(mock_nand_page_written(0, 30, 0));
    CHECK_EQ(nand_program_page(A(0, 30, 0), wbuf), NAND_ERR_INVALID_ARG);  /* 재사용 시도는 위반 */
    CHECK_EQ(nand_program_page(A(0, 30, 1), wbuf), NAND_OK);              /* 1회만 실패 */

    nand_status_t s;
    nand_get_status(0, &s);
    CHECK(!s.program_failed);

    /* 다른 블록은 영향 없음 */
    CHECK_EQ(nand_program_page(A(0, 31, 0), wbuf), NAND_OK);
}

/* 실패 주입: Erase Fail → 데이터가 지워지지 않은 채 남음 */
static void test_inject_erase_fail(void)
{
    fill(wbuf, 5);
    nand_program_page(A(1, 4, 0), wbuf);
    mock_nand_inject(MOCK_OP_ERASE, 1, 4, NAND_ERR_ERASE_FAIL, 1);
    CHECK_EQ(nand_erase_block(1, 4), NAND_ERR_ERASE_FAIL);

    nand_status_t s;
    nand_get_status(1, &s);
    CHECK(s.erase_failed);

    nand_read_page(A(1, 4, 0), rbuf, NULL);
    CHECK(memcmp(wbuf, rbuf, NAND_PAGE_SIZE) == 0);   /* 지워졌다고 믿으면 안 됨 */

    CHECK_EQ(nand_erase_block(1, 4), NAND_OK);       /* 두 번째는 성공 */
    CHECK_EQ(mock_nand_erase_count(1, 4), 2);
}

/* 실패 주입: 영구 실패 (count = 0) → 해제 전까지 계속 */
static void test_inject_permanent(void)
{
    mock_nand_inject(MOCK_OP_ERASE, 2, MOCK_ANY_BLOCK, NAND_ERR_TIMEOUT, 0);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(nand_erase_block(2, (uint16_t)i), NAND_ERR_TIMEOUT);
    }
    CHECK_EQ(nand_erase_block(3, 0), NAND_OK);       /* 다른 칩은 정상 */
    mock_nand_clear_injections();
    CHECK_EQ(nand_erase_block(2, 0), NAND_OK);
}

/* 실패 주입: ECC */
static void test_inject_ecc(void)
{
    nand_ecc_t ecc;
    fill(wbuf, 9);
    nand_program_page(A(0, 40, 0), wbuf);

    mock_nand_inject_ecc_corrected(0, 40, 1);
    CHECK_EQ(nand_read_page(A(0, 40, 0), rbuf, &ecc), NAND_OK);
    CHECK_EQ(ecc, NAND_ECC_CORRECTED);
    CHECK(memcmp(wbuf, rbuf, NAND_PAGE_SIZE) == 0);  /* 정정됐으니 데이터는 맞음 */

    mock_nand_inject(MOCK_OP_READ, 0, 40, NAND_ERR_ECC_UNCORRECTABLE, 1);
    CHECK_EQ(nand_read_page(A(0, 40, 0), rbuf, &ecc), NAND_ERR_ECC_UNCORRECTABLE);
    CHECK_EQ(ecc, NAND_ECC_UNCORRECTABLE);

    CHECK_EQ(nand_read_page(A(0, 40, 0), rbuf, &ecc), NAND_OK);
    CHECK_EQ(ecc, NAND_ECC_NO_ERROR);
}

/* 통계와 모의 시간 */
static void test_stats(void)
{
    fill(wbuf, 1);
    nand_program_page(A(0, 0, 0), wbuf);
    nand_program_page(A(0, 0, 1), wbuf);
    nand_read_page(A(0, 0, 0), rbuf, NULL);
    nand_erase_block(0, 0);

    mock_nand_stats_t st;
    mock_nand_get_stats(&st);
    CHECK_EQ(st.program_ok, 2);
    CHECK_EQ(st.read_calls, 1);
    CHECK_EQ(st.erase_ok, 1);
    CHECK_EQ(st.sim_time_us, 2 * MOCK_T_PROGRAM_US + MOCK_T_READ_US + MOCK_T_ERASE_US);
}

/* 관리 영역 전체(8칩 × 24블록 × 64페이지 = 12,288 Page) 쓰고 읽기 */
static void test_full_managed_area(void)
{
    uint32_t mismatch = 0;
    for (uint8_t c = 0; c < 8; c++)
        for (uint16_t b = 0; b < 24; b++)
            for (uint8_t p = 0; p < 64; p++) {
                fill(wbuf, ((uint32_t)c << 16) | ((uint32_t)b << 6) | p);
                if (nand_program_page(A(c, b, p), wbuf) != NAND_OK) mismatch++;
            }
    for (uint8_t c = 0; c < 8; c++)
        for (uint16_t b = 0; b < 24; b++)
            for (uint8_t p = 0; p < 64; p++) {
                fill(wbuf, ((uint32_t)c << 16) | ((uint32_t)b << 6) | p);
                nand_read_page(A(c, b, p), rbuf, NULL);
                if (memcmp(wbuf, rbuf, NAND_PAGE_SIZE) != 0) mismatch++;
            }
    CHECK_EQ(mismatch, 0);
    CHECK_EQ(mock_nand_violations(), 0);

    mock_nand_stats_t st;
    mock_nand_get_stats(&st);
    CHECK_EQ(st.program_ok, 12288);
    printf("       (12,288 Page 쓰기+읽기, 모의 시간 %.1f s)\n", (double)st.sim_time_us / 1e6);
}

int main(void)
{
    printf("=== Mock NAND test ===\n");
    RUN(test_not_initialized);
    RUN(test_probe_geometry);
    RUN(test_fresh_reads_ff);
    RUN(test_program_read_match);
    RUN(test_isolation_between_chips);
    RUN(test_fill_whole_block);
    RUN(test_reprogram_rejected);
    RUN(test_out_of_order_rejected);
    RUN(test_strict_sequential);
    RUN(test_erase_resets_block);
    RUN(test_bits_only_go_down);
    RUN(test_factory_bad_block);
    RUN(test_bad_arguments);
    RUN(test_inject_program_fail);
    RUN(test_inject_erase_fail);
    RUN(test_inject_permanent);
    RUN(test_inject_ecc);
    RUN(test_stats);
    RUN(test_full_managed_area);
    mock_nand_destroy();

    printf("----------------------\n");
    printf("checks: %d passed, %d failed\n", g_pass, g_fail);
    printf("%s\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}
