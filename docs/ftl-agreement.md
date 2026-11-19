# FTL 공통 계약 — 합의할 것 (지금 4개 + GC 전 2개 + HIL 전 1개)

> 구현 전에 FTL / GC / NAND 담당이 **같은 값**을 쓰기 위해 먼저 정하는 항목입니다.
> 결정되면 `firmware/common/solid_types.h` 한 파일로 옮깁니다.
> 근거: [FTL / GC API](ftl-gc-api.md) · [아키텍처](architecture.md)

## 한눈에 보기

| # | 항목 | 한 줄 설명 | 같이 정할 사람 | 상태 |
|---|---|---|---|---|
| 1 | LPN / PPN 타입 | 주소를 C에서 몇 bit 정수로 쓸지 | FTL · GC | ☐ |
| 2 | UNMAPPED 값 | "매핑 없음"을 나타내는 특수값 | FTL · GC | ☐ |
| 3 | PPN ↔ Chip/Block/Page 변환 | 숫자 하나 ↔ 칩/블록/페이지 | FTL · GC · NAND | ☐ |
| 4 | `nand_*` 헤더 + 오류 코드 | NAND 호출 함수 목록과 실패 종류 | FTL · GC · NAND | ☐ |

---

## 1. LPN / PPN 타입

- **LPN**: Host가 요청하는 논리 Page 번호 (2KiB 단위, 현재 LPN = LBA)
- **PPN**: NAND의 물리 Page를 가리키는 번호

| 결정할 것 | 추천 (초안) | 이유 |
|---|---|---|
| LPN 폭 | `uint16_t` | 논리 8,192개 (16MiB ÷ 2KiB) |
| PPN 폭 | `uint16_t` | 관리 영역 12,288개 (24MiB ÷ 2KiB) |
| PPN 범위 | 전체 Raw가 아닌 **관리 영역 내 번호** | 8칩 Raw 524,288 Page는 16bit로 못 나타냄 |

SRAM 영향: L2P = 8,192 × 2B = **16KiB** (4B면 32KiB, SRAM 128KiB 중)

⚠ 8칩 전체 용량으로 확장하면 16bit로 부족 → 그때 재검토

## 2. UNMAPPED

아직 쓰지 않은 LPN의 L2P 값.

| 결정할 것 | 추천 (초안) |
|---|---|
| 값 | `PPN_UNMAPPED = 0xFFFF` (유효 PPN 최댓값 12,287과 안 겹침) |
| 별도 질문 | **UNMAPPED를 READ하면?** 오류 반환 / 0으로 채움 — 정해야 함 |

## 3. PPN ↔ Chip / Block / Page 변환

```
ppn_to_addr(ppn) → { chip, block, page }
addr_to_ppn(addr) → ppn
```

| 결정할 것 | 비고 |
|---|---|
| 관리 영역을 8칩에 어떻게 배치? | 칩당 균등 / 한 칩에 몰기 / Round Robin |
| 변환식 | 배치가 정해져야 확정. 예) Block당 64 Page → `block = ppn / 64`, `page = ppn % 64` |
| 구조체 | `nand_addr_t { chip; block; page }` 필드 폭 |

참고: W25N01GV는 칩당 1,024 Block, Block당 64 Page

## 4. `nand_*` 헤더 계약 + 오류 코드

### 4-1. 함수

| 함수 | 역할 |
|---|---|
| `nand_read_page(addr, buf)` | 2KiB Page 읽기 |
| `nand_program_page(addr, buf)` | 2KiB Page 쓰기 |
| `nand_erase_block(addr)` | Block 지우기 |
| `nand_get_status(...)` | 상태 조회 |

**반드시 정할 것**

- [ ] 반환 시점 = **NAND 내부 Program/Erase까지 끝난 뒤** (SPI 전송 완료만으로 성공 반환 금지)
- [ ] 동기 호출인지 (블로킹)
- [ ] 버퍼 소유권 (호출 중 `buf` 유지 / 호출 후 해제)
- [ ] Spare 영역 (LPN, Sequence 저장)을 같은 함수로 접근? 별도 API?
- [ ] 초기 Bad Block Marker 조회 API 노출 여부

> Mock NAND와 실제 드라이버가 **같은 헤더**를 구현해야 서로 갈아 끼울 수 있음

### 4-2. 오류 코드 (초안)

| 코드 | 의미 | 주 발생 위치 |
|---|---|---|
| `SOLID_OK` | 성공 | 공통 |
| `SOLID_ERR_RANGE` | LPN/주소 범위 밖 | FTL |
| `SOLID_ERR_UNMAPPED` | 매핑 없음 (정책에 따라) | FTL |
| `SOLID_ERR_NO_SPACE` | 빈 Page / Victim 없음 | FTL / GC |
| `SOLID_ERR_PROGRAM_FAIL` | Program 실패 | NAND |
| `SOLID_ERR_ERASE_FAIL` | Erase 실패 | NAND |
| `SOLID_ERR_ECC` | 읽기 ECC 오류 | NAND |
| `SOLID_ERR_TIMEOUT` | BUSY Timeout | NAND |
| `SOLID_ERR_BAD_BLOCK` | Bad Block 접근 | NAND |

결정할 것: 코드 이름·값, 타입(`enum` vs `int`), Correctable ECC를 오류로 볼지 경고로 볼지

---

## GC 붙이기 전 합의 (2개)

| # | 항목 | 같이 정할 사람 | 상태 |
|---|---|---|---|
| 5 | Page Buffer 소유권 / GC 실행 시점 | FTL · GC · HIL | ☐ |
| 6 | FTL ↔ GC 호출 방식 | FTL · GC | ☐ |

