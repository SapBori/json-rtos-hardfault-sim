# 원본 펌웨어 하드폴트 원인 분석 보고서

> **비공개 문서**입니다. 회사 저장소의 함수 이름과 줄 번호가 들어 있어 공개 저장소(`f429_repro/`)에는 넣지 않습니다.
> 원본 코드는 복사하지 않았고, 아래 수정 예시 코드는 모두 새로 작성한 것입니다.
> 표기: **[확인]**은 코드나 실험으로 확인한 사실, **[가정]**은 추정입니다.
> 기준: 원본 저장소 HEAD `fcbeb33` (2025-02-12), 재현 환경 Renode v1.16.1 + FreeRTOS V10.6.2

---

## 0. 결론

하드폴트를 일으킬 수 있는 결함은 **수신 콜백 → JSON 처리 → 송신 큐**로 이어지는 경로에 몰려 있습니다.

| 순위 | 결함 | 함수 | 재현 | 확신도 |
|---|---|---|---|---|
| 1 | 256B 버퍼에 길이 검사 없이 `strcpy` → 복귀 주소 덮어쓰기 | `app_callback_received` | MODE 6 ✅ | 높음 |
| 2 | 응답 문자열 `free` 누락 → newlib 힙 고갈 | `app_callback_received`, `send_cup_completion`, `send_beer_completion` | MODE 5 ✅ | 높음 |
| 3 | NULL 검사 없음 (응답이 NULL일 때, JSON 키가 없을 때) | `app_callback_received`, `process_json_data`, `Send_Task` | MODE 2·5 (부분) | 중간 |
| 4 | 다른 태스크에서 LwIP Raw API를 락 없이 호출 | `Send_Task` | 재현 안 함 | 중간 [가정] |
| 5 | 여러 태스크가 newlib `malloc`과 송신 큐를 락 없이 공유 | cJSON 전체, `SQenque`/`SQdeque` | 재현 안 함 | 중간 [가정] |

**스택·힙 크기를 늘려도 효과가 없었던 이유**도 실험으로 확인했습니다.
- 1번(버퍼 넘침)은 스택을 1KB에서 4KB로 늘려도 **레지스터 값까지 똑같이** 터졌습니다.
- 2번(newlib 힙 누수)은 FreeRTOS 힙을 늘리면 **오히려 더 빨리** 터졌습니다. 두 힙이 같은 RAM을 나눠 쓰기 때문입니다.

---

## 1. 배경: 원본 시스템의 메모리 구조

### 1.1 태스크와 데이터 흐름 [확인: `.ioc`, 소스]
```
[PC/안드로이드] --TCP--> tcpip_thread (LwIP, 스택 4096)
                           └ app_callback_received()        ← 수신 콜백
                               └ process_json_data()         ← cJSON 파싱 + 응답 생성(malloc)
                               └ strcpy(msg.data, response)  ← 256B 메시지로 복사
                               └ SQenque(&send_queue)        ← 송신 큐 (락 없음)
executeTask / cupSerialTask (스택 256 words)
   └ send_cup_completion() / send_beer_completion()          ← 완료 응답 생성(malloc) → 큐
sendTask (스택 512 words)
   └ Send_Task(): 10ms 루프, 약 1초마다
       ├ 큐에 응답이 있으면 SQdeque → send_to_client()       ← tcp_write (락 없음)
       └ 없으면 STATUS 응답 생성(malloc) → send_to_client()
```

### 1.2 힙이 두 개라는 점 (원인 2를 이해하는 핵심)
| 힙 | 쓰는 쪽 | 크기 | 감시 수단 |
|---|---|---|---|
| FreeRTOS 힙 (`ucHeap`, heap_4) | `xTaskCreate`, `pvPortMalloc` | 65,536B (정적 배열, `.bss`) | `xPortGetFreeHeapSize()`, malloc 실패 훅 |
| LwIP 힙 | pbuf 등 | `MEM_SIZE` 32,768B (정적 배열) | LwIP 통계 |
| **newlib 힙** (`_sbrk`) | **`malloc`: cJSON 전체, printf 내부** | **RAM에서 위 둘과 `.bss`를 뺀 나머지** | **없음** |

