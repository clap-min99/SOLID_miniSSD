/*
 * test_gc_victim.c - Greedy Victim 선택 단위 테스트
 *
 * 오늘 완료 기준 ②: "Victim Block 선택 단위 테스트 통과"
 *
 * FTL 이 아직 없으므로 ftl_get_block_info() 를 가짜 FTL(fake_ftl.c, 표)로 대신한다.
 * 테스트마다 표에 Block 상태를 적어 두고, gc_select_victim() 이 맞는 Block 을 고르는지 본다.
 */
#include <stdio.h>
#include <string.h>
#include "gc.h"
#include "gc_ftl_if.h"
#include "nand.h"

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

/* ================================================================ 가짜 FTL (fake_ftl.c) */
#include "fake_ftl.h"

#define fake_fail           fake_query_fail
#define fake_calls          fake_info_calls
#define set_closed          fake_set_closed
#define set_state           fake_set_state
#define fill_all_full_valid fake_fill_all_full_valid

static void setup(void)
{
    fake_ftl_reset();                           /* 전부 FREE */
    gc_init(NULL);                              /* 8칩 × 24블록(시작 0), 임계값 2 */
}

static gc_victim_t v;

/* ================================================================ 테스트 */

/* 초기화 전 / NULL 인자 */
static void test_init_and_args(void)
{
    gc_config_t bad = gc_default_config();
    bad.chip_count = 0;
    CHECK_EQ(gc_init(&bad), ERR_GC_INVALID_ARG);
    CHECK_EQ(gc_select_victim(&v), ERR_GC_NOT_INITIALIZED);   /* 잘못된 설정이면 미초기화 */

    bad = gc_default_config();
    bad.chip_count = 9;
    CHECK_EQ(gc_init(&bad), ERR_GC_INVALID_ARG);

    CHECK_EQ(gc_init(NULL), SOLID_OK);
    CHECK_EQ(gc_select_victim(NULL), ERR_GC_INVALID_ARG);
}

/* GC 시작 조건: Free Block ≤ 2 */
static void test_should_run_threshold(void)
{
    CHECK(!gc_should_run(10));
    CHECK(!gc_should_run(3));
    CHECK(gc_should_run(2));
    CHECK(gc_should_run(1));
    CHECK(gc_should_run(0));

    gc_config_t c = gc_default_config();
    c.free_threshold = 5;
    gc_init(&c);
    CHECK(gc_should_run(5));
    CHECK(!gc_should_run(6));
}

/* 기본: valid 가 가장 적은 CLOSED Block 선택 */
static void test_picks_min_valid(void)
{
    fill_all_full_valid();
    set_closed(0, 3, 40, 0);
    set_closed(2, 10, 12, 0);   /* ← 정답 */
    set_closed(5, 1, 30, 0);

    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 2);
    CHECK_EQ(v.block, 10);
    CHECK_EQ(v.valid_pages, 12);
    CHECK_EQ(v.invalid_pages, 52);
}

/* valid 0 (전부 쓰레기) 이면 최우선 */
static void test_all_invalid_block_first(void)
{
    fill_all_full_valid();
    set_closed(1, 1, 5, 0);
    set_closed(6, 20, 0, 0);    /* ← 정답: 옮길 게 없음 */
    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 6);
    CHECK_EQ(v.block, 20);
    CHECK_EQ(v.valid_pages, 0);
}

/* FREE / OPEN / BAD / RESERVED 는 valid 가 더 적어도 고르면 안 됨 */
static void test_excludes_non_closed(void)
{
    fill_all_full_valid();
    set_state(0, 0, FTL_BLK_FREE,     0, 0);
    set_state(0, 1, FTL_BLK_OPEN,     0, 10);   /* 쓰는 중인 Block 은 절대 안 됨 */
    set_state(0, 2, FTL_BLK_BAD,      0, 64);
    set_state(0, 3, FTL_BLK_RESERVED, 0, 64);
    set_closed(4, 4, 50, 0);                    /* ← 정답: 유일한 정상 후보 */

    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 4);
    CHECK_EQ(v.block, 4);
}

/* Bad 가 가장 좋아 보여도 건너뛰고 다음 후보 */
static void test_bad_block_skipped_for_second_best(void)
{
    fill_all_full_valid();
    set_state(3, 7, FTL_BLK_BAD, 1, 63);
    set_closed(3, 8, 2, 0);     /* ← 정답 */
    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 3);
    CHECK_EQ(v.block, 8);
}

