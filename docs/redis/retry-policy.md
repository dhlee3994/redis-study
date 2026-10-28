# Redis 오류별 재시도 정책: `MISCONF`, `LOADING`, 연결 끊김을 어떻게 다르게 다루는가

> **핵심 질문**: Spring 앱이 Redis에서 `MISCONF`, `LOADING`, 연결 끊김을 만났을 때 각각 다시 보내도 되는가? 셋 중 무엇이 기다리면 풀리고 무엇이 재시도해도 계속 실패하는가? 같은 예외 종류로 오는 오류를 코드에서 어떻게 구분하며, 응답을 받지 못한 쓰기의 이중 반영은 어떻게 막는가? 재시도, 타임아웃, 서킷 브레이커는 어떤 순서로 조합하는가?
>
> **범위**: Spring Boot + Spring Data Redis + Lettuce와 Redis 서버 한 대를 다룹니다. 연결 구성과 명령 타임아웃 값은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에, Sentinel 장애 조치 동안 앱이 겪는 일은 [Redis 장애 조치와 클라이언트](./failover-client.md)에, 멱등 키의 설계는 [Redis 멱등 키](./idempotency-key.md)에 있습니다. 클러스터의 `MOVED`·`ASK`, 긴 스크립트의 `BUSY`, 메모리 한도의 `OOM`, `NOREPLICAS`·`READONLY`는 다루지 않습니다. 재시도·서킷 브레이커 라이브러리의 설정 코드는 다루지 않습니다.

## 배경 지식

### 이 문서의 예시: 주문 API 한 대, Redis 한 대, 세 가지 사건

[Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 장면을 이어서 씁니다. 문서 끝까지 이 장면 하나를 씁니다.

- 주문 API는 Spring Boot 앱이고 요청 스레드는 200개입니다. 사용자 요청은 초당 500건이고, 요청마다 `GET stock:1`로 재고를 읽고 주문이 성립하면 `DECR stock:1`로 재고를 줄입니다. 주문마다 주문 번호(예: `order-42`)가 있습니다.
- 명령 타임아웃(`spring.data.redis.timeout`)은 그 문서에서 고른 100ms이고, 사용자 요청 하나에 쓸 수 있는 시간(바깥 기한)은 1초입니다. 나머지 Lettuce 설정은 기본값입니다(자동 재연결 켜짐).
- 주문 API 안의 알림 작업자는 `BLPOP notify:queue 5`로 알림을 최대 5초 기다리며 꺼내는 반복문입니다. 사용자를 기다리게 하지 않는 백그라운드 작업이고, 명령 타임아웃을 길게 둔 별도 연결 구성을 씁니다(그 문서의 "기다리는 명령은 명령 타임아웃보다 짧게 기다려야 합니다").
- Redis는 정해 둔 조건마다 메모리 전체를 파일(RDB)로 떠 두는 저장 조건이 켜져 있습니다(기본값). AOF(실행한 쓰기를 로그 파일에 덧붙이는 방식)는 켜지 않았습니다. 둘의 원리는 [Redis 영속성](./persistence.md)에 있습니다.

이 장면에 다음 세 사건이 따로 일어난다고 합시다.

| 사건 | 일어난 일 | 이 문서에서 대응하는 오류 |
| --- | --- | --- |
| ① 디스크 가득 참 | RDB 저장이 실패합니다. 운영자가 디스크를 비우기까지 20분이 걸립니다 | `MISCONF` |
| ② Redis 재시작 | 프로세스가 재시작되어 연결이 끊기고, 다시 뜬 Redis가 데이터 파일을 읽는 데 40초가 걸립니다 | 연결 끊김, 이어서 `LOADING` |
| ③ 응답 직전의 끊김 | `order-42`의 `DECR stock:1`을 Redis가 실행해 응답을 보낸 직후, 응답이 주문 API에 닿기 전에 연결이 끊깁니다 | 연결 끊김 |

```text
사용자 500건/초 ─▶ 주문 API (요청 스레드 200개, 명령 타임아웃 100ms, 기한 1초)
                     │  GET stock:1, DECR stock:1 ─────────┐
                     │  알림 작업자: BLPOP notify:queue 5 ──┼─▶ Redis 서버 ── 디스크
                     ◀──────────── 응답 또는 오류 응답 ─────┘
```

화살표는 명령과 응답이 가는 방향입니다. 주문 API 안에서 명령이 어느 연결로 나가는지(공유 연결, 전용 연결)는 그림에서 뺐습니다. 이 문서의 판단은 어느 연결이든 같고, 연결 구분은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에 있습니다.

시간과 요청 수(200개, 500건, 100ms, 1초, 20분, 40초)와 재시도 간격은 계산을 보여 주기 위한 **가상의 숫자**이며, 이 문서에서 재지 않았습니다. 동작은 `근거` 절의 Redis 8.2.10, Lettuce 6.6.0.RELEASE, Spring Data Redis 3.5.13, Spring Framework 6.2.19, Spring Boot 3.5.16 소스로 확인했습니다. 장애를 재현해 확인하지는 않았습니다.

### 재시도와 서킷 브레이커

- **한 줄**: 재시도는 실패한 요청을 다시 보내는 것이고, 서킷 브레이커는 실패가 이어지면 한동안 상대를 부르지 않고 곧바로 실패하는 장치입니다. 다시 보내기 전에는 "그 요청이 실행됐는지 아는가"와 "기다리면 원인이 풀리는가"를 따로 묻습니다. 재시도는 장애 때 부하를 몇 배로 늘리므로 한 계층에서만, 바깥 기한 안에서, 간격을 늘리고(지수 백오프) 무작위로 흩어(지터) 보냅니다. 원리와 조합 순서는 [재시도와 서킷 브레이커](../cs/retry-and-circuit-breaker.md)에 있습니다.
- **이 주제에서는**: 세 오류는 두 질문의 답이 서로 다릅니다. 그래서 같은 재시도 규칙에 넣으면 풀리지 않는 오류를 계속 두드리거나, 쓰기를 두 번 반영합니다.

### 타임아웃과 모르는 결과, 멱등성

- **한 줄**: 명령 타임아웃은 기다리기를 그만둘 뿐 Redis의 실행을 취소하지 않으므로, 타임아웃이 나거나 연결이 끊긴 명령은 실행됐는지 알 수 없습니다([타임아웃](../cs/timeout.md)). 같은 요청을 여러 번 실행해도 한 번 실행한 것과 효과가 같은 성질이 멱등성입니다([경쟁 상태와 원자성](../cs/race-condition-and-atomicity.md)).
- **이 주제에서는**: `GET stock:1`은 다시 보내도 재고를 바꾸지 않습니다. `DECR stock:1`은 두 번 실행되면 재고가 두 번 줄어듭니다. 사건 ③이 이 경우입니다.

### Redis의 오류 응답과 오류 접두어

이 문서에서 오류를 구분하는 열쇠입니다.

- **한 줄**: Redis는 명령을 처리하지 못하면 결과 대신 `-`로 시작하는 오류 응답을 돌려줍니다. `-` 바로 뒤의 첫 대문자 단어(공백 전까지)가 오류의 종류를 나타내며, 이를 오류 접두어라고 부릅니다.
- **비유**: 우체국 창구가 서류를 돌려주며 맨 위에 도장을 찍어 주는 것과 같습니다. "반려(서류 미비)"인지 "반려(시스템 점검 중)"인지 도장만 보면 알고, 그 아래 설명문은 담당자마다 문구가 다를 수 있습니다.
- **실제로는**: 공식 문서(protocol spec, "Simple errors")는 오류 접두어를 "정확한 메시지를 확인하지 않고" 오류의 종류를 알 수 있게 하는 관례라고 설명합니다. 실제로 같은 `MISCONF`라도 원인이 RDB 저장 실패인지, AOF 쓰기 실패인지, 스크립트 안의 쓰기인지에 따라 뒤의 설명문이 다릅니다. 예를 들어 RDB 저장이 실패한 서버는 `-MISCONF Redis is configured to save RDB snapshots, but it's currently unable to persist to disk. ...`를, 데이터를 읽는 중인 서버는 `-LOADING Redis is loading the dataset in memory`를 돌려줍니다. **비유와 다른 점**: 접두어는 프로토콜이 강제하는 형식이 아니라 Redis가 지키는 관례입니다. 그래서 코드는 설명문 전체가 아니라 첫 단어로 비교합니다.
- **이 주제에서는**: `MISCONF`와 `LOADING`은 연결이 멀쩡한 상태에서 Redis가 돌려주는 오류 응답이고, 연결 끊김은 응답 자체가 없는 상태입니다. 이 차이가 아래에서 '그 명령은 실행됐는가'의 답을 가릅니다.

### Redis는 명령을 실행하기 전에 거부 여부를 검사합니다

- **한 줄**: Redis는 명령을 받으면 실행하기 전에 서버 상태(데이터를 읽는 중인가, 디스크 쓰기가 실패했는가 등)와 명령의 성질(쓰기인가, 읽는 중에도 허용되는가)을 검사하고, 걸리면 실행하지 않고 오류 응답만 돌려줍니다.
- **비유**: 공항 탑승구에서 표를 검사하는 것과 같습니다. 검사에 걸린 승객은 비행기에 타지 않습니다. 비행기 안에서 무슨 일이 있었는지는 묻지 않아도 됩니다.
- **실제로는**: Redis의 명령마다 "쓰기 명령", "데이터를 읽는 중에도 허용" 같은 표시가 정해져 있고, 서버는 이 표시와 서버 상태를 맞춰 봅니다. 거부된 명령은 실행되지 않으며, 명령별 통계의 `rejected_calls`(실행 전에 거부된 횟수)가 늘어납니다. **비유와 다른 점**: Lua 스크립트(앱이 아니라 Redis 서버가 실행하는 스크립트로, 안에서 Redis 명령을 차례로 부릅니다)는 탑승구를 통과한 뒤 비행기 안에서 다시 검사받는 경우가 있습니다. 아래 "`MISCONF`: 영속화가 풀릴 때까지 쓰기만 거부합니다"에서 다룹니다.
- **이 주제에서는**: `MISCONF`와 `LOADING`을 받은 명령은 **그 전송에 한해서는** 실행되지 않았다고 확신할 수 있습니다. "그 전송에 한해서"라는 단서는 아래 "거절 응답이 '처음 보낸 명령은 실행되지 않았다'를 뜻하지는 않습니다"에서 다룹니다.

### Lettuce의 자동 재연결과 재전송

- **한 줄**: Lettuce는 연결이 끊긴 것을 알면 스스로 다시 연결하고, 응답을 못 받은 명령과 끊긴 동안 들어온 명령을 쌓아 두었다가 다시 연결되면 보냅니다(아직 명령 타임아웃이 나지 않은 것만). 자세한 동작은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "연결이 끊기면 Lettuce가 응답 못 받은 명령을 다시 보냅니다"에 있습니다.
- **이 주제에서는**: 앱이 재시도 코드를 쓰지 않아도 Lettuce가 연결 수준에서 이미 한 번 재시도하는 계층입니다. 앱의 재시도는 그 위에 겹칩니다.

## 세 오류를 가르는 두 질문: 실행됐는가, 기다리면 풀리는가

예시의 세 사건을 [재시도와 서킷 브레이커](../cs/retry-and-circuit-breaker.md)의 두 질문에 대입하면 다음과 같습니다.

| 오류 | 그 명령은 실행됐는가 | 기다리면 풀리는가 | 무엇이 막히는가 |
| --- | --- | --- | --- |
| `MISCONF` | 그 전송은 실행되지 않음(실행 전 거부) | 원인(디스크·메모리)이 고쳐져 다음 영속화가 성공할 때까지 풀리지 않음. 예시에서 20분 | 쓰기 명령과 `PING`. 읽기는 됨 |
| `LOADING` | 그 전송은 실행되지 않음(실행 전 거부) | 데이터를 다 읽으면 풀림. 예시에서 40초. 데이터가 클수록 김 | 데이터를 다루는 거의 모든 명령(읽기 포함) |
| 연결 끊김·타임아웃 | **모름** | 끊김이면 다시 연결되면 풀림. 끊긴 원인에 따라 수 초~수십 초, 조용히 끊긴 연결은 더 길 수 있음. 연결이 살아 있는 타임아웃은 Lettuce가 다시 연결하지 않으므로, 느려진 원인이 사라져야 풀림 | 그 연결로 나가는 모든 명령 |

이 표에서 정책이 바로 나옵니다.

- **`MISCONF`는 그 전송이 실행되지 않았지만, 다시 보내도 같은 거부를 받습니다.** 요청 경로에서 재시도하지 않습니다.
- **`LOADING`은 그 전송이 실행되지 않았고 기다리면 풀리지만, 풀리는 데 걸리는 시간이 바깥 기한보다 깁니다.** 요청 하나가 기다려서 넘길 수 있는 오류가 아닙니다.
- **연결 끊김은 기다리면 풀리지만, 실행 여부를 모릅니다.** 읽기는 다시 보내도 되고, 쓰기는 멱등할 때만 다시 보냅니다.

"그 전송은"이라는 단서가 중요합니다. 연결이 끊겼다 이어진 뒤에 받은 `MISCONF`·`LOADING`이라면, 끊기기 전의 앞선 전송은 실행됐는지 모릅니다. 아래 "거절 응답이 '처음 보낸 명령은 실행되지 않았다'를 뜻하지는 않습니다"에서 다룹니다.

아래 세 절이 각 오류를 차례로 봅니다.

## `MISCONF`: 영속화가 풀릴 때까지 쓰기만 거부합니다

### 무엇이 거부되고 무엇이 되는가

RDB 저장이 실패한 상태에서 저장 조건이 있고 `stop-writes-on-bgsave-error`가 `yes`(기본값)이면, 또는 AOF를 켠 서버에서 AOF 파일 쓰기나 백그라운드 `fsync`(운영체제에 맡긴 파일 내용을 저장 장치까지 내리는 호출을 별도 스레드에서 하는 것)가 실패했으면, Redis는 쓰기 명령과 `PING`에 `MISCONF` 오류를 돌려줍니다. 단, `appendfsync always`(응답 전에 `fsync`까지 하는 설정)에서는 AOF 쓰기나 `fsync`가 실패하면 `MISCONF`를 돌려주는 대신 Redis 프로세스가 끝납니다. 앱에는 연결 끊김으로 보입니다. 원인과 회복 조건은 [Redis 영속성](./persistence.md)의 "디스크 쓰기가 실패하면 Redis는 쓰기 명령을 거부합니다"에 있습니다. 예시의 사건 ①에서는 다음과 같습니다.

| 명령 | 사건 ① 동안 |
| --- | --- |
| `GET stock:1` | 성공합니다. 읽기 명령이라 검사에 걸리지 않습니다 |
| `DECR stock:1` | `MISCONF`로 거부됩니다 |
| `BLPOP notify:queue 5` | `MISCONF`로 거부됩니다. 원소를 꺼내 리스트를 바꾸므로 쓰기 명령입니다 |
| `PING` | `MISCONF`로 거부됩니다 |
| `INFO` | 성공합니다 |

여기서 놓치기 쉬운 것이 두 가지입니다.

- **`BLPOP`처럼 "꺼내기"도 쓰기입니다.** 알림 작업자는 읽기만 한다고 생각하기 쉽지만, 대기열에서 꺼내는 명령은 데이터를 바꾸므로 함께 거부됩니다.
- **트랜잭션과 스크립트도 거부됩니다.** `MULTI`는 그 뒤에 보낸 명령을 바로 실행하지 않고 연결에 모아 두었다가 `EXEC`에서 한꺼번에 실행하게 하는 명령입니다. `MULTI` 안의 쓰기는 큐에 넣는 순간 거부되어 `EXEC`가 `EXECABORT`로 끝나고, `MULTI`와 `EXEC` 사이에 디스크 오류가 생기면 쓰기가 담긴 `EXEC` 자체가 `EXECABORT`로 거부되며 그 설명에 `MISCONF` 문구가 들어갑니다. 이때 오류 접두어는 `MISCONF`가 아니라 `EXECABORT`이므로, 아래 "원인 예외와 오류 접두어로 구분합니다"의 접두어 비교로는 잡히지 않습니다. 스크립트는 두 갈래입니다. 맨 앞에 플래그를 선언해(`#!lua`) 쓰기 가능으로 둔 스크립트는 시작할 때 `MISCONF`로 거부됩니다. 플래그 없는 `EVAL` 스크립트는 일단 시작되고, 스크립트 안의 첫 쓰기 명령이 `MISCONF`로 실패합니다. Redis는 스크립트가 한 번이라도 쓰기를 한 뒤에는 이 검사를 하지 않으므로, 앞선 쓰기만 반영되고 뒤의 쓰기가 `MISCONF`로 실패하는 일은 없습니다. 스크립트가 `redis.call`로 부른 명령의 오류는 스크립트 오류로 돌아오고, 그 오류 응답도 `MISCONF`로 시작합니다.

### 언제 풀리는가: 원인이 고쳐지고 다음 영속화가 성공해야 풀립니다

`MISCONF`는 시간이 지난다고 풀리지 않습니다. 디스크를 비우거나 메모리를 늘려 원인을 고친 뒤, Redis의 다음 저장 시도(RDB)나 다음 쓰기 시도(AOF)가 성공해야 풀립니다. Redis는 이 재시도를 스스로 하므로 운영자가 원인을 고치면 대부분 저절로 풀립니다. 예외는 [Redis 영속성](./persistence.md)의 "AOF 쓰기 실패"에 있습니다.

판단에 필요한 결론은 이것입니다. **`MISCONF`가 풀리는 시간은 원인이 사라지는 시간입니다.** 잠깐의 메모리 부족으로 RDB 저장 시도가 실패한 경우라면 다음 저장 시도에서 저절로 풀릴 수도 있습니다. 하지만 예시의 가득 찬 디스크처럼 흔한 원인은 사람이 손봐야 사라지므로, 수 분에서 수 시간이 걸릴 수 있습니다. 앱의 재시도는 그 시간을 줄이지 못합니다.

### 예시의 주문 API가 재시도하면

사건 ①에서 주문 API가 `DECR` 실패마다 100ms 간격으로 3번 재시도한다고 합시다.

- 주문 하나가 300ms 동안 스레드를 붙잡고도 결국 실패합니다. 주문 요청이 초당 500건이라면, 이 대기만으로 약 500 × 0.3 = 150개 스레드가 묶입니다. 200개 중 150개입니다. 재시도가 없었다면 `MISCONF`는 왕복 시간만에 돌아오므로 스레드가 거의 묶이지 않았습니다.
- Redis는 평소의 4배인 초당 2,000건의 `DECR`을 받아 모두 거부합니다. 거부된 명령도 메인 스레드가 명령을 읽고 검사하고 오류를 쓰는 일은 그대로 합니다.

재시도가 얻는 것은 없습니다. 20분 동안 모든 시도가 같은 오류를 받기 때문입니다.

### 알림 작업자: 즉시 오는 거부는 반복문을 바쁘게 만듭니다

알림 작업자의 `BLPOP notify:queue 5`는 평소 알림이 없으면 5초씩 기다립니다. 이 기다림이 반복문의 속도를 저절로 조절합니다. 그런데 사건 ①에서는 `BLPOP`이 기다리지 않고 곧바로 `MISCONF`를 받습니다. 작업자가 "실패하면 곧바로 다시 꺼낸다"고 짜여 있으면, 반복문은 왕복 시간마다 한 번씩 Redis에 명령을 보내고 오류 로그를 남깁니다. 왕복이 1ms라면 작업자 스레드 하나가 초당 약 1,000번입니다. 평소 5초에 한 번 보내던 작업자가 장애 중에 가장 바쁜 클라이언트가 됩니다.

그래서 작업자의 실패 처리에는 반드시 간격을 둡니다. `MISCONF`라면 수 초에서 수십 초 간격으로 늘려 가며(지수 백오프) 다시 시도하고, 작업자가 여럿이면 무작위 폭(지터)을 섞습니다.

### Sentinel을 쓴다면: `MISCONF`가 장애 조치를 부를 수 있습니다

이 문서의 예시는 서버 한 대지만, Sentinel 구성이라면 알아 둘 것이 있습니다. Sentinel은 주 서버를 감시하다가 죽었다고 판단하면 복제본 하나를 새 주 서버로 올리는 별도 프로세스입니다. Sentinel은 주 서버에 주기적으로 `PING`을 보내고, 응답이 `PONG`, `LOADING`, `MASTERDOWN` 중 하나일 때만 "살아 있다"고 기록합니다. `MISCONF`는 이 목록에 없습니다. 그래서 디스크가 가득 찬 주 서버는 프로세스가 멀쩡해도 Sentinel에게는 응답하지 않는 서버로 보이고, `down-after-milliseconds`가 지나면 주관적 다운(SDOWN)으로 표시되어 장애 조치가 시작될 수 있습니다. 이것은 소스를 읽은 해석이며 재현하지 않았습니다.

이렇게 되면 앱이 겪는 오류의 종류가 바뀝니다. `MISCONF`가 연결 끊김과 재연결로 바뀌고, 그동안의 일은 [Redis 장애 조치와 클라이언트](./failover-client.md)를 따릅니다. 장애 조치가 디스크 문제를 우회해 쓰기를 되살릴 수도 있지만, 응답한 쓰기를 잃을 수도 있습니다. 주 서버는 복제본에 쓰기가 전달됐는지 기다리지 않고 앱에 응답하므로(비동기 복제), 승격된 복제본에는 마지막 쓰기 일부가 없을 수 있기 때문입니다.

## `LOADING`: 데이터를 읽는 동안 데이터를 다루는 명령을 거부합니다

### 언제 나오는가

Redis는 시작할 때 연결을 받을 준비(포트 열기)를 먼저 하고 그다음 데이터 파일을 읽습니다. 읽는 동안에도 일정량을 읽을 때마다 연결의 명령을 처리하는데, 이때 "데이터를 읽는 중에도 허용" 표시가 없는 명령은 `LOADING` 오류를 받습니다. 그래서 다시 뜬 Redis에 연결은 되지만 명령은 실패하는 구간이 생깁니다. 복제본이 주 서버에서 전체 데이터를 받아 메모리에 올리는 동안에도, 기본 설정(`repl-diskless-load disabled`)에서는 같은 오류를 돌려줍니다.

| 명령 | 데이터를 읽는 동안 |
| --- | --- |
| `GET`, `DECR`, `BLPOP`, `EVAL`, `PING` | `LOADING`으로 거부 |
| `HELLO`, `AUTH`, `SELECT`, `CLIENT SETNAME`, `CLIENT SETINFO` | 처리됨. 연결을 준비하는 명령들입니다 |
| `INFO` | 처리됨 |

이 표에서 두 가지가 따라옵니다.

- **Lettuce의 재연결은 데이터를 읽는 중에도 성공합니다.** Lettuce는 프로토콜 버전을 따로 지정하지 않으면 연결할 때 `HELLO`를 먼저 보내고, Spring Boot와 Spring Data Redis는 이 버전을 지정하지 않습니다. `HELLO`는 읽는 중에도 허용되므로 연결이 맺어지고, Lettuce는 쌓아 둔 명령을 곧바로 보냅니다. 그 명령들은 즉시 `LOADING`을 받습니다. 그래서 사건 ②에서 앱이 보는 오류는 "처음 몇 초는 연결 끊김(타임아웃), 그 뒤 40초는 `LOADING`"으로 바뀝니다.
- **Spring Boot의 Redis 헬스 체크는 `UP`을 보고합니다.** 단일 서버 구성에서 Spring Boot의 Redis 헬스 체크는 `INFO`가 성공하면 `UP`으로 판단합니다. `INFO`는 읽는 중에도, `MISCONF` 상태에서도 거부되지 않으므로, 헬스 체크의 연결과 `INFO`가 제시간에 끝나는 한 모든 주문이 실패하는 사건 ②의 읽기 구간과 모든 차감이 실패하는 사건 ①에도 헬스 체크는 `UP`입니다. 읽기 구간 내내 `INFO`가 제시간에 응답하는지는 재현하지 않아 확인하지 못했습니다. 헬스 체크만 보고 Redis가 정상이라고 판단하면 안 됩니다.

### 얼마나 걸리는가

읽는 시간은 데이터 크기, 파일 형식, CPU, 저장 장치에 따라 다르므로 운영 환경에서 잽니다([Redis 영속성](./persistence.md)의 "읽는 순서"). 읽는 동안에는 `INFO persistence`의 `loading`이 1이고, `loading_loaded_perc`(읽은 비율)와 `loading_eta_seconds`(남은 시간 추정)로 진행을 볼 수 있습니다.

### 요청 경로에서는 기다려서 넘길 수 없습니다

`LOADING`은 기다리면 풀립니다. 그런데 "기다리면"의 주어가 중요합니다. 예시의 바깥 기한은 1초이고, 읽기는 40초 걸립니다. 요청 하나가 100ms 간격으로 몇 번 다시 보내도 40초를 넘기지 못합니다. 재시도는 `MISCONF` 절의 계산처럼 스레드만 묶고 Redis가 받는 요청을 몇 배로 늘립니다. 메인 스레드는 데이터를 읽는 사이사이에 이 거부 응답도 만들어야 합니다.

그래서 요청 경로의 정책은 다음과 같습니다.

- **요청 안에서 재시도하지 않거나, 남은 기한 안에서 한 번만 합니다.** 데이터가 작아 읽기가 수백 ms 안에 끝나는 서버라면 한 번의 재시도가 구할 수 있습니다. 운영 환경에서 잰 읽기 시간을 보고 정합니다.
- **기다리는 일은 서킷 브레이커가 맡습니다.** `LOADING`이 이어지면 브레이커가 열려 요청은 Redis를 부르지 않고 바로 대체 처리로 갑니다. 반열림 상태에서 시험 호출이 성공하면, 곧 데이터를 다 읽었다는 뜻이므로 다시 닫습니다.
- **대체 처리는 데이터의 성격이 정합니다.** 원본이 DB에 있는 캐시라면 DB에서 읽을 수 있지만, 모든 요청이 한꺼번에 DB로 가면 DB가 버티지 못할 수 있습니다([캐시 스탬피드](./cache-stampede.md)). 예시의 재고처럼 Redis 값으로 판단하는 데이터라면 "잠시 후 다시 시도"로 주문을 거절하는 것이 맞습니다.

백그라운드 작업(알림 작업자)은 사용자를 기다리게 하지 않으므로, 지수 백오프와 지터로 간격을 늘려 가며 읽기가 끝날 때까지 다시 시도해도 됩니다.

## 연결 끊김과 타임아웃: 결과를 모르는 실패

### 사건 ③: 앱이 재시도하지 않아도 두 번 반영될 수 있습니다

사건 ③에서 Redis는 `order-42`의 `DECR stock:1`을 실행했고(재고 10 → 9), 응답이 오는 도중에 연결이 끊겼습니다. 주문 API 쪽에서는 다음 둘 중 하나가 일어납니다.

| Lettuce가 다시 연결하는 시점 | 일어나는 일 |
| --- | --- |
| 명령 타임아웃(100ms) 안 | Lettuce가 같은 `DECR`을 다시 보내 재고가 8이 됩니다. 다시 보낸 명령의 응답까지 처음 보낸 때부터 100ms 안에 오면 앱은 응답 8 하나만 받고 성공으로 처리합니다. 응답이 그보다 늦으면 재고는 8인데 앱은 `QueryTimeoutException`을 봅니다 |
| 명령 타임아웃 뒤 | `DECR`은 `QueryTimeoutException`으로 끝납니다. 재고는 9인데 앱은 실행 여부를 모릅니다. 여기서 앱이 다시 보내면 8이 됩니다 |

어느 쪽이든 쓰기를 그대로 다시 보내면 이중 반영이 생깁니다. 첫 줄은 앱 코드에 재시도가 없어도 생깁니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "왜 중복 실행이 되는가").

