# Redis 지연 진단: "Redis가 느리다"는 신고를 어떤 순서로 가르는가

> **핵심 질문**: Spring 앱에서 Redis 호출이 느리다는 신고를 받으면, 그 시간이 앱 안의 대기, 네트워크, Redis 서버 안의 기다림, 명령 실행, 서버 밖의 멈춤 중 어디서 생겼는지 어떤 순서로 가르는가? 각 단계에서 어떤 지표를 보고, 그 숫자가 무엇을 재고 무엇을 빼는지 어떻게 읽는가?
>
> **범위**: Spring Boot + Spring Data Redis + Lettuce 앱이 Redis 서버 한 대(또는 클러스터 노드 하나)를 부르는 경우의 진단 순서를 다룹니다. 원인 하나하나의 원리와 대책은 이미 다른 문서에 있으므로, 이 문서는 "어느 원인인지 가르는 법"에 집중하고 원리는 한두 문장과 링크로만 짚습니다.
>
> **출처와 판단의 구분**: 각 도구가 무엇을 재는지는 "근거"에 적은 소스와 공식 문서로 확인했습니다. 소스를 따라가 추론했지만 실행해 보지 않은 내용에는 그 자리에 "소스로 추론"이라고 적었습니다. 진단 순서 자체는 이 문서의 판단입니다. 문서의 시간 숫자는 모두 흐름을 보여 주기 위한 **가상의 숫자**이며, 이 문서에서 잰 값이 아닙니다.

## 배경 지식

### 이 문서의 예시: 재고 조회 한 번이 80ms 걸렸다

문서 끝까지 다음 신고 하나를 씁니다.

- 주문 API는 인스턴스 네 대로 돌고, 각 인스턴스는 Spring Boot 기본 구성(연결 풀 없이 공유 연결 하나)으로 Redis 서버 한 대에 붙어 있습니다.
- 주문 API는 `RedisTemplate`으로 `GET stock:1`(재고 값 하나 읽기)을 보냅니다. 평소에는 1ms 안에 끝납니다.
- 신고: "14시 02분부터 주문 API에서 재고 조회가 가끔 80ms씩 걸린다."

이 문서가 할 일은 이 **80ms가 어디서 생겼는지** 가르는 것입니다. 같은 80ms가 전혀 다른 다섯 곳에서 생길 수 있고, 곳마다 대책이 다르기 때문입니다. 뒤에서 이 다섯 곳을 "장면"이라고 부르며 같은 신고에 대입합니다.

| 장면 | 80ms가 생긴 곳 | 같은 시각에 함께 생긴 일(가상) |
| --- | --- | --- |
| 장면 1 | 앱 안 | 주문 API 인스턴스 한 대에서 JVM이 잠시 멈췄거나, 연결 풀 앞에서 기다렸다 |
| 장면 2 | 같은 연결의 앞 응답 | 같은 인스턴스의 상품 목록 기능이 같은 공유 연결로 큰 Hash를 통째로 읽었다 |
| 장면 3 | 서버 안 기다림 | 관리자 배치가 다른 연결로 원소가 아주 많은 키를 통째로 읽어 메인 스레드를 70ms 잡았다 |
| 장면 4 | 서버 밖 멈춤 | Redis가 RDB 저장을 위해 `fork`를 불렀고, 그 호출이 70ms 걸렸다 |
| 장면 5 | 네트워크 | 앱 서버와 Redis 서버 사이의 네트워크가 잠시 느려졌다 |

### 호출 한 번의 시간은 여러 구간의 합입니다

`GET stock:1` 한 번이 앱에서 시작해 값으로 돌아오기까지 거치는 구간을 차례로 나눕니다. 이 문서는 이 구간 번호로 "어느 지표가 어디를 재는가"를 설명합니다.

```text
[앱 JVM]                                       [네트워크]        [Redis 서버 프로세스]
 ① 연결 얻기 ─▶ ② 직렬화 ─▶ ③ 보내기 ──────────── 가는 길 ──────▶ ④ 차례 기다림
                                                                    │
 ⑧ 역직렬화 ◀─ ⑦ 받기·해석 ◀──────────────────── 오는 길 ◀────── ⑤ 실행 ─▶ ⑥ 응답 보내기
```

화살표는 실행 순서이자 데이터가 움직이는 방향입니다. 도형의 길이는 실제 시간과 관계없습니다. Spring의 캐시 추상화, 재시도, 클러스터의 리다이렉트는 그림에서 뺐습니다. 이 셋은 호출 횟수를 늘리는 쪽이라, 한 번의 호출 안에서 시간이 어디로 갔는지를 가르는 이 문서의 질문과는 축이 다릅니다.

| 구간 | 무엇을 하는가 | 누가, 어디서 |
| --- | --- | --- |
| ① 연결 얻기 | 공유 연결이면 거의 없음. 풀을 쓰면 연결을 빌릴 때까지 기다림 | 앱의 요청 스레드 |
| ② 직렬화 | 키와 값을 바이트로 바꿈. 네트워크로는 바이트만 보낼 수 있어서 Kotlin 객체를 그대로 보낼 수 없습니다 | 앱의 요청 스레드(Spring Data Redis) |
| ③ 보내기 | Lettuce가 명령을 연결에 넘겨 소켓(프로그램이 TCP 연결로 데이터를 쓰고 읽는 접점)에 씀, 그리고 네트워크의 가는 길 | 앱의 Lettuce I/O 스레드, 네트워크 |
| ④ 차례 기다림 | 명령이 서버에 도착했지만 메인 스레드가 다른 일을 하느라 손대지 못함 | Redis 메인 스레드(가 다른 일을 하는 시간) |
| ⑤ 실행 | 메인 스레드가 이 명령 자체를 실행 | Redis 메인 스레드 |
| ⑥ 응답 보내기 | 응답을 소켓에 쓰고, 네트워크의 오는 길을 지남. 응답이 크면 길어짐 | Redis 메인 스레드(또는 I/O 스레드), 네트워크 |
| ⑦ 받기·해석 | 같은 연결의 앞 응답이 다 올 때까지 기다린 뒤, 이 응답을 읽어 해석 | 앱의 Lettuce I/O 스레드 |
| ⑧ 역직렬화 | 바이트를 Kotlin 객체로 바꿈 | 앱의 요청 스레드(Spring Data Redis) |

서버 밖의 멈춤(장면 4)은 따로 구간이 없습니다. 메인 스레드가 `fork` 같은 일로 멈추면 그동안 도착한 모든 명령의 ④가 늘어나고, 운영체제가 Redis에 CPU를 주지 않거나 메모리를 디스크에서 다시 읽어 와야 하면 ④와 ⑤가 함께 늘어납니다. **그래서 "어느 구간이 늘었는가"와 "왜 늘었는가"는 다른 질문입니다.** 이 문서는 먼저 구간을 가르고, 그다음 원인을 찾습니다.

Kotlin 코드로 보면 ①~⑧은 `redisTemplate.opsForValue().get("stock:1")` 한 줄 안에 모두 들어 있습니다. 앱 개발자가 보는 시간은 이 한 줄 전체이고, Redis 서버가 아는 시간은 그중 ⑤뿐입니다.

### 서버 안: 메인 스레드 하나가 명령을 한 줄로 실행합니다

- **한 줄**: Redis 서버는 프로세스 하나이고, 키를 읽고 바꾸는 명령은 그 안의 메인 스레드 하나가 이벤트 루프를 돌며 하나씩 끝까지 실행합니다. 한 명령이 길면 그동안 도착한 다른 연결의 명령이 모두 기다립니다.
- **이 주제에서는**: 장면 3에서 주문 API의 `GET stock:1`은 실행이 짧은데도 80ms가 걸립니다. 늘어난 것은 ⑤가 아니라 ④입니다. Tomcat처럼 요청마다 스레드가 따로 도는 구조에 익숙하면 "내 명령은 가벼운데 왜 느린가"에서 막히므로, 이 점을 먼저 잡아 둡니다. 원리는 [I/O 다중화와 이벤트 루프](../cs/io-multiplexing-and-event-loop.md)와 [프로세스와 스레드](../cs/process-and-thread.md)에, Redis의 실제 동작은 [Redis 이벤트 루프](./event-loop.md)에 있습니다.

### 앱 안: 공유 연결 하나 위의 응답은 보낸 순서대로 옵니다

- **한 줄**: Spring Boot 기본 구성의 Lettuce는 일반 명령을 TCP 연결 하나로 모든 요청 스레드가 나눠 보내고, Redis는 한 연결의 응답을 받은 순서대로 돌려줍니다. 그래서 앞에 보낸 명령의 응답이 크거나 늦으면, 뒤에 보낸 명령의 응답은 서버에서 이미 실행됐어도 그 뒤에 줄을 섭니다.
- **이 주제에서는**: 장면 2가 이 경우입니다. 늘어난 곳은 ⑦이고, 서버의 메인 스레드는 바쁘지 않았을 수 있습니다. 공유 연결과 풀의 구성, 그 대가는 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에 있습니다.

### 왕복 시간

- **한 줄**: 왕복 시간은 요청이 상대에게 가고 응답이 돌아오는 데 걸리는, 일을 하지 않고 기다리는 시간입니다. 명령 하나가 짧아도 이 시간은 매번 듭니다.
- **이 주제에서는**: ③과 ⑥의 네트워크 부분이 왕복 시간입니다. 평소의 왕복 시간을 알아야 장면 5에서 "평소보다 늘었는가"를 판단할 수 있습니다. 원리는 [왕복과 묶어 보내기](../cs/round-trip-and-batching.md)에 있습니다.

### 백분위: 지연은 평균이 아니라 분포로 봅니다

- **한 줄**: 같은 명령도 걸리는 시간이 매번 다르므로 지연은 분포로 보고, p99는 "100건 중 99건이 이 시간 안에 끝났다"는 값입니다.
- **이 주제에서는**: 신고는 "가끔 80ms"입니다. 평균은 거의 그대로일 수 있어서, 평균만 보는 지표로는 이 신고가 보이지 않습니다. 아래에서 지표마다 평균인지, 백분위인지, 최댓값인지, **어느 기간을 모은 값인지**를 함께 적는 이유입니다. 원리는 [타임아웃](../cs/timeout.md)의 "응답 시간은 하나의 값이 아니라 분포입니다"에 있습니다.

### fork와 copy-on-write, 큰 페이지(THP)

