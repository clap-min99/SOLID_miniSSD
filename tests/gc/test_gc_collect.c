/*
 * test_gc_collect.c - gc_collect (Victim 1개 회수) 단위 테스트
 *
 * 박수민님 제안(11-20) 반영 확인:
 *   - 상태 전이: CLOSED → RESERVED → (이동) → reclaim → FREE / BAD, 실패 시 CLOSED 복귀
 *   - GC 는 nand.h 를 호출하지 않고 FTL API(reserve / move_one / reclaim)만 사용
 *   - 이동 실패한 Victim 을 바로 다시 고르지 않음 (무한 실패 방지)
 *   - GC 중첩 호출 금지
 */
#include <stdio.h>
#include <string.h>
#include "gc.h"
#include "fake_ftl.h"

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

#define CHECK_STR(a, b) do {                                                 \
        if (strcmp((a), (b)) == 0) { g_pass++; }                             \
        else { g_fail++; printf("  [FAIL] %s:%d  \"%s\" != \"%s\"  (%s)\n",  \
                                __FILE__, __LINE__, (a), (b), g_case); }     \
    } while (0)

/* 이벤트 로그 기록 */
static gc_event_t ev_log[32];
static uint32_t   ev_val[32];
static int        ev_n;
static void logger(gc_event_t ev, uint8_t chip, uint16_t block, uint32_t value)
{
    (void)chip; (void)block;
    if (ev_n < 32) { ev_log[ev_n] = ev; ev_val[ev_n] = value; ev_n++; }
}

static void setup(void)
{
    fake_ftl_reset();
    fake_fill_all_full_valid();     /* 기본: 회수할 게 없는 꽉 찬 장치 */
    gc_init(NULL);
    gc_set_logger(logger);
    ev_n = 0;
}

#define RUN(fn) do {                                                         \
        int before = g_fail; g_case = #fn;                                   \
        setup(); fn();                                                       \
        printf("%s %s\n", (g_fail == before) ? "[ OK ]" : "[FAIL]", #fn);    \
    } while (0)

static gc_victim_t v;
static gc_stats_t  st;

/* ================================================================ 테스트 */

/* 정상: reserve → 유효 Page 3개 이동 → reclaim → FREE */
static void test_collect_success(void)
{
    fake_set_closed(2, 5, 3, 7);
    CHECK_EQ(gc_collect(&v), GC_OK);
    CHECK_EQ(v.chip, 2);
    CHECK_EQ(v.block, 5);
    CHECK_EQ(v.valid_pages, 3);

    CHECK_STR(fake_trace, "RMMME");                 /* 호출 순서 */
    CHECK_EQ(fake_tbl[2][5].state, FTL_BLK_FREE);
    CHECK_EQ(fake_tbl[2][5].erase_count, 8);

    gc_get_stats(&st);
    CHECK_EQ(st.blocks_reclaimed, 1);
    CHECK_EQ(st.pages_moved, 3);
    CHECK_EQ(st.move_fail, 0);
}

/* 유효 Page 가 0 이면 이동 없이 바로 Erase */
static void test_collect_all_invalid(void)
{
    fake_set_closed(0, 0, 0, 0);
    CHECK_EQ(gc_collect(NULL), GC_OK);
    CHECK_STR(fake_trace, "RE");
    CHECK_EQ(fake_tbl[0][0].state, FTL_BLK_FREE);
}

/* 회수할 게 없으면 FTL 상태를 건드리지 않음 */
static void test_collect_no_victim(void)
{
    CHECK_EQ(gc_collect(&v), ERR_GC_NO_SPACE);
    CHECK_STR(fake_trace, "");
}

/* 이동 실패: Erase 하지 않고 CLOSED 로 복귀, 기존 데이터(valid) 보존 */
static void test_move_fail_rolls_back(void)
{
    fake_set_closed(1, 3, 4, 0);
    fake_move_fail_at = 3;                          /* 2개 옮기고 3번째에서 실패 */
    CHECK_EQ(gc_collect(&v), ERR_GC_MOVE_FAIL);
    CHECK_STR(fake_trace, "RMMXU");                 /* E(Erase) 없음 */
    CHECK_EQ(fake_tbl[1][3].state, FTL_BLK_CLOSED);
    CHECK_EQ(fake_tbl[1][3].valid_pages, 2);        /* 남은 유효 Page 그대로 */

    gc_get_stats(&st);
    CHECK_EQ(st.move_fail, 1);
    CHECK_EQ(st.blocks_reclaimed, 0);
}

