# FTL (Flash Translation Layer)

담당: 박수민 · 브랜치: `feature/ftl`

Host가 보는 논리 주소(LPN)를 NAND의 물리 주소(PPN)로 바꿔 주는 계층입니다.
NAND는 덮어쓰기를 못 하고 Block 단위로만 지울 수 있어서, FTL은 데이터를 항상 **새 Page에 쓰고 매핑만 바꿉니다**(Out-of-place).
GC가 빈 Block을 만들 수 있도록 Block 상태도 관리합니다.

```
HIL (도재우)          GC (이규현)
   │ ftl.h               │ ftl_gc.h
   ▼                     ▼
┌──────────────── FTL ────────────────┐
│ ftl.c        write / read / GC 이동  │
│ ftl_alloc.c  Block 상태표, Page 할당 │
│ ftl_map.c    L2P 표 (LPN → PPN)      │
│ address_mapper.c  PPN ↔ 칩/Block/Page│
└──────────────────┬──────────────────┘
                   │ nand.h
                   ▼
            NAND 드라이버 (도재우) / Mock NAND
```

---

## 1. 파일별 내용

| 파일 | 누가 include | 내용 |
| --- | --- | --- |
| `ftl.h` / `ftl.c` | **HIL** | `ftl_init`, `ftl_write`, `ftl_read`. GC 실행 시점 판단, GC Page 이동(`ftl_gc_move_one`), Correctable ECC 처리 |
| `ftl_gc.h` | **GC** | Block 상태 타입과 GC가 부르는 함수 6개. GC는 이 헤더만 include |
| `ftl_alloc.h` / `ftl_alloc.c` | FTL 내부 | Block 상태표, 빈 Page 할당, valid/invalid 표시, reserve/reclaim(Erase), NAND 오류 → 공통 코드 변환 |
| `ftl_map.h` / `ftl_map.c` | FTL 내부 | L2P 표 (8,192칸 × 2B = 16KiB). 이 파일만 표를 직접 만진다 |
| `address_mapper.h` / `.c` | FTL 내부 (GC는 상수만) | PPN ↔ `{chip, block, page}` 변환, 관리 영역 상수 |
| `../common/solid_types.h` | 전체 | 공통 오류 코드 `solid_err_t` (`SOLID_OK`, `ERR_FTL_*`, `ERR_GC_*`, `ERR_NAND_*`) |

---

## 2. 사용 방법

### 2-1. HIL에서 (ftl.h)

```c
#include "ftl.h"

nand_init();                      /* NAND 드라이버 먼저 (쓰기 보호 해제 포함) */
solid_err_t e = ftl_init(1);      /* 연결된 NAND 수. 관리 영역 Erase + GC 초기화 */

uint8_t buf[NAND_PAGE_SIZE];      /* 항상 2KiB */
e = ftl_write(lpn, buf);          /* 필요하면 이 안에서 GC가 먼저 돈다 */
e = ftl_read(lpn, buf);
```

| 함수 | 성공 | 대표 실패 |
| --- | --- | --- |
| `ftl_init(chip_count)` | `SOLID_OK` | `ERR_FTL_INVALID_ARG`(0 또는 8 초과), NAND 오류 |
| `ftl_write(lpn, buf)` | `SOLID_OK` | `ERR_FTL_RANGE`(LPN ≥ 8192), `ERR_FTL_NO_SPACE`(GC를 해도 빈 곳 없음), `ERR_NAND_PROGRAM_FAIL`(재시도 후에도 실패), `ERR_NAND_TIMEOUT` 등 |
| `ftl_read(lpn, buf)` | `SOLID_OK` | `ERR_FTL_UNMAPPED`(쓴 적 없는 LPN), `ERR_NAND_ECC_UNCORRECTABLE`, `ERR_NAND_IO` |
| `ftl_get_ecc_corrected_count()` | 정정된 읽기 누적 횟수 | 통계·발표용 |

- 실패하면 기존 데이터와 매핑은 그대로 남습니다.
- 초기화 전 호출은 `ERR_FTL_NOT_READY`, `buf == NULL`은 `ERR_FTL_INVALID_ARG`.
- 코드 전체 의미와 Host까지 가는지 여부: `docs/ftl-agreement.md` 4-2.

### 2-2. GC에서 (ftl_gc.h)

GC는 PPN·LPN·`nand.h`를 직접 쓰지 않고 아래 함수만 부릅니다.