- cJSON은 훅 설정(`cJSON_InitHooks`) 없이 기본 `malloc`/`free`를 씁니다. 그래서 모든 JSON 처리가 **감시 수단이 없는 newlib 힙**에서 일어납니다. [확인]
- `configTOTAL_HEAP_SIZE`를 키우면 `.bss`가 커지고, 그만큼 newlib 힙이 줄어듭니다. [확인: MODE 5 대조 실험]

### 1.3 감지 장치 [확인: `FreeRTOSConfig.h`]
- `configCHECK_FOR_STACK_OVERFLOW` 꺼짐
- `configUSE_MALLOC_FAILED_HOOK` 꺼짐. 켜더라도 FreeRTOS 힙만 감시하므로 cJSON 누수는 못 잡습니다.
- HardFault 핸들러에 레지스터를 출력하는 코드 없음
- 결과적으로 **폴트가 나도 어디서, 왜 났는지 알 방법이 없는 상태**였습니다.

---

## 2. 문제 함수별 분석

### 2.1 `app_callback_received()`: `tcp_echo.c:212`
LwIP가 TCP 데이터를 받으면 tcpip_thread에서 호출하는 수신 콜백입니다.

#### 결함 A. 256B 메시지에 길이 검사 없는 `strcpy` (`tcp_echo.c:292`) [확인]
- **무엇이**: `process_json_data()`가 돌려준 응답 문자열을 지역 변수 `SendQueueMsg msg`의 `char data[256]`에 `strcpy`로 복사합니다.
- **왜 터지나**
  - sensor_state 응답은 들여쓰기가 포함된 `cJSON_Print`로 만들어서 약 376B입니다. 256B 버퍼를 약 120B 넘칩니다. 길이는 형식에 값을 넣어 계산했습니다 [가정].
  - 지역 변수는 스택에 있고, `strcpy`는 **높은 주소 방향**으로 씁니다. 그 방향에는 함수가 시작할 때 저장해 둔 **레지스터와 복귀 주소(LR)**가 있습니다.
  - 함수가 끝나면서 덮인 복귀 주소로 점프하는 순간 폴트가 납니다.
  - 넘친 글자 4개가 복귀 주소 자리에 들어가므로, 그 값의 bit0에 따라 증상이 달라집니다.
    - bit0이 0이면 UsageFault(INVSTATE)
    - bit0이 1이면 엉뚱한 주소를 실행하거나 BusFault
    - 그래서 응답 내용(센서 값 자릿수)에 따라 증상이 **불규칙**해 보였을 것입니다 [가정, 재현에서 두 가지 증상 모두 관찰].
- **왜 스택을 늘려도 소용없나**
```
 높은 주소   ┌────────────────────────┐
             │ 저장된 레지스터 + 복귀 주소 │ ← 넘친 문자열이 여기를 덮음
             │ msg.port_index         │
             │ msg.data[256]          │ ← strcpy 시작
             │ temp_buffer[1024] 등   │
 낮은 주소   │ (남는 스택)             │ ← 스택을 늘리면 커지는 곳
             └────────────────────────┘
```
  넘침은 위쪽으로 가고 스택 증가는 아래쪽이라 서로 관계가 없습니다. 지역 변수가 실제로 어떤 순서로 배치되는지는 컴파일러가 정하므로 원본 빌드에서는 확인하지 않았습니다 [가정].
- **해결 방안**
```c
/* 새로 작성한 예시 */
char *response = process_json_data(es_index, temp_buffer, data_len);
if (response != NULL) {
    size_t n = strlen(response);
    if (n < sizeof(msg.data)) {
        memcpy(msg.data, response, n + 1);      /* NUL 포함 복사 */
        msg.port_index = es_index;
        SQenque(&send_queue, &msg);
    } else {
        /* 응답이 메시지보다 큼: 로그를 남기고 버림 (또는 Unformatted로 바꿔 크기 축소) */
    }
    cJSON_free(response);                        /* 결함 B 해결 */
}
```
  - 근본 대책은 응답을 처음부터 버퍼 크기 안에서 만드는 것입니다. `cJSON_PrintPreallocated(item, buf, sizeof(buf), 0)`을 쓰면 malloc 없이 고정 버퍼에 쓰고, 넘치면 실패를 반환합니다.
  - 들여쓰기 출력(`cJSON_Print`)을 `cJSON_PrintUnformatted`로 바꾸면 응답이 약 26% 줄어듭니다(376 → 277B). **그래도 256B를 넘으므로 길이 검사는 꼭 필요합니다** [가정: 계산].

