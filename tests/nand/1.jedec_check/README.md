# NAND JEDEC ID 읽기

NUCLEO-F411RE / Flash 5 Click(W25N01GV) / HAL 없이 레지스터 직접 제어.
쓰기·삭제 없이 `0x9F` 명령만 전송한다.

- 전원을 끄고 **PA7–PA6 루프백 점퍼를 제거**한 뒤 NAND를 연결한다.
- PA5/D13 → SCK, PA6/D12 → SDO, PA7/D11 → SDI, PB6/D10 → CS.
- 전원은 3.3V·공통 GND. WP·HLD는 HIGH를 유지한다.

```powershell
cd D:\SOLID_miniSSD\firmware\nand
make run
```

`make`가 없다면 `& C:\msys64\ucrt64\bin\mingw32-make.exe run`을 사용한다.
컴파일러와 Programmer 설치 경로는 Makefile에서 변경할 수 있다.

- ST-LINK COM 포트: **115200 / 8N1 / 흐름 제어 없음**.
- 예상 반복 출력: `JEDEC ID: EF AA 21`.
- 디버거 확인 변수: `jedec_id[3]`. SPI 대기 실패 시 `SPI TIMEOUT` 출력 후 정지.
- HSI 16MHz, SPI Mode 0, 8비트 MSB first, SCK 62.5kHz.
- 폴링 제한은 반복 횟수이며 정확한 시간 단위는 아니다.
- 폴링 전용 최소 시작 코드 사용. 보드·실칩 실행은 아직 검증하지 않았다.

근거: [W25N01GV Rev G, 8.2.2 / Figure 6, 문서 27쪽](https://www.pjrc.com/tmp/winbond_w25n01gv.pdf#page=28).
`CS LOW → 9Fh → 더미 8클록 → ID 24클록 → CS HIGH`.
