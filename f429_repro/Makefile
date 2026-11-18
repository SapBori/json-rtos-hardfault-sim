# =====================================================================
# f429_repro/Makefile
#   STM32CubeIDE 없이 터미널에서 STM32F429 + FreeRTOS 재현 펌웨어를 빌드한다.
#
# 사용법
#   make MODE=4        -> build/mode4/f429_repro.elf  (MODE: 1~4, 생략 시 4)
#   make MODE=1        -> build/mode1/f429_repro.elf  (모드별 폴더라 clean 없이 전환 가능)
#   make MODE=2 HOOK=0 -> build/mode2_nohook/f429_repro.elf (malloc 실패 훅 끔: NULL로 계속 실행)
#   make MODE=1 ASSERT=0 -> build/mode1_noassert/f429_repro.elf (configASSERT 끔: 커널 검사 없이 실행)
#   make MODE=1 STACK=512 -> build/mode1_stack512/ (태스크 스택 512 words = 2KB)
#   make MODE=5 HEAP=30720 -> build/mode5_heap30720/ (FreeRTOS 힙 30720B)
#   make clean         -> build/ 폴더 전체 삭제
#
# 문법 메모
#   - "변수 := 값"  : 즉시 대입. 아래에서 $(변수)로 꺼내 쓴다.
#   - "변수 ?= 값"  : 명령줄에서 값을 안 줬을 때만 대입 (기본값).
#   - "대상: 재료"  : 규칙. 재료가 대상보다 새로우면 아래 명령을 실행한다.
#   - 명령 줄은 반드시 '탭'으로 들여써야 한다 (스페이스면 missing separator 에러).
#   - 변수 줄 끝에 주석을 붙이면 앞의 공백까지 값에 들어가므로, 주석은 항상 윗줄에 쓴다.
# =====================================================================

# ---------------------------------------------------------------------
# 1. 빌드 설정
# ---------------------------------------------------------------------
# 재현 모드 (1 STACK / 2 HEAP / 3 BUFFER / 4 FIXED / 5 NEWLIB_LEAK / 6 COPY_OVERFLOW). make MODE=3 처럼 덮어쓴다.
MODE   ?= 4
# [메모리] FreeRTOS malloc 실패 훅 (1 = 켬, 0 = 끔). FreeRTOSConfig.h의 configUSE_MALLOC_FAILED_HOOK로 전달.
#   0이면 pvPortMalloc이 NULL을 반환한 뒤에도 프로그램이 계속 돈다 (훅이 꺼져 있던 실보드 원본과 같은 조건)
HOOK   ?= 1
# FreeRTOS configASSERT (1 = 켬, 0 = 끔). 0이면 커널이 깨진 데이터를 검사하지 않고 그대로 사용한다
#   (MODE 1에서 assert 대신 어디서 폴트가 나는지 확인용)
ASSERT ?= 1
# [메모리] 크기 대조 실험용. 비워 두면 기본값(스택 256 words, FreeRTOS 힙 15360B)
#   STACK = StatusTask 스택 (워드 단위, x4 = 바이트) -> repro.h의 REPRO_TASK_STACK_WORDS
#   HEAP  = FreeRTOS 힙 (바이트)                   -> FreeRTOSConfig.h의 REPRO_HEAP_SIZE
STACK  ?=
HEAP   ?=
# 결과물 이름 (f429_repro.elf, f429_repro.map)
TARGET := f429_repro
# 모드마다 출력 폴더를 나눈다. 같은 폴더를 쓰면 MODE만 바꿨을 때
# 소스가 안 바뀌어 재컴파일이 안 되고, 이전 모드의 .o가 섞여 들어간다.
# HOOK=0, ASSERT=0, STACK, HEAP도 같은 이유로 폴더 이름에 붙인다 (_nohook, _noassert, _stackN, _heapN).
BUILD  := build/mode$(MODE)$(if $(filter 0,$(HOOK)),_nohook)$(if $(filter 0,$(ASSERT)),_noassert)$(if $(STACK),_stack$(STACK))$(if $(HEAP),_heap$(HEAP))
# STACK/HEAP을 줬을 때만 해당 -D 옵션을 만든다 (안 주면 헤더의 기본값 사용)
SIZE_DEFS := $(if $(STACK),-DREPRO_TASK_STACK_WORDS=$(STACK)U) $(if $(HEAP),-DREPRO_HEAP_SIZE=$(HEAP))

# ---------------------------------------------------------------------
# 2. 외부 라이브러리 경로 (third_party/ 에 git clone 한 공개 저장소)
# ---------------------------------------------------------------------
TP   := third_party
# ST HAL 드라이버: HAL_UART_Transmit, HAL_RCC_* 등
HAL  := $(TP)/stm32f4xx_hal_driver
# F429 레지스터 정의(stm32f429xx.h), 스타트업 코드, SystemInit
DEV  := $(TP)/cmsis_device_f4
# Cortex-M4 코어 헤더(core_cm4.h): SCB->CFSR, __BKPT 등 fault.c가 쓰는 것
CORE := $(TP)/CMSIS_5/CMSIS/Core/Include
# FreeRTOS 커널
RTOS := $(TP)/FreeRTOS-Kernel