- **한 줄**: `fork`는 실행 중인 프로세스를 복제해 그 순간의 메모리를 보는 자식 프로세스를 만드는 시스템 호출이고, 메모리 페이지(운영체제가 메모리를 나눠 관리하는 고정 크기 단위)는 처음엔 부모와 자식이 함께 쓰다가 바뀌는 페이지만 복사됩니다(copy-on-write). THP(Transparent Huge Pages)는 커널이 작은 페이지 여러 개를 큰 페이지 하나로 묶어 쓰게 하는 기능입니다.
- **이 주제에서는**: Redis는 RDB 저장과 AOF 다시 쓰기를 위해 메인 스레드에서 `fork`를 부르고, 그 호출이 끝날 때까지 모든 명령이 기다립니다(장면 4). THP가 켜져 있으면 `fork` 뒤 복사 단위가 커져 지연과 메모리가 늘 수 있다고 공식 문서는 설명합니다. 원리는 [fork와 copy-on-write](../cs/fork-and-copy-on-write.md)에, Redis가 언제 `fork`하는지는 [Redis 영속성](./persistence.md)에 있습니다.

### 스왑: 메모리 일부가 디스크로 밀려나는 것

이 개념은 따로 CS 문서가 없어 여기서 설명합니다.

- **한 줄**: 스왑은 물리 메모리가 모자랄 때 운영체제가 프로세스의 메모리 페이지 일부를 디스크로 옮겨 두는 것입니다. 그 페이지를 다시 쓰려 하면 디스크에서 읽어 올 때까지 그 스레드가 멈춥니다.
- **비유**: 책상(메모리)이 좁아 자주 안 보는 서류를 창고(디스크)에 넣어 둔 것입니다. 그 서류가 필요해지면 창고에 다녀올 때까지 일을 멈춥니다.
- **실제로는**: 프로그램은 자기 메모리가 창고에 있는지 모릅니다. 평소처럼 주소를 읽었는데 운영체제가 끼어들어 디스크에서 페이지를 읽어 온 뒤에야 다음 명령어로 넘어갑니다. 메모리 읽기는 보통 나노초 단위인데 디스크 읽기는 그보다 훨씬 느리므로, 스왑된 페이지를 건드린 순간이 지연으로 튑니다. JVM 힙이 스왑되면 GC가 길어지는 것과 같은 원리입니다.
- **이 주제에서는**: Redis의 메인 스레드가 스왑된 페이지의 키를 읽으면, 원래 짧은 `GET`의 ⑤가 길어지고 그동안 다른 명령의 ④도 늘어납니다. 명령 자체는 가벼운데 실행 시간이 길게 찍히는 이상한 모양이 됩니다. 확인 방법은 공식 문서 latency의 "Latency induced by swapping (operating system paging)"에 있고, 아래 "6단계: 서버도 한가했다면 — 서버 밖과 네트워크를 봅니다"에서 다룹니다.

### 지표를 읽을 때 단위와 모은 기간부터 확인합니다

같은 "지연"이라도 도구마다 단위와 모은 기간이 다릅니다. 1밀리초(ms)는 1,000마이크로초입니다. 여기서 틀리면 1,000배를 잘못 읽거나, 오래전의 일을 지금 일로 읽습니다.

| 지표 | 단위 | 모은 기간 |
| --- | --- | --- |
| `SLOWLOG GET`의 실행 시간 | 마이크로초 | 기준 이상인 최근 항목들(기본 128개까지) |
| `slowlog-log-slower-than` 설정 | 마이크로초 | — |
| `INFO commandstats`의 `usec`, `usec_per_call` | 마이크로초 | 서버 시작 또는 `CONFIG RESETSTAT` 이후 누적 |
| `INFO latencystats`의 `latency_percentiles_usec_<명령>` | 마이크로초 | 서버 시작 또는 `CONFIG RESETSTAT` 이후 누적 |
| `LATENCY LATEST`·`LATENCY HISTORY`, `latency-monitor-threshold` | 밀리초 | 이벤트마다 최근 160개 표본(1초에 하나로 합침) |
| `INFO stats`의 `latest_fork_usec` | 마이크로초 | 마지막 `fork` 한 번 |
| `redis-cli --latency` | 밀리초(정수) | 실행하는 동안 |
| `redis-cli --intrinsic-latency` | 마이크로초 | 실행하는 동안 |
| Lettuce의 `lettuce.command.*` 타이머 | 모니터링 시스템의 시간 단위로 변환됨 | 모니터링 시스템이 모으는 간격 |

`INFO`의 누적 합계와 횟수(`usec`, `calls` 등)는 두 번 읽어 뺀 차이로 그 사이의 양을 봅니다. 백분위는 이렇게 뺄 수 없습니다. 두 시점의 누적 p99를 빼도 그 사이 구간의 p99가 나오지 않기 때문입니다. 그래서 서버가 몇 주 동안 돈 뒤라면 `latencystats`의 p99에 14시 02분의 몇 분은 거의 드러나지 않습니다. 몇 주 분량의 표본에 몇 분의 표본이 묻히기 때문입니다. 구간의 백분위가 필요하면 `CONFIG RESETSTAT`으로 누적을 비우고 새로 모아야 하는데, 이 명령은 `commandstats` 같은 다른 누적 통계도 함께 비웁니다.

## 왜 "Redis가 느리다"에서 바로 Redis를 고치면 안 되는가

신고하는 사람이 본 것은 ①~⑧을 모두 더한 시간입니다. 반면 Redis 서버가 직접 재는 것은 대부분 ⑤입니다. 이 차이 때문에 두 가지 실수가 생깁니다.

- **서버 지표가 깨끗하니 Redis는 무관하다고 결론 내립니다.** 장면 3에서 주문 API의 `GET`은 ⑤가 짧아 SLOWLOG에 없습니다. 그렇다고 Redis와 무관한 것이 아닙니다. 다른 명령이 메인 스레드를 잡았고, 그 명령은 다른 연결이 보냈습니다.
- **앱이 느리니 Redis 서버를 키웁니다.** 장면 1과 2에서는 서버가 바쁘지 않았습니다. 서버를 늘리거나 클러스터로 바꿔도 JVM 멈춤이나 공유 연결의 줄서기는 그대로입니다.

그래서 진단은 "어느 구간이 늘었는가"를 먼저 가르고, 그 구간을 늘린 원인을 찾는 순서로 합니다. 순서의 원칙은 둘입니다.

1. **바깥에서 안으로 갑니다.** 신고자가 본 시간(①~⑧)에서 시작해, 앱 안(①②⑧), 연결과 네트워크(③⑥⑦), 서버(④⑤) 순으로 좁힙니다. 바깥 구간을 먼저 빼면 안쪽 지표가 깨끗할 때 헷갈리지 않습니다.
2. **싸게 많이 가르는 것부터 봅니다.** "한 인스턴스만인가, 모든 인스턴스인가", "한 명령만인가, 모든 명령인가"는 지표 두어 개로 장면을 절반씩 지웁니다.

## 도구마다 어느 구간을 재는가

진단 순서에 들어가기 전에, 쓸 도구가 각각 어느 구간을 재고 어느 구간을 빼는지 한 표로 정리합니다. **이 표에서 가장 많이 틀리는 줄은 SLOWLOG입니다.** SLOWLOG는 ⑤만 잽니다. 기다린 시간(④), 네트워크(③⑥), 앱 안(①②⑦⑧)은 들어가지 않습니다. 공식 문서도 실행 시간에 "talking with the client" 같은 I/O가 들어가지 않는다고 적습니다(slowlog).

| 도구 | 재는 구간 | 빼는 구간 | 어디서 보는가 |
| --- | --- | --- | --- |
| 앱이 Redis 호출을 감싸 잰 시간(앱 로그, 직접 단 타이머) | ①~⑧ 전부 | 없음 | 앱 |
| `lettuce.command.completion` | ③~⑦ | ①②⑧ | 앱의 Micrometer |
| `lettuce.command.firstresponse` | ③에서 이 응답의 첫 부분을 해석하기 시작할 때까지(같은 연결의 앞 응답을 기다린 시간 포함) | ①②⑧, 이 응답의 나머지를 받는 시간 | 앱의 Micrometer |
| `redis-cli --latency`(앱 서버에서 실행) | 새 연결로 보낸 `PING`의 ③~⑥ | 앱의 공유 연결과 앱 안 | 앱 서버의 셸 |
| `SLOWLOG`, `INFO commandstats`, `INFO latencystats`, `LATENCY HISTOGRAM` | ⑤ | ①②③④⑥⑦⑧ | Redis |
| 지연 모니터(`LATENCY LATEST`·`HISTORY`·`DOCTOR`) | ⑤(명령 이벤트)와, 메인 스레드가 명령 밖에서 한 일(`fork`, 만료 정리, 축출, AOF 쓰기 등) | 기준(밀리초) 미만의 일, 네트워크, 앱 | Redis |
| `latest_fork_usec` | 마지막 `fork` 한 번의 시간. 다른 명령의 ④를 늘리는 원인 | — | Redis `INFO stats` |
| `redis-cli --intrinsic-latency`(Redis 서버에서 실행) | Redis와 무관하게, 그 서버에서 프로세스 하나가 CPU를 못 받고 멈출 수 있는 최대 시간 | Redis의 모든 구간 | Redis 서버의 셸 |
| `CLIENT LIST`의 `omem` | 그 연결의 출력 버퍼가 쓰는 메모리. 크면 보내지 못한 응답이 쌓였다는 단서(⑥이 밀린 정도) | 시간이 아니라 메모리 바이트이며, 쌓인 응답 데이터의 정확한 크기와도 같지 않음 | Redis |

아래에서 각 도구가 왜 이 범위를 재는지, 같은 신고의 장면마다 어떻게 보이는지 차례로 봅니다.

### Lettuce 타이머는 명령을 연결에 넘긴 순간부터 잽니다

Spring Boot는 Actuator 자동 구성이 있고 Lettuce가 클래스 경로에 있으며 `MeterRegistry` 빈이 있으면, Lettuce에 명령 지연 기록기를 붙입니다. 이때 쓰는 `MicrometerOptions`는 앱이 같은 타입의 빈을 따로 정의하지 않았을 때만 기본값으로 만들어집니다. 그러면 명령 이름(`command` 태그)과 서버 주소(`remote` 태그)마다 두 타이머가 생깁니다.