### 5. Page Buffer 소유권 / GC 실행 시점

SRAM의 2KiB Page Buffer는 **1개뿐**. WRITE 데이터 수신 후 같은 버퍼로 GC가 Read하면 **Host 데이터가 사라짐**.

| 작업 | 버퍼 점유 구간 (검토안) |
|---|---|
| Host READ | NAND Read 시작 ~ Host 응답 전송 완료 |
| Host WRITE | UART 수신 시작 ~ Program·Mapping 처리 끝 |
| GC 이동 | 유효 Page Read ~ 목적지 Program·Mapping 끝 |

결정할 것 (택1)

- [ ] **A.** WRITE 데이터를 받기 *전에* 필요한 GC를 끝낸다 (수신 허가 절차 필요)
- [ ] **B.** 유휴 시점에 GC를 돌려 쓰기 여유 공간을 미리 확보한다 (예약 공간·부족 시 정책 필요)

추가 버퍼는 기본으로 가정하지 않음 (SRAM 128KiB)

### 6. FTL ↔ GC 호출 방식

GC는 L2P를 **직접 수정하지 않음**. 수정 권한은 FTL만 가짐.

| 방식 | 내용 | 장단점 |
|---|---|---|
| 묶음 | `ftl_move_page(lpn, old_ppn, victim, buf)` 한 번에 할당+Read+Program+remap | GC 단순, FTL 책임 큼 |
| 단계별 | `ftl_allocate_page` → 직접 Read/Program → `ftl_remap` | 유연, 호출 순서 실수 위험 |

함께 정할 것

- [ ] 유효 Page 열거 방식 (`ftl_get_valid_pages`: 배열 / Iterator / Callback)
- [ ] 오래된 조회 결과로 덮어쓰지 않기: `ftl_remap`에서 `old_ppn`이 현재 Mapping과 다르면 거절
- [ ] 중첩 GC 금지 (GC 이동 중 공간 부족으로 GC 재호출 방지)

---

## HIL 연동 전 합의 (1개)

| # | 항목 | 같이 정할 사람 | 상태 |
|---|---|---|---|
| 7 | HIL ↔ FTL 호출 계약 | FTL · HIL | ☐ |

UART 프레임(MAGIC, Opcode, CRC 등)은 HIL / Host 담당이 정함 → [protocol.md](protocol.md) §4·§10. **FTL에는 파싱된 결과(LPN + 2KiB 버퍼)만 전달됨.**

```
Host 명령 → HIL(프레임 파싱·검증) → ftl_read / ftl_write / ftl_flush / 용량 조회
```

### 7. HIL ↔ FTL 호출 계약

| Host 명령 | FTL 호출 | FTL이 돌려줄 것 |
|---|---|---|
| `READ` | `ftl_read(lpn, buf)` | 데이터 + 결과 코드 |
| `WRITE` | `ftl_write(lpn, buf)` | 결과 코드 |
| `FLUSH` | `ftl_flush()` | 영속화 완료 여부 |
| `GET_INFO` | `ftl_get_capacity()`, `ftl_is_ready()` 후보 | 실제 논리 용량(LPN 개수), 준비 상태 |

결정할 것

- [ ] 함수 인자·반환형 (`solid_err_t` 반환 + 출력 인자?)
- [ ] 동기(블로킹) 호출인지 — 초기엔 동기 + 요청 1개 처리 기준
- [ ] **LPN 범위 검증 위치**: HIL이 먼저 거르고 FTL도 한 번 더 검증 (방어적)
- [ ] **GET_INFO 용량**: 16MiB 제안값을 하드코딩하지 않고 FTL이 실제 구성값을 제공
- [ ] **UNMAPPED READ**: 오류 반환 vs 0 채움 → HIL이 Host에 보낼 응답과 같이 결정
- [ ] **FLUSH 의미**: Checkpoint 저장 성공 시에만 OK, 영속화 미구현이면 오류 반환 (무조건 성공 금지)
- [ ] **초기화 순서**: `ftl_init` 완료 전 요청은 거절 (READY 상태 확인)
- [ ] 오류 코드 → Host Status 변환은 HIL 담당 (FTL은 `SOLID_ERR_*`만 반환)

---

## 합의 후 할 일

1. 1~4 확정 → `firmware/common/solid_types.h`, `firmware/nand/nand_if.h` 작성
2. FTL은 Mock NAND로 개발, NAND 담당은 같은 헤더로 실제 드라이버 구현
3. 5~6은 GC 연동 브랜치(`feature/ftl-gc-api`) 시작 전에 확정
3-1. 7은 HIL 연동 시작 전에 확정
4. 이 문서의 상태 열을 ☑로 갱신

## 나중에 정할 것 (구현하며 결정)

지금 합의하지 않아도 다른 담당이 막히지 않는 항목. 해당 단계에서 이 문서에 추가.

| 항목 | 결정 시점 | 담당 |
|---|---|---|
| Page State 구현 방식, Block 통계 자료형 | L2P·할당기 구현 | FTL |
| 역방향 조회(PPN→LPN) 방식 | GC 연동 | FTL · GC |
| Victim 정책, GC 임계값, 예비 Block 수 | GC 구현 | GC |
| Spare 영역 배치 (LPN·Sequence 위치) | Persistence | FTL · NAND |
| Checkpoint 형식·저장 위치·복원 순서 | Persistence | FTL |
| UNMAPPED READ 시 Host 응답 | HIL 연동 | FTL · HIL |
| Correctable ECC 처리 정책 | 실제 NAND 연결 | FTL · NAND |