### 이중 반영은 쓰기를 멱등하게 만들어 막습니다

판단은 하나입니다. **값을 바꾸는 명령은 Lettuce가 다시 보내든 앱이 다시 보내든 한 번만 반영되게 만든 뒤에만 다시 보냅니다.** 예시라면 `DECR` 대신, 서버에서 실행되는 Lua 스크립트 하나 안에서 "`order-42`를 이미 처리했는가"를 확인하고, 처음이면 차감과 처리 기록을 함께 남기며, 이미 처리했으면 기록해 둔 결과를 돌려줍니다. 기록은 다시 보낼 수 있는 기간보다 오래 남깁니다. 이 설계와 기록의 수명, 장애 때의 한계는 [Redis 멱등 키](./idempotency-key.md)에, 확인과 차감을 한 스크립트로 묶어야 하는 이유는 [Redis 원자성](./atomicity.md)의 "원자성이 보장하지 않는 것"에 있습니다.

멱등하게 만들 수 없는 쓰기라면 다시 보내지 않습니다. 결과를 모른다는 사실을 남기고(예: 주문을 "확인 필요"로 표시), 원본(주문 DB)으로 나중에 맞춥니다. 재고처럼 원본에서 다시 계산할 수 있는 값은 "남은 재고 + 성공 주문 수 = 처음 재고" 같은 불변식으로 점검합니다([Redis 영속성](./persistence.md)의 "Spring에서 확인할 것").

### 꺼내는 명령은 두 번 반영되는 대신 잃습니다

쓰기의 재전송이 늘 "두 번 반영"으로 나타나지는 않습니다. 알림 작업자의 `BLPOP`을 Redis가 실행해 알림 하나를 꺼냈는데 응답이 오는 도중에 연결이 끊기면, 그 알림은 리스트에서 이미 빠졌지만 작업자에게는 오지 않습니다. Lettuce가 다시 보낸 `BLPOP`은 다음 알림을 꺼냅니다. **첫 알림은 사라집니다.** 이것은 재시도 정책으로는 막을 수 없고, 꺼낸 원소를 바로 지우지 않는 구조(처리 중 목록으로 옮기기, Streams 소비자 그룹이 받았지만 처리 확인 전인 메시지를 따로 적어 두는 장부(PEL))가 필요합니다([Redis Streams](./streams.md)).

### 읽기는 다시 보내도 되지만, 한 번이면 충분합니다

`GET stock:1`은 다시 보내도 재고를 바꾸지 않습니다. 다만 연결이 끊긴 동안에는 Lettuce가 다시 연결할 때까지 명령이 쌓이기만 하므로, 같은 요청 안에서 여러 번 다시 보내도 같은 연결을 기다릴 뿐입니다. 남은 기한 안에서 한 번이면 충분합니다. Redis가 다시 뜨기까지 수 초 이상 걸리는 사건 ②에서는 그 한 번도 실패하므로, 실패가 이어지면 서킷 브레이커가 열려 기다림 자체를 없앱니다.

### 거절 응답이 '처음 보낸 명령은 실행되지 않았다'를 뜻하지는 않습니다

이 문서에서 가장 잘못 읽기 쉬운 지점입니다. 앞에서 `LOADING`과 `MISCONF`를 받은 명령은 실행되지 않았다고 했습니다. 그런데 사건 ②를 따라가 봅시다.

1. 주문 API가 `order-77`의 `DECR`을 보냈고, Redis가 실행했습니다(재고 9 → 8).
2. 응답을 보내기 전에 Redis 프로세스가 재시작됩니다. 주문 API는 응답을 받지 못했고, Lettuce는 그 `DECR`을 쌓아 둡니다.
3. 다시 뜬 Redis가 데이터를 읽는 동안 Lettuce가 다시 연결합니다(`HELLO`는 허용). 쌓아 둔 `DECR`을 다시 보내고, `LOADING`을 받습니다.
4. 주문 API는 `LOADING`을 보고 "실행되지 않았다"고 판단합니다.

4번의 판단은 **다시 보낸 전송**에 대해서만 맞습니다. 1번의 실행이 영속화돼 있었다면 다시 뜬 Redis의 재고는 8입니다. 이때 주문 API가 "실행되지 않았으니 안전하다"며 데이터를 다 읽은 뒤 `DECR`을 다시 보내면 7이 됩니다. 앱이 받는 예외는 처음 보낸 명령이 거절된 경우와 같은 `LOADING`이므로, 예외만으로는 둘을 구분할 수 없습니다.