- `lettuce.command.firstresponse`: 명령을 보낸 때부터 그 명령의 응답을 처음 해석하기 시작할 때까지
- `lettuce.command.completion`: 명령을 보낸 때부터 응답을 다 해석할 때까지

여기서 "보낸 때"는 Lettuce가 명령을 연결의 응답 대기 목록에 올리고 소켓으로 쓰기 직전입니다. 그래서 다음이 **빠집니다**.

- **① 연결 얻기**: 풀에서 연결을 빌리는 기다림은 명령이 연결에 닿기 전이므로 들어가지 않습니다. 연결이 끊긴 동안 Lettuce가 쌓아 둔 명령도, 다시 연결되어 실제로 쓰일 때부터 잽니다(소스로 추론).
- **②⑧ 직렬화**: Spring Data Redis는 Lettuce에 바이트 배열을 넘기고 바이트 배열을 받습니다. 키·값을 바이트로 바꾸고 되돌리는 일은 `RedisTemplate` 쪽에서 Lettuce 호출 앞뒤에 하므로 이 타이머 밖입니다.

반대로 다음은 **들어갑니다**.

- **⑦ 같은 연결의 앞 응답 기다림**: Lettuce는 한 연결의 응답을 도착 순서대로 해석하므로, 앞 응답을 다 해석해야 이 명령의 응답을 해석하기 시작합니다. 그 기다림은 `firstresponse`와 `completion` 모두에 들어갑니다.
- **JVM 전체의 멈춤**: 응답이 도착했어도 Lettuce의 I/O 스레드가 GC 등으로 멈춰 있으면 해석 시각이 늦어지므로 타이머가 늘어납니다(소스로 추론. 시각을 해석할 때 찍기 때문입니다).

명령 하나만 놓고 보면 `completion − firstresponse`는 **이 명령 자신의 응답을 다 받아 해석하는 데 든 시간**입니다. 응답이 작은 `GET`이면 작을 것이고, 큰 Hash를 통째로 읽는 명령이면 커질 것입니다. 그런데 모니터링 시스템에 남는 것은 명령 하나하나가 아니라 명령 종류·서버 주소별로 모은 값입니다. 그래서 두 타이머의 p99나 최댓값끼리 빼면 아무 뜻이 없습니다. 같은 기간의 **평균끼리는** 뺄 수 있습니다. 합계를 횟수로 나눈 평균은 명령마다의 차이를 평균한 것과 같기 때문입니다. 즉 "이 명령 종류는 평균적으로 응답을 받는 데 얼마나 쓰는가"까지는 볼 수 있고, 어느 호출 하나가 큰 응답이었는지는 볼 수 없습니다.

주의할 기본값이 둘 있습니다.

- **백분위가 기본으로 나오지 않습니다.** Lettuce의 기본 설정은 히스토그램을 만들지 않으므로, 모니터링 시스템에 따라 횟수·합계·최댓값만 보입니다. "가끔 80ms"를 보려면 백분위가 필요합니다. 켜는 길은 둘입니다. 하나는 Spring Boot의 `management.metrics.distribution.percentiles-histogram.lettuce`처럼 계량기 이름 앞부분으로 히스토그램 버킷을 켜는 것입니다. 이 설정은 이름이 `lettuce`로 시작하는 계량기에 함께 적용되고, 백분위는 버킷을 모은 모니터링 시스템이 계산합니다. 다른 하나는 `MicrometerOptions` 빈을 직접 정의해 Lettuce의 히스토그램 옵션을 켜는 것으로, 이때 Lettuce가 정해 둔 백분위도 함께 발행됩니다. 켜면 계량기 수가 늘어 모니터링 저장량이 커지므로, 명령 종류와 서버 주소 수를 곱한 만큼 시계열이 생긴다는 점을 감안합니다.
- **인스턴스 주소는 태그에 없습니다.** 기본값은 앱 쪽 주소(`local` 태그)를 구분하지 않습니다. 인스턴스별로 보려면 모니터링 시스템이 붙이는 인스턴스 태그를 씁니다.

### SLOWLOG, commandstats, latencystats는 모두 실행 시간(⑤)입니다

Redis는 명령 하나를 실행할 때 그 명령 함수가 돈 시간만 잽니다. 이 한 숫자를 세 곳이 나눠 씁니다.

- **`SLOWLOG`**: 그 시간이 `slowlog-log-slower-than`(기본 10000마이크로초 = 10ms) 이상인 명령을, 인자와 보낸 연결의 주소·이름과 함께 최근 `slowlog-max-len`(기본 128)개까지 남깁니다. 넘친 항목은 오래된 것부터 밀려납니다.
- **`INFO commandstats`**: 명령 종류마다 호출 수(`calls`), 실행 시간 합(`usec`), 평균(`usec_per_call`)을 누적합니다.
- **`INFO latencystats`**: 명령 종류마다 실행 시간의 백분위를 냅니다. `latency-tracking`이 기본으로 켜져 있고, 기본 백분위는 p50·p99·p99.9입니다. `LATENCY HISTOGRAM`은 같은 데이터를 명령별 분포로 보여 줍니다.

예외가 하나 있습니다. `BLPOP`처럼 데이터를 기다리는 명령은 기다림이 풀릴 때 기다린 시간과 응답을 만든 시간까지 더해 SLOWLOG·`commandstats`·`latencystats`에 남깁니다. 그래서 기다리는 명령이 SLOWLOG에 길게 남아 있어도 메인 스레드를 그만큼 잡았다는 뜻이 아닙니다. 이 문서의 `GET` 같은 일반 명령은 실행 시간만 남습니다.

셋 다 ⑤이므로 장면 3에서 주문 API의 `GET`은 어디에도 길게 나오지 않습니다. 대신 **관리자 배치가 보낸 명령이 SLOWLOG에 길게 남습니다.** SLOWLOG는 "내 명령이 얼마나 걸렸나"보다 "그 시각에 메인 스레드를 잡은 명령이 무엇인가"를 찾는 도구로 쓰는 편이 맞습니다. 항목에 보낸 연결의 이름이 남으므로, 앱마다 `spring.data.redis.client-name`을 붙여 두면 누가 보냈는지 바로 압니다.

`commandstats`와 `latencystats`는 서버 시작 이후 누적이라 짧은 사건을 보기에 둔합니다. `commandstats`의 `usec`·`calls`는 사건 전후 두 번 읽은 차이로 그 구간의 양을 낼 수 있지만, `latencystats`의 백분위는 그렇게 뺄 수 없어 추세를 보는 데만 씁니다. 그래서 평소에 일정 간격으로 수집해 둡니다. 주로 "어떤 명령 종류가 메인 스레드 시간을 많이 먹는가"(`usec` 증가 폭이 큰 명령)를 찾는 데 씁니다.

### 지연 모니터는 명령이 아닌 일도 기록하지만, 기본으로 꺼져 있습니다

지연 모니터는 메인 스레드에서 오래 걸린 일을 종류(이벤트)별로 기록합니다. 기준은 `latency-monitor-threshold`(밀리초)이고 **기본값 0은 꺼짐**입니다. 장애가 난 뒤에 켜면 그 장애는 기록되지 않습니다.

기록되는 이벤트는 크게 셋입니다.

- **명령**: `command`(일반 명령), `fast-command`(Redis가 명령 정의에 빠른 명령으로 표시해 둔 명령. 주로 O(1)·O(log N) 명령). 시간은 SLOWLOG와 같은 실행 시간입니다.
- **명령 밖의 일**: `fork`, `expire-cycle`(만료 정리), `eviction-cycle`·`eviction-del`(메모리 한도에 닿아 키를 내보냄), `active-defrag-cycle`(단편화 정리) 등
- **디스크 쓰기**: `aof-write`, `aof-fsync-always`, `aof-write-pending-fsync`, `aof-rename` 등

이벤트마다 최근 160개 표본을 두고, 같은 초에 생긴 표본은 최댓값 하나로 합칩니다. `LATENCY LATEST`는 이벤트별 마지막·최대 지연을, `LATENCY HISTORY <이벤트>`는 시각별 표본을 보여 주므로 "14시 02분에 무엇이 메인 스레드를 잡았는가"를 SLOWLOG가 보지 못하는 일까지 넓혀 볼 수 있습니다.

`LATENCY DOCTOR`는 이 기록을 읽어 사람이 읽을 조언을 붙입니다. 진단에 쓸 만한 판단이 두 가지 들어 있습니다.

- **`fast-command` 이벤트가 있으면 시스템이 Redis에 CPU를 주지 않았을 수 있다고 조언합니다.** 빠르게 끝나야 할 명령이 밀리초 단위로 실행됐다면, 명령의 비용보다 그 순간의 실행 환경(CPU를 받지 못함, 스왑된 메모리 읽기)을 먼저 의심할 만하기 때문입니다. 다만 이것은 확인할 가설이지 원인의 확정이 아닙니다. 빠른 명령으로 분류된 명령도 값이 아주 크면 오래 걸릴 수 있습니다. 조언 안에 `redis-cli --intrinsic-latency`로 확인하라는 항목이 있습니다.
- **프로세스 메모리에 큰 페이지(AnonHugePages)가 있으면 THP를 끄라고 조언합니다.** 이 검사는 이벤트 기록과 상관없이 합니다.

### `redis-cli --latency`는 새 연결로 `PING`을 반복합니다

`redis-cli --latency`는 대상 서버에 연결을 하나 만들어 10ms마다 `PING`을 보내고, 왕복에 걸린 시간의 최소·최대·평균을 보여 줍니다. `--latency-history`는 기본 15초마다 구간을 끊어 새로 셉니다.

- **재는 것**: 이 연결 하나의 ③~⑥. 메인 스레드가 바빠서 생긴 ④도 들어갑니다.
- **빼는 것**: 앱의 공유 연결 위 줄서기(⑦), 앱 안의 ①②⑧. 이 도구는 앱과 다른 연결을 쓰기 때문입니다.
- **해상도**: 밀리초 정수로 잽니다. 같은 데이터센터 안이라 평소 왕복이 1ms보다 짧으면 0이나 1로만 보입니다. 이 도구로 볼 수 있는 것은 "평소 0~1ms이던 것이 수십 ms로 튀는가" 정도입니다.

