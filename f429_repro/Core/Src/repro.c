/*
 * repro.c - 1초마다 "상태 요청"에 JSON으로 응답하던 구조를 UART로 재현
 *
 * 이더넷(LwIP)은 Renode에서 지원이 불완전하므로 빼고,
 * 가설의 핵심인 "요청마다 문자열을 만들고 JSON으로 변환" 부분만 남겼습니다.
 * 출력은 USART1 (Renode: showAnalyzer sysbus.usart1)
 */
#include "main.h"
// #include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "repro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

extern UART_HandleTypeDef huart1;

/* ---------- 가짜 장비 상태 (당시: 온도·무게·컵 수 등) ---------- */
typedef struct {
    float    temp_c;
    float    keg_kg;
    uint32_t cups;
    uint32_t seq;
    uint8_t  tap_open;
} Status_t;

/* [메모리] 초기값이 있는 static -> .data (RAM에 상주, 초기값은 Flash에서 복사). 20B */
static Status_t g_status = { 3.5f, 18.20f, 0, 0, 0 };

static void Status_Update(void)
{
    g_status.seq++;
    g_status.cups    += (g_status.seq % 3 == 0) ? 1U : 0U;
    g_status.keg_kg  -= 0.05f;
    g_status.temp_c   = 3.5f + (float)(g_status.seq % 10) * 0.1f;
    g_status.tap_open = (uint8_t)(g_status.seq & 1U);
}

/* ---------- 공통: 로그와 메모리 계측 ---------- */
/*
 * [메모리] 로그 출력. 호출한 태스크의 스택을 line[128] + vsnprintf 내부 프레임만큼 쓴다.
 *   vsnprintf는 sizeof(line)을 넘겨 쓰지 않으며(잘림), 반환값 n은 "잘리기 전 길이"라서
 *   그대로 전송 길이로 쓰면 line 밖을 읽는다 -> 아래에서 n을 잘라 준다.
 */
void Repro_Log(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    HAL_UART_Transmit(&huart1, (uint8_t *)line, (uint16_t)n, 100);
}

/*
 * [메모리] 계측 로그
 *   stackFreeMin : 이 태스크 스택에서 지금까지 한 번도 안 쓴 최소 여유(워드 단위, x4 = 바이트).
 *                  생성 시 0xA5로 채운 영역이 몇 워드 남았는지 센다. 0에 가까우면 오버플로 직전.
 *   heapFree     : FreeRTOS 힙(ucHeap) 현재 잔량(바이트). 누수면 계속 감소.
 *   heapMinEver  : FreeRTOS 힙 잔량의 역대 최솟값.
 *   주의: newlib malloc(sprintf %f 내부, cJSON 등)의 사용량은 여기 안 나온다 (sysmem.c 참고).
 */
static void Report_Memory(const char *tag)
{
    Repro_Log("[MEM] %s seq=%lu stackFreeMin=%lu words heapFree=%u heapMinEver=%u\r\n",
              tag,
              (unsigned long)g_status.seq,
              (unsigned long)uxTaskGetStackHighWaterMark(NULL),
              (unsigned)xPortGetFreeHeapSize(),
              (unsigned)xPortGetMinimumEverFreeHeapSize());
#if REPRO_MODE >= 5
    /* [메모리] newlib 힙(malloc) 사용량. FreeRTOS 힙(heapFree)과 별개 영역 */
    Repro_Log("[NEWLIB] seq=%lu used=%lu limit=%lu\r\n",
              (unsigned long)g_status.seq, Sbrk_Used(), Sbrk_Limit());
#endif
}

/* 전송 계층 흉내: 실제 TCP 대신 UART. N번에 1번은 "전송 실패"로 처리 */
static int Fake_Send(const char *buf, size_t len)
{
    if (g_status.seq % 4U == 0U) return -1;             /* 연결 끊김/재전송 실패 상황 */
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, 200);
    HAL_UART_Transmit(&huart1, (uint8_t *)"\r\n", 2, 10);
    return 0;
}