#### 결함 B. 응답 문자열 `free` 누락 (`tcp_echo.c:297`) [확인]
- **무엇이**: `// free(response);`가 주석 처리되어 있습니다. 바로 위에 `// ???` 주석이 있어서, 해제 여부를 확신하지 못해 막아 둔 것으로 보입니다.
- **왜 터지나**
  - 요청 한 번마다 응답 길이에 malloc 헤더를 더한 만큼 newlib 힙이 샙니다. 힙이 바닥나면 `malloc`이 NULL을 돌려줍니다.
  - cJSON은 파싱 도중에도 malloc을 많이 쓰므로, 그때부터 파싱과 응답 생성이 모두 실패합니다. 결함 C로 이어집니다.
  - FreeRTOS 힙 잔량은 그대로이므로 **힙 감시로는 보이지 않습니다**.
- **해결 방안**: 큐에 복사한 직후 `cJSON_free(response)`를 호출합니다(결함 A 예시 코드). 응답을 큐에 복사하므로 바로 해제해도 안전합니다.

#### 결함 C. NULL 응답을 그대로 `strcpy` [확인]
- **무엇이**: `process_json_data()`는 JSON 파싱이 실패하면 NULL을 반환합니다(`tcp_echo.c:503`). 콜백은 이 값을 검사하지 않고 `strcpy(msg.data, NULL)`을 호출합니다.
- **언제 일어나나**
  - TCP는 메시지 경계를 보장하지 않으므로, 요청 하나가 두 패킷으로 나뉘어 오면 파싱이 실패합니다 [가정].
  - 결함 B로 힙이 바닥나도 파싱이 실패합니다.
- **왜 터지나 (실제 칩)**: STM32F4에서 BOOT0=0이면 0x0번지가 Flash(벡터 테이블)의 별칭입니다. 그래서 `strcpy`가 벡터 테이블 바이트를 0이 나올 때까지 복사합니다. 즉시 터지지 않고 쓰레기 응답이 큐에 들어가거나, 길면 결함 A처럼 스택을 덮을 수 있습니다 [가정].
- **해결 방안**: `if (response != NULL)` 검사(결함 A 예시 코드). 파싱 실패 시에는 에러 응답을 고정 문자열로 보냅니다.

#### 결함 D. `tcp_recved()` 누락 (부수 발견) [확인: 코드 / 영향은 가정]
- **무엇이**: 정상 수신 경로(`ES_RECEIVED`이면서 `es->p == NULL`)는 pbuf를 해제만 하고, 받은 만큼 수신 창을 다시 여는 `tcp_recved()`를 호출하지 않습니다. 원래 호출하던 `app_send_data()`(에코 함수)가 주석 처리되면서 함께 빠졌습니다.
- **영향**: 받은 데이터 누적량이 수신 창 크기(`TCP_WND`)에 이르면 상대가 더 보낼 수 없어 **통신이 멈춥니다**. 폴트는 아니지만 "통신 끊김"으로 보입니다. 저장소에 "IP Connection Self Reset Code" 커밋이 있는 것과 맞아떨어집니다 [가정].
- **해결 방안**: 처리 후 `tcp_recved(tpcb, p->tot_len);`를 호출합니다.

#### 참고: 지역 변수가 큼
`char temp_buffer[1024]`, `SendQueueMsg msg`(260B)가 tcpip_thread 스택에 올라갑니다. 여기에 cJSON 파싱(재귀)과 Print가 더해집니다. 스택이 4096(워드 단위라면 16KB [가정])이라 당장 넘치지는 않겠지만, 측정하지 않은 상태입니다. 또 `temp_buffer`가 꽉 차면(1024B) 문자열 끝 NUL이 없어 `cJSON_Parse`가 버퍼 밖을 읽습니다 [확인: 코드].

---