어디서 실행하느냐에 따라 가르는 것이 달라집니다. **앱 서버에서 실행해 튀고, Redis 서버 자신(또는 같은 네트워크의 다른 호스트)에서 실행해 튀지 않으면** 그 사이 네트워크가 의심됩니다. 둘 다 튀면 서버가 그 시각에 바빴거나 멈춘 것입니다.

### `redis-cli --intrinsic-latency`는 Redis가 아니라 그 서버를 잽니다

`--intrinsic-latency <초>`는 Redis에 연결하지 않습니다. 짧은 계산을 정해 둔 시간 동안 반복하며 한 번이 가장 오래 걸린 시간(마이크로초)을 보여 줍니다. 계산 자체는 짧고 일정하므로, 공식 문서는 오래 걸린 회차를 커널(가상 머신이라면 하이퍼바이저까지)이 그 프로세스에 CPU를 주지 않은 시간으로 해석합니다. 그리고 이 값을 그 환경의 기준선(intrinsic latency)이라 부르며, Redis에 이보다 좋은 지연을 기대할 수 없다고 설명합니다. 이 문서는 이 값을 "그 환경에서 측정한 동안 프로세스가 이만큼 멈출 수 있었다"는 추정치로 씁니다. 공식 문서는 **클라이언트가 아니라 Redis를 돌리는 서버에서 실행해야 한다**고 적습니다.

- 하이퍼바이저: 물리 서버 한 대를 여러 가상 머신으로 나눠 쓰게 하는 소프트웨어입니다. 같은 물리 서버의 다른 가상 머신이 CPU를 많이 쓰면 내 가상 머신이 잠시 CPU를 받지 못할 수 있습니다.

이 값이 수십 ms라면 Redis 설정을 아무리 바꿔도 그만큼의 튐은 남습니다. 다만 이 값은 실행하는 동안의 측정이므로, 신고 시각에 같은 일이 있었는지는 같은 시각의 `fast-command` 이벤트나 호스트의 CPU 지표로 맞춰 봐야 합니다.

### `CLIENT LIST`는 연결마다 밀린 양을 보여 줍니다

`CLIENT LIST`는 서버에 붙은 연결마다 한 줄씩 상태를 보여 줍니다. 지연 진단에서 보는 필드는 다음입니다.

| 필드 | 뜻 | 지연 진단에서 |
| --- | --- | --- |
| `name`, `addr` | 연결 이름(앱이 붙인 경우), 서버가 본 상대 주소 | 어느 앱·인스턴스의 연결인지 |
| `cmd`, `idle` | 마지막 명령, 마지막 명령 뒤 지난 초 | 지금 무엇을 했는가 |
| `qbuf` | 받았지만 아직 처리하지 않은 입력의 바이트 | 크면 서버가 아직 읽어 처리하지 못한 명령이 쌓임 |
| `omem` | 출력 버퍼의 메모리 사용량 | 크면 ⑥이 밀림: 응답이 크거나, 상대가 느리게 읽음 |
| `tot-net-out`, `tot-cmds` | 그 연결이 지금까지 보낸 바이트와 실행한 명령 수 | 큰 응답을 받는 연결 찾기 |

`omem`이 큰 연결이 주문 API의 공유 연결이라면, 서버는 응답을 이미 만들었는데 아직 보내지 못하고 있을 가능성이 큽니다. 응답이 크거나(장면 2), 네트워크나 앱이 느리게 받는 경우(장면 5)가 후보입니다. 출력 버퍼와 메모리의 관계는 [Redis 메모리 지표](./memory-metrics.md)에 있습니다.

## 진단 순서: 여섯 단계로 좁힙니다

각 단계는 "무엇을 보고, 어떻게 읽으면 어느 장면이 남는가"로 씁니다. 단계마다 장면이 지워지므로, 앞 단계에서 원인이 분명해지면 뒤 단계는 확인용으로만 봅니다.

### 1단계: 범위를 정합니다 — 누가, 무엇이, 언제 느렸는가

먼저 신고를 세 축으로 나눕니다. 지표 두어 개로 장면을 크게 지울 수 있는 단계입니다.

| 축 | 관찰 | 남는 장면 |
| --- | --- | --- |
| 인스턴스 | 네 대 중 한 대만 | 장면 1(앱 안), 2(그 인스턴스의 공유 연결), 5(그 인스턴스 쪽 네트워크) |
| 인스턴스 | 네 대 모두 같은 시각에 | 장면 3, 4(서버), 5(서버 쪽 네트워크) |
| 명령 | `GET`만 | 그 명령의 키가 크거나, 그 명령을 부르는 코드 경로 |
| 명령 | 모든 명령이 같은 시각에 | 메인 스레드 전체(장면 3, 4)나 연결 전체(장면 2, 5) |
| 시각 | 몇 분·몇 시간마다 주기적으로 | 정해진 일: RDB 저장 주기의 `fork`, 배치, 만료가 몰린 시각 |
| 시각 | 특정 기능 호출과 겹침 | 그 기능의 큰 명령(장면 2, 3) |

예시에 대입하면, "네 대 모두 같은 14시 02분에, `GET`뿐 아니라 그 인스턴스의 다른 Redis 명령도 함께" 느렸다면 장면 3·4가 앞에 섭니다. "한 대만"이라면 장면 1·2부터 봅니다.

클러스터라면 `lettuce.command.completion`의 `remote` 태그로 노드를 나눕니다. 한 노드로 가는 명령만 느리면 그 노드의 메인 스레드나 그 노드의 키 몇 개를 의심합니다([Hot Key와 Big Key](./hot-key-big-key.md)).

### 2단계: 앱 안인가, 밖인가 — 앱의 시간과 Lettuce의 시간을 나란히 봅니다

같은 인스턴스, 같은 시각의 두 시간을 비교합니다.

- 앱이 잰 Redis 호출 시간(①~⑧)
- `lettuce.command.completion`의 `GET`(③~⑦)

두 시간은 같은 조건으로 맞춰야 비교가 됩니다. Lettuce 타이머는 호출 하나하나가 아니라 명령 종류·서버 주소별로 모은 값이므로, 앱 로그에 찍힌 호출 하나의 80ms를 Lettuce의 평균과 맞대면 안 됩니다. "가끔 80ms"는 평균에 묻혀 Lettuce 평균이 평소대로 보일 수 있기 때문입니다. 같은 집계 구간의 `GET`끼리 놓고 보되, Redis 쪽을 후보에서 빼는 근거로는 최댓값을 씁니다. 백분위는 그보다 드문 지연을 가립니다. 같은 구간의 `GET` 10,000건 중 1건만 80ms라면 p99.9도 평소대로일 수 있습니다(가상 수치). 타이머의 최댓값도 일정한 시간 창 안의 값이므로, 화면에서 고른 조회 기간이 아니라 그 창에 80ms 호출이 들어가는지 확인합니다.

| 비교 | 읽는 법 |
| --- | --- |
| 앱의 시간만 80ms, 같은 구간 Lettuce `GET`의 최댓값은 평소대로 | **Lettuce 타이머 밖이 늘었습니다.** ①②⑧, 그리고 응답을 받은 뒤 기다리던 요청 스레드가 다시 실행되기까지가 여기 들어갑니다. 장면 1입니다. 연결 풀을 쓴다면 빌리기 대기, 직렬화할 객체가 커졌는지, JVM이 멈췄는지(GC 로그), 요청 스레드가 CPU를 받지 못했는지(인스턴스의 CPU 사용률) 봅니다 |
| 둘 다 80ms | **③~⑦이 늘었습니다.** 연결·네트워크·서버 쪽이므로 3단계로 갑니다. 단, JVM 전체가 멈췄을 때도 Lettuce의 시간이 함께 늘 수 있으므로 GC 로그를 함께 확인합니다 |

Lettuce 쪽에 평균이나 백분위밖에 없거나 집계 구간을 앱 쪽과 맞출 수 없다면, Lettuce가 평소대로라는 것만으로 ③~⑦을 후보에서 빼지 않습니다.

예시의 장면 1에서 GC 로그에 14시 02분의 75ms 멈춤이 보인다면(가상), 그 인스턴스의 Redis 호출만이 아니라 DB 호출과 HTTP 응답도 같은 시각에 함께 늘었을 것입니다. **Redis 호출만 늘었는지, 그 인스턴스의 모든 외부 호출이 늘었는지**를 보면 앱 안의 원인인지 빨리 가를 수 있습니다.

앱이 잰 시간이 따로 없다면, Redis 호출을 감싼 타이머가 없어 ①②⑧은 볼 수 없습니다. 그때는 스레드 덤프로 요청 스레드가 무엇을 기다리는지 보는 방법이 남습니다. 풀의 빌리기 대기와 명령 결과 대기를 덤프로 가르는 방법은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "증상과 확인 순서"에 있습니다. 앞으로를 위해 Redis를 부르는 지점에 앱 타이머를 하나 두는 편이 좋습니다.

### 3단계: 응답이 컸는가 — 같은 연결의 큰 응답을 찾습니다

Lettuce의 시간이 늘었다면, 서버로 넘어가기 전에 **같은 연결 위의 큰 응답**을 먼저 봅니다. 서버 지표에 잘 드러나지 않아 놓치기 쉽기 때문입니다.

- 같은 인스턴스·같은 시각에, 명령 종류별로 두 타이머의 평균 차이(`completion` 평균 − `firstresponse` 평균)가 큰 명령이 있는지 봅니다. 장면 2의 상품 목록 기능이 보낸 `HGETALL`이 그런 후보입니다. 이 값은 명령 종류의 평균이고 기본 태그에는 연결 구분이 없으므로, 그 명령이 주문 API의 `GET`과 같은 연결로 나갔는지는 아래 `CLIENT LIST`와 코드 경로(같은 `RedisConnectionFactory`의 공유 연결을 쓰는가)로 확인합니다.
- 그 시각에 주문 API의 `GET`은 `firstresponse`부터 늘어 있을 것입니다. 앞 응답을 다 해석할 때까지 이 응답의 해석을 시작하지 못했기 때문입니다.
- 서버의 `CLIENT LIST`에서 주문 API 연결(`name`으로 찾음)의 `omem`, `tot-net-out`이 크게 늘었는지 봅니다. `INFO clients`의 `client_recent_max_output_buffer`는 모든 연결 중 가장 큰 출력 버퍼이지만 최근 몇 초(8.2.10에서 8초 안팎)만 기억하므로, 사건 뒤에 보려면 이 값을 몇 초 간격으로 수집해 두었어야 합니다.

