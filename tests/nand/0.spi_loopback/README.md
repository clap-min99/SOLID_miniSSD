# SPI Loopback

- Board: NUCLEO-F411RE; register-based SPI1, no HAL.
- Disconnect the NAND module before connecting PA7 (MOSI) to PA6 (MISO).
- PA5 is SCK; CS is not used. Optional logic analyzer: PA5/PA6/PA7 and GND.
- SPI: Mode 0, 8-bit, MSB first, 62.5 kHz with the reset 16 MHz clock.
- UART: ST-LINK VCP, USART2 PA2 TX, 115200 baud, 8N1, no flow control.
- Open the serial terminal before running; press RESET to repeat the test.

From the repository root:

```powershell
make -C tests/nand/0.spi_loopback
make -C tests/nand/0.spi_loopback run
```

If the make executable on PATH does not work, use:

```powershell
& C:/msys64/ucrt64/bin/mingw32-make.exe -C tests/nand/0.spi_loopback
```

The test compares TX/RX for 00, FF, 55, AA, 12, 34 and prints
`LOOPBACK PASS`, `LOOPBACK FAIL`, or a transfer `TIMEOUT` followed by failure.
UART output uses `tests/common/uart/`. Build outputs stay in the local `build/`.
This tests SPI loopback only; it does not verify NAND commands or CS handling.