그래서 결론은 이렇습니다. **연결이 끊겼다가 이어지는 장애에서는 `LOADING`·`MISCONF`를 받은 쓰기도 "결과를 모르는 쓰기"와 같은 규칙으로 다룹니다.** 멱등하게 만든 쓰기라면 다시 보내도 되고, 아니라면 다시 보내지 않습니다. 멱등한 쓰기라면 이 구분 자체가 필요 없다는 점이 멱등성을 먼저 확보하라는 이유입니다.

## Spring에서 오류를 구분하는 법

### 예외 종류와 Spring의 분류

Lettuce는 Redis의 오류 응답을 접두어에 따라 예외로 만들고, Spring Data Redis는 이를 Spring의 데이터 접근 예외로 바꿉니다. 일반 호출(파이프라인·트랜잭션이 아닌 호출)에서 예시의 상황을 따라가면 다음과 같습니다.

| 상황 | Lettuce 예외 | Spring Data Redis가 던지는 예외 | Spring의 분류 | 서버에서 실행됐는가 |
| --- | --- | --- | --- | --- |
| `LOADING` | `RedisLoadingException` | `RedisSystemException`("Error in execution") | 영구적(NonTransient) | 그 전송은 실행되지 않음 |
| `MISCONF` | `RedisCommandExecutionException` | `RedisSystemException`("Error in execution") | 영구적 | 그 전송은 실행되지 않음 |
| 명령 타임아웃(끊긴 동안 쌓여 있다 끝난 경우 포함) | `RedisCommandTimeoutException` | `QueryTimeoutException` | 일시적(Transient) | 모름 |
| 연결을 얻지 못함(연결 맺기 실패, 풀 고갈) | `RedisConnectionException`, 풀의 예외 등 | `RedisConnectionFailureException`("Unable to connect to Redis") | 영구적 | 명령을 보내기 전 |
| 끊긴 동안 새 명령 거절(`DisconnectedBehavior.REJECT_COMMANDS`로 바꾼 경우) | `RedisException` | `RedisSystemException`("Redis exception") | 영구적 | 새 명령은 보내지 않음 |

`RedisSystemException`과 `RedisConnectionFailureException`은 `NonTransientDataAccessException` 아래에, `QueryTimeoutException`은 `TransientDataAccessException` 아래에 있습니다.

`RedisConnectionFailureException`의 "명령을 보내기 전"에는 단서가 있습니다. 이 예외의 대표 경로는 연결 공급자에게서 연결을 얻는 단계이고, 이때는 명령을 보내지 않았습니다. 다만 Spring Data Redis는 명령을 실행하던 중의 네트워크 채널 예외도 같은 예외로 바꾸므로, 쓰기라면 이 예외만으로 "보내지 않았다"고 단정하지 않습니다.

### 함정: Spring의 '일시적·영구적' 분류는 Redis의 재시도 판단과 거꾸로입니다

Spring의 설명에 따르면 일시적 예외는 "애플리케이션의 개입 없이 다시 시도하면 성공할 수 있는" 실패이고, 영구적 예외는 "원인을 고치지 않으면 다시 시도해도 실패하는" 실패입니다. 재시도 도구에서 "일시적 예외만 재시도"로 설정하기 쉬운 이유입니다. 그런데 위 표를 이 기준에 대면 어긋납니다.

| 예외 | Spring의 분류 | 이 문서의 판단 | 어긋나면 생기는 일 |
| --- | --- | --- | --- |
| `QueryTimeoutException` | 일시적 → 재시도 | 결과를 모름. 쓰기는 멱등할 때만 | `DECR`을 다시 보내 이중 반영 |
| `RedisSystemException`(`LOADING`) | 영구적 → 재시도 안 함 | 기다리면 풀림. 백그라운드에서는 재시도 대상 | 알림 작업자가 재시작 뒤 회복하지 못하고 멈춤 |
| `RedisSystemException`(`MISCONF`) | 영구적 → 재시도 안 함 | 재시도 안 함 | 맞음. 다만 같은 예외 종류의 `LOADING`과 구분해야 함 |
| `RedisConnectionFailureException` | 영구적 → 재시도 안 함 | 보내기 전 실패라면 다시 연결된 뒤 재시도해도 됨 | 짧은 재연결로 구할 수 있던 요청을 버림 |

**재시도 대상은 예외 클래스의 상위 분류가 아니라, 이 문서의 두 질문으로 직접 고릅니다.** 특히 `QueryTimeoutException`을 "일시적이니 재시도"로 두면 쓰기의 이중 반영이 바로 생깁니다.

### 원인 예외와 오류 접두어로 구분합니다

`LOADING`과 `MISCONF`는 같은 `RedisSystemException`이고 바깥 메시지도 같습니다. 그래서 원인 예외(`getCause()`)를 꺼내 봅니다.

- **`LOADING`**: 원인이 Lettuce의 `RedisLoadingException`인지 봅니다. Lettuce가 접두어로 이미 종류를 나눠 둔 경우라 클래스로 구분할 수 있습니다.
- **`MISCONF`**: Lettuce에 전용 예외가 없으므로, 원인이 `RedisCommandExecutionException`이고 메시지가 `MISCONF`로 시작하는지 봅니다. 메시지 전체를 비교하지 않습니다. 위 "Redis의 오류 응답과 오류 접두어"에서 본 것처럼 원인(RDB, AOF, 스크립트)에 따라 뒤의 설명문이 다르기 때문입니다.
- **파이프라인은 원인 사슬이 끊깁니다.** 파이프라인은 명령 여러 개를 응답을 기다리지 않고 이어 보낸 뒤 응답을 한꺼번에 받는 방식이고, 결과도 명령마다 따로 나옵니다. Spring Data Redis는 파이프라인에서 Redis 오류를 받은 명령마다 원인 예외 없이 Redis의 오류 문자열(예: `LOADING Redis is loading ...`)만 메시지로 담은 `InvalidDataAccessApiUsageException`을 만들어 결과 목록의 그 자리에 넣고, 첫 오류를 원인으로 한 `RedisPipelineException`을 던집니다. 그래서 `RedisLoadingException`을 원인 사슬에서 찾는 방법이 통하지 않습니다. 파이프라인에서는 결과 목록의 각 예외 메시지를 첫 단어로 분류합니다. 파이프라인 전체가 명령 타임아웃을 넘기면 원인은 `QueryTimeoutException`입니다.
- **트랜잭션은 접두어가 `EXECABORT`로 바뀝니다.** 위 "무엇이 거부되고 무엇이 되는가"에서 본 것처럼, `MULTI`/`EXEC` 안의 쓰기가 영속화 오류로 거부되면 앱이 받는 것은 `EXEC`의 `EXECABORT` 오류입니다. 파이프라인 밖에서 부른 `exec()`라면 Spring Data Redis는 이를 일반 호출과 같은 `RedisSystemException`으로 던지고, 원인 예외는 메시지가 `EXECABORT`로 시작하는 `RedisCommandExecutionException`입니다(파이프라인 안에서 `exec()`를 부르면 바로 앞 항목의 파이프라인 규칙을 따릅니다). 그래서 `MISCONF` 접두어 비교에 걸리지 않습니다. 설명문은 언제 거부됐는가에 따라 갈립니다. Redis는 실행 전 거부 검사를 명령마다 하고, `EXEC`도 그 검사를 받습니다. 이때 대기열에 쓰기가 하나라도 남아 있으면 `EXEC`는 쓰기 명령으로 검사됩니다.
  - `MULTI`와 `EXEC` 사이에 디스크 오류가 생기면, 쓰기는 이미 대기열에 들어가 있으므로 `EXEC` 자체가 거부되고 설명문에 원래 오류가 들어갑니다(`EXECABORT Transaction discarded because of: MISCONF ...`).
  - 이미 오류 상태일 때 쓰기를 대기열에 넣었다면, 그 쓰기는 대기열에 들어가지 못하고 거부됩니다. 대기열에 쓰기가 없으니 `EXEC`는 검사를 통과하고 실패 표시 때문에 버려집니다. `MISCONF`는 그 명령의 응답에만 있고 `EXEC`의 설명문은 "Transaction discarded because of previous errors."뿐입니다. Spring은 대기열에 넣을 때 응답을 기다리지 않고 `exec()`도 명령별 응답을 돌려주지 않으므로, 앱은 이 예외만으로 사유를 알 수 없습니다(소스를 읽은 해석이며 재현하지 않았습니다). 없는 명령이나 인자 개수 오류 같은 프로그래밍 오류도 같은 설명문을 받습니다.
  - `LOADING`은 주로 이 경로로 옵니다. 재시작 뒤 다시 연결하면 `MULTI`는 데이터를 읽는 동안에도 허용되지만, 그 뒤의 쓰기는 대기열에 넣을 때 `LOADING`으로 거부되므로 앱은 사유 없는 `EXECABORT`를 받습니다.

  분류 함수는 원인 메시지가 `EXECABORT`로 시작하면 먼저 "그 전송의 트랜잭션 명령은 하나도 실행되지 않았다"로 분류합니다. 그래서 그 전송만 놓고 보면 다시 보내도 이중 반영은 생기지 않습니다. 다만 그사이 연결이 끊겼다 이어졌다면 위 "거절 응답이 '처음 보낸 명령은 실행되지 않았다'를 뜻하지는 않습니다"처럼 앞선 전송이 실행됐을 수 있습니다. 설명문에 `MISCONF`나 `LOADING`이 있으면 그 오류와 같은 정책을 씁니다. 사유가 없는 `EXECABORT`는 프로그래밍 오류일 수도 있어 다시 보내도 같은 결과일 수 있으므로, 요청 경로에서는 재시도하지 않습니다. 원인은 `INFO`의 `errorstats`에서 `MISCONF`나 `LOADING`이 함께 늘었는지, `INFO`의 `loading`이 1인지로 확인합니다. 반대로 `EXEC` 안의 한 명령이 실행 중 오류(예: `WRONGTYPE`)를 내면 Spring은 그 명령의 오류를 원인으로 던지는데, 이때는 다른 명령이 이미 반영됐을 수 있습니다. 이 경우의 결과 처리는 [Redis 원자성](./atomicity.md)의 "Spring에서 확인할 것"에 있습니다.
- **캐시 추상화**: `@Cacheable` 같은 캐시 추상화는 오류를 캐시 오류 처리기로 넘깁니다([`@Cacheable`이 캐시하지 않거나, 엉뚱한 값을 주거나, 너무 오래 둘 때](./spring-cacheable.md)의 `CacheErrorHandler` 설명). 이 경로는 넘겨받은 예외의 원인 사슬을 훑게 만듭니다.

분류 함수 하나를 두고, 재시도 판단과 서킷 브레이커의 "실패로 셀 것" 판단을 모두 이 함수의 결과로 정하면 두 장치가 같은 기준을 씁니다.