/* 동률: valid 같으면 erase_count 적은 쪽 (마모 분산) */
static void test_tie_lower_erase_count(void)
{
    fill_all_full_valid();
    set_closed(0, 5, 20, 9);
    set_closed(1, 5, 20, 3);    /* ← 정답 */
    set_closed(2, 5, 20, 7);
    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 1);
    CHECK_EQ(v.block, 5);
}

/* 완전 동률: 번호 작은 쪽 (chip → block 순). 같은 입력이면 항상 같은 결과 */
static void test_tie_lowest_index(void)
{
    fill_all_full_valid();
    set_closed(3, 2, 20, 4);
    set_closed(1, 9, 20, 4);    /* ← 정답 (chip 1 < 3) */
    set_closed(1, 15, 20, 4);
    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 1);
    CHECK_EQ(v.block, 9);

    gc_victim_t again;
    gc_select_victim(&again);
    CHECK(again.chip == v.chip && again.block == v.block);
}

/* valid 가 erase_count 보다 우선 */
static void test_valid_beats_erase_count(void)
{
    fill_all_full_valid();
    set_closed(0, 0, 10, 1000);   /* ← 정답: 많이 지웠어도 valid 가 적음 */
    set_closed(0, 1, 11, 0);
    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.block, 0);
}

/* 전부 FREE (막 포맷한 상태) → 후보 없음 */
static void test_no_candidate_all_free(void)
{
    CHECK_EQ(gc_select_victim(&v), ERR_GC_NO_SPACE);
}

/* 전부 CLOSED 인데 쓰레기가 하나도 없음 → 지워도 얻을 게 없으니 후보 없음 */
static void test_no_candidate_all_valid(void)
{
    fill_all_full_valid();
    CHECK_EQ(gc_select_victim(&v), ERR_GC_NO_SPACE);
}

/* 후보가 없으면 out 을 건드리지 않음 */
static void test_out_untouched_on_fail(void)
{
    gc_victim_t sentinel = { 0xAB, 0xBEEF, 0x1111, 0x2222 };
    v = sentinel;
    CHECK_EQ(gc_select_victim(&v), ERR_GC_NO_SPACE);
    CHECK(memcmp(&v, &sentinel, sizeof(v)) == 0);
}

/* 조회 실패한 Block 은 상태를 모르므로 건너뜀 */
static void test_query_failure_skipped(void)
{
    fill_all_full_valid();
    set_closed(2, 2, 0, 0);
    fake_fail[2][2] = true;     /* 가장 좋아 보이지만 조회 실패 */
    set_closed(2, 3, 30, 0);    /* ← 정답 */
    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 2);
    CHECK_EQ(v.block, 3);
}

/* 경계: 마지막 칩의 마지막 관리 Block 도 찾는다, 관리 영역 밖은 보지 않는다 */
static void test_scans_exact_managed_area(void)
{
    fill_all_full_valid();
    set_closed(7, 23, 1, 0);    /* ← 정답: 맨 끝 */
    set_closed(7, 24, 0, 0);    /* 관리 영역 밖 (block 24) → 보면 안 됨 */

    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 7);
    CHECK_EQ(v.block, 23);
    CHECK_EQ(fake_calls, 8 * 24);           /* 192 Block 을 정확히 한 번씩 */
    CHECK_EQ(fake_max_chip_seen, 7);
    CHECK_EQ(fake_max_block_seen, 23);
}

/* 칩 2개만 관리하는 구성이면 그 범위만 본다 */
static void test_smaller_config(void)
{
    gc_config_t c = gc_default_config();
    c.chip_count = 2;
    c.blocks_per_chip = 4;
    gc_init(&c);

    fill_all_full_valid();
    set_closed(5, 0, 0, 0);     /* 범위 밖 칩 → 무시 */
    set_closed(1, 3, 33, 0);    /* ← 정답 */

    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 1);
    CHECK_EQ(v.block, 3);
    CHECK_EQ(fake_calls, 2 * 4);
}