/* ================================================================
 * MODE 1: STACK - PC 습관 그대로. 큰 지역 버퍼 + float sprintf
 *   - char json[768] 이 1KB 태스크 스택 대부분을 차지
 *   - newlib의 printf(%f)는 내부적으로 수백 바이트 이상 스택을 추가로 사용
 *   => 스택 오버플로 -> vApplicationStackOverflowHook 또는 HardFault
 * ================================================================ */
#if REPRO_MODE == 1
static void Build_And_Send(void)
{
    char field[64];
    char json[768];
    json[0] = '\0';

    sprintf(field, "{\"seq\":%lu,", (unsigned long)g_status.seq);           strcat(json, field);
    sprintf(field, "\"temp\":%.2f,", (double)g_status.temp_c);              strcat(json, field);
    sprintf(field, "\"keg\":%.2f,", (double)g_status.keg_kg);               strcat(json, field);
    sprintf(field, "\"cups\":%lu,", (unsigned long)g_status.cups);          strcat(json, field);
    sprintf(field, "\"tap\":%s}", g_status.tap_open ? "true" : "false");    strcat(json, field);

    (void)Fake_Send(json, strlen(json));
}

/* ================================================================
 * MODE 2: HEAP - 응답마다 동적 할당, 실패 경로에서 free 누락
 *   => 힙이 조금씩 줄다가 malloc 실패 -> vApplicationMallocFailedHook
 *      NULL 검사가 없는 코드라면 이후 접근에서 폴트
 * ================================================================ */
#elif REPRO_MODE == 2
static void Build_And_Send(void)
{
    char *json = (char *)pvPortMalloc(256);
    /* 당시처럼 NULL 검사 없음 (의도적) */
    int n = sprintf(json,
                    "{\"seq\":%lu,\"temp\":%d,\"keg\":%d,\"cups\":%lu,\"tap\":%s}",
                    (unsigned long)g_status.seq,
                    (int)(g_status.temp_c * 100), (int)(g_status.keg_kg * 100),
                    (unsigned long)g_status.cups, g_status.tap_open ? "true" : "false");

    if (Fake_Send(json, (size_t)n) != 0) {
        return;                         /* BUG: 실패 시 vPortFree(json) 누락 */
    }
    vPortFree(json);
}

/* ================================================================
 * MODE 3: BUFFER - 고정 버퍼에 길이 검사 없이 누적
 *   - 같은 구조체 안에서 버퍼 바로 뒤에 콜백 함수 포인터를 둠(배치 보장)
 *   - 요청이 쌓일수록 문자열이 길어져 버퍼를 넘고 포인터를 덮어씀
 *   => 덮어쓴 포인터 호출 시 HardFault (INVSTATE/IBUSERR 등)
 * ================================================================ */
#elif REPRO_MODE == 3
static void On_Sent(void) { /* 전송 완료 콜백 */ }

static struct {
    char tx[160];
    void (*on_sent)(void);
} g_tx = { "", On_Sent };

static void Build_And_Send(void)
{
    char field[48];
    /* BUG: 매 요청마다 초기화하지 않고 history 처럼 누적 + 길이 검사 없음 */
    sprintf(field, "{\"seq\":%lu,\"cups\":%lu},",
            (unsigned long)g_status.seq, (unsigned long)g_status.cups);
    strcat(g_tx.tx, field);

    Repro_Log("[BUF] len=%u / %u  on_sent=%p\r\n",
              (unsigned)strlen(g_tx.tx), (unsigned)sizeof(g_tx.tx), (void *)g_tx.on_sent);

    (void)Fake_Send(g_tx.tx, strlen(g_tx.tx));
    g_tx.on_sent();                     /* 덮어써진 뒤 호출되면 폴트 */
}

/* ================================================================
 * MODE 4: FIXED - 수정본 (대조군)
 *   - 정적 버퍼 1개, snprintf 반환값으로 길이 검사
 *   - float 대신 정수 스케일(x100) 포맷 -> printf 스택 사용량 감소
 *   - 동적 할당 없음
 * ================================================================ */