## 재시도, 타임아웃, 서킷 브레이커를 어떤 순서로 조합하는가

### 이미 있는 계층부터 셉니다

주문 API의 `DECR` 하나에는 앱이 아무것도 더하지 않아도 이미 재시도 계층이 있습니다.

```text
사용자의 앱·게이트웨이의 재시도 (있다면)
└─ 주문 API의 재시도 (이 문서에서 정하는 것)
   └─ Lettuce의 재전송 (끊겼다 이어지면 응답 못 받은 명령을 다시 보냄)
      └─ Lettuce의 재연결 (간격을 늘려 가며 다시 연결)
```

선은 포함 관계입니다. 아래 계층은 위 계층의 시도 한 번 안에서 일어납니다. 연결 끊김에는 Lettuce가 이미 재연결과 재전송으로 대응하므로, 주문 API가 연결 문제 때문에 재시도를 여러 번 더할 이유는 적습니다. 바깥 계층(사용자의 앱, 게이트웨이)의 재시도는 [재시도와 서킷 브레이커](../cs/retry-and-circuit-breaker.md)의 "계층마다 재시도하면 곱해집니다"처럼 곱해지므로, 주문 API가 "잠시 후 다시 시도" 응답을 돌려줄 때는 바깥 계층이 곧바로 다시 보내지 않게 정합니다.

### 주문 API의 조합

안쪽부터 바깥쪽으로 다음 순서로 겹칩니다. 순서의 이유는 [재시도와 서킷 브레이커](../cs/retry-and-circuit-breaker.md)의 "어떤 순서로 겹치는가"에 있습니다.

| 층 | 예시의 값 | Redis에서 정할 것 |
| --- | --- | --- |
| 시도별 타임아웃 | 명령 타임아웃 100ms | `spring.data.redis.timeout`. 풀을 쓴다면 풀 앞의 기다림(`max-wait`)도 상한을 둠 |
| 서킷 브레이커 | Redis 호출을 감쌈. 시도마다 결과를 셈 | 무엇을 실패로 셀지(아래) |
| 재시도 | 남은 기한 안에서 최대 한 번, 짧은 간격에 지터 | 분류 함수가 "다시 보내도 됨"이라고 한 것만 |
| 바깥 기한 | 1초 | 재시도 직전에 남은 시간이 "기다릴 간격 + 명령 타임아웃"보다 긴지 확인 |

예시에서 최악의 경우를 계산해 봅니다. `GET`과 멱등하게 만든 차감 스크립트가 각각 한 번씩 타임아웃 뒤 재시도하면, 100ms × 4번 + 간격 두 번(각 50ms)으로 500ms입니다. 바깥 기한 1초 안에 DB 저장과 응답 만들기가 함께 들어가야 하므로 이보다 늘리기 어렵습니다.

### 서킷 브레이커는 무엇을 실패로 세는가

| 실패 | 셀 것인가 | 이유 |
| --- | --- | --- |
| `QueryTimeoutException`, `RedisConnectionFailureException` | 셉니다 | Redis나 네트워크가 아프다는 신호입니다 |
| `LOADING` | 셉니다 | 서버 전체가 지금 데이터 명령을 받지 못합니다. 열린 브레이커가 읽기가 끝나기를 대신 기다립니다 |
| `MISCONF` | 쓰기 브레이커에서만 셉니다 | 읽기는 정상입니다. 읽기와 쓰기가 한 브레이커를 쓰면, 차감 실패만으로 브레이커가 열려 멀쩡한 `GET stock:1`까지 막힙니다 |
| 그 밖의 명령 오류(`WRONGTYPE`, 문법 오류 등) | 세지 않습니다 | Redis는 정상이고, 고칠 곳은 앱의 코드나 데이터입니다 |

예시라면 **읽기 브레이커와 쓰기 브레이커를 나눕니다.** 각 브레이커는 자기가 감싼 호출의 결과만 셉니다. 연결 문제(타임아웃, 연결 실패, `LOADING`)는 공유 연결 하나로 나가는 모든 명령에 나타나지만, 예시의 주문은 `GET`을 먼저 부르고 `GET`이 실패하면 `DECR`까지 가지 않습니다. 그래서 연결 문제에서는 읽기 브레이커가 먼저 열리고, 쓰기 브레이커에는 실패가 거의 쌓이지 않습니다. 읽기 브레이커가 열리면 주문도 `GET` 단계에서 막히므로 결과는 같습니다. 반면 `MISCONF`는 `GET`이 성공하고 `DECR`만 실패하므로 쓰기 브레이커만 엽니다. 사건 ①에서 쓰기 브레이커가 열리면, 주문 API는 차감을 부르지 않고 곧바로 "지금은 주문을 받을 수 없습니다"로 응답하고, 재고 조회는 계속 Redis에서 읽습니다.

나누지 않는 쪽은 단순합니다. 브레이커가 하나라 설정과 관찰할 대상이 적습니다. 그 대신 사건 ①에서 재고 조회까지 막힙니다. 읽기를 막아도 괜찮은 서비스라면 하나로 두고, 읽기를 지켜야 한다면 나눕니다.

### 오류별 정책을 한 표로

| 오류 | 요청 경로(주문 API) | 백그라운드(알림 작업자) | 함께 할 일 |
| --- | --- | --- | --- |
| `MISCONF` | 재시도하지 않습니다. 쓰기 브레이커에 셉니다. 읽기는 계속합니다 | 수 초~수십 초 간격의 지수 백오프와 지터로 다시 시도합니다 | 즉시 경보. 디스크 가득 참처럼 사람이 원인을 고쳐야 풀리는 경우가 많습니다 |
| `LOADING` | 재시도하지 않거나 남은 기한 안에서 한 번만 합니다. 두 브레이커 모두 실패로 봅니다(각자 감싼 호출에서) | 지수 백오프와 지터로 읽기가 끝날 때까지 다시 시도합니다 | `loading_eta_seconds`로 진행을 봅니다. 재시작 뒤라면 연결 끊김의 규칙도 함께 적용합니다 |
| 타임아웃·연결 끊김(읽기) | 남은 기한 안에서 한 번 다시 보냅니다. 두 브레이커 모두 실패로 봅니다(각자 감싼 호출에서) | 지수 백오프와 지터 | 조용히 끊긴 연결이면 재연결이 일어나지 않을 수 있습니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)) |
| 타임아웃·연결 끊김(쓰기) | 멱등하게 만든 쓰기만 다시 보냅니다. 아니면 "확인 필요"로 남깁니다 | 멱등하게 만든 쓰기만 다시 보냅니다. 꺼내는 명령은 잃을 수 있습니다 | 원본으로 정합성 점검 |

## 데이터와 요청이 늘면 무엇이 달라지는가

- **데이터가 늘면 `LOADING`이 길어집니다.** 재시작 한 번이 데이터가 클수록 길어지는 시간만큼의 장애가 됩니다. 운영 환경에서 잰 읽기 시간이 바깥 기한보다 길어지는 순간부터, 요청 경로의 재시도는 `LOADING`을 넘기지 못합니다. 그 시간을 줄이려면 앱 정책이 아니라 서버 구성(복제본과 장애 조치, 파일 형식)을 봐야 합니다.
- **요청이 늘면 재시도가 만드는 요청도 함께 늘어납니다.** `MISCONF`와 `LOADING`은 즉시 돌아오므로, 실패마다 정한 횟수만큼 재시도하고 브레이커나 기한이 막지 않는다면, 재시도하는 명령(예시의 `DECR`)의 시도가 처음 시도까지 더해 (1 + 재시도 횟수) × 초당 요청 수만큼 Redis에 닿습니다. 초당 500건에서 3번 재시도는 초당 2,000건, 1,000건이면 4,000건입니다. 이 양에 상한을 두려면 시도 횟수가 아니라 재시도 예산(재시도가 아닌 원래 요청 수 대비 비율)으로 막습니다.
- **인스턴스가 늘면 회복 순간의 몰림이 커집니다.** 주문 API 인스턴스마다 같은 간격으로 재시도하거나 브레이커가 같은 열림 시간을 쓰면, 회복 직후 모든 인스턴스가 같은 박자로 몰립니다. 간격과 반열림 시각에 지터를 넣습니다.
- **끊긴 동안 쌓이는 명령도 요청에 비례합니다.** 명령 타임아웃이 길수록 끊긴 동안 끝나지 않은 명령이 많이 남고, 다시 연결되는 순간 한꺼번에 나갑니다. 재시작 직후라면 이 명령들이 한꺼번에 `LOADING`을 받습니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "데이터와 요청이 늘면 무엇이 달라지는가").

## 흔한 오해 모아 보기

| 오해 | 실제 | 바로잡은 곳 |
| --- | --- | --- |
| `MISCONF`는 일시적이니 잠깐 기다렸다 재시도하면 된다 | 원인이 사라지고 다음 영속화가 성공해야 풀립니다. 흔한 원인(가득 찬 디스크)은 사람이 손봐야 사라집니다. 재시도는 스레드만 묶습니다 | 언제 풀리는가: 원인이 고쳐지고 다음 영속화가 성공해야 풀립니다 |
| 대기열에서 꺼내기만 하는 작업자는 `MISCONF`와 무관하다 | `BLPOP`은 쓰기 명령이라 함께 거부되고, 기다리지 않고 즉시 실패해 반복문이 바빠집니다 | 알림 작업자: 즉시 오는 거부는 반복문을 바쁘게 만듭니다 |
| `LOADING`은 기다리면 풀리니 요청 안에서 재시도하면 된다 | 읽기 시간이 바깥 기한보다 길면 요청 안의 재시도로는 넘기지 못합니다. 기다림은 서킷 브레이커가 맡습니다 | 요청 경로에서는 기다려서 넘길 수 없습니다 |
| 재시작 중에는 연결도 안 된다 | Redis는 포트를 먼저 열고 데이터를 읽습니다. `HELLO`가 허용되어 Lettuce는 다시 연결되고, 그 뒤 명령이 `LOADING`을 받습니다 | 언제 나오는가 |
| 헬스 체크가 `UP`이면 Redis는 정상이다 | Spring Boot의 Redis 헬스 체크는 `INFO`만 보므로 `LOADING`·`MISCONF` 중에도 `UP`입니다 | 언제 나오는가 |
| `LOADING`·`MISCONF`를 받았으니 그 쓰기는 실행되지 않았다 | 그 전송만 실행되지 않았습니다. Lettuce가 끊긴 뒤 다시 보낸 전송이었다면 처음 전송은 실행됐을 수 있습니다 | 거절 응답이 '처음 보낸 명령은 실행되지 않았다'를 뜻하지는 않습니다 |
| 앱에 재시도가 없으면 쓰기는 한 번만 반영된다 | Lettuce가 다시 연결한 뒤 응답 못 받은 명령을 다시 보냅니다 | 사건 ③: 앱이 재시도하지 않아도 두 번 반영될 수 있습니다 |
| Spring의 `TransientDataAccessException`만 재시도하면 안전하다 | 결과를 모르는 `QueryTimeoutException`은 일시적으로, 기다리면 풀리는 `LOADING`은 영구적으로 분류됩니다 | 함정: Spring의 '일시적·영구적' 분류는 Redis의 재시도 판단과 거꾸로입니다 |
| 오류 메시지 전체로 비교하면 정확하다 | 같은 `MISCONF`도 원인에 따라 설명문이 다릅니다. 첫 단어로 비교합니다 | Redis의 오류 응답과 오류 접두어 |
| Redis 호출 전체를 서킷 브레이커 하나로 감싸면 된다 | `MISCONF`만으로 브레이커가 열려 멀쩡한 읽기까지 막힙니다 | 서킷 브레이커는 무엇을 실패로 세는가 |

