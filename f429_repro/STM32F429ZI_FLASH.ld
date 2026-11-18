/*
 * STM32F429ZI_FLASH.ld - 링커 스크립트 (f429_repro)
 *
 * 링커 스크립트는 "컴파일된 코드와 변수를 칩의 어느 주소에 놓을지"를 정한다.
 * 메모리 관점에서 RAM이 실제로 어떻게 나뉘는지가 전부 여기서 결정된다.
 *
 * RAM 192KB (0x20000000 ~ 0x2002FFFF) 배치 결과 (낮은 주소 -> 높은 주소)
 *
 *   0x20000000 +---------------------------+
 *              | .data  초기값 있는 전역변수  |  예) g_status, MODE 3의 g_tx
 *              +---------------------------+
 *              | .bss   0으로 시작하는 전역변수 |  예) huart1, MODE 4의 s_json,
 *              |                           |      FreeRTOS 힙 ucHeap[configTOTAL_HEAP_SIZE]
 *              |                           |      -> 태스크 스택·TCB·pvPortMalloc이 전부 이 안에서 할당됨
 *              +--------------------------- <- _end / end : newlib 힙 시작
 *              | newlib 힙 (malloc, sbrk)   |  위로 자람. printf(%f)의 내부 malloc, cJSON 등
 *              |            |              |
 *              |            v              |
 *              |      (빈 공간)             |
 *              |            ^              |
 *              |            |              |  <- _estack - _Min_Stack_Size : sbrk 상한 (sysmem.c)
 *              | MSP 스택 (main, 인터럽트,    |  아래로 자람
 *              |          폴트 핸들러)       |
 *   0x20030000 +--------------------------- <- _estack (리셋 시 SP 초기값)
 *
 * 핵심: 메모리 영역이 4개(전역 / FreeRTOS 힙 / newlib 힙 / MSP)로 나뉘고 서로 독립이다.
 *       FreeRTOS 힙을 늘리면(configTOTAL_HEAP_SIZE) .bss가 커져 newlib 힙 공간이 줄어든다.
 *       "힙을 늘렸는데 효과가 없다"면 늘린 힙과 실제로 고갈되는 힙이 다른 것일 수 있다.
 *
 * CCMRAM(64KB, 0x10000000)은 CPU만 접근 가능(DMA 불가)한 별도 RAM. 이 프로젝트에서는 쓰지 않는다.
 */

/* 리셋 후 첫 실행 함수 (startup_stm32f429xx.s). GDB/Renode가 ELF 시작점으로도 사용 */
ENTRY(Reset_Handler)

/* MSP 초기값 = RAM 맨 끝. 스택은 여기서 아래로 자란다 */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/*
 * 최소 확보량 (링크 시점 검사용)
 *   전역변수 + 이 두 값이 RAM에 안 들어가면 링크 에러("region RAM overflowed")가 난다.
 *   실제 크기 제한이 아니다:
 *     - newlib 힙은 sysmem.c의 _sbrk가 (_estack - _Min_Stack_Size)까지 계속 늘릴 수 있다
 *     - MSP는 이 값을 넘어도 하드웨어가 막지 않는다 (MPU 미사용). 넘으면 newlib 힙을 덮는다
 *   보드 원본 프로젝트(CubeIDE 기본값)와 같은 값으로 맞춤.
 */
_Min_Heap_Size  = 0x200;   /* newlib 힙 최소 512B */
_Min_Stack_Size = 0x400;   /* MSP 최소 1KB. 스케줄러 시작 후엔 인터럽트·폴트 핸들러만 사용 */

/* 칩의 물리 메모리 영역 (STM32F429ZI: Flash 2MB, SRAM1+2+3 연속 192KB) */
MEMORY
{
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 192K
  CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
}

SECTIONS
{
  /* 벡터 테이블: 0번 = 초기 SP(_estack), 1번 = Reset_Handler, 3번 = HardFault_Handler ...
     반드시 Flash 맨 앞. --gc-sections로 지워지지 않게 KEEP */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* 코드 (Flash). stacked PC가 이 범위(0x0800xxxx)면 정상 코드에서 폴트,
     벗어나면 덮어쓴 포인터로 엉뚱한 곳에 분기한 것 */
  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  /* 상수 (Flash): 문자열 리터럴, const 배열. RAM을 쓰지 않는다
     예) printf 포맷 문자열, fault.c의 hex[] 테이블 */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* 예외 처리/스택 되감기 정보 (C에서는 거의 비어 있음) */
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  /* 생성자 함수 목록. 스타트업의 __libc_init_array가 main 전에 호출 */
  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH

  /* .data의 초기값이 Flash에 저장된 위치. 스타트업이 여기서 RAM으로 복사한다 */
  _sidata = LOADADDR(.data);

  /* 초기값 있는 전역/static 변수 (RAM에서 실행, 초기값은 Flash에 보관: ">RAM AT> FLASH")
     _sdata ~ _edata 구간을 스타트업이 _sidata에서 복사해 채운다 */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  /* 초기값 없는(0) 전역/static 변수. 스타트업이 _sbss ~ _ebss를 0으로 채운다
     FreeRTOS 힙 배열 ucHeap도 여기 들어가므로 .bss 크기 대부분을 차지한다 */
  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* newlib 힙 시작점 표시 + 최소 힙/스택 공간 예약 (위 _Min_* 검사용)
     end/_end = .bss 바로 뒤 = sysmem.c의 _sbrk가 malloc 메모리를 내주기 시작하는 주소 */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* 위에서 배치되지 않은 표준 라이브러리 섹션은 버린다 (ST 기본 스크립트와 동일) */
  /DISCARD/ :
  {
    libc.a(*)
    libm.a(*)
    libgcc.a(*)
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
