/*
 * FreeRTOSConfig.h - FreeRTOS V10.6.2 설정 (f429_repro)
 *
 * 메모리 관련 설정을 한곳에 모았다. 재현 실험에서 바꿔 볼 값은 [메모리] 표시.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* C 파일에서만 보이는 선언 (어셈블리에서 include될 경우 대비) */
#ifndef __ASSEMBLER__
#include <stdint.h>
extern uint32_t SystemCoreClock;        /* system_stm32f4xx.c, 16MHz(HSI) */
void Fault_Puts(const char *s);         /* fault.c: configASSERT 메시지 출력용 */
void Fault_PutHex(const char *name, unsigned long v);
#endif

/* ---------------------------------------------------------------------
 * 스케줄러 기본
 * --------------------------------------------------------------------- */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)     /* 1틱 = 1ms */
#define configMAX_PRIORITIES                    7
#define configMAX_TASK_NAME_LEN                 16                     /* TCB 안에 이 크기의 배열이 들어감 */
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configUSE_MUTEXES                       0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_CO_ROUTINES                   0
#define configUSE_TIMERS                        0   /* 타이머 태스크 없음 -> 그 스택·큐만큼 힙 절약 */

/* ---------------------------------------------------------------------
 * [메모리] 힙 (heap_4.c)
 *   FreeRTOS 힙 = .bss 안의 정적 배열 ucHeap[configTOTAL_HEAP_SIZE].
 *   xTaskCreate의 스택·TCB, pvPortMalloc이 모두 여기서 나온다.
 *   newlib의 malloc()과는 완전히 별개 영역이다 (sysmem.c 참고).
 *   CubeMX 기본값 15360B로 맞춤. MODE 2의 고갈 시점이 이 값에 비례한다.
 *   make HEAP=30720 처럼 바꿔 "크기만 늘리면 해결되나" 대조 실험에 사용.
 *   늘리면 .bss가 커져 newlib 힙(malloc) 공간은 그만큼 줄어든다 (MODE 5에서 확인).
 * --------------------------------------------------------------------- */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#ifndef REPRO_HEAP_SIZE
#define REPRO_HEAP_SIZE                         15360
#endif
#define configTOTAL_HEAP_SIZE                   ((size_t)REPRO_HEAP_SIZE)

/* [메모리] 태스크 스택 크기의 단위는 바이트가 아니라 워드(4B).
   Idle 태스크 스택 = 128 x 4 = 512B */
#define configMINIMAL_STACK_SIZE                ((uint16_t)128)

/* ---------------------------------------------------------------------
 * [메모리] 감지 장치 (보드 원본 프로젝트에서는 둘 다 꺼져 있었음)
 * --------------------------------------------------------------------- */
/* 스택 오버플로 검사
 *   2 = 태스크 생성 시 스택을 0xA5로 채워 두고, 컨텍스트 전환 때마다
 *       스택 끝 16바이트가 그대로인지 확인 -> 깨졌으면 vApplicationStackOverflowHook()
 *   한계: 전환 시점에만 검사하므로, 넘친 직후 다른 메모리를 덮어 먼저 폴트가 나면 못 잡는다 */
#define configCHECK_FOR_STACK_OVERFLOW          2

/* pvPortMalloc()이 NULL을 반환하기 직전에 vApplicationMallocFailedHook() 호출
 *   make HOOK=0 이면 끔 -> 훅 없이 NULL이 그대로 호출한 쪽으로 돌아간다
 *   (실보드 원본처럼 "할당 실패를 아무도 모르는" 조건. MODE 2에서 NULL에 sprintf하는 경로 확인용) */
#ifndef REPRO_MALLOC_HOOK
#define REPRO_MALLOC_HOOK                       1
#endif
#define configUSE_MALLOC_FAILED_HOOK            REPRO_MALLOC_HOOK

/* newlib 재진입 구조체를 태스크마다 두지 않음.
 *   이 프로젝트는 newlib 함수(vsnprintf/sprintf와 그 안의 malloc)를 StatusTask 하나에서만
 *   부르므로 0으로 충분하다. 여러 태스크가 malloc/printf를 동시에 쓰면 1 + 락 구현이 필요
 *   (보드 원본은 여러 태스크에서 cJSON malloc을 쓰면서 0이었음 - 원인 후보 ③) */
#define configUSE_NEWLIB_REENTRANT              0

/* ---------------------------------------------------------------------
 * 훅
 * --------------------------------------------------------------------- */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     1   /* main.c: HAL_IncTick() 호출용 */

/* ---------------------------------------------------------------------
 * 사용할 API
 * --------------------------------------------------------------------- */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_uxTaskGetStackHighWaterMark     1   /* [메모리] [MEM] 로그의 stackFreeMin */
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    0

/* ---------------------------------------------------------------------
 * Cortex-M4 인터럽트 우선순위 (STM32F4는 우선순위 비트 4개)
 *   숫자가 작을수록 높은 우선순위.
 *   5보다 높은(0~4) 인터럽트에서는 FreeRTOS API(...FromISR 포함)를 부르면 안 된다.
 * --------------------------------------------------------------------- */
#define configPRIO_BITS                                 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5
#define configKERNEL_INTERRUPT_PRIORITY      (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* 커널이 이상(설정 실수, 깨진 리스트, NULL 핸들 등)을 발견하면 위치를 찍고 정지
 *   [메모리] 스택 오버플로로 커널 변수(태스크 리스트 등)가 덮이면 여기서 먼저 걸릴 수 있다.
 *   출력된 파일과 줄 번호로 어느 검사가 실패했는지 확인 (줄 번호는 16진수로 출력됨)
 *   make ASSERT=0 이면 정의하지 않음 -> FreeRTOS 기본값(검사 없음)으로 동작 */
#ifndef REPRO_ASSERT
#define REPRO_ASSERT 1
#endif
#if REPRO_ASSERT
#define configASSERT(x) \
    do { if ((x) == 0) { taskDISABLE_INTERRUPTS(); \
                         Fault_Puts("\r\n!!! configASSERT failed: "); \
                         Fault_Puts(__FILE__); Fault_Puts("\r\n"); \
                         Fault_PutHex("line", (unsigned long)__LINE__); \
                         for (;;) { } } } while (0)
#endif

/* ---------------------------------------------------------------------
 * FreeRTOS 포트의 핸들러를 벡터 테이블 이름에 연결
 *   (CubeMX는 stm32f4xx_it.c에서 하던 일을 이름 치환으로 처리)
 *   HardFault_Handler는 fault.c에 있다.
 * --------------------------------------------------------------------- */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