### 2.2 `process_json_data()`: `tcp_echo.c:491`
요청 JSON을 파싱해서 키를 꺼내고 응답을 만들어 돌려줍니다.

#### 결함 E. JSON 키가 없을 때 NULL 역참조 + 10B 버퍼 `strcpy` (`tcp_echo.c:540~544`) [확인]
- **무엇이**
  - `cJSON_GetObjectItemCaseSensitive()`는 키가 없으면 NULL을 반환합니다.
  - 그런데 `req_data->valuestring`, `opt_value->valuestring`, `opt_mode->valuestring`, `req_meta->valuestring`를 검사 없이 사용합니다. `req_data`는 `->valuestring != NULL` 검사를 하지만, 이것도 `req_data` 자체가 NULL이면 이미 역참조입니다.
  - 복사 대상 `MESSAGE`의 `opt_mode`, `opt_value`, `meta`는 각각 **10B**인데 `strcpy`로 복사합니다.
- **왜 터지나**
  - option이 없는 요청(예: 상태 조회)이 오면 `NULL->valuestring`이 됩니다. 이는 0x10번지를 읽는다는 뜻입니다. cJSON 구조체에서 `valuestring`의 오프셋이 16이기 때문입니다 [가정: 32비트 배치 기준].
  - 실제 칩에서는 0x10이 벡터 테이블(MemManage 핸들러 주소)이므로 그 **Flash 주소를 문자열로 보고 복사**합니다. 0이 나올 때까지 10B 버퍼에 쓰니 지역 변수 `message`가 넘칠 수 있습니다 [가정].
  - 값이 9자를 넘으면 정상 요청이어도 넘칩니다.
- **해결 방안**
```c
/* 새로 작성한 예시: 키가 없거나 문자열이 아니면 빈 문자열, 길면 잘라서 복사 */
static void copy_json_str(char *dst, size_t size, const cJSON *item)
{
    const char *s = cJSON_IsString(item) ? item->valuestring : "";
    snprintf(dst, size, "%s", s);
}
/* 사용 */
if (!cJSON_IsString(req_data) || !cJSON_IsString(req_to)) { /* 에러 응답 */ }
copy_json_str(message.data,      sizeof(message.data),      req_data);
copy_json_str(message.opt_value, sizeof(message.opt_value), opt_value);
copy_json_str(message.opt_mode,  sizeof(message.opt_mode),  opt_mode);
copy_json_str(message.meta,      sizeof(message.meta),      req_meta);
```

#### 결함 F. 들여쓰기 출력(`cJSON_Print`) 사용 (`tcp_echo.c:697, 719, 738`) [확인]
- 응답 크기가 커져서 결함 A(256B 초과)의 직접 원인이 됩니다. 같은 sensor_state 내용이 Print로는 약 376B, Unformatted로는 약 277B입니다 [가정: 계산]. Unformatted만으로는 256B 아래로 내려가지 않으므로, 메시지 크기를 늘리거나 길이 검사를 함께 해야 합니다.
- **해결 방안**: 장비 간 통신에는 `cJSON_PrintUnformatted`를 쓰거나, 더 나아가 `cJSON_PrintPreallocated`로 고정 버퍼에 씁니다.

---

### 2.3 `send_cup_completion()` `main.c:1451` / `send_beer_completion()` `main.c:1675`
작업 완료 응답을 만들어 송신 큐에 넣습니다.

#### 결함 G. cJSON 트리와 문자열 모두 해제 안 함 [확인]
- `cJSON_PrintUnformatted` 결과도, `create_..._completion()`으로 만든 cJSON 트리도 해제하지 않습니다. `cJSON_Delete`와 `cJSON_free`가 둘 다 없습니다.
- 트리는 노드마다 malloc(노드 약 40B + 키/값 문자열)을 쓰므로, **완료 한 번에 문자열 길이보다 훨씬 많은 양이 샙니다** [가정].
- 주문(완료) 횟수에 비례해 newlib 힙이 줄고, 결국 결함 B와 같은 고갈로 이어집니다.
- 이력: 2024-12-16 커밋(`e11ff24`)에서는 해제 코드가 주석 처리되어 있었고 [확인], 현재는 해제 코드 자체가 없습니다 [확인].

