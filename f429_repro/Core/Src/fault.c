/*
 * fault.c - HardFault 원인 추적용 핸들러 (Cortex-M4)
 *
 * 주의: CubeMX가 만든 stm32f4xx_it.c 안의 HardFault_Handler()는 지우거나
 *       USER CODE 영역에서 #if 0 으로 막아야 중복 정의 에러가 나지 않습니다.
 *
 * 폴트 시점에는 HAL/RTOS를 믿을 수 없으므로 USART1 레지스터에 직접 씁니다.
 */
#include "main.h"
#include "repro.h"

void Fault_Puts(const char *s)
{
    while (*s) {
        while ((USART1->SR & USART_SR_TXE) == 0U) { }
        USART1->DR = (uint8_t)*s++;
    }
}

void Fault_PutHex(const char *name, unsigned long v)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 0; i < 8; i++) {
        buf[9 - i] = hex[v & 0xFU];
        v >>= 4;
    }
    Fault_Puts("  ");
    Fault_Puts(name);
    Fault_Puts(" = ");
    Fault_Puts(buf);
    Fault_Puts("\r\n");
}

/* 예외 진입 시 하드웨어가 자동으로 쌓는 8개 레지스터 */
typedef struct {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
} StackFrame_t;

void HardFault_Report(StackFrame_t *f, uint32_t exc_return)
{
    uint32_t cfsr = SCB->CFSR;

    Fault_Puts("\r\n========== HARD FAULT ==========\r\n");
    Fault_PutHex("stacked PC ", f->pc);    /* 폴트를 일으킨 명령 주소 -> addr2line */
    Fault_PutHex("stacked LR ", f->lr);    /* 호출한 쪽 */
    Fault_PutHex("stacked R0 ", f->r0);
    Fault_PutHex("xPSR       ", f->xpsr);
    Fault_PutHex("EXC_RETURN ", exc_return); /* bit2=1 이면 PSP(태스크), 0 이면 MSP */
    Fault_PutHex("IPSR       ", __get_IPSR()); /* 실제 예외 번호: 3=HardFault 4=MemManage 5=BusFault 6=UsageFault */
    Fault_PutHex("HFSR       ", SCB->HFSR);
    Fault_PutHex("CFSR       ", cfsr);
    if (cfsr & SCB_CFSR_MMARVALID_Msk) Fault_PutHex("MMFAR      ", SCB->MMFAR);
    if (cfsr & SCB_CFSR_BFARVALID_Msk) Fault_PutHex("BFAR       ", SCB->BFAR);

    /* CFSR 주요 비트 해석 */
    if (cfsr & (1UL << 16)) Fault_Puts("  -> UNDEFINSTR: 정의되지 않은 명령 실행\r\n");
    if (cfsr & (1UL << 17)) Fault_Puts("  -> INVSTATE: Thumb 비트 0인 주소로 분기 (덮어쓴 함수 포인터 의심)\r\n");
    if (cfsr & (1UL << 8))  Fault_Puts("  -> IBUSERR: 잘못된 주소에서 명령 인출\r\n");
    if (cfsr & (1UL << 9))  Fault_Puts("  -> PRECISERR: 잘못된 주소 데이터 접근 (BFAR 확인)\r\n");
    if (cfsr & (1UL << 10)) Fault_Puts("  -> IMPRECISERR: 부정확 버스 에러 (쓰기 버퍼)\r\n");
    if (cfsr & (1UL << 12)) Fault_Puts("  -> STKERR: 예외 진입 중 스택 쌓기 실패 (스택 오버플로 의심)\r\n");
    if (cfsr & (1UL << 24)) Fault_Puts("  -> UNALIGNED: 정렬되지 않은 접근\r\n");
    if (cfsr & (1UL << 25)) Fault_Puts("  -> DIVBYZERO: 0으로 나눔\r\n");
    if (cfsr == 0U)         Fault_Puts("  -> CFSR=0: 에뮬레이터가 상태 레지스터를 채우지 않았을 수 있음. stacked PC로 추적\r\n");

    Fault_Puts("================================\r\n");
    __BKPT(0);          /* GDB 연결 시 여기서 정지 -> bt, info reg */
    for (;;) { }
}

/* MSP/PSP 중 실제로 쓰던 스택을 골라 C 함수로 넘김 */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst   lr, #4            \n"
        "ite   eq                \n"
        "mrseq r0, msp           \n"
        "mrsne r0, psp           \n"
        "mov   r1, lr            \n"
        "b     HardFault_Report  \n");
}

/*
 * MemManage/BusFault/UsageFault도 같은 보고 루틴으로 보낸다.
 *   실제 칩은 SHCSR에서 이 폴트들을 켜지 않으면 HardFault로 승격시키므로 필요 없지만,
 *   Renode에서 MODE 3 실행 시 HardFault_Handler가 아니라 Default_Handler(무한 루프)로
 *   빠진 것을 확인함 -> 개별 벡터로 들어와도 보고가 출력되게 별칭을 둔다.
 *   (스타트업 파일의 .weak 정의를 이 정의가 덮어씀)
 */
void MemManage_Handler(void)  __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void)   __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));
