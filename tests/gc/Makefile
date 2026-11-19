# GC Victim 선택 단위 테스트 (PC gcc)
#   mingw32-make        빌드 후 실행
#   mingw32-make clean  결과물 삭제
CC      = gcc
GC_DIR  = ../../firmware/gc
CFLAGS  = -std=c11 -Wall -Wextra -O1 -g -I$(GC_DIR) -I../../firmware/common
TARGET  = test_gc_victim
SRCS    = $(GC_DIR)/gc.c test_gc_victim.c

ifeq ($(OS),Windows_NT)
  EXE = .exe
else
  EXE =
endif

all: test

$(TARGET)$(EXE): $(SRCS) $(GC_DIR)/gc.h $(GC_DIR)/gc_ftl_if.h ../../firmware/common/nand.h
	$(CC) $(CFLAGS) $(SRCS) -o $@

test: $(TARGET)$(EXE)
	./$(TARGET)$(EXE)

clean:
	rm -f $(TARGET)$(EXE) $(TARGET)

.PHONY: all test clean
