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

**결정 (2026-11-20):** 관리 영역 시작 Block `FTL_FIRST_BLOCK = 0`. FTL·GC 인터페이스의 `block`은 칩 내 절대 Block 번호. 공장 Bad Block은 초기화 때 `nand_is_factory_bad_block`으로 검사해 BAD 표시

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

### 4-2. 오류 코드

정의: `firmware/common/solid_types.h`의 `solid_err_t`. 값은 로그·Host 전달에 쓰이므로 고정하고, 새 코드는 각 구간 끝에 추가한다.

| 코드 | 값 | 의미 | 발생 | Host까지 | Jetson 처리 (제안) |
|---|---|---|---|---|---|
| `SOLID_OK` | 0x00 | 성공 | 공통 | O | 성공 |
| `ERR_FTL_INVALID_ARG` | 0x10 | 잘못된 인자 (호출 측 버그) | FTL | O | 실패, 재시도 X |
| `ERR_FTL_RANGE` | 0x11 | LPN / chip / block 범위 밖 | FTL | O | 실패, 재시도 X |
| `ERR_FTL_NOT_READY` | 0x12 | `ftl_init` 완료 전 | FTL | O | 잠시 후 재시도 |
| `ERR_FTL_UNMAPPED` | 0x13 | 쓴 적 없는 LPN 읽기 | FTL | O | UNMAPPED READ 정책에 따름 (미정) |
| `ERR_FTL_NO_SPACE` | 0x14 | GC를 해도 빈 Page 없음 | FTL | O | 공간 부족 (`BLK_STS_NOSPC`) |
| `ERR_FTL_BAD_STATE` | 0x15 | Block 상태가 요청과 안 맞음 | FTL→GC | X | - |
| `ERR_FTL_SRC_READ` | 0x16 | GC 이동: 원본 읽기 실패 | FTL→GC | X | - |
| `ERR_FTL_DST_PROGRAM` | 0x17 | GC 이동: 목적지 Program 실패 | FTL→GC | X | - |
| `ERR_FTL_VALID_REMAINS` | 0x18 | 유효 Page가 남아 Erase 거부 | FTL→GC | X | - |
| `ERR_FTL_NOT_SUPPORTED` | 0x19 | 미구현 기능 (예: 영속화 전 FLUSH) | FTL | O | 지원 안 함 (`BLK_STS_NOTSUPP`) |
| `ERR_FTL_FLUSH_FAIL` | 0x1A | 메타데이터 저장 실패 | FTL | O | FLUSH 실패 보고 |
| `ERR_FTL_NO_META` | 0x1B | 저장된 메타데이터 없음 (새 장치) | FTL | O (GET_INFO) | Format 필요 |
| `ERR_FTL_META_CORRUPT` | 0x1C | 메타데이터 손상·구성 불일치 | FTL | O (GET_INFO) | 장치 오류 |
| `ERR_FTL_INTERNAL` | 0x1F | 내부 불변식 깨짐 (버그) | FTL | O | 장치 오류 (`BLK_STS_IOERR`) |
| `ERR_GC_INVALID_ARG` | 0x20 | 잘못된 인자 | GC | X | - |
| `ERR_GC_NOT_INITIALIZED` | 0x21 | `gc_init` 전 | GC | X | - |
| `ERR_GC_NO_SPACE` | 0x22 | 회수할 Victim 없음 (후보 전부 skip 포함) | GC | X (FTL이 `ERR_FTL_NO_SPACE`로 올림) | - |
| `ERR_GC_REENTRANT` | 0x23 | GC 중 GC 재호출 (중첩 금지) | GC | X | - |
| `ERR_NAND_NOT_READY` | 0x30 | 드라이버 초기화 전 | NAND | O | 장치 오류 |
| `ERR_NAND_NO_DEVICE` | 0x31 | 칩 응답 없음 / ID 불일치 | NAND | O | 장치 오류 |
| `ERR_NAND_IO` | 0x32 | SPI 통신 오류 | NAND | O | 재시도 가능 |
| `ERR_NAND_TIMEOUT` | 0x33 | BUSY 해제 안 됨 | NAND | O | 재시도 가능 (`BLK_STS_TIMEOUT`) |
| `ERR_NAND_WRITE_PROTECTED` | 0x34 | 쓰기 보호 해제 안 됨 | NAND | O | 장치 오류 |
| `ERR_NAND_PROGRAM_FAIL` | 0x35 | Program 실패 (FTL 재시도 후에도) | NAND | O | 매체 오류 (`BLK_STS_MEDIUM`) |
| `ERR_NAND_ERASE_FAIL` | 0x36 | Erase 실패 (Block은 BAD 처리) | NAND | X (GC 내부) | - |
| `ERR_NAND_ECC` | 0x37 | 정정 불가 ECC | NAND | O | 매체 오류 (`BLK_STS_MEDIUM`) |
| `ERR_NAND_BAD_BLOCK` | 0x38 | Bad Block 접근 | NAND | X (FTL이 할당 제외) | - |

- **NAND 이름 규칙:** 드라이버(`nand.h`)는 `NAND_ERR_*`를 그대로 쓰고, FTL이 상위로 올릴 때 `ERR_NAND_*`로 바꾼다. `NAND_ERR_INVALID_ARG`는 FTL이 잘못된 주소를 넘긴 버그이므로 `ERR_FTL_INTERNAL`로 바꾼다.
- **Correctable ECC:** 데이터는 정상이므로 오류가 아니다(`SOLID_OK`). 통계로만 센다.
- **Host Status 변환은 HIL 담당.** "Jetson 처리"는 제안이며 HIL·Host와 확정한다.
- **GC 쪽 적용:** `gc.h`의 `gc_result_t`(`GC_OK`, `ERR_GC_*`)를 `solid_err_t`로 바꿔야 한다. 같은 이름이 두 곳에 있으면 함께 include할 때 컴파일 오류가 난다.