# ---------------------------------------------------------------------
# 3. 툴체인 (호스트 gcc가 아니라 ARM용 크로스 컴파일러)
# ---------------------------------------------------------------------
# 컴파일과 링크를 모두 gcc 드라이버로 처리한다
CC   := arm-none-eabi-gcc
# 빌드 후 Flash/RAM 사용량 출력용
SIZE := arm-none-eabi-size

# ---------------------------------------------------------------------
# 4. 컴파일할 소스 목록
#    CubeIDE가 자동으로 해 주던 "어떤 파일을 빌드할지"를 직접 적는 부분.
#    여기 없는 .c는 빌드되지 않으므로, 링크 때 undefined reference가 나면
#    해당 함수가 들어 있는 .c를 이 목록에 추가하면 된다.
# ---------------------------------------------------------------------
C_SRCS := \
    Core/Src/main.c \
    Core/Src/repro.c \
    Core/Src/fault.c \
    Core/Src/sysmem.c \
    $(DEV)/Source/Templates/system_stm32f4xx.c \
    $(HAL)/Src/stm32f4xx_hal.c \
    $(HAL)/Src/stm32f4xx_hal_cortex.c \
    $(HAL)/Src/stm32f4xx_hal_rcc.c \
    $(HAL)/Src/stm32f4xx_hal_gpio.c \
    $(HAL)/Src/stm32f4xx_hal_uart.c \
    $(HAL)/Src/stm32f4xx_hal_dma.c \
    $(RTOS)/tasks.c \
    $(RTOS)/queue.c \
    $(RTOS)/list.c \
    $(RTOS)/timers.c \
    $(RTOS)/portable/GCC/ARM_CM4F/port.c \
    $(RTOS)/portable/MemMang/heap_4.c
# 위 목록 설명
#   main.c/repro.c/fault.c : 직접 작성한 코드 (UART 초기화, 재현 태스크, 폴트 핸들러)
#   sysmem.c               : newlib malloc용 _sbrk() - 상한 검사 포함 (FreeRTOS 힙과 별개)
#   system_stm32f4xx.c     : 리셋 직후 SystemInit() - FPU 활성화, 클럭 기본값
#   stm32f4xx_hal*.c       : HAL 공통/NVIC/클럭/GPIO/UART. uart.c가 DMA 함수를 참조해서 dma.c도 필요
#                            (HAL 틱은 TIM 대신 FreeRTOS 틱 훅에서 올리므로 hal_tim.c 불필요)
#   tasks/queue/list/timers: FreeRTOS 커널 본체 (configUSE_TIMERS=0이라 timers.c는 거의 비어 있음)
#   ARM_CM4F/port.c        : Cortex-M4 + FPU용 컨텍스트 스위칭 (PendSV/SVC/SysTick)
#   heap_4.c               : pvPortMalloc/vPortFree 구현 (MODE 2의 힙 고갈 실험 대상)

# 어셈블리 소스: 벡터 테이블과 Reset_Handler(.data 복사, .bss 0 초기화 후 main 호출).
# 이게 빠지면 링크는 되더라도 벡터 테이블이 없어 CPU가 시작조차 못 한다.
ASM_SRCS := $(DEV)/Source/Templates/gcc/startup_stm32f429xx.s

# ---------------------------------------------------------------------
# 5. 헤더 검색 경로 (-I)
#    #include "xxx.h" 를 찾을 폴더 목록. Core/Inc 에는 main.h, repro.h,
#    FreeRTOSConfig.h, stm32f4xx_hal_conf.h 가 들어간다.
# ---------------------------------------------------------------------
INC := -ICore/Inc \
       -I$(HAL)/Inc \
       -I$(DEV)/Include \
       -I$(CORE) \
       -I$(RTOS)/include \
       -I$(RTOS)/portable/GCC/ARM_CM4F

