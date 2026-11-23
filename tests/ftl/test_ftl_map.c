/* build: gcc -Wall -Wextra -Ifirmware/common -Ifirmware/ftl tests/ftl/test_ftl_map.c firmware/ftl/address_mapper.c firmware/ftl/ftl_map.c -o test_ftl_map */
#include <assert.h>
#include <stdio.h>
#include "ftl_map.h"

static void expect_addr(ppn_t ppn, uint8_t chip, uint16_t block, uint8_t page)
{
    nand_addr_t a;
    assert(ppn_to_addr(ppn, &a));
    assert(a.chip == chip && a.block == block && a.page == page);
}

static void test_address_mapper(void)
{
    nand_addr_t a;
    ppn_t p;

    expect_addr(0, 0, 0, 0);
    expect_addr(63, 0, 0, 63);
    expect_addr(64, 0, 1, 0);
    expect_addr(2000, 0, 31, 16);        /* valid but not active */
    expect_addr(4096, 1, 0, 0);          /* chip-major */
    expect_addr(FTL_PPN_COUNT - 1, 7, 63, 63);

    assert(!ppn_to_addr(FTL_PPN_COUNT, &a));
    assert(!ppn_to_addr(INVALID_PPN, &a));
    assert(!ppn_to_addr(0, NULL));

    /* round trip over the whole reserved space */
    for (uint32_t i = 0; i < FTL_PPN_COUNT; i++) {
        assert(ppn_to_addr((ppn_t)i, &a));
        assert(addr_to_ppn(&a, &p) && p == i);
    }

    /* bad physical addresses */
    nand_addr_t bad[] = { {8, 0, 0}, {0, 64, 0}, {0, 1023, 0}, {0, 0, 64} };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        assert(!addr_to_ppn(&bad[i], &p));
    }

    /* valid vs active */
    assert(ppn_is_active(0) && ppn_is_active(1535));
    assert(!ppn_is_active(1536) && ppn_is_valid(1536));
    assert(ppn_is_active(4096) && !ppn_is_active(4096 + 1536));
    assert(!ppn_is_valid(INVALID_PPN) && !ppn_is_active(INVALID_PPN));
}

static void test_l2p(void)
{
    ppn_t p = 0;

    ftl_map_init();
    for (uint32_t i = 0; i < FTL_LOGICAL_PAGE_COUNT; i++) {
        assert(ftl_map_get((uint16_t)i, &p) && p == INVALID_PPN);
    }

    assert(ftl_map_set(5, 100) && ftl_map_get(5, &p) && p == 100);
    assert(ftl_map_set(5, INVALID_PPN) && ftl_map_get(5, &p) && p == INVALID_PPN);

    assert(!ftl_map_set(FTL_LOGICAL_PAGE_COUNT, 0));   /* lpn out of range */
    assert(!ftl_map_get(FTL_LOGICAL_PAGE_COUNT, &p));
    assert(!ftl_map_set(5, FTL_PPN_COUNT));            /* ppn out of range */
    assert(ftl_map_set(5, 1535));                      /* active 마지막 칸 */
    assert(!ftl_map_set(5, 1536));                     /* valid지만 active 아님 */
    assert(!ftl_map_set(5, 2000));
    assert(ftl_map_get(5, &p) && p == 1535);           /* 거절된 set은 값을 바꾸지 않음 */
    assert(!ftl_map_get(5, NULL));
}

int main(void)
{
    test_address_mapper();
    test_l2p();
    puts("ftl address_mapper + l2p: all tests passed");
    return 0;
}