#elif REPRO_MODE == 4
/* [메모리] 초기값 없는 static -> .bss. 크기가 빌드 시점에 고정되어 스택·힙을 쓰지 않고,
   map 파일에서 주소·크기를 확인할 수 있다. 한 태스크만 쓰므로 공유 문제도 없음 */
static char s_json[160];

static void Build_And_Send(void)
{
    int n = snprintf(s_json, sizeof(s_json),
                     "{\"seq\":%lu,\"temp\":%d,\"keg\":%d,\"cups\":%lu,\"tap\":%s}",
                     (unsigned long)g_status.seq,
                     (int)(g_status.temp_c * 100), (int)(g_status.keg_kg * 100),
                     (unsigned long)g_status.cups, g_status.tap_open ? "true" : "false");

    if (n < 0 || n >= (int)sizeof(s_json)) {
        Repro_Log("[ERR] json truncated (%d)\r\n", n);
        return;
    }
    (void)Fake_Send(s_json, (size_t)n);
}

#elif REPRO_MODE == 5 || REPRO_MODE == 6
/* ================================================================
 * MODE 5·6 공통: 응답 문자열을 malloc으로 만들어 돌려주는 함수
 *   JSON 라이브러리의 Print 함수처럼 동작한다
 *     - 들여쓰기(탭·줄바꿈) 포함 -> 사람이 읽기 좋지만 길이가 커진다
 *     - 필요한 길이만큼 newlib malloc으로 할당해 돌려준다
 *     - [메모리] 돌려받은 쪽이 free해야 한다 (소유권이 호출한 쪽으로 넘어감)
 *   detailed=1이면 센서 상태(ack 객체)를 붙인다 (약 370B, 기본 응답은 약 110B)
 *   detailed=2이면 1초 주기 상태 응답 전체 (약 490B, Status_Format)
 * ================================================================ */
/*
 * 1초 주기 상태(status) 응답: 장치 3종(TAP·CUP·SENSOR)을 중첩 객체로 묶은 형태, 약 490B
 *   실제 장비의 상태 응답과 같은 구조·길이로 만들고 키 이름만 일반화했다.
 *   들여쓰기 없이 한 줄 (JSON 라이브러리의 PrintUnformatted에 해당)
 */
static int Status_Format(char *buf, size_t size)
{
    int t = (int)(g_status.temp_c * 100);
    return snprintf(buf, size,
                    "{\"type\":\"response\",\"from\":\"ALL\",\"to\":\"client\",\"data\":\"status\",\"ack\":{"
                    "\"TAP\":{\"status\":\"%s\",\"a\":{\"mode\":\"auto\",\"tap_pose\":\"%s\",\"level\":\"%d%%\"},"
                    "\"b\":{\"mode\":\"manual\",\"tap_pose\":\"close\",\"level\":\"%d%%\"}},"
                    "\"CUP\":{\"status\":\"ready\",\"a\":{\"status\":\"ready\"},\"b\":{\"status\":\"ready\"},\"c\":{\"status\":\"%s\"}},"
                    "\"SENSOR\":{\"pickup\":{\"a\":\"%s\",\"b\":\"false\",\"c\":\"true\"},"
                    "\"weight_gram\":{\"a\":\"%d\",\"b\":\"10000\",\"c\":\"5000\",\"d\":\"10000\"},"
                    "\"drain\":{\"is_full\":\"false\"},"
                    "\"temperature\":{\"a\":\"%d.%02d\",\"b\":\"%d.%02d\"}}},\"meta\":\"g\"}",
                    g_status.tap_open ? "moving" : "ready", g_status.tap_open ? "pour" : "close",
                    (int)(g_status.keg_kg * 100 / 18.2f), 100,
                    (g_status.cups % 7U == 0U) ? "error" : "ready",
                    g_status.tap_open ? "true" : "false",
                    (int)(g_status.keg_kg * 1000),
                    t / 100, t % 100, (t + 50) / 100, (t + 50) % 100);
}

