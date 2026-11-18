/*
 * main.c - 하드웨어 초기화 후 FreeRTOS 시작 (CubeMX 없이 직접 작성)
 *
 * 초기화 범위를 Renode에서 확실히 동작하는 최소한으로 줄였다.
 *   - 클럭: 리셋 기본값 HSI 16MHz 그대로 (PLL/HSE 대기 루프 없음)
 *   - HAL 틱: TIM6 대신 FreeRTOS 틱 훅에서 증가 (타이머 주변장치 불필요)
 *   - 주변장치: USART1(PA9 TX / PA10 RX, 115200) 하나
 */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "repro.h"

/*
 * UART 핸들 (repro.c에서 extern으로 사용)
 *   초기값이 없으므로 .bss(0 초기화)에 배치. 구조체 약 70B.
 *   스택이 아니라 전역이어야 하는 이유: 모든 태스크가 같은 UART 상태를 공유하고,
 *   main()의 스택은 스케줄러 시작 후 재사용되어 사라지기 때문.
 */
UART_HandleTypeDef huart1;

static void MX_USART1_UART_Init(void);

int main(void)
{
    /* Flash 프리페치/캐시 켜기, NVIC 우선순위 그룹 4(FreeRTOS 요구사항),
       HAL_InitTick() 호출(아래에서 빈 함수로 대체) */
    HAL_Init();

    /* 클럭 설정 생략: 리셋 직후 HSI 16MHz이고 SystemCoreClock 기본값도 16000000 */

    MX_USART1_UART_Init();

    /*
     * 재현 태스크 생성. 여기서 FreeRTOS 힙(ucHeap)에서
     *   스택 REPRO_TASK_STACK_WORDS x 4B + TCB 약 90B 를 pvPortMalloc으로 가져간다.
     */
    Repro_Start();

    /*
     * 스케줄러 시작. 내부에서 Idle 태스크(스택 configMINIMAL_STACK_SIZE x 4B + TCB)를
     * 또 FreeRTOS 힙에서 할당한다.
     * 또한 MSP를 _estack으로 되돌리므로 지금까지 쓰던 main()의 스택 내용은 버려진다.
     * -> main()의 지역변수를 태스크에 포인터로 넘기면 안 되는 이유.
     */
    vTaskStartScheduler();

    /* 여기 도달 = Idle 태스크를 만들 FreeRTOS 힙이 부족 (정상이라면 절대 안 옴) */
    Fault_Puts("\r\n!!! vTaskStartScheduler failed (FreeRTOS heap too small?)\r\n");
    for (;;) { }
}

/* USART1: 115200 8N1, 송수신, 흐름제어 없음 */
static void MX_USART1_UART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 115200;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        Error_Handler();
    }
}

/* HAL_UART_Init()이 호출하는 하드웨어 준비: 클럭 공급 + 핀을 UART 기능으로 전환 */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        GPIO_InitTypeDef gpio = {0};

        __HAL_RCC_USART1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        gpio.Pin       = GPIO_PIN_9 | GPIO_PIN_10;   /* PA9 = TX, PA10 = RX */
        gpio.Mode      = GPIO_MODE_AF_PP;
        gpio.Pull      = GPIO_PULLUP;
        gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
        gpio.Alternate = GPIO_AF7_USART1;
        HAL_GPIO_Init(GPIOA, &gpio);
    }
}

/*
 * HAL 틱 설정 대체 (HAL의 __weak 함수를 덮어씀)
 *   SysTick은 FreeRTOS가 쓰므로 HAL은 아무 타이머도 설정하지 않는다.
 *   틱은 아래 vApplicationTickHook()에서 1ms마다 증가시킨다.
 *   (스케줄러 시작 전에는 HAL_GetTick()이 0에 멈춰 있으므로 HAL_Delay() 사용 금지)
 */
HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    (void)TickPriority;
    return HAL_OK;
}

/* FreeRTOS 틱 훅: SysTick 인터럽트(1kHz)마다 호출 -> HAL 타임아웃용 틱 증가
   (FreeRTOSConfig.h: configUSE_TICK_HOOK = 1) */
void vApplicationTickHook(void)
{
    HAL_IncTick();
}

void Error_Handler(void)
{
    __disable_irq();
    Fault_Puts("\r\n!!! Error_Handler\r\n");
    for (;;) { }
}