장면 2에서 큰 `HGETALL`의 실행 시간이 SLOWLOG 기준(10ms)보다 짧았다면 SLOWLOG에는 아무것도 남지 않습니다. 실행보다 응답 전송과 해석이 더 걸리는 명령은 이렇게 서버 실행 지표로는 보이지 않습니다. 대책은 큰 응답을 나눠 읽는 것이고, 연결을 늘리는 것은 그다음입니다([Redis 파이프라이닝과 다중 키 명령](./pipelining.md)의 "묶음이 너무 크면 무엇이 문제인가").

### 4단계: 내 명령의 실행이 길었는가 — SLOWLOG에서 내 명령을 찾습니다

같은 시각의 `SLOWLOG GET`에서 주문 API의 `GET`을 찾습니다. `INFO latencystats`는 서버 시작이나 마지막 `CONFIG RESETSTAT` 이후의 누적이라 이 사건을 확인하거나 배제하는 데 쓰지 않습니다.

- **내 명령이 SLOWLOG에 길게 있다**: ⑤가 늘었습니다. `GET` 같은 O(1) 명령이고 값이 크지 않다면, 명령의 비용보다 그 순간 서버가 CPU를 받지 못했거나 스왑된 메모리를 읽었다는 가설이 앞에 서므로 6단계(서버 밖)로 가서 확인합니다. 원소 수에 비례하는 명령이라면 키가 커졌는지 봅니다([Hot Key와 Big Key](./hot-key-big-key.md)의 "어떻게 찾는가").
- **내 명령은 없다**: 예시의 대부분 장면이 여기에 해당합니다. 아래 단락의 기록 기준과 보관 개수를 확인한 뒤에도 없다면 내 명령의 실행은 기준보다 짧았다고 봅니다. 2·3단계에서 앱 안과 같은 연결의 큰 응답을 이미 지웠다면, 남는 것은 ④(차례 기다림), 서버 밖 멈춤, 네트워크입니다. 5단계로 갑니다. 2·3단계를 건너뛰었다면 그 후보도 남아 있습니다.

SLOWLOG가 비어 있을 때 결론을 내리기 전에, 기록 기준과 보관 개수를 확인합니다. 기준(기본 10ms)보다 짧은 명령은 남지 않고, 128개를 넘으면 오래된 것부터 밀려나므로 사건 뒤 한참 지나 보면 이미 지워졌을 수 있습니다.

### 5단계: 메인 스레드가 그 시각에 다른 일을 했는가

내 명령의 실행이 짧았는데 늦었다면 메인 스레드가 그사이 무엇을 했는지 찾습니다. ④에는 요청마다 남는 지표가 없으므로, **그 시각에 메인 스레드가 바빴는가**로 판단합니다.

1. **SLOWLOG에서 같은 시각의 다른 명령을 봅니다.** 장면 3의 관리자 배치 명령이 여기서 나옵니다. 연결 이름과 주소로 누가 보냈는지 찾습니다. 같은 키의 전체 읽기가 반복된다면 빅키를, 여러 키가 번갈아 나온다면 호출 패턴을 의심합니다.
2. **지연 모니터를 켜 두었다면 `LATENCY LATEST`와 `LATENCY HISTORY`를 봅니다.** 14시 02분 근처에 `fork`, `expire-cycle`, `eviction-cycle`, AOF 쓰기 이벤트가 있는지 봅니다. 장면 4라면 `fork` 이벤트가 그 시각에 70ms 근처로 찍혀 있을 것입니다.
3. **`INFO stats`의 `latest_fork_usec`와 `INFO persistence`의 저장 시각을 봅니다.** 지연 모니터를 켜지 않았어도 마지막 `fork`가 얼마나 걸렸는지는 남습니다. 마지막으로 저장에 성공한 시각(`rdb_last_save_time`, 유닉스 시각)이나 저장·다시 쓰기가 진행 중인지(`rdb_bgsave_in_progress`, `aof_rewrite_in_progress`)를 신고 시각과 맞춥니다. 이 값들도 마지막 한 번만 남으므로, 지나간 여러 번을 보려면 Redis 로그의 저장 시작 기록을 봅니다. 신고가 몇 분 간격으로 반복된다면 RDB 저장 주기와 겹치는지 봅니다. `fork` 시간은 Redis가 쓰는 메모리가 클수록 대체로 길어지며, 가상화 환경에 따라 크게 다릅니다([Redis 영속성](./persistence.md)).
4. **이벤트 루프 지표와 CPU를 봅니다.** `INFO stats`의 `eventloop_duration_sum`과 `eventloop_duration_cmd_sum`의 증가 폭을 비교하면, 메인 스레드의 바쁜 시간이 명령에서 왔는지 다른 일에서 왔는지 가늠할 수 있습니다. 메인 스레드의 CPU 사용률이 코어 하나 기준으로 높다면 그 시각에 일이 몰렸습니다. 지표의 뜻은 [Redis 이벤트 루프](./event-loop.md)의 "증상과 확인 순서"에 있습니다.

여기서 원인이 나오면 대책은 원인별 문서에 있습니다. 긴 명령은 [Redis 이벤트 루프](./event-loop.md)의 "무엇을 피하고 무엇으로 바꿀까", 긴 Lua 스크립트는 [Redis 원자성](./atomicity.md), `fork`는 [Redis 영속성](./persistence.md), 만료가 몰린 경우는 [캐시 스탬피드](./cache-stampede.md)의 "많은 키의 동시 만료: 같은 시각에 넣은 키는 같은 시각에 사라집니다"에 있습니다.

### 6단계: 서버도 한가했다면 — 서버 밖과 네트워크를 봅니다

5단계에서 메인 스레드가 바빴던 흔적이 없거나, 4단계에서 O(1) 명령의 실행이 길었다면 Redis 밖을 봅니다.

**네트워크(장면 5)**

- 앱 서버에서 `redis-cli --latency`를 돌려 평소와 비교합니다. 같은 시각에 Redis 서버 자신에서도 돌려 봅니다. 앱 서버 쪽만 튀면 그 사이 네트워크를 의심합니다.
- 한 인스턴스만 느리다면 그 인스턴스가 있는 호스트나 구역의 네트워크를 봅니다. 모든 인스턴스가 느린데 서버가 한가하다면 Redis 서버 쪽 네트워크나 가운데 장비를 봅니다.
- 3단계에서 본 큰 응답이 많다면 네트워크 대역폭이 찼을 수 있습니다. `INFO stats`의 `instantaneous_output_kbps`로 서버가 내보내는 양을 봅니다.

**CPU를 받지 못한 경우**

- `LATENCY HISTORY fast-command`에 그 시각의 표본이 있으면, 빠른 명령이 기준 이상으로 실행된 순간이 있었다는 뜻입니다. 그 명령의 값이 크지 않다면, 그 순간 Redis가 CPU를 받지 못했거나 스왑된 메모리를 읽었다는 가설을 아래 도구로 확인합니다.
- Redis 서버에서 `redis-cli --intrinsic-latency`를 돌려 그 환경의 기준선을 봅니다. 가상 머신의 CPU 대기(같은 물리 서버의 다른 가상 머신에 CPU를 빼앗긴 시간)를 클라우드 지표로 함께 봅니다.
- 같은 서버에 다른 프로세스가 돌고 있다면 그 프로세스가 CPU를 잡았는지 봅니다. 공식 문서는 `BGSAVE` 같은 자식 프로세스가 메인 스레드와 같은 코어에서 돌지 않게 하라고 적습니다(latency의 "Latency induced by network and communication").

**메모리가 밀려난 경우(스왑, THP)**

- 공식 문서 latency의 "Latency induced by swapping (operating system paging)"은 Redis 프로세스의 `/proc/<pid>/smaps`에서 `Swap:` 줄을 더해 스왑된 양을 보라고 안내합니다. 운영체제의 스왑 입출력 지표와 신고 시각을 맞춥니다. 스왑이 있다면 Redis가 쓰는 메모리에 비해 물리 메모리가 모자라거나, 같은 서버의 다른 프로세스가 메모리를 쓰는 것입니다. `maxmemory`를 정하는 기준은 [Redis 메모리 지표](./memory-metrics.md)에 있습니다.
- THP는 `LATENCY DOCTOR`가 경고하는지, 운영체제 설정이 어떤지 봅니다. 공식 문서는 끄라고 권합니다. 원리는 [fork와 copy-on-write](../cs/fork-and-copy-on-write.md)의 "큰 페이지(THP)는 복사량을 키울 수 있습니다"에 있습니다.

### 장면별로 보이는 모양을 한 표로 모읍니다

| 장면 | 앱 시간 | Lettuce `completion` | 서버 SLOWLOG | 지연 모니터 | 그 밖의 단서 |
| --- | --- | --- | --- | --- | --- |
| 1. 앱 안 | 늘어남 | 같은 구간의 최댓값도 평소대로(JVM 멈춤이면 함께 늘 수 있음) | 없음 | 없음 | GC 로그, 그 인스턴스의 다른 외부 호출도 함께 늘어남 |
| 2. 같은 연결의 앞 응답 | 늘어남 | `GET`도 늘어남, 큰 명령은 두 타이머의 평균 차이가 큼 | 큰 명령이 기준보다 짧으면 없음 | 없을 수 있음 | 한 인스턴스만, `omem`·`tot-net-out` |
| 3. 서버 안 기다림 | 늘어남 | 늘어남 | 다른 연결의 긴 명령 | `command` | 모든 인스턴스가 같은 시각에 |
| 4. 서버 밖 멈춤(`fork`) | 늘어남 | 늘어남 | 없음 | `fork` | `latest_fork_usec`, 저장 시각, 주기적으로 반복 |
| 4'. 서버 밖 멈춤(CPU·스왑) | 늘어남 | 늘어남 | O(1) 명령이 길게 남을 수 있음 | `fast-command` | `--intrinsic-latency`, 스왑, 가상 머신 CPU 대기 |
| 5. 네트워크 | 늘어남 | 늘어남 | 없음 | 없음 | 앱 서버에서만 `--latency`가 튐, 서버는 한가함 |

장면 4와 5는 서버 실행 지표가 모두 깨끗하다는 점에서 닮았습니다. 둘을 가르는 것은 **모든 연결이 같은 순간 함께 멈췄는가**(장면 4: 서버 안에서 돌린 `--latency`도 튐, 지연 모니터에 흔적)와 **특정 경로만 늦었는가**(장면 5: 서버 안에서는 평소대로)입니다.

