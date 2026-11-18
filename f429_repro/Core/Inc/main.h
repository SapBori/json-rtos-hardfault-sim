/*
 * main.h - 공통 헤더 (repro.c, fault.c가 include)
 *   HAL 전체 선언(UART_HandleTypeDef, USART1->SR, SCB->CFSR 등)을 한 번에 가져온다.
 */
#ifndef MAIN_H
#define MAIN_H

#include "stm32f4xx_hal.h"

void Error_Handler(void);

#endif /* MAIN_H */