## 근거

**Redis**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c)
  - `processCommand`: 실행 전에 디스크 오류(`writeCommandsDeniedByDiskError`)면 쓰기 명령(`CMD_WRITE`, 쓰기가 담긴 `EXEC` 포함)과 `pingCommand`를 거부하고 주 서버에서 온 명령(`obey_client`)은 예외. `server.loading && !server.async_loading`이고 명령에 `CMD_LOADING`이 없으면 `shared.loadingerr`로 거부. `EXEC`의 `is_write_command`·`is_denyloading_command`는 대기열 명령의 플래그(`mstate.cmd_flags`·`mstate.cmd_inv_flags`)로 판정. "대기열에 쓰기가 하나라도 남아 있으면 `EXEC`는 쓰기 명령으로 검사됩니다" "Redis는 명령을 실행하기 전에 거부 여부를 검사합니다"와 `MISCONF`·`LOADING`의 거부 대상
  - `rejectCommand`·`rejectCommandSds`: `flagTransaction`으로 트랜잭션을 실패 표시, `rejected_calls` 증가, `EXEC`이면 `execCommandAbort`. "`rejected_calls`가 늘어납니다", "`MULTI` 안의 쓰기는 큐에 넣는 순간 거부"
  - `writeCommandsDeniedByDiskError`: RDB(`stop_writes_on_bgsave_err && saveparamslen > 0 && lastbgsave_status == C_ERR`), AOF `write` 오류, 백그라운드 `fsync` 오류. `writeCommandsGetDiskErrorMessage`: RDB면 `shared.bgsaveerr`, AOF면 "MISCONF Errors writing to the AOF file: %s". "원인에 따라 설명문이 다르다"
  - `createSharedObjects`: `shared.loadingerr`("-LOADING Redis is loading the dataset in memory"), `shared.bgsaveerr`("-MISCONF Redis is configured to save RDB snapshots, but it's currently unable to persist to disk. ...")
  - `main`: `initListeners()` 뒤에 `loadDataFromDisk()`. "포트를 먼저 열고 데이터를 읽는다"
  - `genRedisInfoString`: `loading`, `loading_loaded_perc`, `loading_eta_seconds`, `errorstats`의 `errorstat_<접두어>`. `genRedisInfoStringCommandStats`: `rejected_calls`
