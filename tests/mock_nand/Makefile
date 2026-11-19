# Mock NAND 테스트 (PC gcc)
#   make        빌드 후 실행
#   make clean  결과물 삭제
CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O1 -g -I. -I../../firmware/common
TARGET  = test_mock_nand
SRCS    = mock_nand.c test_mock_nand.c

ifeq ($(OS),Windows_NT)
  EXE = .exe
else
  EXE =
endif

all: test

$(TARGET)$(EXE): $(SRCS) mock_nand.h ../../firmware/common/nand.h
	$(CC) $(CFLAGS) $(SRCS) -o $@

test: $(TARGET)$(EXE)
	./$(TARGET)$(EXE)

clean:
	rm -f $(TARGET)$(EXE) $(TARGET)

.PHONY: all test clean
