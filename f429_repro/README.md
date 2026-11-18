# STM32F429 + FreeRTOS JSON 하드폴트 재현 (Renode)

1초마다 상태 요청에 JSON으로 응답하던 펌웨어 구조를 UART로 옮겨, 메모리 가설을 하나씩 재현하고 수정본과 비교합니다.
이더넷(LwIP)은 Renode의 STM32 이더넷 지원이 불완전해서 뺐습니다. 가설의 핵심은 통신이 아니라 **"요청마다 문자열을 만들어 JSON으로 변환하는 메모리 사용 방식"**이기 때문입니다.

STM32CubeIDE 없이 `arm-none-eabi-gcc` + `make`로 빌드하고, Renode v1.16.1에서 실행했습니다. **여섯 모드 모두 재현을 마쳤고, 버퍼·스택·힙 크기를 바꾼 대조 실험도 했습니다.** 요약은 [7. 결론](#7-결론)에 있습니다.

| MODE | 가설 | 결과 |
|---|---|---|
| 1 STACK | 1KB 태스크 스택 + `char json[768]` 지역 버퍼 + `sprintf(%f)` | ✅ **재현됨**: 첫 응답에서 스택이 넘쳐 FreeRTOS 커널 변수를 덮음 → `vTaskDelay()`의 `configASSERT` 실패. [기록](#mode-1-stack--재현-결과) |
| 2 HEAP | 응답마다 `pvPortMalloc(256)`, 전송 실패 시 free 누락, NULL 검사 없음 | ✅ **재현됨**: heapFree가 4초마다 264B씩 줄다가 seq 205에 `MALLOC FAILED`. [기록](#mode-2-heap--재현-결과) |
| 3 BUFFER | 구조체 안 `char tx[160]` 바로 뒤에 함수 포인터, 길이 검사 없이 strcat 누적 | ✅ **재현됨**: seq 9에 포인터가 `0x7563222C`(`",\"cu"`)로 덮임 → 호출 시 UsageFault (INVSTATE). [기록](#mode-3-buffer--재현-결과) |
| 4 FIXED | 정적 버퍼 + `snprintf` 반환값 검사 + 정수 스케일 포맷, 동적 할당 없음 | ✅ **대조군**: seq 745(약 12분)까지 폴트 0건, 스택·힙 사용량 변화 없음 |
| 5 NEWLIB_LEAK | 1초마다 상태 응답(약 490B 한 줄 JSON)을 newlib `malloc`으로 만들고 free 안 함, NULL 검사 없음 | ✅ **재현됨**: FreeRTOS heapFree는 끝까지 그대로, newlib 힙만 요청당 504B씩 증가 → seq 356에 `malloc` NULL → 폴트 없이 응답만 사라짐. [기록](#mode-5-newlib_leak--재현-결과) |
| 6 COPY_OVERFLOW | 응답을 256B 지역 메시지에 길이 검사 없이 `strcpy`. 5번째 요청마다 센서 상태 응답(약 371B) | ✅ **재현됨**: seq 5에서 복귀 주소가 덮여 `Build_And_Send` 복귀 시 UsageFault (INVSTATE). **스택을 2배·4배로 키워도 레지스터 값까지 똑같음**. [기록](#mode-6-copy_overflow--재현-결과) |

> MODE 1에서 STKERR가 나오지 않는 이유: STKERR는 예외 진입 시 스택을 쌓다가 **버스 에러**가 날 때만 켜집니다. 태스크 스택은 FreeRTOS 힙(RAM) 안에 있어서, 넘쳐도 그 아래는 멀쩡한 RAM이라 버스 에러가 나지 않습니다. 그래서 다른 데이터를 조용히 덮고, 문제는 나중에 다른 곳에서 드러납니다. 이 프로젝트에서는 스택 바로 아래가 FreeRTOS 커널 변수였습니다.

## 1. 폴더 구성
```
f429_repro/
  Makefile                 make MODE=N 으로 빌드
  STM32F429ZI_FLASH.ld     링커 스크립트 (RAM 배치도 주석 포함)
  Core/Inc/
    repro.h                REPRO_MODE, 주기 1000ms, 태스크 스택 256 words
    main.h                 HAL 헤더 묶음
    FreeRTOSConfig.h       힙 15360B, 스택 오버플로 검사 2, malloc 실패 훅
    stm32f4xx_hal_conf.h   HAL 설정 (ST 템플릿 그대로, BSD-3)
  Core/Src/
    main.c                 HAL 초기화, USART1, 태스크 생성, 스케줄러 시작
    repro.c                MODE 1~4 재현 코드 + [MEM] 계측 + FreeRTOS 훅
    fault.c                폴트 핸들러 (HardFault/MemManage/BusFault/UsageFault 공용, stacked PC/LR, IPSR, CFSR/HFSR 출력 + 해석)
    sysmem.c               newlib malloc용 _sbrk (상한 검사)
  renode/f429_repro.resc   Renode 실행 스크립트 (USART1 창, GDB 3333)
  third_party/             외부 공개 저장소 (git clone, 이 저장소에는 포함하지 않음)
```

설계상 선택:
- **클럭**: 리셋 기본값인 HSI 16MHz를 그대로 씁니다. PLL/HSE 대기 루프가 없어 Renode에서 멈추지 않습니다.
- **HAL 틱**: SysTick은 FreeRTOS가 쓰므로 TIM6 대신 FreeRTOS 틱 훅(`vApplicationTickHook`)에서 `HAL_IncTick()`을 호출합니다. 스케줄러 시작 전에는 `HAL_Delay()`를 쓰면 안 됩니다.
- **핸들러 연결**: `FreeRTOSConfig.h`에서 `SVC/PendSV/SysTick_Handler` 이름을 FreeRTOS 포트 함수에 연결합니다. `HardFault_Handler`는 `fault.c`에 있습니다.

## 2. 준비 (Ubuntu)
```bash
sudo apt update
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi gdb-multiarch make git

cd f429_repro
mkdir -p third_party && cd third_party
git clone --depth 1 https://github.com/STMicroelectronics/stm32f4xx_hal_driver.git
git clone --depth 1 https://github.com/STMicroelectronics/cmsis_device_f4.git
git clone --depth 1 https://github.com/ARM-software/CMSIS_5.git
git clone --depth 1 -b V10.6.2 https://github.com/FreeRTOS/FreeRTOS-Kernel.git
cd ..
```
Renode는 [공식 릴리스](https://github.com/renode/renode/releases)의 portable 패키지 **v1.16.1**을 사용했습니다.

## 3. 빌드
```bash
make MODE=4          # → build/mode4/f429_repro.elf
make MODE=1          # 모드마다 폴더가 따로라 clean 없이 바로 전환 가능
make MODE=2 HOOK=0   # malloc 실패 훅 끔 → build/mode2_nohook/ (NULL 반환 후에도 계속 실행)
make MODE=1 ASSERT=0 # configASSERT 끔 → build/mode1_noassert/ (커널 검사 없이 실행)
make MODE=1 STACK=512   # 태스크 스택 2KB → build/mode1_stack512/ (크기 대조 실험)
make MODE=5 HEAP=30720  # FreeRTOS 힙 2배 → build/mode5_heap30720/ (크기 대조 실험)
make MODE=6 STACK=1024  # 태스크 스택 4KB → build/mode6_stack1024/ (크기 대조 실험)
make clean           # build/ 전체 삭제
```
헤더 파일(`FreeRTOSConfig.h` 등)만 고쳤을 때는 Makefile이 변경을 감지하지 못하므로 `make clean` 후 다시 빌드합니다.
빌드하면 함께 생기는 파일:
- `build/modeN/f429_repro.map`: 변수와 함수의 주소·크기 (MODE 3에서 `g_tx` 뒤에 무엇이 있는지 확인)
- `build/modeN/Core/Src/repro.c.su`: 함수별 스택 사용량 (MODE 1 근거)

### MODE 4 빌드 결과 (메모리 배치)
| 영역 | 주소 | 크기 | 내용 |
|---|---|---|---|
| Flash 전체 | 0x08000000~ | 약 30.5KB | 벡터 0x1AC + 코드 0x6C58 + 상수 0x88C + .data 초기값 0x1E8 |
| `.data` | 0x20000000 | 488B | 초기값 있는 전역 (`g_status` 등) |
| `.bss` | 0x200001E8 | 16,248B | `huart1` 72B, `s_json` 160B, **FreeRTOS 힙 `ucHeap` 15,360B** |
| newlib 힙 | `_end`=0x20004160 ~ | 최대 약 174KB | `_sbrk` 상한 = `_estack`(0x20030000) − `_Min_Stack_Size`(1KB) |
| MSP | ~0x20030000 | 최소 1KB | 스케줄러 시작 후 인터럽트·폴트 핸들러 전용 |

`repro.c.su` 기준으로 `Repro_Log`는 스택 144B를 씁니다(`line[128]` + 16B). MODE 4의 `Build_And_Send`는 24B로, 버퍼를 스택에 두지 않은 효과가 드러납니다.

**FreeRTOS 힙과 newlib 힙은 별개입니다.** `pvPortMalloc`은 `ucHeap`에서, `malloc`(printf `%f`, cJSON 등이 내부에서 사용)은 `_sbrk`로 `_end` 위쪽에서 메모리를 가져갑니다. 그래서 `[MEM]` 로그의 heapFree는 `malloc` 누수를 보여 주지 않습니다. newlib 힙은 GDB에서 `p __sbrk_heap_end`로 확인합니다.

## 4. Renode 실행
`renode/f429_repro.resc`의 `$bin`(6번째 줄)은 `.resc` 파일 위치(`$ORIGIN`) 기준 상대경로입니다. 빌드 폴더 이름만 바꾸면 모드를 전환할 수 있습니다.
```
$bin?=@$ORIGIN/../build/mode4/f429_repro.elf
```
```bash
# 예: MODE 3으로 전환 (마지막 인자로 .resc 경로를 꼭 넣어야 함)
sed -i 's#build/mode[^/]*/f429_repro.elf#build/mode3/f429_repro.elf#' renode/f429_repro.resc

renode renode/f429_repro.resc        # 화면 없는 환경: renode --console renode/f429_repro.resc
(f429) start
```
`.resc`는 Renode가 시작할 때만 읽으므로, 경로를 바꾼 뒤에는 `quit` 후 다시 실행합니다.
MODE 4라면 USART1 창에 다음과 같이 출력되어야 합니다. seq가 4의 배수일 때는 전송 실패를 흉내 내므로 JSON 줄이 빠집니다.
```
=== F429 JSON repro start: MODE 4 ===
[MEM] boot seq=0 stackFreeMin=... words heapFree=... heapMinEver=...
{"seq":1,"temp":360,"keg":1815,"cups":0,"tap":true}
```
출력이 없을 때:
| 증상 | 확인 |
|---|---|
| UART 창이 비어 있음 | 모니터에서 `peripherals` → UART 이름이 `usart1`인지 |
| `ELF ... not found` | `$bin` 경로 |
| 출력 없이 멈춤 | `pause` → `cpu PC` → `addr2line`으로 위치 확인 |

## 5. 폴트 분석
**1) UART 출력**: 폴트가 나면 `fault.c`가 `HARD FAULT` 블록(stacked PC/LR/R0, xPSR, EXC_RETURN, HFSR, CFSR, MMFAR/BFAR와 비트 해석)을 출력합니다.

**2) 소스 줄 찾기**
```bash
arm-none-eabi-addr2line -e build/modeN/f429_repro.elf -f -C 0x<stacked PC>
```

**3) GDB** (Renode가 3333 포트를 열어 둠)
```bash
gdb-multiarch build/modeN/f429_repro.elf -ex "target remote :3333"
```
```gdb
(gdb) break HardFault_Handler
(gdb) continue
(gdb) p/x $lr                       # EXC_RETURN: bit2=1이면 태스크 스택(PSP)
(gdb) x/8wx $psp                    # R0 R1 R2 R3 R12 LR PC xPSR
(gdb) x/wx 0xE000ED28               # CFSR
(gdb) p pxCurrentTCB->pcTaskName    # 폴트가 난 태스크
(gdb) p __sbrk_heap_end             # newlib 힙 끝
```

**4) Renode 모니터**
```
pause                                  # 정지 (이 프로젝트에서 사용)
cpu PC                                 # 현재 PC (이 프로젝트에서 사용)
cpu GetRegisterUnsafe 14               # LR (이 프로젝트에서 사용)
sysbus ReadDoubleWord 0x08000000       # 메모리 읽기 (이 프로젝트에서 사용)
```
아래 두 명령은 하드웨어 디버거에 없는 Renode 기능이지만 이 프로젝트에서는 **실행해 보지 않았습니다**. 명령 이름은 버전에 따라 다를 수 있습니다.
```
cpu AddHook `sysbus GetSymbolAddress "HardFault_Handler"` "self.Pause()"
cpu CreateExecutionTracing "trace" @/tmp/trace.log PC   # 폴트 직전 실행 흐름
```
MODE 3에서 Renode가 CFSR(INVSTATE)을 실제로 채우는 것을 확인했습니다. 다른 폴트에서 CFSR이 0으로 나오면 stacked PC와 trace 로그로 추적합니다.

Renode는 UsageFault 같은 개별 폴트를 HardFault로 승격하지 않고 해당 벡터로 바로 보냅니다(MODE 3에서 IPSR=6으로 확인). 실제 칩은 SHCSR에서 켜지 않으면 HardFault로 승격합니다. 그래서 `fault.c`는 네 폴트 벡터를 모두 같은 보고 루틴에 연결하고, IPSR로 실제 예외 번호를 출력합니다.

## 6. 재현 기록 (모드별)
모든 기록은 `환경 → 증상 → 근거 → 원인 → 수정 → 결과` 순서로 적었습니다. "seq"는 1초마다 오는 상태 요청의 순번입니다.

### MODE 1 (STACK) — 재현 결과
```
- 환경: Renode v1.16.1, stm32f429.repl, FreeRTOS V10.6.2, 태스크 스택 256 words(1KB) / FreeRTOS 힙 15360 B
- 증상: seq 1(첫 응답) 직후 configASSERT 실패 — tasks.c:1312 vTaskDelay()의 uxSchedulerSuspended == 0 검사
        (스택 오버플로 훅보다 먼저 걸림. 훅은 컨텍스트 전환 때만 검사하므로)
- 근거: repro.c.su: Build_And_Send = 840B (json[768] + field[64]) + sprintf(%f) 내부 프레임 > 1KB
        GDB bt: vTaskDelay(1000) tasks.c:1312 ← StatusTask repro.c:304
        pxCurrentTCB = 0x200008D0 <ucHeap+1040> → 스택 = 0x200004C8 ~ 0x200008C7
        uxSchedulerSuspended@0x2000039C (스택 바닥 300B 아래) = 0xFFFFFFEF (정상값 0)
        커널 태스크 리스트 자리에 newlib printf 내부 함수의 복귀 주소와 printf 인자(double 18.15) 발견
- 원인: 큰 지역 버퍼 + float printf로 1KB 태스크 스택 초과 → 스택 아래 FreeRTOS 커널 변수 덮어쓰기
- 수정: (MODE 4) 버퍼를 정적으로 옮기고 정수 포맷 사용 → Build_And_Send 24B
- 결과: MODE 4로 seq 745까지 폴트 0건, stackFreeMin = 108 words 유지
```

**UART 로그**
```
=== F429 JSON repro start: MODE 1 ===
[MEM] boot seq=0 stackFreeMin=116 words heapFree=13624 heapMinEver=13624
!!! configASSERT failed: third_party/FreeRTOS-Kernel/tasks.c
  line = 0x00000520                      ← 1312번째 줄
```

**메모리 배치 (map + GDB)**: 태스크 스택은 FreeRTOS 힙의 첫 블록이고, 그 바로 아래(낮은 주소)는 커널 변수입니다. 스택은 아래로 자라므로 넘치면 커널 변수를 덮습니다.
```
0x2000039C  uxSchedulerSuspended        ┐
0x200003A0  xIdleTaskHandle ~ xTickCount │ FreeRTOS 커널 변수 (tasks.c, port.c, heap_4.c의 .bss)
0x200003C8  xPendingReadyList, 지연 리스트│
0x2000040C  pxReadyTasksLists (0x8C)     │
0x20000498  pxCurrentTCB                 │
0x200004A4  heap_4 통계, pxEnd, xStart   ┘
0x200004C0  ucHeap 시작: 블록 헤더 8B
0x200004C8  StatusTask 스택 바닥 ─┐ 1024B, 위에서 아래로 사용
0x200008C7  StatusTask 스택 꼭대기 ┘
0x200008D0  StatusTask TCB (pxCurrentTCB)
```

**GDB로 확인한 덮인 값** (`x/80wx 0x20000390`, assert로 정지한 상태)
| 주소 | 변수 | 값 | 판단 |
|---|---|---|---|
| 0x2000039C | `uxSchedulerSuspended` | 0xFFFFFFEF | **덮임.** 정상값 0 → assert 실패의 직접 원인 |
| 0x200003A4~A8 | `xNextTaskUnblockTime` 등 | 0x80000000, 0x40322666 | **덮임.** 두 워드를 double로 읽으면 18.15 = seq 1의 `keg` 값(printf `%f` 인자) |
| 0x200003C4 | `uxCurrentNumberOfTasks` | 0x08000B19 | **덮임.** `__ssputs_r`(sprintf의 출력 함수 포인터). 0x200003E8, 0x20000410 등 지연·준비 리스트 자리에도 같은 값 |
| 0x200003CC | `xPendingReadyList` | 0x08002DA7 | **덮임.** `_printf_common+0xAE` 복귀 주소. 0x200003F4(`xDelayedTaskList2`)에도 같은 값 |
| 0x2000041C | `pxReadyTasksLists` | 0x08002EC7 | **덮임.** `_printf_i+0xAA` 복귀 주소 |
| 0x2000044C | `pxReadyTasksLists` | 0x08000E21 | **덮임.** `_svfprintf_r+0x140` 복귀 주소 |
| 0x200004B8 | `xStart.pxNextFreeBlock` | 0x00000000 | **덮인 것으로 판단.** 정상이면 첫 빈 블록 주소. 다음 `pvPortMalloc`이 실패하거나 폴트를 낼 상태 |
| 0x200004C4 | 스택 블록 헤더 크기 | 0x00000000 | **덮인 것으로 판단.** 정상이면 할당 표시 비트 + 1032 |
| 0x20000498 | `pxCurrentTCB` | 0x200008D0 | 정상 |
| 0x200004B0 | `xFreeBytesRemaining` | 13624 | 정상 |
| 0x200004CC | 스택 바닥 +4 | 0xA5A5A5A5 | 정상 (FreeRTOS가 채워 둔 미사용 표시 패턴) |

태스크 리스트에는 원래 RAM 주소(0x2000xxxx)와 리스트 끝 표시(0xFFFFFFFF)만 들어갑니다. 그 자리에 Flash 주소(0x0800xxxx)가 있다는 것은 printf 내부 함수들이 스택 프레임에 저장한 복귀 주소가 그 위치에 쓰였다는 뜻입니다. 위 표의 Flash 주소는 map 파일의 심볼로 함수 이름을 찾았습니다.

**덮어쓰기는 띄엄띄엄 일어납니다.** `pxCurrentTCB`, `xFreeBytesRemaining`처럼 바로 옆 변수는 멀쩡하고, 스택 바닥 위에도 0xA5 패턴이 남은 칸이 있습니다. 프레임 전체가 아니라 함수가 실제로 값을 저장한 칸만 바뀌기 때문입니다. 그래서 같은 오버플로라도 어떤 변수가 깨지느냐에 따라 증상이 매번 달라질 수 있습니다.

**원인 위치 ≠ 증상 위치**: 스택을 넘긴 곳은 `Build_And_Send` 안의 `sprintf`인데, 실행을 멈춘 곳은 다음에 호출한 `vTaskDelay`(커널)입니다. 

#### ASSERT=0 — 커널 검사 없이 실행
```bash
make MODE=1 ASSERT=0   # → build/mode1_noassert/f429_repro.elf
```
```
- 증상: 부팅 로그 2줄 이후 출력 없음. HardFault도 훅도 없이 시스템이 멈춤
- 근거: Renode 경고가 끝없이 반복
          [cpu: 0x8006754] ReadDoubleWord from non existing peripheral at 0x4.
          [cpu: 0x8006756] ReadDoubleWord from non existing peripheral at 0x0.
        map: 0x08006754/56 = vListInsert+0x10/+0x12 (list.c, 삽입 위치를 찾는 for 루프)
- 원인: 덮인 리스트를 따라가다 pxNext가 NULL이 됨 → 0x4(pxNext)와 0x0(xItemValue)을 계속 읽는 무한 루프
```
UART 출력:
```
=== F429 JSON repro start: MODE 1 ===
[MEM] boot seq=0 stackFreeMin=116 words heapFree=13624 heapMinEver=13624
(이후 출력 없음)
```

`vListInsert`의 루프는 `pxIterator->pxNext->xItemValue`가 삽입할 값보다 클 때까지 리스트를 따라갑니다. 리스트 항목에서 `xItemValue`는 오프셋 0, `pxNext`는 오프셋 4에 있습니다. 포인터가 NULL이 되면 0x4와 0x0을 읽게 되는데, Renode는 연결되지 않은 주소를 0으로 읽습니다. 그래서 `pxNext`가 다시 0, `xItemValue`도 0이 되어 루프가 끝나지 않습니다.

FreeRTOS 소스의 이 루프 바로 위 주석에는 "여기서 멈추면 가능한 원인"의 첫 번째로 **Stack overflow**가 적혀 있습니다(`list.c`). 재현 결과와 일치합니다.

**같은 오버플로, 다른 증상**: assert를 켜면 `vTaskDelay`에서 원인이 가까운 곳에 기록되고, assert를 끄면 폴트 메시지 없이 시스템이 멈춥니다. 실제 칩에서는 0x0이 Flash 별칭이라 벡터 테이블 값을 포인터로 따라가므로 결과가 다를 수 있습니다(무한 루프 또는 BusFault, 가정). 

**GDB `bt`로 확인한 호출 경로**
```
#0  vListInsert (pxList=0x8000b19 <__ssputs_r>, pxNewListItem=0x3008cc)  list.c:179
#1  prvAddCurrentTaskToDelayedList (xTicksToWait=1000, ...)             tasks.c:5478
#2  vTaskDelay (xTicksToDelay=1000)                                       tasks.c:1324
#3  StatusTask                                                            repro.c:304
```
`prvAddCurrentTaskToDelayedList`는 현재 태스크를 `pxDelayedTaskList`가 가리키는 지연 리스트에 넣습니다. 그런데 넘겨준 리스트 주소가 RAM의 리스트가 아니라 **`__ssputs_r`(sprintf 내부의 출력 함수) 주소**였습니다. 커널 변수 `pxDelayedTaskList`가 printf 프레임이 저장한 함수 포인터로 덮였다는 뜻입니다. `vListInsert`는 Flash의 코드 바이트를 리스트 항목으로 읽으며 따라가다 NULL에 도달했고, 위에서 설명한 무한 루프에 빠졌습니다. MODE 1(assert 켬) 덤프에서도 커널 리스트 자리에 같은 값 0x08000B19가 있었습니다.

`pxNewListItem=0x3008cc`는 정상이라면 TCB 안의 주소(0x2000xxxx)여야 합니다. 다만 `-Og` 최적화로 레지스터가 재사용됐을 수 있어 이 값만으로 손상 여부를 판단하지는 않았습니다. 마지막 `uxListRemove` 프레임은 태스크의 맨 바깥 프레임이라 GDB가 잘못 되감은 것이고, 실제 호출이 아닙니다.

### MODE 2 (HEAP) — 재현 결과
```
- 환경: Renode v1.16.1, stm32f429.repl, FreeRTOS V10.6.2, 태스크 스택 256 words / FreeRTOS 힙 15360 B, HOOK=1
- 증상: seq 205회차에 MALLOC FAILED (HardFault 없음, 훅의 무한 루프에서 정지)
- 근거: [MEM] seq=200 heapFree=424 heapMinEver=424 → 훅 출력 heapFree=0xA0(160)
        13624 − 51 × 264(256 + heap_4 헤더 8) = 160 < 264 → 다음 할당 실패
        heapFree == heapMinEver로 단조 감소, stackFreeMin = 108 words (MODE 4와 같음, 스택 무관)
- 원인: 전송 실패 경로에서 vPortFree 누락 → 4번에 1번 264B 누수 → 51회 만에 FreeRTOS 힙 고갈
- 수정: (MODE 4) 동적 할당 제거, 정적 버퍼 사용
- 결과: MODE 4로 seq 745까지 heapFree 13624 유지 (heapFree == heapMinEver, 감소 없음)
```

**UART 로그 (발췌)**
```
[MEM] periodic seq=200 stackFreeMin=108 words heapFree=424 heapMinEver=424
{"seq":201,"temp":360,"keg":815,"cups":67,"tap":true}
{"seq":202,"temp":370,"keg":810,"cups":67,"tap":false}
{"seq":203,"temp":380,"keg":805,"cups":67,"tap":true}
!!! MALLOC FAILED (heap exhausted)
  heapFree = 0x000000A0
```

**힙 잔량 계산**
| 시점 | heapFree | 계산 |
|---|---|---|
| 부팅 직후 | 13,624B | 15,360B − 태스크 2개(StatusTask, Idle)의 스택·TCB |
| seq 200 | 424B | 13,624 − 50회(seq 4, 8, …, 200) × 264 |
| seq 204 | 160B | 51번째 누수. 전송 실패 회차라 JSON 줄이 출력되지 않음 |
| seq 205 | 할당 실패 | 남은 160B < 필요한 264B. 단편화가 아니라 순수 고갈 |

블록 하나가 요청한 256B가 아니라 264B를 차지하는 것은 heap_4가 블록마다 8B 헤더(다음 블록 포인터와 크기)를 붙이기 때문입니다.

#### HOOK=0 — 훅 없이 NULL로 계속 실행
훅을 켜 두면 할당 실패 즉시 멈춥니다. 훅을 끄면 `pvPortMalloc`이 NULL을 반환하고, 코드는 NULL 검사 없이 그대로 진행합니다.
```bash
make MODE=2 HOOK=0   # → build/mode2_nohook/f429_repro.elf
```
```
- 증상: seq 205 이후 HardFault 없음 (seq 680, 약 8분까지 확인). JSON 응답 줄만 사라지고 [MEM] 로그는 계속 출력
- 근거: [MEM] seq=675/680 heapFree=160 heapMinEver=160 (고정, 더 이상 할당 성공 없음)
        Fake_Send(NULL, n) → HAL_UART_Transmit이 pData == NULL이면 HAL_ERROR로 즉시 반환 → 아무것도 전송 안 함
        Fake_Send가 HAL 반환값을 검사하지 않으므로 실패가 어디에도 기록되지 않음
- 0x0 쓰기: Renode에서 0x0 = 0x00000000, 0xC = 0x00000000, 0x08000000 = 0x20030000 (벡터 테이블 정상)
        → Renode의 stm32f429.repl에서는 0x0이 Flash 별칭이 아니어서 sprintf(NULL)의 쓰기가 반영되지 않음
- 원인: 할당 실패를 알리는 장치(훅, NULL 검사, 전송 반환값 검사)가 모두 없어서, 메모리 고갈이 "응답 없음"으로만 드러남
```
**의미**: 같은 누수라도 훅이 있으면 즉시 멈추고(원인이 바로 보임), 훅이 없으면 폴트 없이 응답만 사라집니다(통신 장애처럼 보임). 원인과 증상이 서로 다른 곳에서 나타나므로 추적이 어려워집니다.

**실제 칩과의 차이 (가정)**: F429는 BOOT0=0일 때 0x0이 Flash(0x08000000)의 별칭입니다. 실제 칩에서 `sprintf(NULL)`은 잠긴 Flash에 쓰는 동작이 되며, 무시되는지 BusFault가 나는지는 칩에서 확인이 필요합니다. Renode 결과만으로 실제 칩에서도 폴트가 나지 않는다고 결론 내리면 안 됩니다.

### MODE 3 (BUFFER) — 재현 결과
```
- 환경: Renode v1.16.1, stm32f429.repl, FreeRTOS V10.6.2, 태스크 스택 256 words / FreeRTOS 힙 15360 B
- 증상: seq 9회차에 UsageFault. [BUF] len=171 / 160, on_sent 0x08005581 → 0x7563222C
- 근거: IPSR=6 (UsageFault), CFSR=0x00020000 (INVSTATE), HFSR=0, EXC_RETURN=0xFFFFFFED (태스크/PSP, FPU 프레임)
        stacked PC=0x0800563E, LR=0x08005641 → addr2line: Build_And_Send repro.c:161 g_tx.on_sent(); (LR → repro.c:162, 호출 다음 위치)
        map: g_tx@0x200001C8(164B) 바로 뒤 g_status@0x2000026C → 넘친 8B가 temp_c/keg_kg까지 손상
- 원인: 응답 문자열을 길이 검사 없이 strcat으로 누적 → 160B 버퍼 초과 → 뒤의 함수 포인터와 인접 전역 변수 덮어쓰기
- 수정: (MODE 4) 정적 버퍼 + snprintf 반환값으로 길이 검사, 누적하지 않음
- 결과: MODE 4로 seq 745까지 폴트 0건, heapFree == heapMinEver (동적 할당 0)
```

**UART 로그 (발췌)**: 매 요청마다 문자열이 19B씩 쌓이다가, 9번째에 160B를 넘으면서 포인터 값이 바뀝니다.
```
[BUF] len=133 / 160  on_sent=0x8005581
[BUF] len=152 / 160  on_sent=0x8005581
[BUF] len=171 / 160  on_sent=0x7563222c      ← 버퍼 초과, 함수 포인터가 문자열로 덮임
...
========== HARD FAULT ==========
  stacked PC  = 0x0800563E
  stacked LR  = 0x08005641
  stacked R0  = 0x00000000
  xPSR        = 0x61000000
  EXC_RETURN  = 0xFFFFFFED
  IPSR        = 0x00000006
  HFSR        = 0x00000000
  CFSR        = 0x00020000
  -> INVSTATE: Thumb 비트 0인 주소로 분기 (덮어쓴 함수 포인터 의심)
================================
```

**덮어쓴 값 해석**
| 버퍼 오프셋 | 메모리 | 덮인 내용 |
|---|---|---|
| 0~159 | `g_tx.tx[160]` | 누적 JSON (정상 범위) |
| 160~163 | `g_tx.on_sent` @0x20000268 | `,"cu` → 리틀 엔디언 `0x7563222C`. bit0=0이라 Thumb 상태가 아님 |
| 164~171 | `g_status.temp_c`, `keg_kg` @0x2000026C | `ps":3},\0` → 옆 전역 변수 손상 |

`On_Sent`의 실제 주소는 0x08005580이고, 포인터에는 Thumb 비트를 더한 0x08005581이 저장됩니다. Cortex-M은 Thumb 명령만 실행하므로, bit0이 0인 주소로 `blx`하면 INVSTATE 폴트가 납니다.

**폴트 위치 추적**: stacked PC 0x0800563E는 map에서 `Build_And_Send`(0x080055FC~0x08005653) 안에 있습니다. LR이 PC+2에 Thumb 비트를 더한 0x08005641이므로, 0x0800563E의 2바이트 `blx` 명령이 덮인 `on_sent`를 호출한 지점입니다.
```bash
arm-none-eabi-addr2line -e build/mode3/f429_repro.elf -f -C 0x0800563E
# Build_And_Send
# .../f429_repro/Core/Src/repro.c:161     ← g_tx.on_sent();
arm-none-eabi-addr2line -e build/mode3/f429_repro.elf -f -C 0x08005641
# Build_And_Send
# .../f429_repro/Core/Src/repro.c:162     ← 함수 끝 }  (blx 다음 명령 = 복귀 위치)
```
빌드 폴더를 지우고 다시 빌드한 뒤 실행해도 레지스터 값이 모두 같았습니다(stacked PC/LR, IPSR, CFSR). 같은 입력이면 같은 지점에서 폴트가 나는 결정적인 결함입니다.

줄 번호는 현재 소스 기준입니다. 측정할 때는 MODE 5·6을 추가하기 전이라 156·157번째 줄이었고, 코드가 추가되며 161·162로 밀렸습니다. MODE 3의 실행 코드는 바뀌지 않았습니다.

**Renode와 실제 칩의 차이 (실제 칩 쪽은 아키텍처 문서 기준 예상)**
| 항목 | Renode (측정) | 실제 칩 (예상) |
|---|---|---|
| 벡터 | UsageFault로 바로 감 (IPSR=6, HFSR=0) | UsageFault가 꺼져 있으면 HardFault로 승격 (IPSR=3, HFSR.FORCED=1) |
| stacked PC | 분기 명령 주소 0x0800563E | 분기 대상 주소 0x7563222C |
| CFSR | INVSTATE | INVSTATE |

처음에는 Renode가 HardFault 핸들러만 연결된 상태에서 `Default_Handler`(무한 루프)로 빠져 아무것도 출력되지 않았습니다. `cpu PC`=0x08006E4C(`Default_Handler`), LR=0xFFFFFFED(예외 처리 중)로 이 상태를 확인했고, 네 폴트 벡터를 모두 보고 루틴에 연결해 해결했습니다.

### MODE 5 (NEWLIB_LEAK) — 재현 결과
- 환경: Renode v1.16.1, `build/mode5`, 태스크 스택 256 words / FreeRTOS 힙 15360 B
- 응답: 장치 3종(TAP·CUP·SENSOR)을 중첩 객체로 묶은 상태 응답입니다. 실제 장비의 상태 응답과 같은 구조로 만들고 키 이름만 일반화했습니다. 들여쓰기 없는 한 줄이고 약 490B입니다. 1초마다 `malloc`으로 만들어 보내고 **free하지 않습니다**.
- 증상: **seq 356에서 `malloc`이 NULL을 반환했습니다.** HardFault는 나지 않았고, 그 뒤로 **응답 줄이 사라지고 `[MEM]` 로그만** 나왔습니다(seq 370 이후까지 확인).

| seq | FreeRTOS heapFree | newlib used | newlib limit |
|---|---|---|---|
| 110 | 13616 | 55,440 | 179,000 |
| 210 | 13616 | 105,840 | 179,000 |
| 355 | 13616 | 178,896 | 179,000 |
| 356 | 13616 | **`malloc returned NULL`** | |
| 370 | 13616 | 178,896 (멈춤) | 179,000 |

- 근거
  - 100초 동안 50,400B 증가 = **요청당 504B**입니다. 응답 약 494B(NUL 포함)에 malloc의 정렬과 헤더가 더해진 값입니다. 헤더와 정렬로 나눈 내역은 가정입니다.
  - 남은 공간 179,000 − 178,896 = 104B < 504B라서 다음 할당이 실패했습니다. 조각나서가 아니라 **공간이 다 차서** 실패한 것입니다.
  - **FreeRTOS heapFree는 13616에서 한 번도 변하지 않았습니다.** `xPortGetFreeHeapSize()`로 힙을 감시해도 이 누수는 보이지 않습니다.
- NULL 이후: `strlen(NULL)`이 0x0을 읽습니다. Renode는 0x0을 0으로 읽으므로 길이가 0이 되어 아무것도 보내지 않습니다. 실제 칩은 0x0이 Flash 별칭이라 다른 결과가 나올 수 있습니다(가정).
- 참고: NULL 이후 `stackFreeMin`이 104에서 82 words로 줄었습니다. 할당 실패 경로가 스택을 더 쓰는 것으로 보이지만 원인은 확인하지 않았습니다.
- 앞선 버전(응답 약 110B, 들여쓰기 형식)에서는 seq 75에 used=8,960(요청당 약 120B)이었습니다. 응답이 클수록 정확히 그만큼 빨리 샙니다.
- 원인: 응답 문자열을 받는 쪽이 free를 하지 않았고, 누수가 FreeRTOS 힙이 아니라 newlib 힙에서 일어나 감시에 걸리지 않았습니다.
- 수정: 응답을 정적 버퍼에 `snprintf`로 만들거나(MODE 4), 받은 쪽이 보낸 직후 반드시 free합니다. 할당 결과는 NULL 검사합니다.

### MODE 6 (COPY_OVERFLOW) — 재현 결과
- 환경: Renode v1.16.1, `build/mode6`, `mode6_stack512`, `mode6_stack1024`
- 동작: 응답을 `malloc`으로 만든 뒤 지역 변수 `SendMsg_t msg`(`char data[256]` + `int`)에 길이 검사 없이 `strcpy`합니다. 5번째 요청마다 센서 상태 응답(들여쓰기 포함 371B)을 만듭니다. free는 제대로 하므로 누수는 없습니다.
- 증상: seq 5에서 `[COPY] seq=5 len=371 / 256`이 출력되고 응답이 전송된 뒤, **함수가 돌아가는 순간 UsageFault**가 났습니다.

| 빌드 | 태스크 스택 | stacked PC | stacked LR | IPSR | CFSR |
|---|---|---|---|---|---|
| `mode6` | 256 words (1KB) | 0x0800581C | 0x080061CF | 6 | 0x00020000 (INVSTATE) |
| `mode6_stack512` | 512 words (2KB) | 0x0800581C | 0x080061CF | 6 | 0x00020000 (INVSTATE) |
| `mode6_stack1024` | 1024 words (4KB) | 0x0800581C | 0x080061CF | 6 | 0x00020000 (INVSTATE) |

- 근거
  - map 파일에서 `Build_And_Send`는 0x080057C0~0x0800582B(0x6C)입니다. stacked PC 0x0800581C는 그 안 +0x5C로, **함수 끝의 복귀 부분**입니다.
  - stacked LR(0x080061CF)은 `HAL_UART_Transmit` 안을 가리킵니다. 직전 전송 호출에서 남은 값이라 판단 근거로 쓰지 않았습니다.
  - `repro.c.su`에 따르면 `Build_And_Send`의 스택 프레임은 280B입니다. 그런데 371B + NUL = 372B를 썼으므로, `msg.data`를 116B 넘어 **프레임 전체(저장된 레지스터와 복귀 주소)**를 덮었습니다.
  - 복귀 주소 자리에 문자열 일부가 들어갔고, 그 값의 bit0이 0이라 INVSTATE가 났습니다. 정확히 어느 글자가 들어갔는지는 디스어셈블리로 확인하지 않았습니다(가정).
- **스택을 키워도 결과가 같은 이유**: `strcpy`는 `msg.data`에서 높은 주소 쪽, 즉 이미 쌓여 있는 복귀 주소 쪽으로 씁니다. 스택을 키우면 늘어나는 곳은 그 반대쪽인 낮은 주소의 빈 공간입니다. 그래서 덮이는 자리와 값이 그대로이고, 레지스터 값까지 똑같이 나옵니다.
```
 높은 주소   ┌───────────────────────┐
             │ 저장된 레지스터, 복귀 주소 │ ← 372B를 쓰면 여기까지 덮임
             │ msg.port_index        │
             │ msg.data[256]         │ ← strcpy 시작
 낮은 주소   │ (남는 스택)            │ ← 스택을 키우면 늘어나는 곳
             └───────────────────────┘
```
- 같은 결함, 다른 증상: 앞선 버전(10번째 요청마다 다른 모양의 상세 응답)에서는 폴트 보고 없이 Renode가 `CPU abort: Trying to execute code outside RAM or ROM at 0x226C6D5E`로 멈췄습니다.
  - 0x226C6D5F는 문자열 `_ml"`의 바이트입니다. bit0이 1이라 Thumb 주소로는 유효했고, 그래서 INVSTATE 대신 메모리 밖 주소로 실제로 점프한 것으로 해석합니다(가정).
  - 즉 **어떤 글자가 복귀 주소 자리에 오느냐에 따라 증상이 달라집니다.** 실제 칩이라면 UsageFault, 버스 폴트, 엉뚱한 코드 실행이 섞여 불규칙한 하드폴트로 보였을 수 있습니다(가정).
- 원인: 고정 크기 메시지에 길이 검사 없이 문자열을 복사했습니다.
- 수정: `snprintf(msg.data, sizeof(msg.data), "%s", response)` 또는 복사 전에 길이를 검사합니다. 근본적으로는 응답 크기를 빌드 시점에 정하고, 그보다 큰 응답은 만들지 않습니다.

### 크기 대조 실험 — "크기만 늘리면 해결되나?"
| 실험 | 기본 크기 결과 | 크기를 키운 결과 | 해석 |
|---|---|---|---|
| MODE 1, 스택 1KB → 2KB (`STACK=512`) | seq 1에 커널 assert 실패 | **멈춤 없음** (seq 85 이후까지). 시작 시 stackFreeMin 372 → 이후 118 words | 최대 사용량 394 words = 1,576B > 1KB. 스택 **깊이** 문제라서 키우면 해결됨 |
| MODE 2, FreeRTOS 힙 15KB → 30KB (`HEAP=30720`) | seq 205에 MALLOC FAILED | **seq 437에 MALLOC FAILED** (heapFree=0xD0=208B) | 28,984 − 109회 × 264B = 208B. 누수는 그대로고 **고장 시점만 2배 늦어짐** |
| MODE 3, 스택 1KB → 2KB + 힙 15KB → 30KB (`STACK=512 HEAP=30720`) | seq 9에 UsageFault | **똑같이 seq 9, `on_sent`=0x7563222C, PC=0x0800563E, LR=0x08005641, CFSR=INVSTATE** | 넘치는 곳이 정적 구조체(`.data`)라 스택·힙 크기와 무관 |
| MODE 5, FreeRTOS 힙 15KB → 30KB (`HEAP=30720`) | seq 356에 newlib malloc NULL (limit 179,000B) | **newlib limit 163,640B로 줄어듦**, used 163,296B에서 멈춤. heapFree 28,976B는 계속 그대로 | FreeRTOS 힙을 늘린 만큼(15,360B) newlib 힙이 줄어 **seq 325에 malloc NULL → 오히려 31회 빨리 고장**. 163,296 ÷ 504 = 324회 성공 후 325번째 실패로 계산과 일치 |
| MODE 6, 스택 1KB → 2KB → 4KB | seq 5에 UsageFault | **똑같이 seq 5, 레지스터 값까지 동일** | 버퍼 **넘침**이라 스택 크기와 무관 |

크기를 키워서 해결되는 경우는 MODE 1(스택 깊이)뿐이었습니다. 누수(MODE 2)는 고장을 늦출 뿐이고, 버퍼 넘침(MODE 3·6)에는 전혀 효과가 없었습니다. newlib 힙 누수(MODE 5)는 FreeRTOS 힙을 늘리면 **오히려 고장이 앞당겨졌습니다**. 두 힙이 같은 RAM을 나눠 쓰기 때문입니다.

## 7. 결론

| MODE | 결과 | 문제가 드러난 위치 | 원인이 있던 위치 |
|---|---|---|---|
| 1 STACK | seq 1에 커널 assert 실패 | `vTaskDelay` (커널) | `Build_And_Send`의 큰 지역 버퍼 + float printf |
| 2 HEAP | seq 205에 MALLOC FAILED (훅 없으면 응답만 사라짐) | 할당 실패 시점 / 통신 | 전송 실패 경로의 free 누락 |
| 3 BUFFER | seq 9에 UsageFault (INVSTATE) | 덮인 함수 포인터 호출 | 길이 검사 없는 strcat 누적 |
| 4 FIXED | seq 745까지 폴트 0건 | — | — |
| 5 NEWLIB_LEAK | seq 356에 newlib malloc NULL → 응답만 사라짐 | 통신 (응답 없음) | 응답 문자열 free 누락 (FreeRTOS 힙 감시에 안 보임) |
| 6 COPY_OVERFLOW | seq 5에 UsageFault (INVSTATE), 스택 4배여도 동일 | 함수 복귀 | 256B 메시지에 길이 검사 없는 strcpy |

**결함들의 공통점**
- 결함이 있는 코드와 문제가 드러난 코드가 서로 달랐습니다. MODE 1은 커널에서, MODE 3은 함수 포인터 호출에서, MODE 6은 함수 복귀에서, MODE 2(훅 없음)와 MODE 5는 통신 쪽(응답 없음)에서 증상이 나타났습니다.
- 크기를 늘려서 해결된 것은 스택 깊이 문제(MODE 1)뿐이었습니다. 누수는 고장 시점만 늦어지고(MODE 2: seq 205 → 437), 버퍼 넘침은 전혀 바뀌지 않았습니다(MODE 3: 스택·힙 2배, MODE 6: 스택 4배에도 레지스터 값까지 동일). 자세한 결과는 [크기 대조 실험](#크기-대조-실험--크기만-늘리면-해결되나)에 있습니다.
- 원인을 확정한 근거는 크기 조정이 아니라 레지스터(CFSR, IPSR, stacked PC/LR)와 메모리 덤프, map 파일이었습니다.

**수정 원칙 (MODE 4)**
- 응답 버퍼는 정적으로 한 번만 잡고, 크기를 빌드 시점에 고정합니다.
- `snprintf` 반환값으로 길이를 검사합니다.
- float printf 대신 정수 스케일 포맷을 써서 스택 사용량을 줄입니다.
- 응답마다 동적 할당을 하지 않습니다.

**Renode 사용 시 주의**
- CFSR은 실제로 채워지지만, UsageFault 같은 개별 폴트를 HardFault로 승격하지 않습니다. 모든 폴트 벡터를 보고 루틴에 연결해야 합니다.
- 0x0이 Flash 별칭이 아니므로, NULL 포인터 쓰기 결과는 실제 칩과 다를 수 있습니다.

## 8. 한계와 남은 과제
- **에뮬레이터 결과입니다.** 실제 칩과 다를 수 있는 부분(폴트 승격 방식, stacked PC 값, 0x0 별칭)은 본문에 "(가정)"으로 표시했습니다. 실보드(예: Nucleo-F429ZI) 확인은 하지 않았습니다.
- **이더넷(LwIP)은 범위에서 뺐습니다.** 요청·응답 경로는 UART로 대신했습니다.
- **MODE 4의 온도 표시 반올림**: `(int)(temp_c * 100)`이 소수점을 버리므로 4.20이 `419`로 출력되는 경우가 있습니다(float 4.2는 4.1999…로 저장됨). 메모리 문제와는 관계없지만 고칠 대상입니다.
- **MODE 6에서 복귀 주소 자리에 들어간 글자**는 디스어셈블리(`arm-none-eabi-objdump -d`)로 확인하지 않았습니다.
- 여러 태스크가 락 없이 newlib `malloc`을 동시에 호출하는 시나리오는 재현하지 않았습니다.
- **빌드**: Makefile은 헤더 의존성을 추적하지 않습니다. 헤더를 고치면 `make clean` 후 다시 빌드해야 합니다.
