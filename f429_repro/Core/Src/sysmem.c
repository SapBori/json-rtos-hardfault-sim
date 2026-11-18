/*
 * sysmem.c - newlib 힙(malloc)이 메모리를 얻어 가는 _sbrk() 구현
 *
 * [메모리] malloc()/free()(newlib)와 pvPortMalloc()/vPortFree()(FreeRTOS)는 다른 힙이다.
 *   - pvPortMalloc : .bss 안의 고정 배열 ucHeap에서 할당 (configTOTAL_HEAP_SIZE)
 *   - malloc       : 이 _sbrk()로 .bss 끝(_end)부터 RAM 위쪽으로 늘려 가며 할당
 *   printf("%f")의 내부 변환, cJSON 같은 라이브러리는 malloc을 쓴다.
 *   따라서 FreeRTOS 힙 잔량(xPortGetFreeHeapSize)은 malloc 누수를 보여 주지 않는다.
 *
 * libnosys에도 _sbrk가 있지만 상한 검사가 없어 MSP 스택을 덮을 때까지 늘어난다.
 * 여기서 직접 정의하면 링커가 이쪽을 쓰고 libnosys 것은 무시한다.
 * (CubeIDE가 생성하는 sysmem.c와 같은 동작, 새로 작성)
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * [메모리] 현재 newlib 힙의 끝 = 다음 sbrk 호출이 내줄 시작 주소.
 *   GDB에서 `p __sbrk_heap_end` 를 주기적으로 보면 newlib 힙 사용량 추이를 알 수 있다.
 *   (값이 계속 커지면 malloc 누수. free된 메모리는 newlib 내부에서 재사용되므로
 *    이 값은 줄어들지 않고 "최대 사용량"만 보여 준다)
 *   이름을 CubeIDE 생성 코드와 같게 해서 실보드 프로젝트에서도 같은 명령으로 확인 가능.
 */
static uint8_t *__sbrk_heap_end = NULL;

/*
 * newlib malloc이 메모리가 더 필요할 때 호출.
 *   incr 바이트만큼 힙 끝을 늘리고, 늘리기 전 주소를 반환한다.
 *   상한(_estack - _Min_Stack_Size)을 넘으면 실패(-1) -> malloc()이 NULL 반환.
 */
void *_sbrk(ptrdiff_t incr)
{
    extern uint8_t _end;              /* 링커 스크립트: .bss 끝 = newlib 힙 시작 */
    extern uint8_t _estack;           /* 링커 스크립트: RAM 끝 = MSP 초기값 */
    extern uint32_t _Min_Stack_Size;  /* 링커 심볼은 '주소'가 곧 값이므로 &로 읽는다 */

    /* MSP용으로 남겨 둘 영역 바로 아래까지만 힙을 허용 */
    const uint32_t stack_limit = (uint32_t)&_estack - (uint32_t)&_Min_Stack_Size;
    const uint8_t *max_heap = (uint8_t *)stack_limit;
    uint8_t *prev_heap_end;

    if (__sbrk_heap_end == NULL) {
        __sbrk_heap_end = &_end;      /* 첫 호출: .bss 바로 뒤에서 시작 */
    }

    if (__sbrk_heap_end + incr > max_heap) {
        errno = ENOMEM;               /* 힙이 MSP 예약 영역을 침범하려 함 -> 거절 */
        return (void *)-1;
    }

    prev_heap_end = __sbrk_heap_end;
    __sbrk_heap_end += incr;
    return (void *)prev_heap_end;
}

/*
 * [메모리] newlib 힙 계측 (MODE 5·6의 [MEM] 로그용)
 *   Sbrk_Used : _sbrk로 지금까지 내준 바이트 수. free된 메모리는 newlib이 재사용하므로
 *               이 값은 줄지 않는다 = "newlib 힙이 최대로 커진 크기"
 *   Sbrk_Limit: _sbrk가 내줄 수 있는 최대 바이트 수 (.bss 끝 ~ MSP 예약 영역 직전)
 *               FreeRTOS 힙(configTOTAL_HEAP_SIZE)을 늘리면 .bss가 커져 이 값이 줄어든다
 */
unsigned long Sbrk_Used(void)
{
    extern uint8_t _end;
    return (__sbrk_heap_end == NULL) ? 0UL : (unsigned long)(__sbrk_heap_end - &_end);
}

unsigned long Sbrk_Limit(void)
{
    extern uint8_t _end;
    extern uint8_t _estack;
    extern uint32_t _Min_Stack_Size;
    return (unsigned long)((uint32_t)&_estack - (uint32_t)&_Min_Stack_Size - (uint32_t)&_end);
}
