# Same compiler installation as the ARM lab. Override TOOL_DIR if needed.
TOOL_DIR ?= C:/arm-gnu-toolchain-15.2.rel1-mingw-w64-i686-arm-none-eabi
PROGRAMMER ?= C:/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe
CC := $(TOOL_DIR)/bin/arm-none-eabi-gcc.exe
SIZE := $(TOOL_DIR)/bin/arm-none-eabi-size.exe
CFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -std=c11 -Og -g3
CFLAGS += -Wall -Wextra -Werror -ffreestanding -fno-builtin
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -Wa,--noexecstack
NAND_DIR := ../../../firmware/nand
CFLAGS += -I../../common/uart -I$(NAND_DIR)
LDFLAGS := -nostdlib -T stm32f411.ld -Wl,--gc-sections,-Map=build/spi_loopback.map

.PHONY: all run clean
all: build/spi_loopback.elf

build:
	powershell.exe -NoProfile -Command "New-Item -ItemType Directory -Force build | Out-Null"

build/main.o: main.c ../../common/uart/uart.h $(NAND_DIR)/spi.h $(NAND_DIR)/stm32f411_regs.h | build
	"$(CC)" $(CFLAGS) -c $< -o $@

build/uart.o: ../../common/uart/uart.c ../../common/uart/uart.h | build
	"$(CC)" $(CFLAGS) -c $< -o $@

build/spi.o: $(NAND_DIR)/spi.c $(NAND_DIR)/spi.h $(NAND_DIR)/stm32f411_regs.h | build
	"$(CC)" $(CFLAGS) -c $< -o $@

build/startup.o: startup.s | build
	"$(CC)" $(CFLAGS) -c $< -o $@

build/spi_loopback.elf: build/startup.o build/main.o build/uart.o build/spi.o stm32f411.ld
	"$(CC)" $(CFLAGS) build/startup.o build/main.o build/uart.o build/spi.o $(LDFLAGS) -o $@
	"$(SIZE)" $@

# Explicit action only: compile, then flash through ST-LINK/SWD.
run: build/spi_loopback.elf
	"$(PROGRAMMER)" -c port=SWD -w build/spi_loopback.elf -v -rst -q

clean:
	powershell.exe -NoProfile -Command "Remove-Item -LiteralPath build/main.o,build/uart.o,build/spi.o,build/startup.o,build/spi_loopback.elf,build/spi_loopback.map -Force -ErrorAction SilentlyContinue"