static int Json_Format(char *buf, size_t size, int detailed)
{
    if (detailed == 2) {
        return Status_Format(buf, size);   /* MODE 5: 상태 응답 */
    }
    /* buf == NULL, size == 0 이면 쓰지 않고 필요한 길이만 계산 (snprintf 규칙) */
    int n = snprintf(buf, size,
                     "{\n\t\"type\":\t\"status\",\n\t\"seq\":\t%lu,\n\t\"temp\":\t%d,"
                     "\n\t\"keg\":\t%d,\n\t\"cups\":\t%lu,\n\t\"tap\":\t%s,\n\t\"uptime_ms\":\t%lu",
                     (unsigned long)g_status.seq,
                     (int)(g_status.temp_c * 100), (int)(g_status.keg_kg * 100),
                     (unsigned long)g_status.cups, g_status.tap_open ? "true" : "false",
                     (unsigned long)xTaskGetTickCount());
    if (detailed) {
        /* 센서 상태 응답: 중첩 객체 4개 (픽업 스위치·무게·배수·온도), 들여쓰기 포함 약 370B */
        int t = (int)(g_status.temp_c * 100);
        n += snprintf(buf ? buf + n : NULL, buf ? size - (size_t)n : 0,
                      ",\n\t\"ack\":\t{"
                      "\n\t\t\"pickup\":\t{\n\t\t\t\"a\":\t\"%s\",\n\t\t\t\"b\":\t\"%s\",\n\t\t\t\"c\":\t\"%s\"\n\t\t},"
                      "\n\t\t\"weight_gram\":\t{\n\t\t\t\"a\":\t\"%lu\",\n\t\t\t\"b\":\t\"%lu\",\n\t\t\t\"c\":\t\"%lu\",\n\t\t\t\"d\":\t\"%lu\"\n\t\t},"
                      "\n\t\t\"drain\":\t{\n\t\t\t\"is_full\":\t\"%s\"\n\t\t},"
                      "\n\t\t\"temperature\":\t{\n\t\t\t\"a\":\t\"%d.%02d\",\n\t\t\t\"b\":\t\"%d.%02d\"\n\t\t}\n\t}",
                      g_status.tap_open ? "true" : "false", "false", "true",
                      (unsigned long)(g_status.keg_kg * 1000), 10000UL, 5000UL, 10000UL,
                      "false",
                      t / 100, t % 100, (t + 50) / 100, (t + 50) % 100);
    }
    n += snprintf(buf ? buf + n : NULL, buf ? size - (size_t)n : 0, "\n}");
    return n;
}

static char *Json_Print(int detailed)
{
    int n = Json_Format(NULL, 0, detailed);
    char *s = (char *)malloc((size_t)n + 1U);   /* [메모리] newlib 힙에서 n+1바이트 할당 */
    if (s == NULL) {
        return NULL;
    }
    (void)Json_Format(s, (size_t)n + 1U, detailed);
    return s;
}

#if REPRO_MODE == 5
/* ================================================================
 * MODE 5: NEWLIB_LEAK - 응답 문자열을 받아 보내고 free하지 않음
 *   - 1초마다 상태 응답(약 490B)을 만들어 보내고 해제하지 않는다
 *   - 누수는 newlib 힙(_sbrk 영역)에서 일어난다
 *   - FreeRTOS 힙 잔량(heapFree)은 전혀 줄지 않는다 -> FreeRTOS 힙을 늘려도 효과 없음
 *   => newlib 힙이 한계(Sbrk_Limit)에 닿으면 malloc이 NULL을 반환
 * ================================================================ */
static int s_null_logged;   /* 계측용: NULL을 처음 받은 시점을 한 번만 기록 */

static void Build_And_Send(void)
{
    char *response = Json_Print(2);

    if (response == NULL && !s_null_logged) {   /* 계측용 로그 (당시 코드에는 없던 것) */
        Repro_Log("[NEWLIB] malloc returned NULL at seq=%lu used=%lu limit=%lu\r\n",
                  (unsigned long)g_status.seq, Sbrk_Used(), Sbrk_Limit());
        s_null_logged = 1;
    }

    /* BUG: NULL 검사 없음 (당시 구조와 같음) */
    (void)Fake_Send(response, strlen(response));
    /* BUG: free(response) 누락 -> 요청마다 응답 길이만큼 newlib 힙 누수 */
}