#### 결함 H. `memcpy` 길이 검사 없음 + NULL 검사 없음 [확인]
- `strlen(completion_string) + 1`만큼 256B `completion_msg.data`에 `memcpy`합니다. 완료 응답이 256B를 넘으면 결함 A와 같은 스택 덮어쓰기가 일어납니다. 이 함수를 부르는 태스크의 스택은 256 words(1KB)로 작습니다.
- 힙이 바닥나 `completion_string`이 NULL이면 `strlen(NULL)`이 됩니다.
- **해결 방안**
```c
/* 새로 작성한 예시 */
cJSON *ack = create_completion(...);
char *s = (ack != NULL) ? cJSON_PrintUnformatted(ack) : NULL;
if (s != NULL && strlen(s) < sizeof(msg.data)) {
    strcpy(msg.data, s);          /* 위에서 길이 확인함 */
    SQenque(&send_queue, &msg);
}
cJSON_free(s);                    /* NULL이어도 안전 */
cJSON_Delete(ack);                /* NULL이어도 안전 */
```

---

### 2.4 `Send_Task()`: `main.c:1899`
10ms 루프에서 큐의 응답을 보내고, 큐가 비면 약 1초마다 STATUS 응답을 만들어 보냅니다.

#### 결함 I. tcpip_thread가 아닌 태스크에서 Raw API 호출 [확인: 코드 / 영향은 가정]
- `send_to_client()` → `send_data_to_client()` → `tcp_write()`, `tcp_output()`을 Send_Task에서 직접 호출합니다. 프로젝트 전체에 `LOCK_TCPIP_CORE()` 호출이 없습니다.
- LwIP Raw API는 **tcpip_thread 안에서만** 호출해야 합니다. 다른 태스크에서 부르면 수신 처리와 동시에 `tcp_pcb`와 pbuf 리스트를 건드려 LwIP 내부 구조가 깨질 수 있습니다.
- 증상은 LwIP 함수 안에서 나는 불규칙한 HardFault로, "자주, 불규칙"이라는 당시 증상과 가장 잘 맞는 후보 중 하나입니다 [가정, 이더넷을 뺀 Renode 환경이라 재현 안 함].
- **해결 방안** (둘 중 하나)
  - `LWIP_TCPIP_CORE_LOCKING=1`로 설정하고 `LOCK_TCPIP_CORE(); tcp_write(...); tcp_output(...); UNLOCK_TCPIP_CORE();`
  - `tcpip_callback(send_fn, ctx)`로 전송을 tcpip_thread에 맡깁니다.
  - Netconn이나 Socket API를 쓰면 이 문제를 구조적으로 피할 수 있습니다.

#### 결함 J. STATUS 응답 NULL 검사 없음 [확인]
- `cJSON_PrintUnformatted(base_response)` 결과를 바로 `strlen`합니다. STATUS 경로는 `cJSON_Delete`와 `cJSON_free`를 하므로 누수는 없지만 [확인: git 이력 전체], 다른 경로의 누수로 힙이 바닥나면 매초 NULL을 쓰게 됩니다.

---

### 2.5 `SQenque()` / `SQdeque()`: `main_queue.c:77, 91`
#### 결함 K. 여러 태스크가 공유하는 큐에 락 없음 [확인: 코드 / 영향은 가정]
- tcpip_thread, executeTask, cupSerialTask가 넣고 Send_Task가 꺼냅니다. 그런데 `current_load++`/`--`와 `end`/`begin` 갱신을 보호하는 장치가 없습니다.
- 동시에 실행되면 개수가 틀어져 메시지가 빠지거나 중복되거나, 빈 칸을 보낼 수 있습니다. 직접 폴트를 만들기보다는 "응답 누락, 이상한 응답"으로 보입니다 [가정].
- **해결 방안**: FreeRTOS 큐 `xQueueCreate(QUEUE_SIZE, sizeof(SendQueueMsg))`로 바꾸거나 mutex로 감쌉니다. FreeRTOS 큐는 50 × 260B = 13KB를 FreeRTOS 힙에서 씁니다.

