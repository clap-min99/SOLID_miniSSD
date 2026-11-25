# FTL 단위 테스트 (PC gcc). 단독 실행: tests/ftl 에서 make test
# PowerShell(cmd)·Git Bash 모두 동작하도록 mkdir -p, rm -rf 등 셸 의존 명령을 쓰지 않는다.
ROOT   = ../..
CC     = gcc
CFLAGS = -Wall -Wextra -I$(ROOT)/firmware/common -I$(ROOT)/firmware/ftl -I$(ROOT)/firmware/gc

FTL_SRC = $(ROOT)/firmware/ftl/address_mapper.c $(ROOT)/firmware/ftl/ftl_map.c
HEADERS = $(wildcard $(ROOT)/firmware/ftl/*.h $(ROOT)/firmware/common/*.h $(ROOT)/firmware/gc/*.h)

.PHONY: test

test: build/test_ftl_map build/test_ftl_alloc build/test_ftl build/test_ftl_gc build/test_ftl_bbt
	build/test_ftl_map
	build/test_ftl_alloc
	build/test_ftl
	build/test_ftl_gc
	build/test_ftl_bbt

build/test_ftl_map: test_ftl_map.c $(FTL_SRC) $(HEADERS) | build
	$(CC) $(CFLAGS) test_ftl_map.c $(FTL_SRC) -o $@

# 가짜 NAND 함수(nand_is_factory_bad_block, nand_erase_block)는 test_ftl_alloc.c 안에 있음
ALLOC_SRC = $(ROOT)/firmware/ftl/address_mapper.c $(ROOT)/firmware/ftl/ftl_alloc.c $(ROOT)/firmware/ftl/ftl_bbt.c
build/test_ftl_alloc: test_ftl_alloc.c $(ALLOC_SRC) $(HEADERS) | build
	$(CC) $(CFLAGS) test_ftl_alloc.c $(ALLOC_SRC) -o $@

# 가짜 NAND(메모리 배열)는 test_ftl.c 안에 있음
FTL_ALL = $(FTL_SRC) $(ROOT)/firmware/ftl/ftl_alloc.c $(ROOT)/firmware/ftl/ftl_bbt.c $(ROOT)/firmware/ftl/ftl.c $(ROOT)/firmware/gc/gc.c
build/test_ftl: test_ftl.c $(FTL_ALL) $(HEADERS) | build
	$(CC) $(CFLAGS) test_ftl.c $(FTL_ALL) -o $@

# FTL-07: 실제 GC + Mock NAND
MOCK = $(ROOT)/tests/mock_nand
build/test_ftl_gc: test_ftl_gc.c $(FTL_ALL) $(HEADERS) $(MOCK)/mock_nand.c | build
	$(CC) $(CFLAGS) -I$(MOCK) test_ftl_gc.c $(FTL_ALL) $(MOCK)/mock_nand.c -o $@

# 영속 BBT: 재부팅 뒤 불량 Block 기억 (Mock NAND)
build/test_ftl_bbt: test_ftl_bbt.c $(ALLOC_SRC) $(HEADERS) $(MOCK)/mock_nand.c | build
	$(CC) $(CFLAGS) -I$(MOCK) test_ftl_bbt.c $(ALLOC_SRC) $(MOCK)/mock_nand.c -o $@

build:
	mkdir build