#### FTL → GC 결과와 GC 처리

| 코드 | 의미 | GC 처리 |
|---|---|---|
| `SOLID_OK` | 성공 | 계속 진행 |
| `ERR_FTL_SRC_READ` | 원본 Page ECC 실패 | CLOSED 복귀 + 그 Victim 건너뜀 |
| `ERR_FTL_DST_PROGRAM` | 목적지 Program 실패 | CLOSED 복귀 |
| `ERR_FTL_NO_SPACE` | 옮길 빈 공간 없음 | CLOSED 복귀 |
| `ERR_FTL_VALID_REMAINS` | 유효 Page가 남아 Erase 거부 | CLOSED 복귀 |
| `ERR_NAND_ERASE_FAIL` | Erase 실패 (FTL이 BAD 처리) | 다음 Victim으로 진행 |
| `ERR_FTL_BAD_STATE` / `RANGE` / `INVALID_ARG` / `INTERNAL` | 호출 순서·인자 버그 | GC 중단 + 위로 전달 |
| `ERR_FTL_NOT_READY` | FTL 초기화 전 | GC 중단 + 위로 전달 |
| `ERR_NAND_IO` / `TIMEOUT` / `NO_DEVICE` / `NOT_READY` / `WRITE_PROTECTED` | 장치 문제 | GC 중단 + 그대로 위로 전달 |

#### GC가 돌려주는 결과

| 코드 | 의미 | 처리 |
|---|---|---|
| `ERR_GC_NO_SPACE` | 회수할 Victim 없음 (후보 전부 건너뜀 포함) | FTL이 Host WRITE에 `ERR_FTL_NO_SPACE`로 전달 |
| `ERR_GC_REENTRANT` | GC 중 GC 재호출 | 버그 |
| `ERR_GC_INVALID_ARG` / `ERR_GC_NOT_INITIALIZED` | 잘못된 인자 / 초기화 전 | 버그 |

#### 규칙

- `SRC_READ`는 원본 ECC 실패, `DST_PROGRAM`은 목적지 Program 실패일 때만 쓴다. SPI 오류·타임아웃·칩 없음은 `ERR_NAND_*` 그대로 올려서, 장치 고장이 "공간 부족"으로 바뀌지 않게 한다.
- Erase 타임아웃은 지워졌는지 모르므로 BAD 처리한다 (GC에는 `ERR_NAND_ERASE_FAIL`로 보임).

---

## GC 붙이기 전 합의 (2개)

| # | 항목 | 같이 정할 사람 | 상태 |
|---|---|---|---|
| 5 | Page Buffer 소유권 / GC 실행 시점 | FTL · GC · HIL | ☑ |
| 6 | FTL ↔ GC 호출 방식 | FTL · GC | ☑ |

### 5. Page Buffer 소유권 / GC 실행 시점

SRAM의 2KiB Page Buffer는 **1개뿐**. WRITE 데이터 수신 후 같은 버퍼로 GC가 Read하면 **Host 데이터가 사라짐**.

| 작업 | 버퍼 점유 구간 (검토안) |
|---|---|
| Host READ | NAND Read 시작 ~ Host 응답 전송 완료 |
| Host WRITE | UART 수신 시작 ~ Program·Mapping 처리 끝 |
| GC 이동 | 유효 Page Read ~ 목적지 Program·Mapping 끝 |

**결정 (2026-11-20):** A·B 대신 **GC 전용 Buffer 분리**. HIL 수신 2KiB × 2 + FTL 내부 GC Buffer 2KiB × 1 (합계 6KiB). WRITE 중 공간이 부족하면 FTL이 바로 GC를 실행하고, Host 데이터는 HIL Buffer에 남아 있으므로 프로토콜은 바꾸지 않음. GC가 끼면 WRITE 응답이 최대 약 2초 늦어질 수 있음(SPI 1MHz 추정) → Host Timeout 확인 필요. 상세: [protocol.md 5.3](protocol.md)

### 6. FTL ↔ GC 호출 방식

GC는 L2P를 **직접 수정하지 않음**. 수정 권한은 FTL만 가짐.

| 방식 | 내용 | 장단점 |
|---|---|---|
| 묶음 | `ftl_move_page(lpn, old_ppn, victim, buf)` 한 번에 할당+Read+Program+remap | GC 단순, FTL 책임 큼 |
| 단계별 | `ftl_allocate_page` → 직접 Read/Program → `ftl_remap` | 유연, 호출 순서 실수 위험 |

**결정 (2026-11-20):** 묶음 방식. GC는 `ftl_gc_move_one(chip, block, &moved)`로 한 Page씩 옮기고, Erase도 `ftl_reclaim_block`으로 FTL이 수행. GC는 PPN·LPN과 `nand.h`를 직접 쓰지 않음.

- [x] 유효 Page 열거: FTL 내부에서 Victim마다 L2P 1회 스캔 (GC에 노출하지 않음)
- [x] 오래된 조회 결과로 덮어쓰지 않기: 이동 직전 L2P가 아직 그 PPN을 가리키는지 재검증
- [x] 공간 확보: 마지막 Free Block 1개는 GC 전용, GC 시작은 Free ≤ 2
- [x] 오류: `ERR_FTL_SRC_READ` / `ERR_FTL_DST_PROGRAM` / `ERR_FTL_NO_SPACE` / `ERR_FTL_VALID_REMAINS`

전체 표: [ftl-gc-api.md 5.2.1](ftl-gc-api.md)

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