### 2.6 newlib `malloc` 동시 호출 [확인: 설정 / 영향은 가정]
- tcpip_thread(수신 파싱), executeTask(완료 응답), Send_Task(STATUS)가 모두 cJSON, 즉 `malloc`을 씁니다.
- 그런데 `configUSE_NEWLIB_REENTRANT 0`이고 `__malloc_lock`도 구현하지 않았습니다. 두 태스크가 동시에 malloc/free를 하면 힙의 연결 리스트가 깨져, 이후 **malloc/free 안에서 HardFault**가 납니다.
- **해결 방안** (권장): cJSON을 FreeRTOS 힙으로 돌립니다. 한 번에 세 가지 문제가 해결됩니다.
```c
/* 새로 작성한 예시: main()에서 스케줄러 시작 전에 한 번 */
cJSON_Hooks hooks = { .malloc_fn = pvPortMalloc, .free_fn = vPortFree };
cJSON_InitHooks(&hooks);
```
  1. heap_4는 내부에서 스케줄러를 멈추고 할당하므로 **스레드 안전**합니다.
  2. cJSON 사용량이 `xPortGetFreeHeapSize()`에 나타나 **누수가 보입니다**.
  3. `configUSE_MALLOC_FAILED_HOOK`으로 **고갈 순간을 잡을 수 있습니다**.

---

## 3. 원인을 찾기 위한 테스트 결과

원본 코드는 쓰지 않고, 같은 패턴을 새로 작성한 펌웨어(`f429_repro/`)를 Renode에서 실행했습니다. 이더넷은 UART로 대체했습니다.

### 3.1 모드별 재현 결과
| MODE | 재현한 원본 패턴 | 결과 [확인] | 핵심 근거 |
|---|---|---|---|
| 1 STACK | 큰 지역 버퍼 + float printf | seq 1에서 커널 assert 실패 (`vTaskDelay`) | 커널 변수 `uxSchedulerSuspended`=0xFFFFFFEF로 덮임, 메모리에 printf 복귀 주소 흔적 |
| 2 HEAP | 실패 경로 free 누락 (FreeRTOS 힙) | seq 205에서 MALLOC FAILED | 13,624 − 51 × 264 = 160B (계산과 일치) |
| 3 BUFFER | 고정 버퍼 strcat 누적, 뒤에 함수 포인터 | seq 9에서 UsageFault | `on_sent`=0x7563222C(`",\"cu"`), CFSR=INVSTATE |
| 4 FIXED | 수정본 (정적 버퍼 + snprintf 검사) | seq 745까지 폴트 0건 | 스택·힙 사용량 변화 없음 |
| **5 NEWLIB_LEAK** | **결함 B·G**: 상태 응답(약 493B) free 누락 | **seq 356에서 malloc NULL → 폴트 없이 응답만 사라짐** | FreeRTOS heapFree는 13,616 고정, newlib 힙만 요청당 504B 증가 |
| **6 COPY_OVERFLOW** | **결함 A·F**: 들여쓰기 센서 응답(371B)을 256B에 strcpy | **seq 5에서 함수 복귀 시 UsageFault** | PC=0x0800581C(`Build_And_Send` 복귀부), CFSR=0x00020000(INVSTATE), 프레임 280B < 복사 372B |

### 3.2 "크기만 늘리면 해결되나?" 대조 실험 (당시 했던 조치를 그대로 검증)
| 실험 | 기본 | 크기를 키운 결과 | 결론 |
|---|---|---|---|
| MODE 1 스택 1KB → 2KB | seq 1 assert | 멈춤 없음 (최대 1,576B 사용) | 스택 **깊이** 문제는 키우면 해결 |
| MODE 2 FreeRTOS 힙 15KB → 30KB | seq 205 | seq 437 | 누수는 **늦어질 뿐** |
| MODE 3 스택·힙 2배 | seq 9 | seq 9, 레지스터 동일 | 정적 버퍼 넘침은 **무관** |
| **MODE 5 FreeRTOS 힙 15KB → 30KB** | seq 356 | **seq 325** (newlib limit 179,000 → 163,640B) | newlib 누수는 **오히려 빨라짐** |
| **MODE 6 스택 1KB → 2KB → 4KB** | seq 5 | **seq 5, PC·LR·CFSR 모두 동일** | 지역 버퍼 넘침은 **무관** |

