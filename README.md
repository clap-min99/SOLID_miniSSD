# SOLID_miniSSD

FTL·GC 및 Linux 블록 장치 드라이버 기반 SSD 모사 시스템 구현 프로젝트입니다.

## 프로젝트 개요

Jetson Linux, STM32, SPI NAND Flash를 연동해 Mini SSD Controller를 구현합니다. NAND의 읽기·쓰기·삭제부터 FTL의 주소 매핑, Garbage Collection(GC)의 공간 회수까지 구현하며 SSD 컨트롤러의 동작 원리를 이해하는 것을 목표로 합니다.

## 시스템 구성

```text
Jetson Linux                 STM32                         SPI NAND Flash
[블록 장치 드라이버] <-- USB --> [FTL / GC / NAND 드라이버] <-- SPI --> [저장 공간]
```

## 주요 구현 목표

- **NAND 드라이버**: 페이지 읽기·쓰기, 블록 삭제, 상태 확인 및 오류 처리
- **FTL**: 논리 주소와 물리 주소 매핑, 페이지 할당 및 데이터 갱신
- **GC**: 회수할 블록 선정, 유효 데이터 이동 및 저장 공간 확보
- **Linux 드라이버**: Jetson–STM32 USB 통신, 블록 장치 등록 및 읽기·쓰기 요청 처리
- **검증**: Mock NAND 기반 테스트, 반복 쓰기 및 GC 이후 데이터 정합성 확인, 쓰기 증폭(WAF)과 I/O 지연 측정

## 개발 환경

- **하드웨어**: Jetson, STM32, SPI NAND Flash
- **소프트웨어**: C/C++, Embedded Linux, Linux Kernel Driver
- **인터페이스**: USB, SPI
- **개발 및 검증 도구**: Git, Mock NAND, CLI, 자동화 테스트

## 개발 계획

NAND 드라이버와 FTL을 구현하고, GC 및 Linux 드라이버를 연동한 뒤 통합 테스트와 성능 검증을 진행할 예정입니다. 빌드 및 실행 방법은 구현이 진행되면서 추가합니다.