- [`src/aof.c`](https://github.com/redis/redis/blob/8.2.10/src/aof.c): `flushAppendOnlyFile`(`appendfsync always`에서 `write`·`fsync` 실패면 `exit(1)`). "`appendfsync always`에서는 … 프로세스가 끝납니다"
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `processEventsWhileBlocked`(읽는 도중 이벤트 처리), `afterErrorReply`(오류 응답의 첫 단어로 오류 통계를 셈)
- [`src/rdb.c`](https://github.com/redis/redis/blob/8.2.10/src/rdb.c): `rdbLoadProgressCallback`이 `loading-process-events-interval-bytes`마다 `processEventsWhileBlocked` 호출. `rdbLoadWithEmptyFunc`가 `loadingSetFlags`로 `server.loading`을 켬("emptyDbCallback() may yield back to event-loop to reply -LOADING")
- [`src/replication.c`](https://github.com/redis/redis/blob/8.2.10/src/replication.c): `readSyncBulkPayload`(디스크 기반 적재는 `rdbLoadWithEmptyFunc`로 읽음, 비동기 적재는 `repl-diskless-load swapdb`이고 복제 ID가 같을 때만). "복제본이 전체 데이터를 받아 올리는 동안에도 기본 설정에서는 같은 오류"
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `repl-diskless-load` 기본 `disabled`, `stop-writes-on-bgsave-error` 기본 `yes`
- [`src/script.c`](https://github.com/redis/redis/blob/8.2.10/src/script.c): `scriptPrepareForRun`(플래그를 선언하고 `no-writes`가 없는 스크립트는 디스크 오류면 시작 시 `MISCONF`, "Writable scripts are blocked"), `scriptVerifyWriteCommandAllow`(스크립트 안의 첫 쓰기를 디스크 오류면 거부, 이미 쓰기를 했으면(`SCRIPT_WRITE_DIRTY`) 검사하지 않음)
- [`src/script_lua.c`](https://github.com/redis/redis/blob/8.2.10/src/script_lua.c): `luaPushErrorBuff`(명령 오류의 접두어를 오류 코드로 보존), `luaCallFunction`(스크립트 오류 응답을 그 코드로 시작). "스크립트 오류 응답도 `MISCONF`로 시작"
- [`src/multi.c`](https://github.com/redis/redis/blob/8.2.10/src/multi.c): `execCommand`(실패 표시된 트랜잭션은 `shared.execaborterr`), `execCommandAbort`("-EXECABORT Transaction discarded because of: %s"). `src/server.c` `createSharedObjects`의 `shared.execaborterr`("-EXECABORT Transaction discarded because of previous errors."). "트랜잭션은 접두어가 `EXECABORT`로 바뀝니다"
- [`src/sentinel.c`](https://github.com/redis/redis/blob/8.2.10/src/sentinel.c): `sentinelPingReplyCallback`(`PONG`, `LOADING`, `MASTERDOWN`만 정상 응답으로 기록), `sentinelCheckSubjectivelyDown`(대기 중인 `PING`의 경과 시간이 `down_after_period`를 넘으면 SDOWN). "`MISCONF`가 장애 조치를 부를 수 있다"(소스 해석)
- [`src/commands/*.json`](https://github.com/redis/redis/tree/8.2.10/src/commands): `get`(`READONLY`), `decr`·`blpop`(`WRITE`), `ping`(`LOADING` 없음), `eval`(`LOADING` 없음), `hello`·`auth`·`select`·`client-setname`·`client-setinfo`·`info`(`LOADING`). "데이터를 읽는 동안" 표. `multi`·`exec`(`LOADING`). "`MULTI`는 데이터를 읽는 동안에도 허용되지만"

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`)

- [protocol spec](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/reference/protocol-spec.md): "Simple errors"(첫 대문자 단어가 오류 접두어, Redis의 관례이며 RESP 형식의 일부가 아님, 정확한 메시지 없이 오류 종류를 알게 함)

**Lettuce**(태그 [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE), 커밋 `643bd4709ce2c7a2c263dd50ed0320ce0f972c22`)

- [`internal/ExceptionFactory.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/internal/ExceptionFactory.java): `createExecutionException`(`BUSY`, `NOSCRIPT`, `LOADING`, `READONLY`만 전용 예외, 나머지는 `RedisCommandExecutionException`). [`RedisLoadingException.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisLoadingException.java)
- [`protocol/AsyncCommand.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/AsyncCommand.java): 오류 응답이면 `createExecutionException(getOutput().getError())`로 완료
- [`RedisHandshake.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisHandshake.java): 프로토콜 버전이 지정되지 않으면 `tryHandshakeResp3`로 `HELLO`부터 보냄. [`ClientOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ClientOptions.java): `protocolVersion`은 빌더에서 지정하지 않으면 비어 있음(`getConfiguredProtocolVersion`)
- [`output/MultiOutput.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/output/MultiOutput.java): `EXEC` 자체의 오류 응답은 `EXEC` 명령의 오류로 남김(`setError`의 `discarded == null` 분기). 결과 배열 안의 오류는 `complete`에서 `ExceptionFactory.createExecutionException`으로 만든 예외를 그 자리에 넣음
- [`protocol/DefaultEndpoint.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/DefaultEndpoint.java): 끊긴 동안 거절이면 `RedisException("Currently not connected. Commands are rejected.")`
- 자동 재연결·재전송의 동작은 Spring 앱의 Redis 연결과 명령 타임아웃 문서의 근거(같은 태그의 `DefaultEndpoint`, `CommandHandler`, `CommandExpiryWriter`)를 따름

**Spring Data Redis**(태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13), 커밋 `42619b203148bae01235fe14f3cff3e171d93ebf`)

- [`LettuceConnection.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnection.java): `closePipeline`(오류로 끝난 명령마다 Redis 오류 문자열만 메시지로 담은 `InvalidDataAccessApiUsageException`을 결과 목록에 넣고, 첫 오류를 원인으로 `RedisPipelineException`을 던짐. 전체가 기한을 넘기면 `QueryTimeoutException`을 원인으로). "파이프라인은 원인 사슬이 끊깁니다". MULTI 중의 명령은 응답을 기다리지 않고 결과 자리만 쌓음(`doInvoke`의 `isQueueing` 분기), `exec`는 `EXEC` 오류를 `convertLettuceAccessException`으로 바꿔 던짐. [`TransactionResultConverter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/convert/TransactionResultConverter.java): 결과 배열에서 처음 만난 예외를 변환해 던짐. "트랜잭션은 접두어가 `EXECABORT`로 바뀝니다"
- [`LettuceExceptionConverter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceExceptionConverter.java): `RedisCommandExecutionException` → `RedisSystemException("Error in execution", ex)`(원인 예외가 있으면 그것을 변환), `ChannelException`·`RedisConnectionException` → `RedisConnectionFailureException`, `RedisCommandTimeoutException` → `QueryTimeoutException`, 그 밖의 `RedisException` → `RedisSystemException("Redis exception")`
- [`LettuceConnectionFactory.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionFactory.java): `ExceptionTranslatingConnectionProvider.translateException`(연결을 얻지 못하면 `RedisConnectionFailureException("Unable to connect to Redis")`). [`LettucePoolingConnectionProvider.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettucePoolingConnectionProvider.java): 풀에서 빌리지 못하면 `PoolException("Could not get a resource from the pool")`
- [`RedisSystemException.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/RedisSystemException.java)(`UncategorizedDataAccessException` 상속), [`RedisConnectionFailureException.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/RedisConnectionFailureException.java)(`DataAccessResourceFailureException` 상속)

**Spring Framework**(태그 [`v6.2.19`](https://github.com/spring-projects/spring-framework/tree/v6.2.19), 커밋 `6214eae8bd02c2ed7ab382bb8d16a9cc6de49522`. Spring Boot 3.5.16 `gradle.properties`의 `springFrameworkVersion`)

- `spring-tx`의 [`TransientDataAccessException.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-tx/src/main/java/org/springframework/dao/TransientDataAccessException.java)(클래스 설명 "might be able to succeed when the operation is retried without any intervention"), [`NonTransientDataAccessException.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-tx/src/main/java/org/springframework/dao/NonTransientDataAccessException.java)("a retry of the same operation would fail unless the cause of the Exception is corrected"), [`QueryTimeoutException.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-tx/src/main/java/org/springframework/dao/QueryTimeoutException.java)(`TransientDataAccessException` 상속), [`DataAccessResourceFailureException.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-tx/src/main/java/org/springframework/dao/DataAccessResourceFailureException.java)(`NonTransientDataAccessResourceException` → `NonTransientDataAccessException`), [`UncategorizedDataAccessException.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-tx/src/main/java/org/springframework/dao/UncategorizedDataAccessException.java)(`NonTransientDataAccessException` 상속)

**Spring Boot**(태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16), 커밋 `0566f6933049aca6bc5ffc6d559fffade9cd2e0c`)

- [`RedisHealthIndicator.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator/src/main/java/org/springframework/boot/actuate/data/redis/RedisHealthIndicator.java), [`RedisHealth.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator/src/main/java/org/springframework/boot/actuate/data/redis/RedisHealth.java): 클러스터가 아니면 `serverCommands().info()`가 성공할 때 `up`
- [`LettuceConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/LettuceConnectionConfiguration.java): `ClientOptions`를 만들며 프로토콜 버전을 지정하지 않음(Spring Data Redis 3.5.13 소스에도 `protocolVersion` 지정 없음)

## 실무 포인트

### 중요도별 점검 항목

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | 오류 분류 함수 하나를 두고 재시도와 서킷 브레이커가 함께 쓴다 | `LOADING`과 `MISCONF`가 같은 예외로 오고, Spring의 일시적·영구적 분류는 Redis의 판단과 어긋납니다. 일반 호출은 원인 사슬에서 `RedisLoadingException`과 `MISCONF` 접두어를 찾고, 파이프라인은 결과 목록의 예외 메시지를, 트랜잭션은 `EXECABORT` 설명문을 따로 봅니다 |
| 필수 | `QueryTimeoutException`을 쓰기의 재시도 대상에 넣지 않는다(멱등하게 만든 쓰기 제외) | 결과를 모르는 쓰기를 다시 보내면 이중 반영이 생깁니다 |
| 필수 | 값을 바꾸는 쓰기를 멱등하게 만든다 | Lettuce가 연결 수준에서 이미 재전송하므로, 앱의 재시도가 없어도 중복이 생깁니다. 재시작 직후의 `LOADING`도 처음 전송의 실행 여부를 알려 주지 않습니다 |
| 필수 | `MISCONF`는 요청 경로에서 재시도하지 않고 즉시 경보한다 | 원인이 사라져야 풀리고, 흔한 원인은 사람이 손봐야 사라집니다 |
| 필수 | 백그라운드 반복문(대기열 작업자, 스트림 소비자)의 실패 처리에 백오프와 지터를 둔다 | 즉시 오는 오류에서 간격 없는 반복문이 가장 바쁜 클라이언트가 됩니다 |
| 필수 | 재시도는 바깥 기한에서 남은 시간 안에서만 한다 | 요청 경로의 재시도는 수백 ms짜리 실패만 넘깁니다 |
| 알면 유리 | 읽기·쓰기 서킷 브레이커를 나눈다 | `MISCONF`에서 읽기를 지킵니다 |
| 알면 유리 | 운영 환경에서 재시작 뒤 데이터를 읽는 시간을 잰다 | `LOADING`을 요청 안에서 한 번이라도 재시도할지, 브레이커에 맡길지의 기준입니다 |
| 알면 유리 | Sentinel 구성이면 `MISCONF`가 장애 조치로 바뀔 수 있음을 장애 대응 절차에 넣는다 | 디스크 경보와 장애 조치 알림이 함께 오면 같은 원인일 수 있습니다 |
| 참고 | `DisconnectedBehavior.REJECT_COMMANDS`를 쓴다면 끊김이 `RedisSystemException`으로 온다는 점을 분류 함수에 넣는다 | 같은 `RedisSystemException`이지만 원인은 `LOADING`·`MISCONF`가 아닌 `RedisException`입니다 |

### 관찰할 것

- **`INFO errorstats`**: `errorstat_MISCONF`, `errorstat_LOADING`의 증가로 어떤 오류가 얼마나 나가는지 서버에서 봅니다. 앱의 재시도가 장애 중 요청을 몇 배로 늘리는지도 이 증가 속도와 정상 요청 수를 비교해 봅니다.
- **`INFO commandstats`의 `rejected_calls`**: 명령별로 실행 전에 거부된 횟수입니다. `DECR`만 늘면 `MISCONF`, 모든 명령이 늘면 `LOADING`일 가능성이 큽니다.
- **`INFO persistence`**: `loading`, `loading_eta_seconds`(읽기 진행), `rdb_last_bgsave_status`, `aof_last_write_status`(`MISCONF` 원인). 원인은 [Redis 영속성](./persistence.md)의 "관찰할 것"에 있습니다.
- **앱 쪽 지표**: 오류 분류 함수의 결과별 횟수, 시도 수 ÷ 요청 수, 서킷 브레이커의 상태 변화. 헬스 체크의 `UP`만으로 판단하지 않습니다.

### 증상과 확인 순서

| 증상 | 의심할 원인 | 확인 | 대응과 부작용 |
| --- | --- | --- | --- |
| 쓰기만 실패하고 조회는 됨, 헬스 체크는 `UP` | `MISCONF` | 원인 예외 메시지의 첫 단어, `errorstat_MISCONF`, `INFO persistence` | 디스크·메모리 원인을 고칩니다. `stop-writes-on-bgsave-error no`로 급히 풀면 RDB에 새 변경이 남지 않는 상태를 놓칠 수 있고, AOF 쓰기 실패의 거부는 이 설정으로 풀리지 않습니다 |
| 재시작 직후 모든 명령이 수십 초 실패, 연결은 됨 | `LOADING` | `errorstat_LOADING`, `loading_eta_seconds` | 기다립니다. 요청 경로의 재시도를 늘리면 스레드만 묶입니다. 브레이커가 열려 있는지 봅니다 |
| 장애 중 Redis가 받는 명령 수가 평소보다 많음 | 즉시 오는 오류에 대한 앱 재시도, 간격 없는 백그라운드 반복문 | 앱의 시도 수 ÷ 요청 수, 작업자 로그의 오류 빈도 | 재시도를 줄이고 백오프·지터를 둡니다 |
| 장애 뒤 재고가 주문 수보다 많이 줄어 있음 | 결과를 모르는 쓰기의 재전송(Lettuce 또는 앱) | 같은 주문 번호의 차감 기록, 재시도 로그 | 원본으로 재고를 다시 맞추고, 차감을 멱등하게 만듭니다 |
| 장애 뒤 알림 일부가 사라짐 | 응답을 못 받은 `BLPOP`의 재전송 | 끊김 시각과 사라진 알림의 시각 | 꺼낸 원소를 바로 지우지 않는 구조로 바꿉니다 |
| 재시작 뒤 알림 작업자가 회복하지 않음 | "영구적 예외는 재시도 안 함" 규칙에 `LOADING`이 걸림 | 작업자가 멈춘 시각의 예외 원인 | 분류 함수로 `LOADING`을 재시도 대상에 넣습니다 |
| 디스크 경보 뒤 장애 조치가 일어남 | Sentinel이 `MISCONF` 응답을 무응답으로 봄(소스 해석) | Sentinel 로그의 `+sdown`, 디스크 경보 시각 | 장애 조치 뒤 비동기 복제로 잃은 쓰기를 원본으로 점검합니다 |

## 말로 설명하기

### 30초 버전

세 오류는 "실행됐는가"와 "기다리면 풀리는가"가 다릅니다. `MISCONF`를 받은 전송은 실행되지 않았지만 디스크 같은 원인이 사라질 때까지 계속 실패하므로 재시도하지 않고 경보합니다. `LOADING`을 받은 전송도 실행되지 않았고 기다리면 풀리지만 수십 초가 걸려 요청 안에서는 못 넘기므로, 서킷 브레이커가 대신 기다립니다. 연결 끊김은 실행 여부를 모르므로 읽기만 다시 보내고, 쓰기는 멱등하게 만든 뒤에만 다시 보냅니다. Lettuce가 이미 스스로 다시 보내기 때문입니다.

### 2분 버전

Redis는 명령을 실행하기 전에 거부 여부를 검사합니다. 그래서 `MISCONF`와 `LOADING`을 받은 전송은 실행되지 않았습니다. 차이는 풀리는 방식입니다. `MISCONF`는 영속화가 실패한 동안 쓰기와 `PING`만 거부하고 읽기는 처리하며, 원인을 고친 뒤 다음 영속화가 성공해야 풀립니다. `LOADING`은 재시작이나 전체 동기화로 데이터를 읽는 동안 데이터 명령을 거의 다 거부하고, 다 읽으면 풀립니다. Redis는 포트를 먼저 열므로 Lettuce는 다시 연결되고, 쌓아 둔 명령이 곧바로 `LOADING`을 받습니다.

연결 끊김은 다릅니다. 응답 직전에 끊기면 Lettuce가 다시 연결한 뒤 같은 명령을 보내므로, 앱에 재시도가 없어도 `DECR`이 두 번 반영될 수 있습니다. 재시작 직후라면 다시 보낸 명령이 `LOADING`을 받아 "실행 안 됨"처럼 보이지만, 처음 전송은 실행됐을 수 있습니다. 그래서 쓰기는 주문 번호로 확인과 차감을 한 스크립트에 묶어 멱등하게 만듭니다.

Spring에서는 `LOADING`과 `MISCONF`가 같은 `RedisSystemException`으로 오므로, 원인 사슬에서 `RedisLoadingException`과 `MISCONF` 접두어를 찾습니다. Spring의 일시적·영구적 분류에 기대면 타임아웃의 쓰기를 재시도하고 `LOADING`은 버리는 거꾸로 된 정책이 됩니다. 조합은 안쪽부터 명령 타임아웃, 서킷 브레이커, 재시도, 바깥 기한 순서이고, 브레이커는 읽기와 쓰기를 나눠 `MISCONF`가 읽기까지 막지 않게 합니다.

### 예상 질문과 꼬리 질문

- **Q. `MISCONF`가 났을 때 왜 재시도하지 않나요? 디스크가 금방 비워질 수도 있잖아요.**
  - 풀리는 시간이 원인이 사라지는 시간이고, 가득 찬 디스크 같은 흔한 원인은 사람이 손봐야 하므로 바깥 기한(초 단위)과 규모가 다릅니다. 요청 안의 재시도는 그동안 같은 오류를 받으며 스레드를 묶고 Redis에 거부할 명령을 몇 배로 보냅니다. 본문 "예시의 주문 API가 재시도하면".
  - **꼬리: 그럼 디스크가 고쳐진 걸 앱은 어떻게 아나요?**
    - 쓰기 서킷 브레이커의 반열림 시험 호출이 성공하면 압니다. Redis는 원인이 고쳐지면 다음 영속화를 스스로 시도하고, 성공하면 쓰기를 다시 받습니다. AOF의 백그라운드 `fsync` 실패처럼 저절로 풀린다고 장담할 수 없는 경우가 있어 서버 쪽 확인(`INFO persistence`)도 함께 봅니다.
  - **꼬리: 읽기는 계속 된다는데, 재고 조회만 살려 두는 게 의미가 있나요?**
    - 상품 화면과 재고 표시는 계속 동작하고 주문만 "잠시 후"로 막힙니다. 그래서 읽기·쓰기 브레이커를 나눌지가 판단이 됩니다. 읽기를 막아도 되는 서비스라면 하나로 둬 단순하게 갑니다.
- **Q. `LOADING`은 기다리면 풀린다면서 왜 요청 안에서 기다리지 않나요?**
  - 읽기에 수십 초가 걸리면 바깥 기한 1초 안의 재시도로는 넘기지 못하고, 스레드와 Redis 부하만 늘어납니다. 기다림은 요청이 아니라 서킷 브레이커가 요청 바깥에서 맡습니다. 데이터가 작아 읽기가 수백 ms라면 한 번의 재시도는 의미가 있으므로, 운영 환경에서 읽기 시간을 재고 정합니다. 본문 "요청 경로에서는 기다려서 넘길 수 없습니다".
  - **꼬리: 재시작 중이면 연결이 안 될 텐데 `LOADING`은 언제 보나요?**
    - Redis는 포트를 먼저 열고 데이터를 읽으며, 읽는 사이사이 명령을 처리합니다. Lettuce의 연결 준비 명령(`HELLO`)은 읽는 중에도 허용되므로 연결이 맺어지고, 그 뒤 데이터 명령이 `LOADING`을 받습니다. 프로세스가 뜨기 전의 몇 초는 연결 끊김으로 보입니다.
  - **꼬리: 헬스 체크로 걸러 내면 되지 않나요?**
    - Spring Boot의 Redis 헬스 체크는 단일 서버에서 `INFO`만 보는데, `INFO`는 읽는 중에도 `MISCONF` 중에도 성공해 `UP`을 보고합니다. 헬스 체크만으로는 두 상태를 알 수 없습니다.
- **Q. `LOADING`을 받았으면 그 `DECR`은 실행되지 않은 것 아닌가요? 다 읽은 뒤 다시 보내면 되잖아요.**
  - 받은 `LOADING`은 마지막 전송에 대한 답입니다. 재시작 직전에 처음 전송이 실행되고 응답만 못 받았다면, Lettuce가 다시 연결한 뒤 같은 명령을 다시 보내 `LOADING`을 받은 것일 수 있습니다. 처음 실행이 영속화됐다면 다시 보낸 `DECR`이 두 번째 차감이 됩니다. 예외만으로는 구분되지 않으므로 끊김이 끼는 장애에서는 이런 쓰기도 결과를 모르는 쓰기로 다룹니다. 본문 "거절 응답이 '처음 보낸 명령은 실행되지 않았다'를 뜻하지는 않습니다".
- **Q. 앱에 재시도 코드가 없는데도 재고가 두 번 줄었습니다. 어디부터 보나요?**
  - Lettuce의 자동 재연결과 재전송입니다. 응답을 못 받은 명령을 다시 연결한 뒤 보내므로, 실행 직후 끊기면 같은 `DECR`이 다시 실행됩니다. 끊김 시각과 같은 주문의 차감 기록을 맞춰 봅니다. 설정으로 이 재전송을 막으려면 자동 재연결을 꺼야 해서 대가가 크고, 쓰기를 멱등하게 만드는 것이 근본 대책입니다.
  - **꼬리: 명령 타임아웃을 짧게 하면 재전송이 줄지 않나요?**
    - 타임아웃이 먼저 난 명령은 다시 보내지 않으므로 줄기는 합니다. 하지만 다시 연결이 타임아웃보다 빠르면 재전송이 일어나므로 막아 주지는 않습니다.
- **Q. 멱등하게 만든 차감도 장애 조치에서는 기록을 잃지 않나요?**
  - 잃을 수 있습니다. 이 문서는 서버 한 대를 다루므로 장애 조치에서 기록이 어떻게 되는지는 다루지 않습니다. 그 경우의 결과와 조건은 [Redis 장애 조치와 클라이언트](./failover-client.md)의 "앱이 다시 보내는 경우"와 [Redis 멱등 키](./idempotency-key.md)에 있습니다.
- **Q. Spring의 `TransientDataAccessException`만 재시도하도록 설정하면 왜 안 되나요?**
  - `QueryTimeoutException`이 일시적으로 분류되어, 결과를 모르는 쓰기를 다시 보내게 됩니다. 반대로 `LOADING`은 `RedisSystemException`으로 와 영구적으로 분류되어, 기다리면 풀리는데도 백그라운드 작업이 재시도를 포기합니다. Spring의 분류는 "다시 하면 성공할 수 있는가"만 보고 "실행됐는가"는 보지 않기 때문입니다. 본문 "함정: Spring의 '일시적·영구적' 분류는 Redis의 재시도 판단과 거꾸로입니다".
  - **꼬리: `RedisConnectionFailureException`은요?**
    - 대표 경로는 연결을 얻는 단계의 실패라 명령을 보내기 전입니다. 그런데 영구적으로 분류되어 기본 규칙으로는 재시도하지 않습니다. 다만 같은 예외가 실행 중 채널 예외에서도 나올 수 있으므로, 쓰기라면 보내지 않았다고 단정하지 않습니다.
- **Q. `MISCONF`와 `LOADING`을 코드에서 어떻게 구분하나요?**
  - 둘 다 `RedisSystemException`이고 바깥 메시지도 같으므로 원인 사슬을 봅니다. `LOADING`은 Lettuce가 `RedisLoadingException`으로 따로 만들고, `MISCONF`는 전용 예외가 없어 `RedisCommandExecutionException`의 메시지가 `MISCONF`로 시작하는지 봅니다. 원인에 따라 설명문이 다르므로 메시지 전체가 아니라 첫 단어로 비교합니다. 캐시 추상화처럼 다른 예외에 감싸여 오면 원인 사슬을 따라갑니다. 단, 파이프라인에서는 Spring Data Redis가 원인 예외 없이 오류 문자열만 담은 예외를 만들므로, 결과 목록의 각 예외 메시지를 첫 단어로 봅니다. 트랜잭션은 영속화 오류도 `EXECABORT`로 오므로 그 설명문에 `MISCONF`가 있는지 봅니다. 대기열에 넣을 때 거부된 경우에는 설명문에 사유가 없어, 하나도 실행되지 않았다는 것만 확정하고 사유는 `errorstats`의 `MISCONF`·`LOADING`과 `INFO`의 `loading`으로 확인합니다. 재시작 직후의 `LOADING`은 대부분 이 경우로 옵니다.
- **Q. 재시도와 서킷 브레이커는 어느 쪽이 바깥인가요?**
  - 재시도가 바깥, 브레이커가 안쪽, 그 안에 명령 타임아웃입니다. 브레이커가 시도마다 세야 장애를 빨리 알고, 열리면 재시도의 다음 시도도 Redis에 닿지 않습니다. 재시도는 브레이커의 "열려 있음"을 다시 보내지 않습니다. 가장 바깥에는 바깥 기한을 두어 남은 시간이 다음 간격과 명령 타임아웃의 합보다 짧으면 멈춥니다. 본문 "주문 API의 조합".
  - **꼬리: Lettuce의 재연결도 재시도인데, 앱 재시도와 어떻게 겹치나요?**
    - 연결 끊김에는 Lettuce가 이미 재연결과 재전송으로 대응하므로, 앱이 연결 문제 때문에 여러 번 재시도를 더하면 같은 연결을 기다리는 요청만 늘어납니다. 앱의 재시도는 남은 기한 안의 한 번으로 두고, 긴 장애는 브레이커에 맡깁니다.
- **Q. 서킷 브레이커가 `MISCONF`도 실패로 세야 하나요?**
  - 쓰기 브레이커에서만 셉니다. 읽기와 쓰기가 한 브레이커면 차감 실패만으로 열려 정상인 조회까지 막힙니다. 타임아웃, 연결 실패, `LOADING`은 서버나 연결 전체의 문제이므로 두 브레이커 모두 실패로 봅니다. 각 브레이커는 자기가 감싼 호출의 결과만 셉니다. `WRONGTYPE` 같은 명령 오류는 Redis가 정상이므로 세지 않습니다.
- **Q. 대기열 작업자는 사용자 요청이 아니니 재시도를 무한히 해도 되지 않나요?**
  - 기한이 없으니 오래 재시도하는 것은 괜찮지만, 간격이 없으면 안 됩니다. 평소 `BLPOP`은 알림이 없으면 5초씩 기다려 속도가 조절되는데, `MISCONF`·`LOADING`에서는 즉시 실패하므로 간격 없는 반복문이 왕복 시간마다 한 번, 왕복 1ms라면 초당 약 1,000번을 보냅니다. 지수 백오프로 간격을 늘리고 작업자 여럿이면 지터로 흩습니다. 본문 "알림 작업자: 즉시 오는 거부는 반복문을 바쁘게 만듭니다".
  - **꼬리: 연결이 끊기면 작업자에게는 무엇이 문제인가요?**
    - 꺼낸 알림의 응답을 못 받으면 그 알림은 리스트에서 빠졌는데 작업자에게 오지 않습니다. Lettuce가 다시 보낸 `BLPOP`은 다음 알림을 꺼냅니다. 이중 반영이 아니라 유실이라, 재시도 정책이 아니라 꺼낸 원소를 바로 지우지 않는 구조로 막습니다.
- **Q. Sentinel 구성에서 디스크가 가득 차면 무엇이 달라지나요?**
  - Sentinel은 `PING`의 응답이 `PONG`, `LOADING`, `MASTERDOWN`일 때만 살아 있다고 기록하는데, 디스크 문제의 주 서버는 `PING`에 `MISCONF`를 답합니다. 그래서 `down-after-milliseconds` 뒤 장애 조치가 시작될 수 있습니다. 소스를 읽은 해석이며 재현하지 않았습니다. 이때 앱이 보는 오류는 `MISCONF`에서 연결 끊김과 재연결로 바뀌고, 비동기 복제로 응답한 쓰기를 잃을 수 있습니다.