| 함수 | 하는 일 |
| --- | --- |
| `ftl_get_block_info(chip, block, &info)` | 상태, write pointer, valid/invalid 수, erase 횟수 (복사본) |
| `ftl_get_free_block_count()` | FREE Block 수 (OPEN 제외) |
| `ftl_reserve_victim(chip, block)` | CLOSED → RESERVED. 아니면 `ERR_FTL_BAD_STATE` |
| `ftl_gc_move_one(chip, block, &moved)` | 유효 Page 1개 이동. `moved == false`면 다 옮김 |
| `ftl_reclaim_block(chip, block)` | 유효 0이면 Erase → FREE. 남아 있으면 `ERR_FTL_VALID_REMAINS` |
| `ftl_unreserve_victim(chip, block)` | RESERVED → CLOSED (실패 시 되돌리기) |

```c
/* Victim 하나 회수 (gc.c의 gc_collect가 실제로 하는 순서) */
ftl_reserve_victim(c, b);
bool moved;
do {
    if (ftl_gc_move_one(c, b, &moved) != SOLID_OK) { ftl_unreserve_victim(c, b); return; }
} while (moved);
ftl_reclaim_block(c, b);   /* ERR_NAND_ERASE_FAIL이면 FTL이 이미 BAD 처리 */
```

`block`은 칩 안의 절대 Block 번호입니다 (`FTL_FIRST_BLOCK` ~ `+ FTL_ACTIVE_BLOCKS_PER_CHIP - 1`).

---

## 3. 동작 규칙

### 주소와 용량

| 항목 | 값 | 위치 |
| --- | --- | --- |
| Page 크기 (= LBA = LPN 단위) | 2KiB | `nand.h` |
| 논리 용량 | 8,192 LPN = 16MiB | `FTL_LOGICAL_PAGE_COUNT` |
| 칩당 관리 Block | 24개 (3MiB), 최대 64개까지 PPN 예약 | `FTL_ACTIVE_BLOCKS_PER_CHIP`, `FTL_RESERVED_BLOCKS_PER_CHIP` |
| PPN 배치 | 칩마다 4,096칸 고정 (Chip-major) | `address_mapper.h` |
| 매핑 없음 | `INVALID_PPN = 0xFFFF` | `address_mapper.h` |

PPN = `chip × 4096 + (block − FTL_FIRST_BLOCK) × 64 + page`. 활성 Block을 늘려도 기존 PPN 위치는 그대로입니다.

### Block 상태

```
FREE ──할당──▶ OPEN ──64 Page 다 참──▶ CLOSED ──reserve──▶ RESERVED
                                          ▲                    │
                                          └──── unreserve ─────┤
FREE ◀──────────── reclaim (Erase 성공) ──────────────────────┤
BAD  ◀──────────── reclaim (Erase 실패·타임아웃) ─────────────┘
```

- 공장 Bad Block과 시작 시 Erase 실패 Block은 처음부터 BAD → 할당 제외.
- OPEN Block은 전체에서 1개. Page는 0번부터 순서대로만 씁니다 (NAND 규칙).
- 새 Block은 erase 횟수가 가장 적은 FREE Block을 엽니다 (간단한 wear leveling).

### 쓰기 (`ftl_write`)

1. Free Block ≤ 2이면 GC(`gc_collect`)를 반복해 빈 Block을 만든다.
2. 빈 Page 할당 → Program.
3. Program 실패면 그 Page는 버리고 다른 Page로 **1회 재시도** (`FTL_PROGRAM_RETRY`). 타임아웃·통신 오류는 재시도 안 함.
4. 성공하면 새 Page는 valid, 예전 Page는 invalid, L2P를 새 PPN으로.

**마지막 FREE Block 1개는 GC 전용**(`FTL_GC_RESERVED_BLOCKS`)이라 Host 쓰기는 FREE가 2개 이상일 때만 새 Block을 엽니다. GC가 옮길 자리가 항상 남게 하기 위해서입니다.

### 읽기 (`ftl_read`)와 ECC

| NAND ECC 결과 | FTL 반환 |
| --- | --- |
| 오류 없음 | `SOLID_OK` |
| 정정됨 (1~4bit, Correctable) | `SOLID_OK` + Block별 횟수 +1. 같은 Block에서 **3회 이상**이면 그 Page를 새 Page로 옮겨 씀 (read reclaim, 실패해도 읽기는 성공) |
| 정정 불가 | `ERR_NAND_ECC_UNCORRECTABLE` |
| 모름 | `ERR_NAND_IO` |