→ 당시 "버퍼·스택·힙 크기를 늘려 테스트"한 방법으로는 결함 A·B를 고칠 수 없었다는 것이 수치로 확인됩니다.

### 3.3 같은 결함, 다른 증상 (MODE 6)
- 응답 모양을 바꾸기 전 버전에서는 같은 `strcpy` 넘침인데도 UsageFault가 아니라 `CPU abort: execute code outside RAM or ROM at 0x226C6D5E`로 멈췄습니다.
- 복귀 주소 자리에 들어간 글자는 `_ml"`(0x226C6D5F)였습니다. bit0이 1이라 Thumb 주소로는 "유효"했고, 그래서 메모리 밖으로 실제로 점프한 것으로 해석합니다.
- 즉 **응답 내용에 따라 같은 버그가 다른 폴트로 나타납니다.** 원인 추적이 어려웠던 이유 중 하나로 볼 수 있습니다 [가정].

### 3.4 Renode와 실제 칩의 차이 (결과 해석 시 주의)
| 항목 | Renode (측정) | 실제 칩 (예상) [가정] |
|---|---|---|
| UsageFault | UsageFault 벡터로 바로 감 (IPSR=6) | UsageFault가 꺼져 있으면 HardFault로 승격 (IPSR=3, HFSR.FORCED) |
| 0x0번지 읽기 | 0 (미매핑) | Flash 별칭 → 벡터 테이블 값 |
| NULL 문자열 처리 | 길이 0 → 조용히 응답 없음 | 쓰레기 데이터 복사 → 이후 폴트 가능 |

---

## 4. 수정 우선순위 체크리스트

| 순위 | 작업 | 대상 결함 | 난이도 |
|---|---|---|---|
| 1 | 응답 복사 전 길이 검사, 응답 해제 | A, B, C | 낮음 |
| 2 | `cJSON_InitHooks`로 cJSON을 FreeRTOS 힙에 연결 | B, G, 2.6 | 낮음 |
| 3 | JSON 키 NULL 검사 + `snprintf` 복사 | E | 낮음 |
| 4 | completion 함수에 `cJSON_Delete`/`cJSON_free` + 길이 검사 | G, H | 낮음 |
| 5 | `cJSON_PrintPreallocated`(고정 버퍼) 또는 Unformatted + 메시지 크기 재산정 | F | 낮음 |
| 6 | `tcp_recved()` 추가 | D | 낮음 |
| 7 | Send_Task 전송을 `LOCK_TCPIP_CORE` 또는 `tcpip_callback`으로 | I | 중간 |
| 8 | 송신 큐를 FreeRTOS 큐로 교체 | K | 중간 |
| 9 | 감지 장치 켜기: `CHECK_FOR_STACK_OVERFLOW 2`, `MALLOC_FAILED_HOOK 1`, CFSR 출력 HardFault 핸들러 | 전체 | 낮음 |

**검증 방법**
- 수정 후 MODE 4처럼 장시간 실행하면서 `xPortGetFreeHeapSize()`와 `uxTaskGetStackHighWaterMark()`를 주기적으로 출력합니다. 2번을 적용하면 cJSON 사용량도 여기에 보입니다.
- **잔량이 일정하고 폴트가 0건이면 통과**입니다.

---

## 5. 확인하지 못한 것
- 원본 빌드의 실제 응답 길이. 지금 값은 형식에 값을 넣어 계산한 것입니다. 호스트 gcc로 `process_json_data()`를 돌려 실측하면 확정됩니다.
- 원본 빌드에서 `app_callback_received()` 지역 변수의 배치. map과 `-fstack-usage`, 디스어셈블리로 확인할 수 있습니다.
- 결함 I(LwIP 락)와 2.6(malloc 동시 호출)은 이더넷이 필요해 재현하지 않았습니다. 실보드(Nucleo-F429ZI)에서 확인해야 합니다.
- 보드에 실제로 올라간 바이너리가 git HEAD와 같은지 여부. 사용자 기억으로는 "STATUS도 해제 안 했다"였지만, git 이력에서는 STATUS를 항상 해제했습니다.