## 사건이 지난 뒤에는 볼 수 없는 것들: 미리 켜 둘 것

이 진단 순서는 대부분 "그 시각에 무엇이 있었는가"를 묻습니다. 그런데 몇몇 지표는 기본으로 꺼져 있거나, 짧게만 남거나, 누적이라 시각을 잃습니다. 신고는 보통 일이 지난 뒤에 오므로 다음은 평소에 정해 둡니다.

| 무엇을 | 기본 상태 | 미리 하지 않으면 |
| --- | --- | --- |
| 지연 모니터(`latency-monitor-threshold`) | 0(꺼짐) | `fork`·만료 정리 같은 명령 밖의 멈춤이 남지 않음. 서비스가 견딜 지연(예: 명령 타임아웃의 일부)을 기준으로 켭니다 |
| SLOWLOG 보관 개수(`slowlog-max-len`) | 128 | 사건 뒤 다른 긴 명령에 밀려 지워짐. 주기적으로 수집해 두거나 개수를 늘립니다. 항목마다 서버 메모리를 씁니다 |
| 연결 이름(`spring.data.redis.client-name`) | 없음 | SLOWLOG와 `CLIENT LIST`에서 누가 보낸 명령인지 주소로만 추적해야 함 |
| Lettuce 타이머의 히스토그램 | 꺼짐 | 평균과 최댓값만 있어 "가끔 80ms"의 비율을 모름 |
| 앱의 Redis 호출 타이머 | 없음 | 2단계에서 앱 안과 밖을 가를 수 없음 |
| `INFO` 주기 수집 | 없음 | 누적값의 차이를 낼 기준점이 없음 |

## 데이터와 요청이 늘면 무엇이 달라지는가

- **데이터가 커지면 `fork`가 길어집니다.** Redis가 쓰는 메모리가 클수록 페이지 테이블을 복사하는 일이 커지므로, 몇 달 동안 아무 일 없던 저장 주기가 어느 날 신고로 바뀔 수 있습니다. `latest_fork_usec`를 메모리 사용량과 함께 추세로 봅니다.
- **키 하나가 커지면 그 키를 통째로 다루는 명령이 길어집니다.** 장면 3의 원인 명령은 처음에는 SLOWLOG 기준 아래였다가 원소 수가 늘며 기준을 넘습니다. 기준 아래에서도 응답이 커지면 장면 2처럼 같은 연결의 다른 명령을 늦춥니다.
- **요청이 늘면 ④가 길어집니다.** 메인 스레드가 한가할 때는 차례가 바로 오지만, 바빠질수록 도착한 명령이 기다리는 시간이 늘어납니다. 이때는 특정 긴 명령이 없어도 모든 명령의 지연이 조금씩 늘어납니다. 5단계에서 SLOWLOG가 비어 있는데 메인 스레드 CPU가 계속 높다면 이 모양입니다.
- **인스턴스가 늘면 서버 쪽 지표의 해석이 어려워집니다.** 서버 지표는 서버 전체를 세므로, 다른 앱과 함께 쓰는 Redis라면 "그 시각에 바빴다"가 우리 앱 때문인지 알 수 없습니다. 연결 이름으로 앱을 가르는 일이 더 중요해집니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 본문 위치 |
| --- | --- | --- |
| SLOWLOG에 없으면 Redis 쪽 원인이 아니다 | SLOWLOG는 실행 시간(⑤)만 잽니다. 다른 명령을 기다린 시간, `fork` 같은 명령 밖의 멈춤, 네트워크, 앱 안은 들어가지 않습니다 | 도구마다 어느 구간을 재는가 |
| SLOWLOG의 숫자는 클라이언트가 기다린 시간이다 | 서버가 그 명령을 실행한 시간입니다. 클라이언트가 본 시간은 그보다 길고, 그 차이에 다른 원인이 숨어 있습니다 | SLOWLOG, commandstats, latencystats는 모두 실행 시간(⑤)입니다 |
| `lettuce.command.completion`이 앱에서 본 Redis 시간이다 | 연결 얻기와 직렬화·역직렬화는 빠집니다. 앱 안의 대기는 이 지표로 보이지 않습니다 | Lettuce 타이머는 명령을 연결에 넘긴 순간부터 잽니다 |
| `latencystats`의 p99로 지금의 지연을 본다 | 서버 시작 이후 누적이라 몇 분의 사건은 묻히고, 두 시점의 백분위를 빼도 그 구간의 백분위가 나오지 않습니다. 구간을 보려면 앱 쪽 백분위를 봅니다 | 지표를 읽을 때 단위와 모은 기간부터 확인합니다 |
| `redis-cli --latency`가 0이면 지연이 없다 | 밀리초 정수로 재므로 1ms 미만은 0으로 보입니다 | `redis-cli --latency`는 새 연결로 `PING`을 반복합니다 |
| `--intrinsic-latency`는 앱 서버에서 돌린다 | Redis를 돌리는 서버에서 돌려야 그 서버의 기준선을 봅니다. Redis에 연결하지도 않습니다 | `redis-cli --intrinsic-latency`는 Redis가 아니라 그 서버를 잽니다 |
| `GET`처럼 가벼운 명령이 SLOWLOG에 있으면 그 키가 문제다 | 값이 크지 않은데 O(1) 명령의 실행이 길었다면, 키보다 먼저 CPU를 받지 못했거나 스왑된 메모리를 읽었을 가능성을 확인합니다 | 4단계: 내 명령의 실행이 길었는가 — SLOWLOG에서 내 명령을 찾습니다, 6단계: 서버도 한가했다면 — 서버 밖과 네트워크를 봅니다 |
| 지연 모니터를 켜면 지난 장애를 볼 수 있다 | 켠 뒤부터 기록합니다. 기본은 꺼져 있습니다 | 사건이 지난 뒤에는 볼 수 없는 것들: 미리 켜 둘 것 |

## 근거

기준: Redis [`8.2.10`](https://github.com/redis/redis/tree/8.2.10) 소스, 공식 문서 [redis/docs `e53775dd`](https://github.com/redis/docs/tree/e53775dde0ee7e39ebe327e7ffff583caa9fbd65), Lettuce [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE), Spring Boot [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16), Spring Data Redis [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13).

**Redis 8.2.10 소스**

- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c)
  - `call`: 명령 함수(`c->cmd->proc`) 앞뒤로만 시간을 재어 `c->duration`에 더함. 같은 값을 `slowlogPushCurrentCommand`(SLOWLOG), `real_cmd->microseconds`(commandstats의 `usec`), `updateCommandLatencyHistogram`(latencystats), `latencyAddSampleIfNeeded`의 `command`/`fast-command`(지연 모니터, 밀리초로 나눔. 둘의 구분은 명령의 `CMD_FAST` 표시로 정함)에 씀 — "SLOWLOG, commandstats, latencystats는 모두 실행 시간(⑤)입니다", "지연 모니터는 명령이 아닌 일도 기록하지만, 기본으로 꺼져 있습니다"
  - `initServerConfig`: `latency_tracking_info_percentiles` 기본 50, 99, 99.9 — latencystats 기본 백분위
  - `resetCommandTableStats`: `calls`, `microseconds`, 지연 히스토그램을 지움(`CONFIG RESETSTAT`에서 호출, `config.c`의 `configResetStatCommand`) — "지표를 읽을 때 단위와 모은 기간부터 확인합니다"의 누적 기준
  - `fork` 직후 `stat_fork_time`을 재고 `latencyAddSampleIfNeeded("fork", ...)`; INFO의 `latest_fork_usec` — 장면 4, 5단계
  - INFO 생성부: `latency_percentiles_usec_<명령>`, `client_recent_max_output_buffer`, `instantaneous_output_kbps`, `eventloop_duration_sum`, `eventloop_duration_cmd_sum`, `rdb_last_save_time`, `rdb_bgsave_in_progress`, `aof_rewrite_in_progress`
  - `CLIENTS_PEAK_MEM_USAGE_SLOTS 8`: 연결 버퍼의 최근 최댓값을 초 단위 칸 8개로 기억함 — "`client_recent_max_output_buffer`는 최근 몇 초만 기억"
