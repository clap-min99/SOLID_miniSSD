# FTL 단위 테스트 (PC gcc). 단독 실행: tests/ftl 에서 make test
# PowerShell(cmd)·Git Bash 모두 동작하도록 mkdir -p, rm -rf 등 셸 의존 명령을 쓰지 않는다.
ROOT   = ../..
CC     = gcc
CFLAGS = -Wall -Wextra -I$(ROOT)/firmware/common -I$(ROOT)/firmware/ftl

FTL_SRC = $(ROOT)/firmware/ftl/address_mapper.c $(ROOT)/firmware/ftl/ftl_map.c
HEADERS = $(wildcard $(ROOT)/firmware/ftl/*.h $(ROOT)/firmware/common/*.h)

.PHONY: test

test: build/test_ftl_map
	build/test_ftl_map

# ponytail: 테스트 파일이 늘면 이 규칙을 복사하거나 패턴 규칙으로 일반화
build/test_ftl_map: test_ftl_map.c $(FTL_SRC) $(HEADERS) | build
	$(CC) $(CFLAGS) test_ftl_map.c $(FTL_SRC) -o $@

build:
	mkdir build
