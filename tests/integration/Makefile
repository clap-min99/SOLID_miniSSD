# 통합 시뮬레이션: 진짜 FTL + GC(Victim 선택) + Mock NAND (PC gcc)
#   mingw32-make        빌드 후 실행
#   mingw32-make clean  결과물 삭제
CC      = gcc
ROOT    = ../..
CFLAGS  = -std=c11 -Wall -Wextra -O1 -g \
          -I$(ROOT)/firmware/common -I$(ROOT)/firmware/ftl -I$(ROOT)/firmware/gc -I$(ROOT)/tests/mock_nand
SRCS    = test_ftl_mock.c \
          $(ROOT)/firmware/ftl/address_mapper.c $(ROOT)/firmware/ftl/ftl_map.c \
          $(ROOT)/firmware/ftl/ftl_alloc.c $(ROOT)/firmware/ftl/ftl.c \
          $(ROOT)/firmware/gc/gc.c $(ROOT)/tests/mock_nand/mock_nand.c
HEADERS = $(wildcard $(ROOT)/firmware/common/*.h $(ROOT)/firmware/ftl/*.h $(ROOT)/firmware/gc/*.h) \
          $(ROOT)/tests/mock_nand/mock_nand.h

ifeq ($(OS),Windows_NT)
  EXE = .exe
else
  EXE =
endif

all: test

test_ftl_mock$(EXE): $(SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(SRCS) -o $@

test: test_ftl_mock$(EXE)
	./test_ftl_mock$(EXE)

clean:
	rm -f test_ftl_mock$(EXE) test_ftl_mock

.PHONY: all test clean