# ---------------------------------------------------------------------
# 6. CPU 옵션
#    -mcpu=cortex-m4        : F429 코어
#    -mthumb                : Cortex-M은 Thumb 명령만 실행 (함수 주소 bit0=1의 이유)
#    -mfpu=fpv4-sp-d16      : F429의 단정밀도 FPU
#    -mfloat-abi=hard       : float 인자를 FPU 레지스터로 전달. ARM_CM4F 포트와 짝이 맞아야 함
#    컴파일, 어셈블, 링크 모두에 같은 값을 줘야 한다 (다르면 링크 에러).
# ---------------------------------------------------------------------
MCU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# ---------------------------------------------------------------------
# 7. 컴파일 옵션
#    -DSTM32F429xx          : stm32f4xx.h가 F429 레지스터 정의를 고르게 함
#    -DUSE_HAL_DRIVER       : HAL 헤더(stm32f4xx_hal_conf.h)를 포함하게 함
#    -DREPRO_MODE=$(MODE)   : repro.c의 #if REPRO_MODE 분기 선택 (CubeIDE 전처리기 설정 대신)
#    -DREPRO_MALLOC_HOOK=$(HOOK) : FreeRTOSConfig.h의 malloc 실패 훅 켜기/끄기
#    -DREPRO_ASSERT=$(ASSERT)    : FreeRTOSConfig.h의 configASSERT 켜기/끄기
#    $(SIZE_DEFS)                : STACK/HEAP 대조 실험 크기 (주지 않으면 빈 값)
#    -Og -g3                : 디버깅하기 좋은 최적화 + 매크로까지 포함한 디버그 정보
#                             (GDB bt, addr2line이 정확한 줄을 가리키게 함)
#    -Wall -Wextra          : 경고 최대한 켜기
#    -ffunction-sections -fdata-sections
#                           : 함수/변수마다 섹션을 나눠, 링크 때 안 쓰는 것을 지울 수 있게 함
#    -fstack-usage          : 함수별 스택 사용량을 .su 파일로 출력
#                             (build/modeN/Core/Src/repro.c.su -> MODE 1 오버플로의 수치 근거)
# ---------------------------------------------------------------------
CFLAGS := $(MCU) -DSTM32F429xx -DUSE_HAL_DRIVER -DREPRO_MODE=$(MODE) -DREPRO_MALLOC_HOOK=$(HOOK) -DREPRO_ASSERT=$(ASSERT) $(SIZE_DEFS) $(INC) \
          -Og -g3 -Wall -Wextra -ffunction-sections -fdata-sections \
          -fstack-usage

# ---------------------------------------------------------------------
# 8. 링크 옵션
#    -TSTM32F429ZI_FLASH.ld : 링커 스크립트. Flash(0x08000000)/RAM(0x20000000) 배치를 정함
#    --specs=nano.specs     : 작은 C 라이브러리(newlib-nano) 사용
#    --specs=nosys.specs    : OS 없는 환경용 시스템 호출 스텁(_sbrk, _write 등)
#    -u _printf_float       : nano에서 printf("%f") 지원 켜기 (MODE 1에서 필요)
#                             CubeIDE "Use float with printf from newlib-nano"와 같음
#    -Wl,--gc-sections      : 위 7번에서 나눈 섹션 중 안 쓰는 것 제거
#    -Wl,-Map=...           : 맵 파일 생성. 변수·함수가 어느 주소에 몇 바이트로
#                             배치됐는지 확인 (MODE 3의 g_tx 뒤에 뭐가 있는지 등)
# ---------------------------------------------------------------------
LDFLAGS := $(MCU) -TSTM32F429ZI_FLASH.ld --specs=nano.specs --specs=nosys.specs \
           -u _printf_float -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map

# ---------------------------------------------------------------------
# 9. 오브젝트 파일 목록
#    소스 경로 앞에 build/modeN/ 을 붙이고 뒤에 .o 를 붙인다.
#    예) Core/Src/repro.c -> build/mode4/Core/Src/repro.c.o
#    (.c.o 처럼 원래 확장자를 남겨 .c와 .s의 이름이 겹치지 않게 함)
# ---------------------------------------------------------------------
OBJS := $(patsubst %,$(BUILD)/%.o,$(C_SRCS) $(ASM_SRCS))

# ---------------------------------------------------------------------
# 10. 빌드 규칙
#    $@ = 만들 대상(왼쪽), $< = 첫 번째 재료, $(dir $@) = 대상의 폴더 부분
# ---------------------------------------------------------------------

# 기본 대상: 그냥 'make' 하면 맨 처음 규칙인 all이 실행된다
all: $(BUILD)/$(TARGET).elf

# .c -> .o : 소스 하나씩 컴파일 (-c: 링크는 하지 않음)
# 출력 폴더가 없으면 gcc가 실패하므로 먼저 만든다 (@ = 명령 자체는 화면에 안 찍음)
$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# .s -> .o : 스타트업 어셈블리. C 옵션은 필요 없고 CPU 옵션만 맞춘다
$(BUILD)/%.s.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(MCU) -c $< -o $@

# 모든 .o -> .elf : 링크해서 최종 펌웨어 생성, 이어서 메모리 사용량 출력
#   text = Flash 코드, data = 초기값 있는 변수(Flash+RAM), bss = 0으로 시작하는 변수(RAM)
#   FreeRTOS 힙(ucHeap)도 bss에 포함된다
$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

# 빌드 결과 전체 삭제 (모든 모드)
clean:
	rm -rf build

# all, clean은 파일 이름이 아니라 명령 이름임을 표시
# (같은 이름의 파일이 생겨도 항상 실행되게 함)
.PHONY: all clean
