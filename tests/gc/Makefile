# GC 단위 테스트 (PC gcc)
#   mingw32-make        빌드 후 두 테스트 모두 실행
#   mingw32-make clean  결과물 삭제
CC      = gcc
GC_DIR  = ../../firmware/gc
CFLAGS  = -std=c11 -Wall -Wextra -O1 -g -I. -I$(GC_DIR) -I../../firmware/common
COMMON  = $(GC_DIR)/gc.c fake_ftl.c
DEPS    = $(COMMON) fake_ftl.h $(GC_DIR)/gc.h $(GC_DIR)/gc_ftl_if.h ../../firmware/common/nand.h

ifeq ($(OS),Windows_NT)
  EXE = .exe
else
  EXE =
endif

all: test

test_gc_victim$(EXE): test_gc_victim.c $(DEPS)
	$(CC) $(CFLAGS) $(COMMON) test_gc_victim.c -o $@

test_gc_collect$(EXE): test_gc_collect.c $(DEPS)
	$(CC) $(CFLAGS) $(COMMON) test_gc_collect.c -o $@

test: test_gc_victim$(EXE) test_gc_collect$(EXE)
	./test_gc_victim$(EXE)
	./test_gc_collect$(EXE)

clean:
	rm -f test_gc_victim$(EXE) test_gc_collect$(EXE) test_gc_victim test_gc_collect

.PHONY: all test clean
