/*
 * repro.h - STM32F429 + FreeRTOS JSON/메모리 하드폴트 재현 실험
 *
 * REPRO_MODE 값을 바꿔 빌드하면서 가설을 하나씩 검증합니다.
 *   1 = STACK  : 작은 태스크 스택 + 큰 지역 버퍼 + sprintf(%f)  -> 스택 오버플로
 *   2 = HEAP   : 응답마다 malloc, 전송 실패 경로에서 free 누락   -> 힙 고갈
 *   3 = BUFFER : 고정 버퍼에 strcat 누적, 길이 검사 없음         -> 옆 메모리(함수 포인터) 덮어쓰기
 *   4 = FIXED  : 정적 버퍼 + snprintf 길이 검사 + 정수 포맷      -> 수정본(대조군)
 *   5 = NEWLIB_LEAK   : 응답 문자열을 newlib malloc으로 만들고 free 안 함 -> newlib 힙 고갈
 *                       (FreeRTOS 힙 잔량에는 안 보임)
 *   6 = COPY_OVERFLOW : 가끔 긴 응답을 256B 지역 버퍼에 strcpy          -> 복귀 주소 덮어쓰기
 *   5·6은 "malloc으로 만든 응답 문자열을 돌려받아 고정 크기 메시지에 복사"하던 당시 구조의 패턴
 */
#ifndef REPRO_H
#define REPRO_H

#ifndef REPRO_MODE
#define REPRO_MODE 1
#endif

#define REPRO_PERIOD_MS        1000U   /* 당시와 같은 1초 상태 요청 주기 */
/* [메모리] 단위는 워드(4B) -> 1KB. 당시처럼 작게 잡음 (MODE 1의 핵심 조건)
   make STACK=512 처럼 바꿔 "크기만 늘리면 해결되나" 대조 실험에 사용 */
#ifndef REPRO_TASK_STACK_WORDS
#define REPRO_TASK_STACK_WORDS 256U
#endif

void Repro_Start(void);               /* main.c에서 vTaskStartScheduler() 전에 호출 */
void Repro_Log(const char *fmt, ...); /* 평상시 로그 (HAL UART) */
void Fault_Puts(const char *s);       /* 폴트 핸들러 전용 (레지스터 직접 접근) */
void Fault_PutHex(const char *name, unsigned long v);
unsigned long Sbrk_Used(void);        /* sysmem.c: newlib 힙이 지금까지 가져간 바이트 수 */
unsigned long Sbrk_Limit(void);       /* sysmem.c: newlib 힙이 가져갈 수 있는 최대 바이트 수 */

#endif /* REPRO_H */