#else
/* ================================================================
 * MODE 6: COPY_OVERFLOW - 응답을 256B 고정 메시지에 길이 검사 없이 복사
 *   - 평소 응답은 약 110B라 문제없음
 *   - 5번째 요청마다 센서 상태 응답(약 370B) -> 지역 변수 msg를 넘어
 *     그 위에 저장된 레지스터와 복귀 주소(LR)를 덮는다
 *   => Build_And_Send가 끝나 돌아가는 순간 덮인 주소로 점프 -> 폴트
 *   - 원인을 하나로 나누기 위해 MODE 6은 free를 제대로 한다 (누수 없음)
 *   - 스택을 키워도(make MODE=6 STACK=512 / 1024) 결과가 같은지 확인:
 *     넘침은 msg 위쪽(복귀 주소 쪽)으로 가고, 스택을 키우면 늘어나는 곳은 아래쪽이다
 * ================================================================ */
typedef struct {
    char data[256];            /* [메모리] 고정 크기 메시지 (송신 큐 항목과 같은 구조) */
    int  port_index;
} SendMsg_t;

static void Build_And_Send(void)
{
    SendMsg_t msg;             /* [메모리] 지역 변수 -> 태스크 스택에 260B */
    int detailed = (g_status.seq % 5U == 0U);   /* 5초에 1번 센서 상태 요청 (가정) */
    char *response = Json_Print(detailed);

    msg.port_index = 0;
    strcpy(msg.data, response);   /* BUG: 길이 검사 없음. 256B를 넘으면 msg 뒤의 스택을 덮음 */
    free(response);

    Repro_Log("[COPY] seq=%lu len=%u / %u\r\n", (unsigned long)g_status.seq,
              (unsigned)strlen(msg.data), (unsigned)sizeof(msg.data));
    (void)Fake_Send(msg.data, strlen(msg.data));
}   /* 여기서 복귀할 때, 덮인 복귀 주소로 점프 */
#endif

#else
#error "REPRO_MODE must be 1..6"
#endif

/* ---------- 상태 응답 태스크 ---------- */
static void StatusTask(void *arg)
{
    (void)arg;
    Repro_Log("\r\n=== F429 JSON repro start: MODE %d ===\r\n", REPRO_MODE);
    Report_Memory("boot");

    for (;;) {
        Status_Update();
        Build_And_Send();
        if ((g_status.seq % 5U) == 0U) {
            Report_Memory("periodic");
        }
        vTaskDelay(pdMS_TO_TICKS(REPRO_PERIOD_MS));
    }
}

/*
 * [메모리] 태스크 생성 = FreeRTOS 힙에서 2번 할당
 *   1) 스택: REPRO_TASK_STACK_WORDS(256) x 4B = 1024B  (0xA5로 채워짐)
 *   2) TCB : 약 90B (우선순위, 스택 포인터, 이름 등)
 *   스택을 먼저, TCB를 나중에 할당한다. 스택은 아래로 자라므로 넘치면
 *   자기 TCB가 아니라 그보다 앞에 할당된 블록(heap_4 헤더, 다른 태스크)을 덮는다.
 */
void Repro_Start(void)
{
    BaseType_t ok = xTaskCreate(StatusTask, "status", REPRO_TASK_STACK_WORDS,
                                NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        Repro_Log("[ERR] task create failed\r\n");
    }
}

/* ---------- FreeRTOS 훅 (CubeMX에서 해당 옵션을 켜야 호출됨) ---------- */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    taskDISABLE_INTERRUPTS();
    Fault_Puts("\r\n!!! STACK OVERFLOW in task: ");
    Fault_Puts(pcTaskName);
    Fault_Puts("\r\n");
    for (;;) { }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    Fault_Puts("\r\n!!! MALLOC FAILED (heap exhausted)\r\n");
    Fault_PutHex("heapFree", (unsigned long)xPortGetFreeHeapSize());
    for (;;) { }
}