- [`src/slowlog.c`](https://github.com/redis/redis/blob/8.2.10/src/slowlog.c): `slowlogPushEntryIfNeeded`(실행 시간이 기준 이상이면 맨 앞에 넣고, `slowlog-max-len`을 넘으면 가장 오래된 항목부터 지움), 항목마다 연결 주소와 이름 저장
- [`src/rdb.c`](https://github.com/redis/redis/blob/8.2.10/src/rdb.c), [`src/aof.c`](https://github.com/redis/redis/blob/8.2.10/src/aof.c): "Background saving started by pid", "Background append only file rewriting started by pid" 로그(NOTICE 수준) — "Redis 로그의 저장 시작 기록"
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `latency-tracking` 기본 yes, `slowlog-log-slower-than` 기본 10000, `slowlog-max-len` 기본 128, `latency-monitor-threshold` 기본 0 — "사건이 지난 뒤에는 볼 수 없는 것들: 미리 켜 둘 것"의 기본 상태
- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): SLOW LOG 절(`slowlog-log-slower-than`의 단위는 마이크로초, 음수는 끔), LATENCY MONITOR 절
- [`src/blocked.c`](https://github.com/redis/redis/blob/8.2.10/src/blocked.c): `updateStatsOnUnblock`(기다림이 풀린 명령은 실행 시간 + 기다린 시간 + 응답 시간을 `commandstats`의 `usec`, 지연 히스토그램, SLOWLOG에 기록) — 기다리는 명령의 예외
- [`src/latency.c`](https://github.com/redis/redis/blob/8.2.10/src/latency.c)
  - `latencyAddSample`: 같은 초의 표본은 최댓값으로 합침
  - `createLatencyReport`(`LATENCY DOCTOR`): 모니터를 켜지 않았으면 안내만 함, `fast-command` 이벤트에 `advise_scheduler`(시스템이 Redis에 CPU를 주지 않는다는 설명과 `redis-cli --intrinsic-latency 100` 안내), `THPGetAnonHugePagesSize() > 0`이면 THP 끄기 조언, `fork` 이벤트에 fork 속도 평가
- [`src/latency.h`](https://github.com/redis/redis/blob/8.2.10/src/latency.h): `LATENCY_TS_LEN 160`, `latencyAddSampleIfNeeded`(기준이 0이면 기록하지 않음)
- [`src/redis-cli.c`](https://github.com/redis/redis/blob/8.2.10/src/redis-cli.c): `latencyMode`(`PING`을 `mstime()`으로 재므로 밀리초 정수, `LATENCY_SAMPLE_RATE 10`ms 간격, `LATENCY_HISTORY_DEFAULT_INTERVAL 15000`ms), `intrinsicLatencyMode`(서버에 연결하지 않고 `compute_something_fast`를 반복해 최대 시간을 마이크로초로 출력)
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `catClientInfoString`(`CLIENT LIST`의 `name`, `addr`, `idle`, `qbuf`, `omem`, `tot-mem`, `cmd`, `tot-net-out`, `tot-cmds` 등)

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`. 특정 Redis 버전 전용 문서가 아니므로 소스와 다르면 소스를 따릅니다)

- [latency](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/optimization/latency.md): "Measuring latency"(`redis-cli --latency`), "Latency baseline"(intrinsic latency, 서버에서 실행해야 하며 Redis에 연결하지 않음), "Latency induced by network and communication"(자식 프로세스를 메인 이벤트 루프와 같은 코어에서 돌리지 말 것), "Latency generated by fork", "Latency induced by transparent huge pages"(끄기 권고), "Latency induced by swapping (operating system paging)"(`/proc/<pid>/smaps`의 `Swap:` 확인)
- [latency-monitor](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/optimization/latency-monitor.md): 이벤트 목록(`command`, `fast-command`, `fork`, `expire-cycle`, `eviction-cycle`, `eviction-del`, `aof-*` 등), 이벤트당 160개, 같은 초는 최댓값으로 합침, `LATENCY LATEST`·`LATENCY HISTORY`
- [slowlog](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/slowlog.md): 실행 시간에 클라이언트와 주고받는 I/O가 들어가지 않음
- [info](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/info.md): `latencystats` 절(명령별 백분위, `latency-tracking`, `latency-tracking-info-percentiles`)
- [client-list](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/client-list.md): 필드의 뜻

**Lettuce 6.6.0.RELEASE**

- [`protocol/CommandHandler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/CommandHandler.java): `addToStack`에서 `potentiallyWrapLatencyCommand`가 `sent(nanoTime())`을 찍음(명령을 응답 대기 목록에 올릴 때), `decode`에서 그 명령의 해석을 처음 시작할 때 `firstResponse`를 찍고, 해석을 마치면 `recordLatency`가 `completion`을 계산 — "Lettuce 타이머는 명령을 연결에 넘긴 순간부터 잽니다"
- [`metrics/MicrometerCommandLatencyRecorder.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/metrics/MicrometerCommandLatencyRecorder.java): `lettuce.command.completion`, `lettuce.command.firstresponse`, `command`·`local`·`remote` 태그, 히스토그램을 켰을 때만 백분위 발행
- [`metrics/MicrometerOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/metrics/MicrometerOptions.java): `DEFAULT_HISTOGRAM = false`, `DEFAULT_LOCAL_DISTINCTION = false`, `DEFAULT_TARGET_PERCENTILES`(0.50, 0.90, 0.95, 0.99, 0.999)

**Spring Boot v3.5.16**

- [`LettuceMetricsAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/metrics/redis/LettuceMetricsAutoConfiguration.java): `MeterRegistry` 빈이 있으면 `MicrometerOptions.create()`(기본값)로 `MicrometerCommandLatencyRecorder`를 붙임
- [`PropertiesMeterFilter.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/metrics/PropertiesMeterFilter.java): `management.metrics.distribution.percentiles-histogram.*`을 계량기 이름과 그 앞부분(점 단위)으로 찾아 적용

**Spring Data Redis 3.5.13**

- [`core/AbstractOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/AbstractOperations.java): `ValueDeserializingRedisCallback.doInRedis`가 `rawKey`로 직렬화한 뒤 연결을 부르고, 받은 바이트를 `deserializeValue`로 바꿈 — ②⑧이 Lettuce 타이머 밖이라는 근거
- [`connection/lettuce/LettuceConnection.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnection.java): `ByteArrayCodec`으로 Lettuce에 바이트 배열을 넘김

## 실무 포인트

### 중요도별 점검 항목

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | 지연 모니터를 평소에 켜 둔다 | 기본으로 꺼져 있고 지난 일은 기록되지 않습니다. `fork`·만료 정리처럼 SLOWLOG에 나오지 않는 멈춤을 가를 유일한 서버 쪽 기록입니다. 기준은 서비스가 견딜 지연보다 낮게, 평소 정상 동작이 쏟아지지 않을 만큼 높게 정합니다 |
| 필수 | 앱마다 `spring.data.redis.client-name`을 붙인다 | SLOWLOG와 `CLIENT LIST`에서 원인 명령을 보낸 앱을 바로 찾습니다. 장면 3에서 관리자 배치를 찾는 단서입니다 |
| 필수 | SLOWLOG를 "내 명령의 시간"이 아니라 "메인 스레드를 잡은 명령 찾기"로 읽는다 | 실행 시간만 재므로, 느린 요청 본인은 대부분 나오지 않습니다 |
| 알면 유리 | Lettuce 타이머의 히스토그램을 켜고, 앱의 Redis 호출 타이머를 둔다 | 2단계의 비교와 "가끔 느림"의 비율을 보려면 둘 다 필요합니다. 대가는 시계열 수와 모니터링 비용입니다 |
| 알면 유리 | `INFO`(stats, commandstats, clients)와 SLOWLOG를 주기 수집한다 | 누적값의 차이와 밀려난 SLOWLOG를 사건 뒤에도 볼 수 있습니다 |
| 알면 유리 | 운영 환경의 `--intrinsic-latency`를 한 번 재 둔다 | 그 환경의 기준선을 알면, 그보다 작은 목표 지연이 무리인지 미리 가늠할 수 있습니다. 측정한 동안의 값이므로 부하가 다른 시각에는 달라질 수 있습니다 |
| 참고 | `LATENCY DOCTOR` 출력을 읽어 둔다 | 기록을 사람이 읽을 조언으로 바꿔 줍니다. 판단은 위 단계로 직접 확인합니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 연결 이름, Lettuce 히스토그램, 앱 타이머 | 앱 시간과 `lettuce.command.completion`의 차이 |
| 큰 응답을 받는 명령을 나눠 읽을지, 공유 연결에서 뺄지 | 두 타이머의 평균 차이가 큰 명령 종류, `omem`, `tot-net-out` |
| 키 하나의 원소 수 상한, 전체 읽기 금지 | SLOWLOG의 반복 명령, `commandstats`의 `usec` 증가 폭 |
| 지연 모니터 기준, SLOWLOG 보관 개수 | `LATENCY LATEST`·`HISTORY`의 `fork`, `expire-cycle`, `fast-command` |
| RDB 저장 주기와 영속성 방식(운영 담당과 함께) | `latest_fork_usec`의 추세, 저장 시각과 신고 시각 |
| Redis 서버를 다른 프로세스와 함께 둘지 | `--intrinsic-latency`, 가상 머신 CPU 대기, 스왑 |

### 증상과 확인 순서

**증상 1**: "주문 API 한 대에서만 Redis 호출이 가끔 느리다."

1. 그 인스턴스의 앱 시간과 `lettuce.command.completion`을 같은 구간의 최댓값으로 비교합니다. 앱 시간만 늘었다면 GC 로그, 풀 빌리기 대기, 그 인스턴스의 다른 외부 호출을 봅니다.
2. Lettuce 시간도 늘었다면 같은 시각 그 인스턴스의 명령 종류 중 두 타이머의 평균 차이가 큰 것을 찾습니다. 있으면 같은 연결을 쓰는지 확인하고 그 명령의 응답 크기를 줄입니다.
3. 없다면 그 인스턴스에서 `redis-cli --latency`를 돌려 네트워크를 봅니다.

**대응과 부작용**: 큰 응답을 나눠 읽으면 왕복 수가 늘어 그 기능 자체는 조금 느려질 수 있습니다. 큰 응답을 받는 기능에 별도 연결을 주면 다른 명령의 줄서기는 사라지지만, 서버의 연결 수가 늘고 큰 응답이 서버 출력 버퍼와 네트워크를 쓰는 것은 그대로입니다.

**증상 2**: "모든 인스턴스가 같은 시각에, 모든 Redis 명령이 함께 느리다."

1. 그 시각의 SLOWLOG에서 다른 연결의 긴 명령을 찾습니다. 연결 이름으로 보낸 앱을 찾습니다.
2. 없으면 `LATENCY HISTORY`에서 `fork`, `expire-cycle`, `eviction-cycle`, AOF 이벤트를 봅니다. 꺼져 있었다면 `latest_fork_usec`와 저장 시각을 맞춰 보고, 지연 모니터를 켭니다.
3. 둘 다 없고 `fast-command` 표본이 있거나 O(1) 명령이 SLOWLOG에 있다면 서버 밖(CPU, 스왑, THP)을 봅니다.

**대응과 부작용**: 원인 명령을 나눠 실행하면 한 번에 메인 스레드를 잡는 시간은 줄지만 전체 일의 양과 원자성 단위가 바뀝니다. 저장 주기를 늘리면 `fork` 횟수는 줄지만 장애 때 잃는 데이터의 범위가 넓어집니다([Redis 영속성](./persistence.md)).

**증상 3**: "서버 지표는 모두 깨끗한데 앱은 느리다고 한다."

1. 앱 시간과 Lettuce 시간 중 무엇이 늘었는지부터 다시 봅니다. 같은 구간의 Lettuce 최댓값까지 평소대로이고 앱 시간만 늘었다면 Redis 밖입니다.
2. Lettuce 시간이 늘었다면 같은 연결의 큰 응답(3단계)을 먼저 지우고, 증상이 이어지는 동안 앱 서버와 Redis 서버 자신에서 `redis-cli --latency`를 함께 돌려 봅니다. 서버 안에서 평소대로이고 앱 서버에서만 튄다면 네트워크가 유력합니다. 서버 안에서도 튄다면 지연 모니터 기준 아래의 짧은 멈춤이 반복되는 것일 수 있습니다. 지연 모니터 기준을 낮추고 다음 발생을 기다립니다. `--latency`는 앱과 다른 연결이므로, 이 결과만으로 앱의 연결이 겪은 일을 확정하지는 않습니다.

**대응과 부작용**: 원인을 못 찾은 채 명령 타임아웃만 줄이면 느린 요청이 실패로 바뀔 뿐 원인은 남습니다. 실패를 받는 쪽의 동작을 함께 정합니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)).

## 말로 설명하기

### 30초 버전

앱이 본 Redis 호출 시간은 연결 얻기, 직렬화, 네트워크, 서버 안의 기다림, 실행, 응답 받기를 모두 더한 것이고, Redis 서버가 직접 재는 것은 대부분 실행 시간뿐입니다. 그래서 "SLOWLOG에 없으니 Redis 문제가 아니다"는 틀릴 수 있습니다. 바깥에서 안으로 좁힙니다. 앱 시간과 Lettuce 시간을 비교해 앱 안을 빼고, 같은 연결의 큰 응답을 보고, SLOWLOG와 지연 모니터로 메인 스레드가 그 시각에 무엇을 했는지 보고, 그래도 없으면 네트워크와 서버 밖을 봅니다. 지연 모니터는 기본으로 꺼져 있어 평소에 켜 둬야 합니다.

### 2분 버전

`GET` 하나가 80ms 걸렸다는 신고는 다섯 곳 중 어디서든 생길 수 있습니다. JVM 멈춤이나 풀 대기 같은 앱 안, 같은 공유 연결 위의 앞선 큰 응답, 다른 연결의 긴 명령 때문에 서버에서 차례를 기다린 시간, `fork`나 CPU·스왑 같은 서버 밖 멈춤, 네트워크입니다.

도구마다 재는 구간이 다릅니다. `lettuce.command.completion`은 명령을 연결에 넘긴 순간부터 응답을 해석할 때까지라 연결 얻기와 직렬화가 빠집니다. SLOWLOG, `commandstats`, `latencystats`는 모두 서버의 실행 시간만 잽니다. 지연 모니터는 실행 시간에 더해 `fork`, 만료 정리, AOF 쓰기 같은 명령 밖의 멈춤도 기록하지만 기본으로 꺼져 있습니다. `redis-cli --latency`는 별도 연결의 `PING` 왕복을 밀리초 단위로 재고, `--intrinsic-latency`는 Redis와 무관하게 그 서버가 프로세스를 얼마나 멈출 수 있는지를 잽니다.

순서는 범위부터 정합니다. 한 인스턴스만이면 앱과 그 연결 쪽, 모든 인스턴스가 같은 시각이면 서버 쪽입니다. 그다음 앱 시간과 Lettuce 시간을 비교하고, `completion`과 `firstresponse`의 평균 차이로 큰 응답을 받는 명령 종류를 찾고, SLOWLOG에서 내 명령과 다른 명령을 보고, 지연 모니터와 `latest_fork_usec`로 명령 밖의 멈춤을 보고, 마지막으로 네트워크와 CPU·스왑을 봅니다. 값이 크지 않은 O(1) 명령이 SLOWLOG에 길게 있으면, 키보다 먼저 서버가 CPU를 받지 못했거나 스왑된 메모리를 읽었다는 가설을 확인합니다.

### 예상 질문과 꼬리 질문

- **Q. SLOWLOG가 비어 있으면 Redis는 원인에서 빠지나요?**
  아닙니다. SLOWLOG는 기준(기본 10ms) 이상인 명령의 실행 시간만 남깁니다. 메인 스레드가 `fork`나 만료 정리로 멈춘 시간, 기준보다 짧지만 응답이 큰 명령, 네트워크는 들어가지 않습니다. 또 128개를 넘으면 오래된 것부터 밀려나므로 사건 뒤 늦게 보면 지워졌을 수 있습니다. 비어 있다면 지연 모니터와 `latest_fork_usec`, 이벤트 루프 지표를 이어서 봅니다("5단계: 메인 스레드가 그 시각에 다른 일을 했는가").
- **꼬리: 그럼 SLOWLOG 기준을 1ms로 낮추면 되지 않나요?**
  실행 시간은 더 잘게 잡히지만, 기다린 시간과 명령 밖의 일은 여전히 들어가지 않습니다. 대신 기록이 많아져 128개가 더 빨리 밀려납니다. 재는 대상이 정해진 도구이므로, 빠진 구간은 다른 도구로 채웁니다.
- **Q. Lettuce 지표가 정상이면 Redis 쪽은 깨끗하다고 봐도 되나요?**
  같은 구간의 Lettuce 최댓값까지 평소대로라면 네트워크와 서버 구간(③~⑦)은 깨끗하다고 볼 수 있습니다. 평균이나 백분위만 평소대로인 것으로는 부족합니다. "가끔 80ms"는 평균에 묻히고, 충분히 드물면 백분위에도 묻히기 때문입니다. 그러면 남는 것은 연결 얻기, 직렬화, 역직렬화, JVM 멈춤 같은 앱 안입니다. 다만 JVM 전체가 멈췄다면 Lettuce 시간도 함께 늘 수 있으므로, 거꾸로 "Lettuce 시간이 늘었으니 Redis 쪽"이라고 단정하지는 않고 GC 로그를 함께 봅니다("2단계: 앱 안인가, 밖인가 — 앱의 시간과 Lettuce의 시간을 나란히 봅니다").
- **꼬리: 풀을 쓰는데 풀 대기는 어디서 보나요?**
  Lettuce 타이머는 명령이 연결에 닿은 뒤부터 재므로 풀 대기가 들어가지 않습니다. 앱이 Redis 호출 전체를 감싼 타이머와 Lettuce 타이머의 차이로 보거나, 스레드 덤프에서 요청 스레드가 풀 빌리기를 기다리는지 봅니다. 풀의 상한과 기다림의 기본값은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에 있습니다.
- **Q. 서버가 한가했는데 `GET`만 SLOWLOG에 3ms로 찍혀 있습니다. 키가 큰가요?**
  `GET`은 키를 찾는 일이 O(1)인 명령입니다. 그런데도 서버가 한가한 시각에 실행이 밀리초 단위였다면, 키보다 먼저 그 순간 Redis가 CPU를 받지 못했거나 스왑된 메모리를 읽었을 가능성을 봅니다. 지연 모니터의 `fast-command` 이벤트, `--intrinsic-latency`, 스왑 여부를 확인합니다("6단계: 서버도 한가했다면 — 서버 밖과 네트워크를 봅니다").
- **꼬리: 값이 아주 큰 키라면요?**
  그 가능성도 남습니다. 다만 값이 큰 `GET`의 비용이 실행과 응답 전송에 어떻게 나뉘는지는 이 문서에서 재지 않았습니다. 큰 값이 의심되면 실행 시간만 보지 말고 `GET`의 두 타이머 평균 차이와 `omem`으로 응답 쪽을 함께 봅니다("3단계: 응답이 컸는가 — 같은 연결의 큰 응답을 찾습니다").
- **Q. 모든 인스턴스가 5분마다 잠깐씩 느립니다. 무엇을 먼저 보나요?**
  주기적이라는 점이 정해진 일을 가리킵니다. RDB 저장 주기와 겹치는지 저장 시각과 `latest_fork_usec`로 맞추고, 지연 모니터에 `fork` 이벤트가 있는지 봅니다. 배치나 대량 만료가 같은 주기로 도는지도 봅니다.
- **꼬리: `fork`가 원인이면 메모리를 늘리면 되나요?**
  아닙니다. `fork` 시간은 Redis가 쓰는 메모리가 클수록 대체로 길어지므로 메모리를 늘린다고 짧아지지 않습니다. 저장 빈도, 영속성 방식, 가상화 환경이 선택지이고, 저장 빈도를 줄이면 장애 때 잃는 데이터 범위가 넓어지는 대가가 있습니다([Redis 영속성](./persistence.md)).
- **Q. 서버 지표는 깨끗하고 한 인스턴스만 느린데 네트워크라고 어떻게 확신하나요?**
  확신이 아니라 남은 후보입니다. 그 인스턴스의 앱 시간과 Lettuce 시간이 함께 늘었고, 같은 연결의 큰 응답이 없고, 그 인스턴스 호스트에서 돌린 `redis-cli --latency`는 튀는데 Redis 서버에서 돌린 것은 평소대로라면 네트워크 쪽이 가장 유력합니다. 다만 `--latency`는 밀리초 정수로 재므로 1ms 미만의 차이는 가르지 못합니다.
- **Q. `latencystats`에 p99가 있는데 왜 앱 쪽 백분위가 따로 필요한가요?**
  `latencystats`는 서버의 실행 시간 백분위이고 서버 시작 이후 누적입니다. 기다린 시간과 네트워크가 빠지고, 몇 분의 사건은 몇 주의 누적에 묻힙니다. 사용자가 겪는 꼬리는 앱 쪽 백분위로만 보입니다.
- **꼬리: 그럼 서버 쪽 백분위는 어디에 쓰나요?**
  명령 종류별로 실행이 무거워지는 추세를 봅니다. 특정 명령의 p99.9가 키가 커지며 함께 오른다면, SLOWLOG 기준에 닿기 전에 미리 알 수 있습니다. 누적 백분위라 짧은 변화는 둔하게 드러나므로, 주기 수집해 장기 추세로 봅니다.
- **Q. Redis를 클러스터로 바꾸면 이런 지연이 줄어드나요?**
  원인에 따라 다릅니다. 여러 키에 흩어진 일이 메인 스레드 하나를 채웠다면 나눠집니다. 하지만 키 하나가 큰 경우, `fork`, 네트워크, 앱 안의 원인은 클러스터로 풀리지 않습니다. 진단 없이 클러스터로 가면 비용만 치르고 증상은 남을 수 있습니다([Hot Key와 Big Key](./hot-key-big-key.md)).
- **Q. 장애가 끝난 뒤 신고를 받았는데 무엇이 남아 있나요?**
  SLOWLOG는 밀려나지 않았다면 남아 있습니다. `latest_fork_usec`는 마지막 한 번만 남습니다. 지연 모니터는 켜져 있었다면 이벤트별 최근 160개 표본이 남습니다. `commandstats`·`latencystats`는 누적이라 시각을 잃습니다. 앱 쪽 Micrometer 지표는 모니터링 시스템에 남습니다. 그래서 "사건이 지난 뒤에는 볼 수 없는 것들: 미리 켜 둘 것"이 진단 순서만큼 중요합니다.
