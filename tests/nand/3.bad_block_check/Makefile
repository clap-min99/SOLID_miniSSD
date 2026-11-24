CROSS_COMPILE ?= arm-none-eabi-
PROGRAMMER ?= STM32_Programmer_CLI
CC := $(CROSS_COMPILE)gcc
SIZE := $(CROSS_COMPILE)size

NAND_DIR := ../../../firmware/nand
NAND_COMMON_DIR := ../../../firmware/common
UART_DIR := ../../common/uart
TARGET := build/bad_block_check.elf
SOURCES := startup.s main.c $(UART_DIR)/uart.c $(NAND_DIR)/spi.c
SOURCES += $(NAND_DIR)/nand.c $(NAND_DIR)/nand_geometry.c $(NAND_DIR)/nand_io.c
HEADERS := $(NAND_COMMON_DIR)/nand.h $(UART_DIR)/uart.h $(wildcard $(NAND_DIR)/*.h)

CFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Og -g3
CFLAGS += -Wall -Wextra -Werror -ffreestanding -fno-builtin -ffunction-sections -fdata-sections
CFLAGS += -Wa,--noexecstack
CPPFLAGS += -I$(NAND_COMMON_DIR) -I$(UART_DIR)
LDFLAGS := -nostdlib -T stm32f411.ld -Wl,--gc-sections,-Map=build/bad_block_check.map

.PHONY: all run
all: $(TARGET)

build:
	powershell.exe -NoProfile -Command "New-Item -ItemType Directory -Force build | Out-Null"

$(TARGET): Makefile $(SOURCES) $(HEADERS) stm32f411.ld | build
	"$(CC)" $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@
	"$(SIZE)" $@

run: $(TARGET)
	"$(PROGRAMMER)" -c port=SWD -w $(TARGET) -v -rst -q
