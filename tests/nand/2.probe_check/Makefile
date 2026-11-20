TOOL_DIR ?= C:/arm-gnu-toolchain-15.2.rel1-mingw-w64-i686-arm-none-eabi
PROGRAMMER ?= C:/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe
CC := $(TOOL_DIR)/bin/arm-none-eabi-gcc.exe
SIZE := $(TOOL_DIR)/bin/arm-none-eabi-size.exe

NAND_DIR := ../../../firmware/nand
NAND_COMMON_DIR := ../../../firmware/common
UART_DIR := ../../common/uart
BOARD_DIR := ../1.jedec_check
LINKER_SCRIPT := $(BOARD_DIR)/stm32f411.ld

CFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -std=c11 -Og -g3
CFLAGS += -Wall -Wextra -Werror -ffreestanding -fno-builtin
CFLAGS += -ffunction-sections -fdata-sections -Wa,--noexecstack
CFLAGS += -I$(NAND_COMMON_DIR) -I$(UART_DIR)
LDFLAGS := -nostdlib -T $(LINKER_SCRIPT) -Wl,--gc-sections,-Map=build/probe_check.map

TARGET := build/probe_check.elf
OBJECTS := build/startup.o build/main.o build/uart.o build/spi.o build/nand.o

.PHONY: all run clean
all: $(TARGET)

build:
	powershell.exe -NoProfile -Command "New-Item -ItemType Directory -Force build | Out-Null"

build/main.o: main.c $(NAND_COMMON_DIR)/nand.h $(UART_DIR)/uart.h | build
	"$(CC)" $(CFLAGS) -c $< -o $@

build/uart.o: $(UART_DIR)/uart.c $(UART_DIR)/uart.h | build
	"$(CC)" $(CFLAGS) -c $< -o $@

build/spi.o: $(NAND_DIR)/spi.c $(NAND_DIR)/spi.h $(NAND_DIR)/stm32f411_regs.h | build
	"$(CC)" $(CFLAGS) -c $< -o $@

build/nand.o: $(NAND_DIR)/nand.c $(NAND_COMMON_DIR)/nand.h $(NAND_DIR)/spi.h $(NAND_DIR)/stm32f411_regs.h | build
	"$(CC)" $(CFLAGS) -c $< -o $@

build/startup.o: $(BOARD_DIR)/startup.s | build
	"$(CC)" $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS) $(LINKER_SCRIPT)
	"$(CC)" $(CFLAGS) $(OBJECTS) $(LDFLAGS) -o $@
	"$(SIZE)" $@

run: $(TARGET)
	"$(PROGRAMMER)" -c port=SWD -w $(TARGET) -v -rst -q

clean:
	powershell.exe -NoProfile -Command "Remove-Item -LiteralPath build/startup.o,build/main.o,build/uart.o,build/spi.o,build/nand.o,build/probe_check.elf,build/probe_check.map -Force -ErrorAction SilentlyContinue"