/* 이동 실패한 Victim 은 다음 GC 에서 다시 고르지 않음 (무한 실패 방지) */
static void test_failed_victim_skipped_next_time(void)
{
    fake_set_closed(1, 3, 4, 0);    /* 1순위 */
    fake_set_closed(4, 4, 10, 0);   /* 2순위 */
    fake_move_fail_at = 1;
    CHECK_EQ(gc_collect(&v), ERR_GC_MOVE_FAIL);
    CHECK_EQ(v.block, 3);

    fake_move_fail_at = -1;
    CHECK_EQ(gc_collect(&v), GC_OK);
    CHECK_EQ(v.chip, 4);                            /* 실패한 1-3 대신 4-4 */
    CHECK_EQ(v.block, 4);

    gc_init(NULL);                                  /* init 하면 Skip 목록 초기화 */
    CHECK_EQ(gc_select_victim(&v), GC_OK);
    CHECK_EQ(v.chip, 1);
    CHECK_EQ(v.block, 3);
}

/* 목적지 쓰기 실패(DST_PROGRAM)는 Victim 문제가 아니므로 Skip 하지 않음 → 다음에 같은 Victim 재시도 */
static void test_dst_program_fail_not_skipped(void)
{
    fake_set_closed(1, 3, 4, 0);    /* 1순위 */
    fake_set_closed(4, 4, 10, 0);   /* 2순위 */
    fake_move_fail_at = 2;
    fake_move_fail_code = ERR_FTL_DST_PROGRAM;
    CHECK_EQ(gc_collect(&v), ERR_GC_MOVE_FAIL);
    CHECK_STR(fake_trace, "RMXU");
    CHECK_EQ(fake_tbl[1][3].state, FTL_BLK_CLOSED);

    fake_move_fail_at = -1;
    CHECK_EQ(gc_collect(&v), GC_OK);
    CHECK_EQ(v.chip, 1);                            /* 같은 Victim 을 다시 골라 성공 */
    CHECK_EQ(v.block, 3);
}

/* 목적지 공간 없음(NO_SPACE) → ERR_GC_NO_SPACE 로 올라가고 Victim 은 CLOSED 복귀, Skip 안 함 */
static void test_dst_no_space(void)
{
    fake_set_closed(2, 2, 5, 0);
    fake_move_fail_at = 1;
    fake_move_fail_code = ERR_FTL_NO_SPACE;
    CHECK_EQ(gc_collect(&v), ERR_GC_NO_SPACE);
    CHECK_STR(fake_trace, "RXU");
    CHECK_EQ(fake_tbl[2][2].state, FTL_BLK_CLOSED);

    CHECK_EQ(gc_select_victim(&v), GC_OK);          /* 여전히 후보 */
    CHECK_EQ(v.block, 2);
}

/* FTL 계약 위반(BAD_STATE 등)은 ERR_GC_FTL */
static void test_move_bad_state(void)
{
    fake_set_closed(0, 4, 3, 0);
    fake_move_fail_at = 1;
    fake_move_fail_code = ERR_FTL_BAD_STATE;
    CHECK_EQ(gc_collect(&v), ERR_GC_FTL);
    CHECK_EQ(fake_tbl[0][4].state, FTL_BLK_CLOSED);
}

/* 후보가 전부 Skip 되면 ERR_GC_NO_SPACE 로 끝남 (호출 측 while 루프가 무한히 돌지 않음) */
static void test_all_candidates_skipped(void)
{
    fake_set_closed(0, 1, 5, 0);
    fake_set_closed(0, 2, 6, 0);
    fake_move_fail_at = 1;          /* 매 Victim 첫 이동에서 원본 읽기 실패 */
    fake_move_fail_code = ERR_FTL_SRC_READ;

    int rounds = 0;
    gc_result_t r = GC_OK;
    while (gc_should_run(ftl_get_free_block_count()) && rounds < 10) {
        r = gc_collect(&v);
        rounds++;
        if (r == ERR_GC_NO_SPACE) break;
        /* 가짜 FTL 의 실패 카운터를 다시 1 번째로 맞춤 */
        fake_ftl_reset_move_counter();
    }
    CHECK_EQ(r, ERR_GC_NO_SPACE);
    CHECK_EQ(rounds, 3);                            /* 실패 2번 → 3번째에 후보 없음 */
}

/* Erase 실패: FTL 이 BAD 처리, GC 는 오류 보고 */
static void test_erase_fail_marks_bad(void)
{
    fake_set_closed(3, 1, 2, 0);
    fake_erase_fail = true;
    CHECK_EQ(gc_collect(&v), ERR_GC_ERASE_FAIL);
    CHECK_STR(fake_trace, "RMME");
    CHECK_EQ(fake_tbl[3][1].state, FTL_BLK_BAD);

    gc_get_stats(&st);
    CHECK_EQ(st.erase_fail, 1);
    CHECK_EQ(st.blocks_reclaimed, 0);

    /* BAD 가 된 블록은 다시 고르지 않음 */
    CHECK_EQ(gc_select_victim(&v), ERR_GC_NO_SPACE);
}