### GC Page 이동 (`ftl_gc_move_one`)

1. Victim에서 가장 앞의 유효 Page를 찾는다 (Block별 valid 비트맵).
2. L2P를 훑어 그 PPN을 가리키는 LPN을 찾는다.
3. **GC 전용 2KiB 버퍼**(`ftl.c`의 `gc_buf`)로 읽는다. HIL 버퍼와 따로라서 Host 데이터를 덮지 않는다.
4. 빈 Page(GC 예비 Block 사용 가능)에 쓰고 매핑을 바꾼다.

| 실패 | 반환 | GC 처리 |
| --- | --- | --- |
| 원본 정정 불가 / ECC 모름 | `ERR_FTL_SRC_READ` | CLOSED로 되돌리고 그 Victim 건너뜀 |
| 목적지 Program 실패 | `ERR_FTL_DST_PROGRAM` | CLOSED로 되돌림 |
| 빈 Page 없음 | `ERR_FTL_NO_SPACE` | CLOSED로 되돌림 |
| SPI·타임아웃 등 | `ERR_NAND_*` 그대로 | GC 중단, 위로 전달 |

---

## 4. 조정할 수 있는 값

| 상수 | 값 | 파일 | 의미 |
| --- | --- | --- | --- |
| `FTL_ACTIVE_BLOCKS_PER_CHIP` | 24 | `address_mapper.h` | 칩당 쓰는 Block 수 (최대 64) |
| `FTL_FIRST_BLOCK` | 0 | `address_mapper.h` | 관리 영역 시작 Block |
| `FTL_GC_RESERVED_BLOCKS` | 1 | `ftl_alloc.h` | GC 전용 예비 Block 수 |
| `FTL_PROGRAM_RETRY` | 1 | `ftl.c` | Program 실패 시 재시도 횟수 |
| `FTL_ECC_RELOCATE_THRESHOLD` | 3 | `ftl.c` | Block당 정정 횟수가 이 값 이상이면 옮겨 씀 |
| `GC_DEFAULT_FREE_THRESHOLD` | 2 | `../gc/gc.h` | Free ≤ 이 값이면 GC 시작 (GC 담당) |

## 5. 메모리 (SRAM)

| 항목 | 크기 |
| --- | --- |
| L2P 표 | 16KiB |
| Block 상태표 (192 Block × 16B) | 3KiB |
| GC 버퍼 | 2KiB |
| 합계 | 약 21KiB |

---

## 6. 테스트

PC (gcc):

```
cd tests/ftl
make test
```

| 테스트 | 내용 |
| --- | --- |
| `test_ftl_map` | PPN 변환, L2P |
| `test_ftl_alloc` | Block 상태표, 할당 순서, Bad Block, GC 예비 Block |
| `test_ftl` | write/read, 덮어쓰기, Program 재시도, 타임아웃, ECC, 공간 부족 (가짜 NAND) |
| `test_ftl_gc` | **실제 GC + Mock NAND**: 무작위 덮어쓰기, Erase 실패, 원본 읽기 실패, 상태 규칙, 꽉 찰 때 |

실물(W25N01GV 1개): `firmware/bringup/ftl_hw` (gitignore, 로컬 전용). 11-23 기준 15개 항목 통과 — GC 8회, Block 8개 회수, Page 6개 이동 후 데이터 전부 일치.

---

## 7. 아직 안 된 것

| 항목 | 현재 | 계획 |
| --- | --- | --- |
| 영속화 | 전원을 켤 때마다 관리 영역을 Erase(포맷). 매핑은 RAM에만 있음 | Checkpoint / Spare 영역 |
| TRIM | 없음 | FTL-08 |
| PPN → LPN 찾기 | 옮길 때마다 L2P 8,192칸 검색 | 느리면 Victim마다 1회 검색한 목록으로 |
| UNMAPPED 읽기 | `ERR_FTL_UNMAPPED` (임시) | HIL·Host와 0 채우기 여부 결정 |

관련 문서: `docs/ftl-agreement.md`(오류 코드), `docs/ftl-gc-api.md` 5.2.1(FTL↔GC 합의), `docs/protocol.md` 5.3(버퍼)