/* 관리 영역 시작 Block 이 0 이 아니어도 그 범위(first ~ first+24)만 본다 (FTL_FIRST_BLOCK 대비) */
static void test_first_block_offset(void)
{
    gc_config_t c = gc_default_config();
    CHECK_EQ(c.first_block, 0);                 /* 합의: FTL_FIRST_BLOCK = 0 */
    c.first_block = 10;
    CHECK_EQ(gc_init(&c), SOLID_OK);

    fill_all_full_valid();
    set_closed(0, 9, 0, 0);     /* 범위 앞 → 무시 */
    set_closed(0, 34, 0, 0);    /* 범위 뒤(10+24=34) → 무시 */
    set_closed(3, 33, 5, 0);    /* ← 정답: 범위 마지막 블록 */
    set_closed(2, 10, 6, 0);

    CHECK_EQ(gc_select_victim(&v), SOLID_OK);
    CHECK_EQ(v.chip, 3);
    CHECK_EQ(v.block, 33);
    CHECK_EQ(fake_calls, 8 * 24);

    c.first_block = 1010;       /* 1010 + 24 > 1024 → 거부 */
    CHECK_EQ(gc_init(&c), ERR_GC_INVALID_ARG);
}

/* GC 는 FTL 표를 수정하지 않는다 (조회 전용) */
static void test_does_not_modify_ftl(void)
{
    fill_all_full_valid();
    set_closed(0, 7, 3, 2);
    set_state(1, 1, FTL_BLK_OPEN, 10, 0);

    static ftl_block_info_t before[NAND_MAX_CHIPS][NAND_BLOCKS_PER_CHIP];
    memcpy(before, fake_tbl, sizeof(fake_tbl));
    gc_select_victim(&v);
    gc_select_victim(&v);
    CHECK(memcmp(before, fake_tbl, sizeof(fake_tbl)) == 0);
}

/* 랜덤 상태 192개에서 정답을 단순 계산과 비교 (여러 번) */
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245U + 12345U; return (rng >> 16) & 0x7FFF; }

static void test_random_matches_reference(void)
{
    for (int round = 0; round < 200; round++) {
        setup();
        for (uint8_t c = 0; c < 8; c++)
            for (uint16_t b = 0; b < 24; b++) {
                uint32_t r = rnd() % 10;
                if (r == 0)      set_state(c, b, FTL_BLK_FREE, 0, 0);
                else if (r == 1) set_state(c, b, FTL_BLK_BAD, 0, 0);
                else if (r == 2) set_state(c, b, FTL_BLK_OPEN, (uint16_t)(rnd() % 30), 0);
                else             set_closed(c, b, (uint16_t)(rnd() % 65), rnd() % 5);
            }

        /* 기준 답: 단순 전수 비교 */
        int bc = -1, bb = -1;
        for (int c = 0; c < 8; c++)
            for (int b = 0; b < 24; b++) {
                ftl_block_info_t *x = &fake_tbl[c][b];
                if (x->state != FTL_BLK_CLOSED || x->invalid_pages == 0) continue;
                if (bc < 0) { bc = c; bb = b; continue; }
                ftl_block_info_t *y = &fake_tbl[bc][bb];
                if (x->valid_pages < y->valid_pages ||
                    (x->valid_pages == y->valid_pages && x->erase_count < y->erase_count)) {
                    bc = c; bb = b;
                }
            }

        solid_err_t r = gc_select_victim(&v);
        if (bc < 0) {
            CHECK_EQ(r, ERR_GC_NO_SPACE);
        } else {
            CHECK_EQ(r, SOLID_OK);
            CHECK_EQ(v.chip, bc);
            CHECK_EQ(v.block, bb);
        }
    }
}

int main(void)
{
    printf("=== GC Victim selection test ===\n");
    RUN(test_init_and_args);
    RUN(test_should_run_threshold);
    RUN(test_picks_min_valid);
    RUN(test_all_invalid_block_first);
    RUN(test_excludes_non_closed);
    RUN(test_bad_block_skipped_for_second_best);
    RUN(test_tie_lower_erase_count);
    RUN(test_tie_lowest_index);
    RUN(test_valid_beats_erase_count);
    RUN(test_no_candidate_all_free);
    RUN(test_no_candidate_all_valid);
    RUN(test_out_untouched_on_fail);
    RUN(test_query_failure_skipped);
    RUN(test_scans_exact_managed_area);
    RUN(test_smaller_config);
    RUN(test_first_block_offset);
    RUN(test_does_not_modify_ftl);
    RUN(test_random_matches_reference);

    printf("----------------------\n");
    printf("checks: %d passed, %d failed\n", g_pass, g_fail);
    printf("%s\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}