/* reserve 가 거부되면 아무것도 옮기지 않음 */
static void test_reserve_rejected(void)
{
    fake_set_closed(0, 2, 1, 0);
    fake_reserve_reject = true;
    CHECK_EQ(gc_collect(&v), ERR_GC_FTL);
    CHECK_STR(fake_trace, "R");
    CHECK_EQ(fake_tbl[0][2].state, FTL_BLK_CLOSED);
}

/* FTL 이 "유효 Page 남음"으로 Erase 를 거부하면 되돌리고 오류 */
static void test_reclaim_valid_remains(void)
{
    fake_set_closed(0, 6, 1, 0);
    fake_reclaim_force_valid_remains = true;
    CHECK_EQ(gc_collect(&v), ERR_GC_FTL);
    CHECK_STR(fake_trace, "RMEU");
    CHECK_EQ(fake_tbl[0][6].state, FTL_BLK_CLOSED);
}

/* GC 실행 중 다시 GC 를 부르면 BUSY (중첩 금지) */
static gc_result_t nested_res;
static void call_gc_again(void) { nested_res = gc_collect(NULL); }

static void test_nested_gc_rejected(void)
{
    fake_set_closed(5, 5, 2, 0);
    nested_res = GC_OK;
    fake_on_move = call_gc_again;
    CHECK_EQ(gc_collect(&v), GC_OK);
    CHECK_EQ(nested_res, ERR_GC_BUSY);

    fake_on_move = NULL;            /* 끝난 뒤에는 다시 실행 가능 */
    fake_set_closed(5, 6, 1, 0);
    CHECK_EQ(gc_collect(&v), GC_OK);
}

/* 이벤트 로그: Victim → 이동 → Erase(Free 수) */
static void test_event_log(void)
{
    fake_set_closed(0, 1, 2, 0);
    CHECK_EQ(gc_collect(&v), GC_OK);
    CHECK_EQ(ev_n, 3);
    CHECK_EQ(ev_log[0], GC_EV_VICTIM);
    CHECK_EQ(ev_val[0], 2);
    CHECK_EQ(ev_log[1], GC_EV_MOVED);
    CHECK_EQ(ev_val[1], 2);
    CHECK_EQ(ev_log[2], GC_EV_ERASED);
    CHECK_EQ(ev_val[2], 1);                         /* 회수 후 Free 블록 1개 */
}

/* 반복 GC: Free 가 임계값을 넘을 때까지 돌리면 쓰레기 많은 순서대로 회수 */
static void test_repeat_until_enough_free(void)
{
    fake_set_closed(0, 0, 60, 0);
    fake_set_closed(0, 1, 10, 0);
    fake_set_closed(0, 2, 30, 0);
    fake_set_closed(0, 3, 50, 0);

    uint16_t order[4];
    int n = 0;
    while (gc_should_run(ftl_get_free_block_count()) && n < 4) {
        if (gc_collect(&v) != GC_OK) break;
        order[n++] = v.block;
    }
    CHECK_EQ(n, 3);                                 /* Free 0 → 3 이 되면 멈춤 */
    CHECK_EQ(order[0], 1);                          /* valid 10 */
    CHECK_EQ(order[1], 2);                          /* valid 30 */
    CHECK_EQ(order[2], 3);                          /* valid 50 */

    gc_get_stats(&st);
    CHECK_EQ(st.pages_moved, 10 + 30 + 50);
}

/* 초기화 전 */
static void test_not_initialized(void)
{
    gc_config_t bad = gc_default_config();
    bad.chip_count = 0;
    gc_init(&bad);
    CHECK_EQ(gc_collect(&v), ERR_GC_NOT_INITIALIZED);
}

int main(void)
{
    printf("=== GC collect test ===\n");
    RUN(test_collect_success);
    RUN(test_collect_all_invalid);
    RUN(test_collect_no_victim);
    RUN(test_move_fail_rolls_back);
    RUN(test_failed_victim_skipped_next_time);
    RUN(test_dst_program_fail_not_skipped);
    RUN(test_dst_no_space);
    RUN(test_move_bad_state);
    RUN(test_all_candidates_skipped);
    RUN(test_erase_fail_marks_bad);
    RUN(test_reserve_rejected);
    RUN(test_reclaim_valid_remains);
    RUN(test_nested_gc_rejected);
    RUN(test_event_log);
    RUN(test_repeat_until_enough_free);
    RUN(test_not_initialized);

    printf("----------------------\n");
    printf("checks: %d passed, %d failed\n", g_pass, g_fail);
    printf("%s\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}
