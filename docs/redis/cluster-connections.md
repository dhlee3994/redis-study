# Redis Cluster의 연결 수: Spring 앱 한 대가 노드마다 연결을 몇 개 여는가

> **핵심 질문**: Spring Boot + Lettuce 앱은 Redis Cluster에 TCP 연결을 몇 개 열고, 그 수는 어디서 곱해지는가? 노드의 연결 한도(`maxclients`)에 닿으면 무엇이 실패하며, 배포·스케일아웃·지도 갱신 때 연결은 어떻게 몰리는가? 그래서 무엇을 미리 정하고 무엇을 지켜봐야 하는가?
>
> **범위**: Spring Boot의 기본 구성(Lettuce, 공유 연결)과 commons-pool2 풀을 쓰는 앱이 Redis Cluster에 여는 연결, 노드 쪽의 연결 한도와 연결 하나의 비용을 다룹니다. 서버 한 대에서의 공유 연결·전용 연결·풀·타임아웃은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에, 슬롯 지도와 토폴로지 갱신 설정은 [Redis Cluster](./cluster.md)에, 장애 조치 뒤 다시 연결하기까지의 시간은 [Redis 장애 조치와 클라이언트](./failover-client.md)에 있습니다. Jedis, Sentinel, TLS 설정은 다루지 않습니다.

## 배경 지식

### 이 문서의 예시: 노드 6대 클러스터와 주문 API 20대

문서 끝까지 다음 장면 하나를 씁니다. [Redis Cluster](./cluster.md)의 예시 클러스터에 복제본을 그려 넣은 것입니다.

- **클러스터**: 주 서버 A(슬롯 0~5460), B(5461~10922), C(10923~16383)와 각각의 복제본 A', B', C', 모두 노드 6대입니다. 슬롯은 Redis Cluster가 키 이름으로 계산하는 0~16383의 번호이고, 슬롯마다 맡는 주 서버가 정해져 있습니다. 주 서버 하나가 슬롯 수천 개를 함께 맡습니다(아래 "슬롯 지도와 토폴로지 갱신").
- **주문 API**: Spring Boot 앱이고 Lettuce를 씁니다. 평소 20대가 돌고, 특가 행사 때 오토스케일링으로 60대까지 늘어납니다.
- **Redis 설정**: `spring.data.redis.cluster.nodes`에 A, B, C의 주소를 적었습니다. [Redis Cluster](./cluster.md)의 권장대로 `spring.data.redis.lettuce.cluster.refresh.adaptive=true`, `spring.data.redis.lettuce.cluster.refresh.period=30s`로 토폴로지 갱신을 켰습니다. 복제본 읽기 설정(`spring.data.redis.lettuce.read-from`)은 두지 않았습니다. 다른 의존성이 commons-pool2를 끌어와 연결 풀이 저절로 켜져 있고, 풀 설정은 기본값(`max-active` 8, `max-idle` 8)입니다.
- **요청**: 일반 요청은 `GET stock:{item:N}`, `DECR stock:{item:N}`과 주문 스크립트입니다. 상품이 많아 키가 A, B, C에 고루 흩어집니다.
- **알림 작업자**: 인스턴스마다 스레드 2개가 `BLPOP notify:queue 5`로 알림을 기다립니다. `notify:queue`의 슬롯은 14116이라 C가 맡습니다.
- **헬스 체크**: Spring Boot Actuator의 헬스 엔드포인트를 켜 두었고, 모니터링이 주기적으로 부릅니다.

```text
주문 API 인스턴스 1대 (평소 20대, 행사 때 60대)
┌───────────────────────────────────────────┐
│ 요청 스레드들 ── 공유 연결 ───────────────┼──▶ A, B, C
│ 알림 작업자 2개 ── 전용 연결 2개 ─────────┼──▶ C
│ 헬스 체크 ── 리액티브 공유 연결 ──────────┼──▶ 6대 중 무작위 하나
│ 토폴로지 갱신(30초마다) ── 임시 연결 ─────┼──▶ 6대 모두
└───────────────────────────────────────────┘
        A  B  C  (주 서버)     A' B' C' (복제본)
```

화살표는 연결이 향하는 노드입니다. 그림의 "연결" 하나하나가 TCP 연결 하나가 아니라는 것이 이 문서의 본론이라, 실제 TCP 연결 수는 아래 "클러스터 연결 하나는 TCP 연결 여러 개를 묶은 것입니다"에서 다시 셉니다. 노드끼리의 연결(클러스터 버스)은 그림에서 뺐고 "노드 하나가 받는 연결 수를 셉니다"에서 다룹니다.

인스턴스 수, 스레드 수, 주기 같은 숫자는 계산을 보여 주기 위한 **가상의 숫자**이며 이 문서에서 재지 않았습니다. 동작은 `근거` 절의 Redis 8.2.10, Lettuce 6.6.0.RELEASE, Spring Data Redis 3.5.13, Spring Boot 3.5.16 소스로 확인했습니다.

### Redis에게 클라이언트는 연결 하나입니다

- **한 줄**: Redis 서버는 맺어진 TCP 연결 하나를 클라이언트 하나로 셉니다. 주문 API 인스턴스 한 대가 연결 15개를 열면 서버에게는 클라이언트 15개입니다.
- **이 주제에서는**: 서버의 연결 한도와 연결마다 드는 메모리는 "앱이 몇 대인가"가 아니라 "연결이 몇 개인가"로 정해집니다. 서버가 연결마다 입력 버퍼와 응답 버퍼를 따로 둔다는 점은 [Redis 이벤트 루프](./event-loop.md)의 "Redis에서 '클라이언트'는 연결 하나입니다"에 있습니다.

### 파일 디스크립터: 운영체제가 연결마다 나눠 주는 번호

- **한 줄**: 파일 디스크립터는 운영체제가 프로세스에게 열린 파일이나 소켓(네트워크 연결의 끝점)마다 하나씩 나눠 주는 번호입니다.
- **비유**: 물품 보관소의 번호표입니다. 짐(연결)을 맡길 때마다 번호표를 하나 받고, 보관소(운영체제)는 한 사람(프로세스)이 동시에 가질 수 있는 번호표 수를 정해 둡니다.
- **실제로는**: Redis 서버 프로세스는 클라이언트 연결마다, 노드끼리의 연결마다, 그리고 로그 파일·영속화 파일·대기 중인 포트에도 번호를 하나씩 씁니다. 프로세스가 동시에 열 수 있는 수의 상한은 운영체제 설정이며, Linux에서는 `ulimit -n`으로 봅니다. **비유와 다른 점**: 보관소는 번호표가 떨어지면 줄을 세우지만, 운영체제는 상한에 닿으면 새로 열기를 바로 실패시킵니다. JVM에서 소켓 하나를 열 때도 같은 자원을 하나 씁니다.
- **이 주제에서는**: Redis의 연결 한도 `maxclients`는 이 상한과 묶여 있어, 운영체제 상한이 낮으면 설정 파일에 적은 값보다 낮아집니다("`maxclients`에 닿으면 새 연결만 거절됩니다").

### 공유 연결, 전용 연결, 풀

- **한 줄**: Spring Data Redis는 일반 명령을 모든 스레드가 함께 쓰는 공유 연결 하나로 보내고, `BLPOP` 같은 기다리는 명령이나 파이프라인(`executePipelined`처럼 명령 여러 개를 모아 보내고 응답을 한꺼번에 받는 API)은 그 작업 동안만 쓰는 전용 연결로 보냅니다. 공유 연결도 응답을 기다리지 않고 명령을 이어 보내지만, 파이프라인은 모은 명령의 응답을 한 덩어리로 받아야 해서 다른 스레드의 명령과 섞이지 않게 따로 둡니다. `ReactiveRedisTemplate` 같은 리액티브 API(결과를 `Mono`·`Flux`로 돌려받는 비동기 API)는 일반 API와 다른 형식의 연결을 쓰므로, 리액티브 API용 공유 연결이 따로 하나 생깁니다. commons-pool2가 클래스패스에 있으면 Spring Boot가 풀을 켜서 전용 연결을 다시 쓰고, 공유 연결도 풀에서 하나를 빌려 계속 붙잡습니다.
- **이 주제에서는**: 서버 한 대라면 이 "연결" 하나가 TCP 연결 하나입니다. 클러스터에서는 그렇지 않다는 것이 이 문서의 출발점입니다. 공유 연결로 충분한 이유와 풀의 기본값은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "기본 구성: 연결 하나를 모든 요청 스레드가 나눠 씁니다"와 "연결 풀은 언제 필요하고, 무엇을 대가로 치르는가"에 있습니다.

### 슬롯 지도와 토폴로지 갱신

- **한 줄**: 클러스터 클라이언트는 "어느 슬롯을 어느 노드가 맡는가"의 지도(슬롯 지도)를 들고, 키의 슬롯을 계산해 맡은 노드로 직접 보냅니다. 지도를 노드에 물어 다시 받는 일을 토폴로지 갱신이라고 합니다.
- **이 주제에서는**: 앱이 어느 노드에 연결을 여는지는 지도가 정하고, 지도를 받는 일 자체가 노드마다 연결을 엽니다. 지도와 갱신 설정은 [Redis Cluster](./cluster.md)의 "Lettuce는 지도를 언제 받고, 언제 고치는가"에 있습니다.

## 클러스터 연결 하나는 TCP 연결 여러 개를 묶은 것입니다

Spring Data Redis가 클러스터에서 쓰는 "연결" 하나는 Lettuce의 클러스터 연결(`StatefulRedisClusterConnection`) 하나입니다. 그리고 클러스터 연결 하나는 안에 TCP 연결을 여러 개 둡니다.

```text
클러스터 연결 1개 (주문 API의 JVM 안에 있는 객체)
├─ 기본 연결 ─────▶ 노드 하나    키가 없는 명령용. 클러스터 연결을 만들 때 맺음
├─ 노드 연결 ─────▶ A            A의 슬롯에 속한 키를 처음 보낼 때 맺음
├─ 노드 연결 ─────▶ B            〃
└─ 노드 연결 ─────▶ C            〃
```

화살표 하나가 TCP 연결 하나이고, 명령이 그 방향으로 나갑니다. 복제본 연결은 아래 "복제본 읽기를 켜면 그 샤드의 후보 노드 모두에 한꺼번에 맺습니다"에서 더합니다.

### 기본 연결은 만들 때, 노드 연결은 처음 쓸 때 맺습니다

- **기본 연결**: 클러스터 연결을 만들면 Lettuce는 곧바로 노드 하나에 TCP 연결을 맺습니다. `PING`처럼 키가 없어 슬롯으로 노드를 고를 수 없는 명령이 이 연결로 나갑니다. 어느 노드에 맺을지는 지도를 받을 때 본 노드별 연결 수가 적은 순으로 정렬한 목록에서 고릅니다.
- **노드 연결**: 키가 있는 명령은 키의 슬롯으로 주 서버를 찾고, 그 주 서버로의 연결이 아직 없으면 그때 맺습니다. 한 번 맺은 연결은 그 주 서버가 맡은 모든 슬롯이 함께 씁니다. 슬롯마다 연결이 생기는 것이 아니라 **주 서버마다 하나**입니다.

Kotlin으로 비유하면 클러스터 연결 안에 `노드 주소 → 연결` 맵이 있고, 명령을 보낼 때 `computeIfAbsent`로 채우는 것과 같습니다. 다른 점은 지도가 바뀌어 사라진 노드의 연결을 Lettuce가 닫는다는 것입니다(`closeStaleConnections` 기본 켜짐).

그래서 예시의 공유 연결은 첫 요청에서 기본 연결 1개로 시작해, 요청이 A, B, C의 키를 한 번씩 건드리고 나면 **TCP 연결 4개**(기본 1 + 주 서버 3)가 됩니다. 주 서버가 많을수록 클러스터 연결 하나가 여는 TCP 연결도 늘어납니다. Lettuce 6.6.0에서 노드 연결을 미리 모두 맺어 두는 설정은 찾지 못했습니다.

### 복제본 읽기를 켜면 그 샤드의 후보 노드 모두에 한꺼번에 맺습니다

`spring.data.redis.lettuce.read-from`을 정하면 읽기 명령은 다른 길로 갑니다. 여기서 샤드는 주 서버 하나와 그 복제본들의 묶음입니다(예시의 C와 C'). 어떤 슬롯을 처음 읽을 때, Lettuce는 그 슬롯을 맡은 주 서버와 그 복제본 중 `read-from`이 고른 후보 **모두**에 한꺼번에 연결을 맺고, 그중 하나로 읽기를 보냅니다. 복제본 연결은 쓰기용과 따로 맺으며, 맺자마자 복제본에서 읽기를 허락받는 `READONLY`를 보냅니다. 주 서버로의 연결은 쓰기용 연결을 함께 씁니다.

예를 들어 `replicaPreferred`(복제본 우선, 없으면 주 서버)는 복제본과 주 서버를 모두 후보로 고릅니다. 예시에 이 값을 주면 공유 연결 하나가 기본 1 + 주 서버 3 + 복제본 3 = **TCP 연결 7개**가 됩니다. 복제본은 지도를 받을 때 본 복제 진행 위치가 0이 아닌 것, 곧 한 번이라도 복제를 받은 것만 후보가 됩니다. 이 조건이 지금 복제 연결이 살아 있거나 데이터가 최신이라는 뜻은 아닙니다. 복제본을 더 둘수록 클러스터 연결 하나가 여는 연결이 함께 늘어납니다.

### 지도를 받을 때마다 모든 노드에 잠깐 쓰는 연결을 엽니다

토폴로지 갱신은 이미 맺어 둔 연결을 쓰지 않습니다. 갱신할 때마다 물어볼 노드마다 **새 TCP 연결**을 맺고, `lettuce#ClusterTopologyRefresh`라는 이름을 붙인 뒤 `CLUSTER NODES`와 `INFO`를 보내고, 답을 받으면 닫습니다.

- **처음 지도를 받을 때**: 먼저 설정에 적은 시드 노드(예시의 A, B, C)에 연결합니다. `dynamic-refresh-sources`가 기본값 `true`면 받은 지도에서 새로 알게 된 노드(A', B', C')에도 이어서 연결합니다. 예시에서는 6개입니다.
- **그 뒤의 갱신**: `dynamic-refresh-sources`가 `true`면 지도에 있는 모든 노드에 묻습니다. 예시에서는 갱신 한 번에 임시 연결 6개입니다.
- **누가 하는가**: 갱신은 클러스터 연결마다가 아니라 Lettuce 클라이언트(`RedisClusterClient`)마다 합니다. Spring Data Redis는 연결 팩토리 하나에 클라이언트 하나를 두므로, 보통은 인스턴스마다 하나입니다.

예시의 주기 갱신 30초를 60대에 곱하면, 클러스터는 30초마다 6 × 60 = 360개의 연결을 새로 받고 곧 닫습니다. 노드 하나로 보면 30초마다 60개입니다. 이 연결은 오래 남지 않지만, 맺고 닫는 일은 그때마다 일어나고 열려 있는 동안은 연결 한도에도 들어갑니다. 갱신 명령의 수가 늘어나는 비용은 [Redis Cluster](./cluster.md)의 "노드와 요청이 늘면 무엇이 달라지는가"에 있습니다.

## Spring 앱 한 대는 클러스터 연결을 몇 개 만드는가

앞 절에 따르면 TCP 연결 수는 클러스터 연결마다 (닿은 노드 수 + 기본 연결 1)을 구해 모두 더한 값입니다. 클러스터 연결마다 닿는 노드 수가 다르므로, 닿는 노드가 같은 연결끼리만 개수를 곱해 묶을 수 있습니다. 남은 질문은 앱이 클러스터 연결을 몇 개 만드는가입니다. 애플리케이션 코드가 직접 쓰지 않는 경로도 클러스터 연결을 만든다는 점이 중요합니다.

| 누가 쓰는가 | 언제 생기는가 | 개수 | 닿는 노드 |
| --- | --- | --- | --- |
| 공유 연결 | 첫 Redis 명령 때. Spring Boot는 미리 만들지 않습니다 | 1 | 명령이 간 주 서버 |
| 전용 연결(`BLPOP`, 파이프라인 등) | 작업마다 풀에서 빌림. 풀이 없으면 작업마다 새로 만들고 닫음 | 동시에 쓰는 수. 풀이 있으면 `max-active`에서 공유 연결 몫 1을 뺀 수까지 | 그 작업의 키가 있는 주 서버 |
| 노드를 인자로 받는 명령(`RedisClusterConnection`의 `keys(node, pattern)`, 노드별 `CLUSTER INFO` 등) | 처음 쓸 때 하나를 만들어 앱이 끝날 때까지 보유 | 1 | 명령을 보낸 노드. 복제본도 포함될 수 있음 |
| 리액티브 API의 공유 연결 | 리액티브 API를 처음 쓸 때. Actuator 헬스 체크가 이 경로를 씀 | 1 | 명령이 간 노드 |
| Pub/Sub 구독(`RedisMessageListenerContainer` 등) | 구독 연결마다 | 구독 연결 수 | 일반 `SUBSCRIBE`·`PSUBSCRIBE`는 키가 없어 그 클러스터 연결의 기본 연결 하나로 나감 |

Pub/Sub 행을 뺀 모든 행에 기본 연결 1개씩이 더 붙습니다. Pub/Sub 구독은 기본 연결 자체가 구독 연결이라 더하지 않습니다. 표에 없는 토폴로지 갱신의 임시 연결은 앞 절처럼 따로 셉니다.

두 행은 설명이 필요합니다.

- **노드를 지정하는 명령은 공유 연결을 쓰지 않습니다.** Spring Data Redis는 이런 명령을 위해 클러스터 연결을 하나 따로 빌려 계속 붙잡습니다. 풀이 켜져 있어도 이 연결은 일반 연결과 다른 종류로 빌리므로, 일반 연결의 `max-active` 몫을 줄이지 않습니다. 풀은 빌리는 연결 종류마다 따로 만들어지기 때문입니다.
- **헬스 체크는 리액티브 연결을 씁니다.** Lettuce는 리액티브 라이브러리(Reactor)에 의존하므로 Lettuce를 쓰는 앱에는 늘 Reactor가 있습니다. 이때 Spring Boot는 Redis 헬스 체크로 리액티브 쪽을 등록하고, 그 헬스 체크는 리액티브 공유 연결로 활성 노드 중 **무작위 하나**에 `CLUSTER INFO`를 보냅니다. 복제본도 후보라서, 헬스 체크가 여러 번 불리고 나면 이 클러스터 연결 하나가 노드 6대 모두에 연결을 맺게 됩니다. 이 경로는 자동 구성의 등록 조건과 소스를 읽어 판단한 것이며, 실행해 확인하지 않았습니다.
- **같은 노드에 연결이 둘 생길 수도 있습니다.** Lettuce는 노드 연결을 "주소·포트로 찾은 연결"과 "노드 ID로 찾은 연결"로 따로 캐시합니다. 키로 고른 연결은 주소·포트로, 헬스 체크처럼 노드를 지정한 리액티브 명령은 노드 ID로 찾습니다. 그래서 앱이 리액티브 API로 일반 키 명령도 보낸다면, 리액티브 공유 연결 하나가 같은 주 서버에 연결 2개를 가질 수 있습니다. 예시는 리액티브 API를 헬스 체크에만 쓰므로 이 겹침이 없고, 아래 계산은 이 예시의 경로에 한정한 것입니다.

### 예시의 인스턴스 한 대가 여는 연결

일반 요청이 A, B, C를 모두 건드리고, 헬스 체크가 6대를 한 번씩 거친 뒤의 상태입니다.

| 노드 | 공유 연결 | 헬스 체크(리액티브) | 알림 작업자 전용 2개 | 합 |
| --- | --- | --- | --- | --- |
| A, B | 1 | 1 | 0 | 각 2 |
| C | 1 | 1 | 2 | 4 |
| A', B', C' | 0 | 1 | 0 | 각 1 |
| 기본 연결 | 1 | 1 | 2 | 4 (어느 노드인지는 그때그때 다름) |

인스턴스 한 대가 늘 들고 있는 TCP 연결은 2 + 2 + 4 + 3 + 4 = **15개**이고, 30초마다 갱신용 6개가 잠깐 더해집니다. 풀 기준으로 보면 공유 연결 1 + 전용 2 = 3개를 빌렸을 뿐인데 그 TCP 연결이 8개(공유 4 + 전용 2 × 2)이고, 일반 풀과 따로 빌리는 헬스 체크(리액티브) 연결이 7개를 더합니다. `max-active` 8이라는 숫자를 "Redis 연결 8개"로 읽으면 안 되는 이유입니다.

## 노드 하나가 받는 연결 수를 셉니다

### 한도는 노드마다 따로입니다

`maxclients`는 노드 하나하나의 설정이고, 노드마다 자기에게 맺어진 연결만 셉니다. 클러스터 전체의 합이 아니라 **가장 많이 받는 노드**가 한도에 먼저 닿습니다. 노드 하나가 받는 연결은 다음과 같습니다.

```text
노드 X가 받는 연결
  = Σ(그 클러스터를 쓰는 서비스마다) 인스턴스 수 × 인스턴스 하나가 X에 연 연결
  + 갱신 중인 임시 연결
  + 복제 연결 (X가 주 서버면 붙어 있는 복제본 수, 복제본이면 주 서버로의 연결 1)
  + 클러스터 버스 연결 2 × (노드 수 − 1)
```

마지막 항은 노드끼리의 연결입니다. 노드마다 다른 노드 하나에 대해 나가는 연결 하나, 들어오는 연결 하나를 두므로 노드 6대 클러스터에서는 노드마다 2 × 5 = 10개입니다. Redis 8.2.10은 이 연결도 `maxclients`에 넣어 셉니다. 노드가 200대인 큰 클러스터라면 노드마다 398개가 클러스터 버스에 먼저 들어갑니다.

예시에서 가장 많이 받는 노드는 C입니다. 행사 때 60대가 되면 다음과 같습니다.

| 항목 | 계산 | 연결 수 |
| --- | --- | --- |
| 노드 연결 | 60 × 4 | 240 |
| 기본 연결 | 인스턴스마다 4개가 노드 6대에 흩어짐. 모두 C에 몰리는 최악이면 60 × 4 | 0~240 |
| 갱신 임시 연결 | 갱신이 겹친 순간 최대 60 × 1 | 0~60 |
| 복제 연결 | 복제본 C'가 C에 붙은 연결 | 1 |
| 클러스터 버스 | 2 × 5 | 10 |
| 합 | | 약 250~550 |

기본 한도 10,000에 비하면 여유가 큽니다. **공유 연결을 쓰는 기본 구성에서는 연결 수가 요청량이 아니라 인스턴스 수와 노드 수로 정해지고, 그 값은 대개 작습니다.** 요청이 초당 500건이든 5만 건이든 공유 연결은 하나이기 때문입니다.

### 공유를 끄고 풀을 키우면 곱이 커집니다

예시의 주문 API가 큰 응답 문제를 피하려고 `LettuceConnectionFactory`의 `shareNativeConnection`을 `false`로 바꾸고 `max-active`를 64로 올렸다고 합시다. 이제 일반 명령도 요청마다 풀에서 클러스터 연결을 빌립니다.

- 풀의 클러스터 연결은 여러 요청이 돌려쓰고, 요청의 키는 A, B, C에 흩어져 있습니다. 그래서 풀의 연결이 닫히지 않고 여러 번 재사용되면, 시간이 지나며 풀의 클러스터 연결 하나하나가 A, B, C 모두에 노드 연결을 갖게 됩니다(키 분포에서 끌어낸 판단. 실제로 얼마나 퍼지는지는 재사용 횟수와 키 분포에 달려 있어 재 봐야 합니다).
- 바쁜 시간에 풀이 64개까지 차면, 인스턴스 한 대가 주 서버 하나에 여는 노드 연결은 64개입니다. 60대면 주 서버 하나에 3,840개, 200대면 **12,800개로 기본 한도 10,000을 넘습니다**. 기본 연결 64개씩은 여기에 더해집니다.
- **`max-idle`을 그대로 두면 연결이 계속 새로 맺어집니다.** 풀은 연결을 돌려받을 때 놀고 있는 연결이 이미 `max-idle`개면 돌려받은 연결을 닫습니다. `max-active` 64에 `max-idle` 기본 8이면, 바쁜 구간이 끝나 연결이 한꺼번에 반납될 때 놀고 있는 연결 8개를 넘는 몫(최대 56개)이 닫히고, 다음 바쁜 순간에 다시 만들어집니다. 클러스터 연결 하나를 다시 만들 때마다 기본 연결과 노드 연결을 처음부터 다시 맺으므로, 노드가 받는 새 연결 수가 요청량을 따라 출렁입니다.

공유를 끄는 판단 자체는 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "공유를 끄면 모든 명령이 풀을 거칩니다"에서 다룹니다. 클러스터에서는 그 대가가 **(주 서버 수 + 1)배**로 커진다는 점을 더해 판단합니다.

## `maxclients`에 닿으면 새 연결만 거절됩니다

### 무엇을 세고, 무엇을 거절하는가

Redis 8.2.10은 새 연결을 받아들이는 순간에만 한도를 검사합니다. "지금 연결된 클라이언트 수(복제본 연결 포함) + 클러스터 버스 연결 수"가 `maxclients` 이상이면, 클러스터 모드에서는 `-ERR max number of clients + cluster connections reached`라는 오류 한 줄을 보내고 그 연결을 닫습니다. TLS 연결이면 암호화 협상 전이라 이 오류 문장조차 보내지 못하고 끊기만 합니다. 거절할 때마다 `INFO stats`의 `rejected_connections`가 1 늘어납니다.

가장 중요한 성질은 **이미 맺어진 연결은 건드리지 않는다**는 것입니다. 한도 근처에서 돌던 클러스터는 평소에는 멀쩡해 보이다가, 누군가 새 연결을 맺어야 하는 순간에만 실패합니다.

### 실제 한도는 설정 파일의 값보다 낮을 수 있습니다

Redis는 시작할 때 `maxclients` + 32개의 파일 디스크립터를 열 수 있도록 운영체제 상한을 올려 보고, 올리지 못하면 `maxclients`를 (실제 상한 − 32)로 낮춘 뒤 로그에 경고를 남깁니다. 32개는 영속화 파일, 로그, 대기 중인 포트 같은 내부 용도로 남겨 두는 몫입니다. 예를 들어 상한이 1,024인 환경에서는 `maxclients`가 992가 됩니다. 컨테이너나 관리형 환경의 상한은 환경마다 다르므로, 설정 파일 대신 **각 노드의 `INFO clients`에 나오는 `maxclients`**를 실제 한도로 봅니다.

### 예시에서 무엇이 실패하는가

앞 절의 "공유를 끄고 64로 키운" 주문 API가 150대에서 200대로 늘어나는 중이라고 합시다. A, B, C는 이미 한도 근처입니다.

| 새 연결이 필요한 일 | 결과 |
| --- | --- |
| 새 인스턴스가 첫 명령을 보낼 때 노드 연결을 맺음 | 연결 맺기가 실패해 그 명령이 실패합니다. Spring에서는 Lettuce의 연결 실패가 `RedisConnectionFailureException`으로 바뀌어 올라옵니다. 기존 150대는 정상이라 "새로 뜬 인스턴스만 Redis가 안 된다"로 보입니다 |
| 풀이 늘어나며 새 클러스터 연결을 만듦 | 기존 인스턴스도 바쁜 순간에 풀을 늘리다 실패합니다. 이미 있던 풀 연결로 처리되는 요청은 성공하므로 일부 요청만 실패합니다 |
| 토폴로지 갱신의 임시 연결 | 그 노드에서는 지도를 받지 못합니다. Lettuce는 경고 로그(`Unable to connect to ...`)를 남기고 답한 노드들의 지도로 갱신합니다 |
| 끊긴 연결을 Lettuce가 다시 맺음 | 다시 맺기가 실패하고, Lettuce는 간격을 늘려 가며 계속 시도합니다. 그동안 그 연결로 가는 명령은 대기하거나 실패합니다 |
| 복제본이 주 서버에 다시 붙음 | 복제본의 연결도 같은 입구로 들어오므로 거절될 수 있습니다. 그러면 복제가 끊긴 채 남습니다(수락 경로에서 끌어낸 판단) |

첫 행의 예외 변환은 Spring Data Redis의 예외 변환 규칙과 Lettuce의 연결 경로를 읽어 판단한 것이며, 한도에 닿는 상황을 재현하지는 않았습니다.

마지막 행이 이 상황을 단순한 "앱 몇 대의 실패"보다 무겁게 만듭니다. 연결 한도는 앱만의 문제가 아니라, 노드가 복제와 장애 대비를 유지하는 데 필요한 연결과 같은 몫을 나눠 씁니다.

## 연결이 한꺼번에 몰리는 순간

평소의 연결 수는 앞 절의 계산으로 충분합니다. 위험한 것은 **짧은 시간에 새 연결이 몰리는 순간**입니다. 새 연결을 받아들이는 일과, Lettuce가 연결을 준비하며 보내는 명령(프로토콜 협상, 이름 붙이기 등)도 Redis 메인 스레드가 다른 명령 사이에 처리합니다. 하나하나는 짧지만 몰리면 그만큼 다른 명령이 기다립니다([Redis 이벤트 루프](./event-loop.md)의 "명령 하나가 모두를 멈추는 과정").

| 사건 | 몰리는 연결 | 예시에서 |
| --- | --- | --- |
| 스케일아웃 | 새 인스턴스마다 지도를 받는 임시 연결(노드 전부) + 기본 연결 + 첫 요청들의 노드 연결 | 20대 → 60대면 새 인스턴스 40대가 각자 임시 연결 6개, 기본 연결, 노드 연결을 맺습니다 |
| 롤링 배포 | 옛 인스턴스와 새 인스턴스가 함께 떠 있는 동안, 인스턴스 수가 평소보다 많아집니다 | 배포 도구가 한 번에 더 띄우는 수만큼 노드당 연결도 잠시 늘어납니다. 한도는 평소가 아니라 이 순간으로 계산합니다(배포 방식에서 끌어낸 판단) |
| 노드 재시작·장애 조치 | 그 노드로 가던 모든 인스턴스의 연결이 한꺼번에 끊기고, 같은 재연결 간격 규칙으로 거의 동시에 다시 맺습니다. 재연결이 5번 이상 이어지면 사건 기반 갱신이 일어나 인스턴스마다 임시 연결을 다시 엽니다 | C가 재시작되면 60대 × 4~8개의 연결이 비슷한 순간마다 몰리고, 갱신으로 60 × 6개가 더해집니다 |
| 주기 갱신 | 인스턴스 수 × 노드 수의 임시 연결이 주기마다 | 30초마다 360개 |

노드 재시작 때 재연결이 겹치는 것은 인스턴스들이 같은 순간에 끊김을 알고, 같은 간격 규칙(1, 2, 4, …밀리초로 늘어나 상한 30초)으로 다시 시도하기 때문입니다. 간격을 무작위로 흩는 방법과 그 대가는 [Redis 장애 조치와 클라이언트](./failover-client.md)의 "재연결 간격이 늘어나, 승격 뒤에도 최대 30초를 더 기다릴 수 있습니다"에 있습니다. 사건 기반 갱신은 인스턴스마다 30초에 한 번으로 제한되지만, 그 제한은 인스턴스 안의 것이라 인스턴스 60대는 각자 한 번씩 갱신합니다.

## 시작 시간: 첫 요청이 연결 비용을 냅니다

Spring Boot는 연결을 미리 맺지 않습니다(`LettuceConnectionFactory`의 `eagerInitialization` 기본 `false`를 Spring Boot가 바꾸지 않음). 그래서 새로 뜬 인스턴스의 **첫 Redis 명령**이 다음을 차례로 기다립니다.

1. **지도 받기**: 시드 노드에 임시 연결을 맺고 `CLUSTER NODES`와 `INFO`를 보낸 뒤, 새로 알게 된 노드에 다시 같은 일을 합니다. 왕복이 두 번 이어집니다.
2. **기본 연결 맺기**: 공유 연결을 만들며 노드 하나에 연결합니다.
3. **노드 연결 맺기**: 그 명령의 키가 있는 주 서버에 연결합니다. 이후 B, C의 키를 처음 쓰는 요청도 각각 연결 맺기를 한 번씩 기다립니다. `read-from`을 켰다면 슬롯을 처음 읽을 때 후보 노드 모두를 기다립니다.

TCP 연결 맺기와 연결 준비 명령에는 각각 왕복이 들므로, 배포 직후 첫 요청들은 평소보다 느립니다. 같은 노드로 동시에 들어온 첫 요청들은 연결 하나가 맺어지기를 함께 기다립니다.

**한 노드가 응답하지 않으면 1번이 길어집니다.** Lettuce는 지도를 받을 때 연결을 시도한 노드 **모두**의 결과(성공이든 실패든)가 나온 뒤에 다음으로 넘어갑니다. 연결을 거부하는 노드(프로세스가 죽었지만 서버는 살아 있음)는 곧바로 실패하지만, 응답 자체가 없는 노드(서버나 네트워크가 사라짐)에는 시간 제한 둘 중 먼저 끝나는 것까지 기다립니다. TCP 연결 맺기의 제한(`spring.data.redis.connect-timeout`, 비우면 Lettuce 기본 10초)과, 연결을 맺기 시작할 때부터 재는 연결 초기화 제한(명령 타임아웃 값)입니다. 예시의 설정에는 없지만 `spring.data.redis.timeout=100ms`를 정했다고 하면 초기화 제한 100ms가 먼저 작동해 그 노드를 포기합니다. 두 값을 모두 비워 두면 연결 제한 10초가 먼저 끝나므로, 첫 명령이 약 10초를 기다릴 수 있습니다(두 경우 모두 코드 경로에서 끌어낸 판단이며, 실제 경과 시간은 재현해 재지 않았습니다). 연결 타임아웃이나 명령 타임아웃 중 하나는 서비스 기한에 맞게 정해 두어야 하는 이유가 하나 더 생깁니다.

미리 맺으려면 `LettuceConnectionFactory`의 `setEagerInitialization(true)`를 쓸 수 있습니다. Spring Boot에는 이 값을 바꾸는 속성이 없어 팩토리를 직접 조정해야 합니다. 이 설정은 앱이 시작할 때 지도 받기(1번)와 공유 연결·리액티브 공유 연결의 기본 연결(2번)을 끝내 두지만, **주 서버마다의 노드 연결(3번)은 여전히 첫 사용 때 맺습니다.** 그래서 시작 시간이 1번만큼 늘어나는 대신, 첫 요청은 3번만 기다립니다. 대부분의 서비스에서는 준비 상태 확인(readiness)이 통과하기 전에 1번을 끝내는 것만으로도 배포 직후의 느린 첫 요청이 줄어듭니다.

## 연결 하나가 서버에 남기는 비용

### 메모리: 놀고 있는 연결은 작고, 응답을 쌓는 연결은 큽니다

서버가 연결 하나에 두는 메모리는 크게 셋입니다.

- **연결 상태**: 그 연결의 설정과 진행 상태를 담는 구조체입니다.
- **응답 버퍼**: 새 연결은 16KB로 시작합니다. 서버의 정기 작업이 최근에 실제로 쓴 양에 비해 버퍼가 크면 줄이므로, 놀고 있는 연결의 응답 버퍼는 시간이 지나면 작아집니다.
- **입력 버퍼**: 보통은 연결들이 함께 쓰는 버퍼 하나로 명령을 읽으므로, 짧은 명령만 오가는 연결은 입력 버퍼를 따로 들고 있지 않습니다. 받다 만 명령이나 큰 인자가 있으면 그 연결이 따로 버퍼를 갖고, 이 버퍼는 명령이 끝나도 바로 사라지지 않고 연결이 한동안 놀면 정기 작업이 줄이거나 풉니다.

정확한 크기는 빌드와 메모리 할당기에 따라 다르므로 문서에 숫자를 두지 않습니다. 대신 `INFO memory`의 `mem_clients_normal`(일반·Pub/Sub 연결과, 복제본 노드라면 주 서버로의 연결이 쓰는 메모리 합)을 `INFO clients`의 `connected_clients`로 나누면 지금 연결 하나의 평균을 잴 수 있고, `CLIENT LIST`의 `tot-mem`으로 연결마다 볼 수 있습니다. 한 가지만 계산해 두면, 새 연결 1,000개가 몰리면 응답 버퍼만으로 1,000 × 16KB, 약 16MB가 잠시 생깁니다.

연결 수보다 큰 위험은 **응답을 쌓는 연결**입니다. 큰 응답을 받거나 응답을 늦게 읽는 연결의 출력 버퍼는 커지고, 일반 연결의 출력 버퍼에는 기본 한도가 없습니다. 연결이 많을수록 이런 연결이 동시에 생길 수 있는 자리도 많아집니다. 이 메모리도 `maxmemory` 판단에 들어가므로 키 축출로 이어질 수 있습니다([Redis 메모리 지표](./memory-metrics.md)의 "클라이언트 버퍼"). 일반·Pub/Sub 연결의 메모리 합에 상한을 두는 `maxmemory-clients`는 기본 0(꺼짐)이며, 공식 문서는 큰 운영 환경에서 0이 아닌 값(예: `maxmemory`의 5%)을 권합니다. 상한을 넘으면 메모리를 가장 많이 쓰는 연결부터 끊습니다. 복제 연결과 `CLIENT NO-EVICT on`을 보낸 연결은 끊지 않으므로, 이 상한이 연결 메모리 전체를 막아 주지는 않습니다. 켜기 전에 끊길 수 있는 연결이 다시 연결해 회복하는지 봐야 합니다.

### 메인 스레드: 연결이 많으면 정기 작업도 늡니다

서버는 정기 작업에서 연결마다 타임아웃 확인과 버퍼 크기 조정을 하며, 모든 연결을 돌아가며 주기적으로 살핍니다. 연결 수가 많아지면 서버가 정기 작업의 주기 자체를 올립니다(`dynamic-hz`, [Redis 이벤트 루프](./event-loop.md)의 "메인 스레드는 한 바퀴에 무엇을 하는가"). 놀고 있는 연결 하나의 비용은 작지만 0이 아니며, 그 비용은 연결 수에 비례해 메인 스레드에 쌓입니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

- **인스턴스가 늘면 노드마다 연결이 비례해 늡니다.** 인스턴스 하나가 모든 주 서버에 연결을 갖기 때문입니다. 오토스케일링의 최대치와 배포 중 겹치는 수를 기준으로 계산합니다.
- **주 서버를 늘려도 노드당 앱 연결은 줄지 않습니다.** 데이터와 요청은 노드 수로 나뉘지만, 인스턴스 하나는 여전히 모든 주 서버에 연결 하나씩을 둡니다. 오히려 클러스터 연결 하나가 여는 TCP 연결이 주 서버 하나만큼 늘고, 지도 갱신의 임시 연결과 클러스터 버스 연결도 노드 수에 비례해 늡니다. 연결 한도 문제는 샤드를 늘려 풀리지 않습니다.
- **복제본을 늘리면 `read-from`을 쓰는 앱의 연결이 함께 늡니다.** 슬롯을 처음 읽을 때 후보 노드 모두에 연결하기 때문입니다.
- **요청량은 공유 연결을 쓰는 동안 연결 수를 바꾸지 않습니다.** 공유를 끄면 연결 수는 동시에 Redis를 쓰는 요청 수를 따라 `max-active`까지 늘어납니다. 그 평균은 "초당 요청 수 × 요청 하나가 연결을 붙잡는 시간(초)"이고, 순간 최대치는 요청이 몰릴 때 이보다 큽니다. 한 번 늘어난 연결은 `max-idle`까지 놀면서도 남습니다.
- **같은 클러스터를 쓰는 서비스가 늘면 합이 늡니다.** 노드의 한도는 서비스들이 나눠 쓰는 하나의 예산입니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 바로잡은 곳 |
| --- | --- | --- |
| 풀의 `max-active`가 8이면 Redis 연결도 최대 8개다 | 클러스터 연결 하나는 기본 연결 1 + 닿은 노드마다 1개의 TCP 연결입니다 | 클러스터 연결 하나는 TCP 연결 여러 개를 묶은 것입니다 |
| 노드를 늘리면 노드당 연결이 줄어든다 | 인스턴스마다 모든 주 서버에 연결하므로 노드당 앱 연결은 그대로입니다 | 데이터와 요청이 늘면 무엇이 달라지는가 |
| `maxclients`는 클러스터 전체에 하나다 | 노드마다 따로이고, 가장 많이 받는 노드가 먼저 닿습니다 | 한도는 노드마다 따로입니다 |
| `maxclients`에는 앱 연결만 들어간다 | 복제본 연결과 클러스터 버스 연결도 들어갑니다 | 한도는 노드마다 따로입니다 |
| `maxclients`에 닿으면 모든 요청이 실패한다 | 새 연결만 거절되고, 이미 맺은 연결은 그대로 동작합니다 | 무엇을 세고, 무엇을 거절하는가 |
| 설정 파일의 `maxclients`가 실제 한도다 | 운영체제의 파일 디스크립터 상한이 낮으면 시작할 때 낮춰집니다 | 실제 한도는 설정 파일의 값보다 낮을 수 있습니다 |
| 앱 코드가 쓰는 연결만 세면 된다 | 헬스 체크, 노드를 인자로 받는 명령, 토폴로지 갱신도 연결을 만듭니다 | Spring 앱 한 대는 클러스터 연결을 몇 개 만드는가 |
| 앱이 시작하면 연결이 다 맺어져 있다 | 지도와 기본 연결은 첫 명령 때, 노드 연결은 그 노드를 처음 쓸 때 맺습니다 | 시작 시간: 첫 요청이 연결 비용을 냅니다 |

## 근거

**Redis**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `acceptCommonHandler`(새 연결을 받을 때만 `listLength(server.clients) + getClusterConnectionsCount() >= server.maxclients`를 검사, 클러스터 모드의 오류 문장 "max number of clients + cluster connections reached", TLS는 협상 전이라 문장 없이 닫힘이라는 주석, `stat_rejected_conn` 증가, 연결 객체를 만들기 전에 검사), `createClient`(응답 버퍼를 `PROTO_REPLY_CHUNK_BYTES`로 할당), `readQueryFromClient`(입력 버퍼가 없으면 스레드가 함께 쓰는 재사용 버퍼를 씀), `getClientMemoryUsage`(연결 메모리에 출력 버퍼, 입력 버퍼, 구조체, 응답 버퍼 포함), `clientAcceptHandler`(보호 모드로 외부 연결을 거절할 때도 `stat_rejected_conn` 증가), `evictClients`(`maxmemory-clients` 판단은 일반·Pub/Sub 연결 메모리 합)
- [`src/replication.c`](https://github.com/redis/redis/blob/8.2.10/src/replication.c): `replicationCreateMasterClient`(복제본 노드에서 주 서버로의 연결도 `createClient`로 만든 클라이언트라 연결 수에 들어감)
- [`src/cluster_legacy.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster_legacy.c): `getClusterConnectionsCount`(클러스터 모드에서 `(노드 수 − 1) × 2`, 노드마다 들어오는 연결과 나가는 연결)
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c): `adjustOpenFilesLimit`(`maxclients + CONFIG_MIN_RESERVED_FDS`만큼 상한을 올려 보고, 못 올리면 `maxclients`를 상한 − 32로 낮추고 경고 로그), `clientsCron`(한 번에 `연결 수 / hz`개를 처리해 약 1초에 한 바퀴), `clientsCronResizeOutputBuffer`(최근 최댓값이 버퍼의 절반보다 작으면 최댓값 + 1, 최소 `PROTO_REPLY_MIN_BYTES`로 줄임, 최댓값은 주기마다 초기화), `clientsCronResizeQueryBuffer`(놀고 있는 연결의 개별 입력 버퍼를 줄이거나 풂), `clientEvictionAllowed`(`CLIENT NO-EVICT` 연결과 일반·Pub/Sub이 아닌 연결은 끊지 않음), `serverCron`의 `dynamic_hz`(연결 수 / hz가 `MAX_CLIENTS_PER_CLOCK_TICK`을 넘으면 hz를 올림), `INFO`의 `# Clients`(`connected_clients`는 복제본 연결 제외, `cluster_connections`, `maxclients`), 초기값 `reply_buffer_peak_reset_time` 5000ms·`reply_buffer_resizing_enabled` 1
- [`src/server.h`](https://github.com/redis/redis/blob/8.2.10/src/server.h): `CONFIG_MIN_RESERVED_FDS` 32, `PROTO_REPLY_CHUNK_BYTES` 16KB, `PROTO_REPLY_MIN_BYTES` 1KB, `REPLY_BUFFER_DEFAULT_PEAK_RESET_TIME` 5000ms, `MAX_CLIENTS_PER_CLOCK_TICK` 200
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `maxclients` 기본 10000(실행 중 변경 가능), `maxmemory-clients` 기본 0
- [`src/object.c`](https://github.com/redis/redis/blob/8.2.10/src/object.c): `mem_clients_normal`은 일반·Pub/Sub·주 서버 연결 종류의 메모리 합
- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): CLIENTS 절(한도에 닿으면 'max number of clients reached'로 새 연결을 닫음, 상한을 못 맞추면 파일 상한 − 32, 클러스터에서는 노드마다 연결 2개씩을 클러스터 버스와 나눠 씀)

**Redis 공식 문서**(저장소 [`redis/docs`](https://github.com/redis/docs/tree/e53775dde0ee7e39ebe327e7ffff583caa9fbd65), 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`)

- [`content/develop/reference/clients.md`](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/reference/clients.md): "Maximum Concurrent Connected Clients"(기본 10,000, 파일 상한 + 32에 맞춰 낮춤), "Output Buffer Limits"(일반 연결은 기본 한도 없음), "Client Eviction"(`maxmemory-clients` 기본 0, 큰 운영 환경에서 0이 아닌 값, 예 5% 권장, 메모리를 가장 많이 쓰는 연결부터 끊음). 이 문서는 "클라이언트를 초기화한 뒤" 한도를 검사한다고 적지만, 8.2.10 소스는 연결 객체를 만들기 전에 검사합니다. 판단은 같습니다
- [`content/commands/info.md`](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/info.md): `maxclients`는 `connected_clients`, `connected_slaves`, `cluster_connections` 합의 상한, `rejected_connections`, `total_connections_received`, `mem_clients_normal`
- [`content/commands/client-list.md`](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/client-list.md): `addr`, `name`, `age`, `tot-mem` 필드

**Lettuce**(태그 [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE))

- [`cluster/RedisClusterClient.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/RedisClusterClient.java): `connectClusterAsync`(클러스터 연결을 만들 때 기본 연결을 곧바로 맺음, 연결 수로 정렬한 노드 목록에서 주소를 고름), `getPartitions`(지도가 없으면 처음 받기를 기다림), `getTopologyRefreshSource`(동적 갱신이면 지도의 모든 노드, 아니면 시드 노드), `fetchPartitions`(연결 타임아웃을 넘겨 지도 받기), `topologyRefreshScheduler`가 클라이언트마다 하나, 노드 연결도 첫 번째 시드 주소의 설정으로 준비(`connectToNodeAsync`, `createConnectionBuilder`)
- [`cluster/PooledClusterConnectionProvider.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/PooledClusterConnectionProvider.java): `getWriteConnection`(슬롯의 주 서버 호스트·포트로 연결을 처음 쓸 때 맺고 슬롯별로 캐시), `getReadConnection`·`getReadFromConnections`(`readFrom`이 고른 후보 모두에 한꺼번에 연결, 주 서버는 쓰기 연결과 같은 키), `isReadCandidate`(복제 진행 위치가 0이 아닌 복제본만), `NodeConnectionPostProcessor`(읽기용 연결에 `READONLY`), `closeStaleConnections`(지도에서 사라진 노드의 연결을 닫음)
- [`cluster/topology/DefaultClusterTopologyRefresh.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/DefaultClusterTopologyRefresh.java): `loadViews`(물을 노드마다 새 연결, 동적이면 새로 알게 된 노드에 이어서 연결, 끝나면 모두 닫음, 연결 시도 전체를 감싸는 제한 = 명령 타임아웃 + 연결 타임아웃), `openConnections`(연결 이름 `lettuce#ClusterTopologyRefresh`, 실패하면 경고 로그 "Unable to connect to"), `ConnectionTracker.whenComplete`(시도한 연결 모두가 끝난 뒤 진행), `getNodeSpecificViews`(답하지 못한 노드는 건너뜀)
- [`ConnectionBuilder.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ConnectionBuilder.java), [`protocol/RedisHandshakeHandler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/RedisHandshakeHandler.java): TCP 연결 제한은 `CONNECT_TIMEOUT_MILLIS`(연결 타임아웃), 연결 초기화 제한은 채널이 등록될 때(`channelRegistered`)부터 명령 타임아웃 값으로 재고, 넘으면 "Connection initialization timed out"으로 연결을 실패시킴
- [`internal/AsyncConnectionProvider.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/internal/AsyncConnectionProvider.java): 노드 연결을 키마다 하나의 연결 시도(future)로 캐시해, 같은 노드로 동시에 온 요청이 한 연결 맺기를 함께 기다림
- [`cluster/ClusterNodeConnectionFactory.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterNodeConnectionFactory.java): `ConnectionKey`가 노드 ID 또는 주소·포트로 연결을 구분(둘은 다른 키)
- [`pubsub/PubSubCommandArgs.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/pubsub/PubSubCommandArgs.java): 구독 명령은 첫 키가 없음(`getFirstEncodedKey`가 null) → 클러스터 연결의 기본 연결로 나감
- [`cluster/topology/Connections.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/Connections.java): 지도 받기에 `CLUSTER NODES`와 `INFO`
- [`cluster/topology/NodeTopologyViews.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/NodeTopologyViews.java): `getClusterNodes`(받은 지도의 모든 노드 주소)
- [`ReadFromImpl.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ReadFromImpl.java): `ReadFromReplicaPreferred`(복제본, 그다음 주 서버를 모두 후보로)
- [`cluster/api/StatefulRedisClusterConnection.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/api/StatefulRedisClusterConnection.java): `getConnection(host, port)`는 쓰기 의도의 노드 연결
- [`pom.xml`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/pom.xml): `reactor-core`가 선택(optional)이 아닌 의존성

**Spring Data Redis**(태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13))

- [`LettuceConnectionFactory.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionFactory.java): `eagerInitialization` 기본 `false`, `start`(미리 만들기가 켜져 있고 공유할 때만 `initConnection`), `initConnection`(공유 클러스터 연결과 리액티브 공유 연결), `createClusterCommandExecutor`(노드 지정 명령용 실행기를 팩토리마다 하나), `getSharedClusterConnection`, `SharedConnection.getNativeConnection`(공유 연결도 `StatefulConnection` 종류로 연결 공급자에게서 빌림), `createConnectionProvider`(풀 설정이면 `LettucePoolingConnectionProvider`), `createClusterClient`·`createRedisURIAndApplySettings`(시드 주소마다 클라이언트 이름과 명령 타임아웃을 넣음), `destroy`(팩토리를 종료할 때 Lettuce 클라이언트를 `shutdown`)
- [`ClusterConnectionProvider.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/ClusterConnectionProvider.java): 처음 쓸 때 `getPartitions`, 연결 하나 = `connectAsync`로 만든 클러스터 연결 하나, Pub/Sub은 `connectPubSubAsync`
- [`LettuceClusterConnection.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClusterConnection.java): `LettuceClusterNodeResourceProvider`(`StatefulRedisClusterConnection` 종류로 클러스터 연결 하나를 빌려 보유하고 노드마다 `getConnection(host, port)`), `clusterGetClusterInfo`(임의 노드)
- [`LettuceConnection.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnection.java): `doGetAsyncDedicatedConnection`(전용 연결을 `StatefulConnection` 종류로 빌림), `close`→`reset`(전용 연결은 이 연결을 닫을 때 연결 공급자에 돌려줌. `switchToPubSub`도 `reset`을 부름)
- [`LettuceKeyCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceKeyCommands.java), [`LettuceClusterKeyCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClusterKeyCommands.java): `keys(pattern)`은 공유 연결로, `keys(node, pattern)`만 노드 지정 실행기로
- [`LettuceListCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceListCommands.java), [`LettuceClusterListCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClusterListCommands.java): 키가 한 슬롯인 `BLPOP`은 전용 연결로
- [`LettuceConnectionProvider.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionProvider.java): 기본 `release`는 연결을 닫음(`closeAsync`). 풀이 없을 때 돌려준 전용 연결은 TCP 연결까지 닫힘
- [`LettucePoolingConnectionProvider.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettucePoolingConnectionProvider.java): 연결 종류마다 풀을 따로 둠(`pools`)
- [`LettuceReactiveRedisClusterConnection.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceReactiveRedisClusterConnection.java): `clusterGetClusterInfo`가 활성 노드 중 무작위 하나(`executeCommandOnArbitraryNode`), 노드별 연결은 노드 ID로(`getCommands`)
- [`ClusterCommandExecutor.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/ClusterCommandExecutor.java), [`ClusterTopology.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/ClusterTopology.java): 임의 노드는 연결됨·실패 아님인 노드(복제본 포함) 중 무작위
- [`LettuceExceptionConverter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceExceptionConverter.java): `RedisConnectionException` → `RedisConnectionFailureException`

**Spring Boot**(태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16), 커밋 `0566f6933049aca6bc5ffc6d559fffade9cd2e0c`. Spring Data Redis 3.5.13, Lettuce 6.6.0.RELEASE, Commons Pool2 2.12.1을 지정)

- [`LettuceConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/LettuceConnectionConfiguration.java): 풀이 켜지면 `LettucePoolingClientConfiguration`, `read-from` 적용, 클러스터 속성이 있을 때 갱신 옵션(`dynamicRefreshSources`, `enablePeriodicRefresh`, `enableAllAdaptiveRefreshTriggers`), `eagerInitialization`을 설정하지 않음
- [`RedisProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisProperties.java): `clientName`, `cluster.nodes`, `lettuce.readFrom`, 풀 `maxActive`·`maxIdle` 기본 8
- [`RedisReactiveHealthContributorAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/data/redis/RedisReactiveHealthContributorAutoConfiguration.java), [`RedisHealthContributorAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/data/redis/RedisHealthContributorAutoConfiguration.java): `Flux`와 `ReactiveRedisConnectionFactory` 빈이 있으면 리액티브 헬스 체크를 `redisHealthContributor`로 등록하고, 일반 헬스 체크는 그 이름의 빈이 없을 때만 등록
- [`RedisReactiveHealthIndicator.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator/src/main/java/org/springframework/boot/actuate/data/redis/RedisReactiveHealthIndicator.java): 클러스터면 `clusterGetClusterInfo`

**Apache Commons Pool**(태그 [`rel/commons-pool-2.12.1`](https://github.com/apache/commons-pool/tree/rel/commons-pool-2.12.1))

- [`GenericObjectPool.java`](https://github.com/apache/commons-pool/blob/rel/commons-pool-2.12.1/src/main/java/org/apache/commons/pool2/impl/GenericObjectPool.java): `returnObject`(반납 시점에 놀고 있는 객체 수가 `maxIdle` 이상이면 돌려받은 객체를 닫음), `borrowObject`(객체가 모두 빌려졌으면 `blockWhenExhausted`일 때 최대 대기 시간까지 기다림)

## 실무 포인트

### 무엇을 미리 정할까

| 중요도 | 결정 | 이유 |
| --- | --- | --- |
| 필수 | 노드당 연결 예산을 계산해 각 노드의 실제 `maxclients`와 비교한다 | 한도는 노드마다이고, 넘으면 새 연결만 실패해 배포·재시작 같은 순간에야 드러납니다. "같은 클러스터를 쓰는 모든 서비스의 (최대 인스턴스 수 + 배포 중 겹치는 수) × 인스턴스 하나가 그 노드에 여는 연결 + 복제 연결 + 2 × (노드 수 − 1) + 갱신·재연결 여유"로 셉니다 |
| 필수 | 공유 연결(기본값)을 유지한다 | 공유를 끄면 연결 수가 요청 동시성 × (주 서버 수 + 1)로 커집니다. 끌 때는 위 예산을 다시 계산합니다 |
| 필수 | 풀을 쓴다면 `max-active`와 `max-idle`을 함께 정한다 | `max-idle`이 작으면 바쁠 때 늘어난 클러스터 연결이 바쁜 구간이 끝나 반납될 때 닫히고 다음에 다시 만들어지며, 그때마다 노드 연결을 처음부터 맺습니다 |
| 알면 유리 | `spring.data.redis.connect-timeout`을 정한다 | 처음 지도를 받을 때 응답 없는 노드 하나가 첫 명령을 붙잡습니다. 연결 타임아웃과 명령 타임아웃을 모두 비우면 연결 타임아웃 기본 10초만큼입니다 |
| 알면 유리 | `read-from`의 연결 비용을 셈에 넣는다 | 슬롯을 처음 읽을 때 그 샤드의 후보 노드 모두에 연결하므로, 복제본 수만큼 클러스터 연결 하나의 TCP 연결이 늘어납니다 |
| 알면 유리 | `spring.data.redis.client-name`으로 앱 이름을 붙인다 | `CLIENT LIST`의 `name`으로 어느 서비스의 연결인지 가릅니다. Lettuce는 노드 연결도 첫 시드 주소의 설정으로 준비하므로 이름이 함께 붙습니다. 지도 갱신 연결은 `lettuce#ClusterTopologyRefresh`로 따로 보입니다 |
| 참고 | `maxmemory-clients`를 검토한다 | 일반·Pub/Sub 연결 메모리 합의 상한입니다. 넘으면 많이 쓰는 연결부터 끊으므로, 끊겨도 다시 연결해 회복하는 연결인지 먼저 봅니다 |

`maxclients`를 올리는 것은 대응이 될 수 있지만 대가가 있습니다. 운영체제의 파일 디스크립터 상한도 함께 올려야 실제로 적용되고, 연결이 늘어나는 만큼 연결 메모리와 메인 스레드의 정기 작업도 늘어납니다. 관리형 서비스라면 노드 크기마다 바꿀 수 없는 상한이 정해져 있을 수 있으니 서비스 문서를 확인합니다.

### 무엇을 관찰할까

| 어디서 | 지표 | 무엇을 읽는가 |
| --- | --- | --- |
| 각 노드 `INFO clients` | `connected_clients`, `cluster_connections`, `maxclients` | 앞의 둘에 복제본 연결 수(`INFO replication`의 `connected_slaves`)를 더한 값이 `maxclients`에 얼마나 가까운가. `maxclients`는 실제로 적용된 값 |
| 각 노드 `INFO stats` | `rejected_connections` | 0이 아니면 이미 새 연결을 거절했습니다. 증가 시점을 배포·스케일아웃 시각과 맞춰 봅니다 |
| 각 노드 `INFO stats` | `total_connections_received`의 증가 속도 | 평소에도 꾸준히 크면 갱신 주기나 풀이 `max-idle`을 넘는 연결을 닫는 일로 연결이 계속 새로 맺어진다는 신호 |
| 각 노드 `INFO memory` | `mem_clients_normal` ÷ `connected_clients` | 연결 하나의 평균 메모리. 갑자기 크면 출력 버퍼가 쌓인 연결이 있는지 `CLIENT LIST`로 봅니다 |
| 각 노드 `CLIENT LIST` | `addr`(서버가 본 상대편 IP:포트. NAT나 프록시를 거치지 않으면 IP 부분이 앱 인스턴스), `name`, `age`, `tot-mem` | 인스턴스·서비스별 연결 수, `age`(서버가 그 연결을 맺은 뒤 지난 초. 마지막 사용 뒤 지난 시간이 아님)가 계속 짧은 연결(자주 새로 맺는 연결) |
| 앱 | `RedisConnectionFailureException` 수, Lettuce 경고 로그 `Unable to connect to` | 새 연결이 실패하고 있는지. 새 인스턴스에서만 나는지 |

### 증상과 확인 순서

**새로 뜬 인스턴스에서만 Redis 오류가 나고 기존 인스턴스는 정상입니다.**

1. 각 노드의 `rejected_connections`가 늘었는지 봅니다. 이 값은 보호 모드(비밀번호 없이 외부 연결을 막는 설정)의 거절로도 늘므로, 늘었다면 앱이 받은 오류 문장이 연결 한도의 것인지와 그때의 연결 수를 함께 봅니다. 늘지 않았다면 네트워크 정책, 인증, 시드 노드 주소처럼 새 인스턴스의 환경을 봅니다.
2. 늘어난 노드의 `connected_clients + connected_slaves + cluster_connections`와 `maxclients`를 비교합니다. `maxclients`가 설정한 값보다 낮다면 파일 디스크립터 상한 때문에 낮춰진 것이니 서버 시작 로그를 봅니다.
3. `CLIENT LIST`를 `addr`의 IP 부분(포트는 연결마다 다르므로 떼어 냄)과 `name`으로 묶어 어느 서비스가 연결을 많이 쓰는지 찾습니다. NAT나 프록시를 거치면 여러 인스턴스가 같은 IP로 보이므로, 그때는 IP로 인스턴스를 가를 수 없고 `name`으로 서비스까지만 가릅니다. 인스턴스 하나의 연결 수가 앞의 계산보다 훨씬 크면 공유를 껐거나 풀이 큰 서비스입니다.
4. 대응은 연결을 줄이는 쪽이 먼저입니다. 공유 연결을 되돌리거나 풀 최대치를 줄이면 한도 문제가 근본적으로 사라집니다. 급하면 스케일아웃 폭을 줄이거나 `maxclients`와 파일 상한을 올리되, 메모리와 정기 작업 비용이 함께 는다는 점을 감수합니다.

**요청량은 그대로인데 노드의 새 연결 수가 꾸준히 많습니다.**

1. `CLIENT LIST`에서 `age`가 짧은 연결의 `name`을 봅니다. `lettuce#ClusterTopologyRefresh`라면 토폴로지 갱신이니 주기와 인스턴스 수를 봅니다.
2. 앱 이름이라면 풀이 바쁜 구간마다 연결을 닫고 다시 만드는지 의심합니다. `max-active`에 비해 `max-idle`이 작은지 확인합니다.
3. 특정 노드에 몰린다면 그 노드의 재시작이나 네트워크 끊김으로 재연결이 반복되는지 Lettuce 로그를 봅니다.

**인스턴스 수와 설정은 그대로인데 노드의 연결 수가 계속 오릅니다.**

앞 증상이 연결을 닫고 다시 맺는 것이라면, 이 증상은 연결을 닫지 않고 쌓는 것(연결 누수)입니다.

1. `CLIENT LIST`를 앞 증상처럼 서비스·인스턴스로 묶어, 앞의 계산(풀을 쓰면 `max-active`까지 찬 경우 포함)보다 연결이 많은 곳을 찾습니다. 그 연결들의 `age`가 짧고 계속 바뀌면 앞 증상(반복해서 새로 맺음)에 가깝고, 오래 산 연결의 수가 그 계산을 넘어 계속 늘면 닫히지 않는 연결을 의심합니다. `age`는 연결을 맺은 뒤 지난 시간일 뿐이라 정상적으로 커진 풀의 연결도 오래 살아 있으므로, 이것만으로 누수라고 정하지 않고 2번의 코드 확인으로 원인을 좁힙니다.
2. 그 서비스의 코드에서 두 가지를 찾습니다. 하나는 `RedisConnectionFactory.getConnection()`으로 직접 얻은 연결을 닫지 않는 곳입니다. 그 연결로 `BLPOP` 같은 전용 연결 명령을 보냈다면, 전용 클러스터 연결은 명령이 끝나도 돌려지지 않고 보통 그 연결을 닫을 때 돌려지므로, 닫지 않으면 그대로 남습니다. 여기서 "닫는다"는 Spring 연결 객체를 닫아 전용 클러스터 연결을 돌려주는 일입니다. 풀이 있으면 돌려받은 클러스터 연결은 풀에 남아 TCP 연결도 유지되고, 풀이 없으면 그때 TCP 연결까지 닫힙니다. 다른 하나는 요청이나 작업마다 연결 팩토리나 Lettuce 클라이언트를 새로 만들고 종료하지 않는 곳입니다. 그 클라이언트의 연결은 종료할 때 닫힙니다.
3. 풀의 상한 `max-active`는 연결 팩토리 하나의 풀에 걸립니다. 그래서 같은 팩토리에서 닫지 않은 연결은 풀이 가득 차면 더 늘지 않고, 그 뒤에는 노드의 연결 증가 대신 풀에서 연결을 기다리는 지연으로 드러납니다. 반면 팩토리를 계속 새로 만들면 풀도 새로 생기므로 이 상한이 연결 증가를 막지 못합니다. 풀 대기의 상한과 증상은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에 있습니다.

**배포 직후 첫 요청들만 느립니다.**

1. 앱에서 Redis 명령 지연을 배포 시각과 함께 봅니다. 첫 몇 초에 몰렸다가 사라지면 연결 맺기와 지도 받기입니다.
2. 몇 초가 아니라 10초 근처라면 응답 없는 노드를 기다렸을 가능성이 큽니다(시간만으로 원인이 확정되지는 않으니 Lettuce 로그의 연결 실패 기록과 맞춰 봅니다). 시드 목록에 사라진 노드가 있는지, `connect-timeout`이나 `timeout`을 정했는지 봅니다.
3. 미리 맺기(`setEagerInitialization(true)`)는 지도 받기와 기본 연결만 앞당긴다는 점을 감안해 효과를 판단합니다.

### Spring에서 확인할 것

- 클래스패스에 commons-pool2가 있는지, 그래서 풀이 켜져 있는지 확인합니다. 클러스터에서는 풀의 연결 하나가 TCP 연결 여러 개입니다.
- `shareNativeConnection`을 `false`로 바꾼 코드가 있는지 찾습니다. 있다면 그 서비스의 노드당 연결이 `max-active` × 인스턴스 수까지 갑니다.
- 노드를 인자로 받는 명령(`RedisClusterConnection`의 `keys(node, pattern)` 등)을 쓰면 그 명령용 클러스터 연결이 하나 더 생기고, 복제본에도 연결할 수 있습니다. 노드를 받지 않는 `RedisTemplate.keys(pattern)`은 공유 연결이 주 서버들에 나눠 보냅니다.
- Actuator의 Redis 헬스 체크는 리액티브 공유 연결로 무작위 노드에 가므로, 헬스 체크만으로도 인스턴스마다 노드 전부에 연결이 생길 수 있습니다. 헬스 체크가 Redis를 볼 필요가 없다면 `management.health.redis.enabled=false`로 끌 수 있습니다.
- 토폴로지 갱신 주기를 정할 때 인스턴스 수 × 노드 수의 임시 연결이 주기마다 생긴다는 점을 함께 셉니다.

## 말로 설명하기

### 30초 버전

Lettuce의 클러스터 연결 하나는 기본 연결 하나에, 처음 쓸 때 주 서버마다 하나씩 맺는 노드 연결을 묶은 것입니다. 그래서 Spring에서 "연결 하나"는 TCP 연결 (주 서버 수 + 1)개이고, 노드 하나가 받는 연결은 인스턴스 수에 비례합니다. 기본값인 공유 연결을 쓰면 요청량과 무관하게 작게 유지되지만, 공유를 끄고 풀을 키우면 노드마다 "인스턴스 수 × 풀 크기"가 되어 `maxclients`에 닿을 수 있습니다. 한도는 노드마다이고 새 연결만 거절하므로, 문제는 스케일아웃·배포·노드 재시작처럼 새 연결이 필요한 순간에 드러납니다.

### 2분 버전

Spring Boot + Lettuce로 클러스터를 쓰면 Spring Data Redis는 일반 명령을 공유 연결 하나로 보냅니다. 그런데 그 공유 연결은 Lettuce의 클러스터 연결이고, 안에 기본 연결 하나와 주 서버별 노드 연결을 둡니다. 노드 연결은 그 주 서버의 키를 처음 보낼 때 맺습니다. 복제본 읽기를 켜면 슬롯을 처음 읽을 때 그 샤드의 후보 노드 모두에 한꺼번에 연결합니다.

앱 한 대는 공유 연결 말고도 클러스터 연결을 더 만듭니다. `BLPOP`이나 파이프라인 같은 전용 연결, 노드를 지정하는 명령용 연결, 헬스 체크가 쓰는 리액티브 공유 연결입니다. 헬스 체크는 무작위 노드에 가므로 결국 복제본까지 모든 노드에 연결합니다. 여기에 토폴로지 갱신이 주기마다 모든 노드에 임시 연결을 열고 닫습니다.

한도 `maxclients`는 노드마다 따로이고, 앱 연결뿐 아니라 복제본 연결과 노드끼리의 클러스터 버스 연결 2 × (노드 수 − 1)개도 들어갑니다. 운영체제의 파일 디스크립터 상한이 낮으면 시작할 때 낮아집니다. 넘으면 새 연결에 오류를 보내고 닫을 뿐 기존 연결은 그대로라, 새 인스턴스나 다시 연결하는 쪽, 다시 붙으려는 복제본이 실패합니다.

그래서 정할 것은 셋입니다. 공유 연결을 유지해 연결 수를 요청량에서 떼어 놓고, 노드당 연결 예산을 배포 중 최대 인스턴스 수로 계산해 실제 `maxclients`와 비교하고, 풀을 쓴다면 `max-idle`을 `max-active`와 맞춰 연결이 계속 새로 맺어지지 않게 합니다. 관찰은 노드별 `rejected_connections`, 연결 수 합과 `maxclients`, `total_connections_received`의 증가 속도, `CLIENT LIST`의 서비스별 묶음으로 합니다.

### 예상 질문과 꼬리 질문

**Q. 풀 `max-active`가 8인데 왜 Redis 노드에 연결이 수십 개씩 보이나요?**

풀에 담긴 것이 TCP 연결이 아니라 Lettuce의 클러스터 연결이기 때문입니다. 클러스터 연결 하나는 기본 연결 1개에 닿은 주 서버마다 1개씩을 묶습니다. 게다가 헬스 체크와 노드 지정 명령은 일반 명령과 다른 풀(풀이 꺼져 있으면 따로 만든 연결)에서 클러스터 연결을 하나씩 따로 갖고, 지도 갱신은 임시 연결을 엽니다("Spring 앱 한 대는 클러스터 연결을 몇 개 만드는가").

- **꼬리: 그럼 노드마다 보이는 수는 어떻게 예측하나요?** 인스턴스 하나가 그 노드에 연 연결을 세고 인스턴스 수를 곱합니다. 공유 연결은 주 서버마다 1, 전용 연결은 그 작업의 키가 있는 노드에만, 헬스 체크 연결은 모든 노드에 1입니다. 기본 연결은 어느 노드에 갈지 정해져 있지 않으므로 범위로 셉니다("노드 하나가 받는 연결 수를 셉니다").
- **꼬리: 인스턴스 수가 그대로인데 연결 수가 계속 오르면 무엇을 의심하나요?** 오래 살아 있는 연결(`CLIENT LIST`의 `age`가 큰 연결)의 수가 풀 최대치까지 포함한 계산을 넘어 계속 느는지 먼저 봅니다. `age`는 맺은 뒤 지난 시간일 뿐이라 이것만으로 누수라고 정하지는 않고, 코드에서 직접 얻은 연결을 닫지 않았거나 연결 팩토리·클라이언트를 만들고 종료하지 않은 곳을 찾아 확인합니다. 같은 팩토리 안이라면 풀이 `max-active`에서 멈추고 풀 대기로 드러나지만, 팩토리를 계속 새로 만들면 풀도 새로 생겨 멈추지 않습니다("증상과 확인 순서").

**Q. 노드를 늘리면 노드당 연결이 줄어드니 한도 문제도 풀리지 않나요?**

아닙니다. 데이터와 요청은 나뉘지만, 인스턴스 하나는 키가 흩어져 있는 한 모든 주 서버에 노드 연결을 둡니다. 주 서버가 늘면 클러스터 연결 하나가 여는 TCP 연결이 오히려 하나 늘고, 클러스터 버스 연결과 갱신 임시 연결도 노드 수에 비례해 늡니다("데이터와 요청이 늘면 무엇이 달라지는가").

- **꼬리: 그럼 연결 한도에 닿으면 무엇으로 푸나요?** 연결을 만드는 쪽을 줄이는 것이 먼저입니다. 공유 연결로 되돌리거나 풀 크기를 줄입니다. `maxclients`를 올리는 것은 파일 디스크립터 상한을 함께 올려야 적용되고, 연결 메모리와 정기 작업 비용을 감수하는 대응입니다.

**Q. `maxclients`에 닿으면 서비스 전체가 멈추나요?**

아닙니다. 한도는 새 연결을 받아들일 때만 검사하므로 이미 맺은 연결로 오가는 명령은 계속 처리됩니다. 실패하는 것은 새 연결이 필요한 일입니다. 새 인스턴스의 첫 명령, 풀의 확장, 끊긴 연결의 재연결, 지도 갱신의 임시 연결, 그리고 다시 붙으려는 복제본입니다("예시에서 무엇이 실패하는가").

- **꼬리: 그래서 더 위험한 점은요?** 평소 지표로는 멀쩡해 보인다는 것입니다. 한도 근처에서 돌다가 배포나 노드 재시작처럼 연결이 몰리는 순간에만 실패하므로, 평소 연결 수가 아니라 그 순간의 최대치로 예산을 잡아야 합니다. 그리고 복제본 재연결이 막히면 장애 대비가 함께 약해집니다.
- **꼬리: 복제본이 거절된다는 건 확인된 동작인가요?** 소스에서 새 연결을 받는 입구가 하나이고 그 입구에서 한도를 검사한다는 것을 확인했고, 복제본도 주 서버에 일반 연결로 붙으므로 같은 검사를 받는다고 판단했습니다. 재현해 본 것은 아닙니다.

**Q. 설정 파일에 `maxclients 10000`을 적었으니 한도는 10,000 아닌가요?**

시작할 때 운영체제의 파일 디스크립터 상한이 `maxclients + 32`보다 낮고 올리지도 못하면, Redis는 `maxclients`를 상한 − 32로 낮추고 경고만 남깁니다. 또 이 한도에는 앱 연결뿐 아니라 복제본 연결과 클러스터 버스 연결 2 × (노드 수 − 1)개가 들어갑니다. 그래서 각 노드의 `INFO clients`에 나오는 `maxclients`와 세 연결 수의 합으로 판단합니다("실제 한도는 설정 파일의 값보다 낮을 수 있습니다").

**Q. 공유 연결 하나로 모든 요청을 보내면 처리량이 부족하지 않나요? 공유를 끄는 게 낫지 않나요?**

공유 연결은 응답을 기다리지 않고 명령을 이어 보내므로 연결 하나로 많은 요청을 나를 수 있고, 노드 하나는 어차피 명령을 한 줄로 실행합니다. 공유를 끄면 얻는 것은 큰 응답 하나가 같은 연결의 다른 응답을 늦추는 일이 줄어드는 것이고, 잃는 것은 연결 수입니다. 클러스터에서는 그 연결 수가 (주 서버 수 + 1)배가 되므로, 큰 응답 문제는 명령을 나눠 푸는 쪽을 먼저 봅니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "공유를 끄면 모든 명령이 풀을 거칩니다").

- **꼬리: 그래도 꺼야 한다면요?** 노드당 예산을 "최대 인스턴스 수 × `max-active`"로 다시 계산하고, `max-idle`을 `max-active`와 맞춥니다. 그렇지 않으면 바쁠 때 늘어난 연결이 바쁜 구간이 끝날 때마다 닫혀, 노드들이 새 연결을 되풀이해 받습니다.

**Q. `max-idle`이 왜 연결 수와 관련 있나요? 놀고 있는 연결 수일 뿐이잖아요.**

풀은 연결을 돌려받을 때 놀고 있는 연결이 이미 `max-idle`개면 돌려받은 연결을 닫습니다. `max-active`가 64이고 `max-idle`이 8이면, 바쁜 구간이 끝나 반납이 몰릴 때 놀고 있는 연결 8개를 넘는 몫(최대 56개)이 닫히고 다음 바쁜 순간에 다시 만들어집니다. 클러스터에서는 그때마다 기본 연결과 노드 연결을 다시 맺으므로, 노드의 `total_connections_received`가 요청량을 따라 출렁입니다("공유를 끄고 풀을 키우면 곱이 커집니다").

**Q. 토폴로지 갱신은 이미 맺어 둔 연결로 물어보지 않나요?**

Lettuce 6.6.0은 갱신할 때마다 물을 노드마다 새 연결을 맺고 `CLUSTER NODES`와 `INFO`를 보낸 뒤 닫습니다. `dynamic-refresh-sources`가 기본값 `true`면 지도의 모든 노드에 묻습니다. 그래서 주기 갱신은 "인스턴스 수 × 노드 수"의 연결을 주기마다 맺고 닫습니다("지도를 받을 때마다 모든 노드에 잠깐 쓰는 연결을 엽니다").

- **꼬리: 그럼 갱신을 끄는 게 낫나요?** 아닙니다. 갱신을 끄면 장애 조치나 리샤딩 뒤에 지도가 낡은 채로 남습니다([Redis Cluster](./cluster.md)의 "Lettuce는 지도를 언제 받고, 언제 고치는가"). 인스턴스와 노드가 많을 때 줄일 수 있는 것은 주기와 물어볼 노드 수입니다. `dynamic-refresh-sources`를 `false`로 하면 시드 노드에만 묻는 대신, 시드 노드가 모두 내려가면 지도를 받을 곳이 없어집니다.

**Q. 배포 직후 첫 요청이 느린 이유는 무엇이고, 미리 연결하면 해결되나요?**

Spring Boot는 연결을 미리 맺지 않아서, 첫 Redis 명령이 지도 받기(시드 노드, 이어서 새로 알게 된 노드), 기본 연결 맺기, 그 키의 주 서버로의 연결 맺기를 차례로 기다립니다. `setEagerInitialization(true)`는 앞의 둘을 시작 시점으로 옮기지만, 주 서버마다의 노드 연결은 여전히 처음 쓸 때 맺습니다("시작 시간: 첫 요청이 연결 비용을 냅니다").

- **꼬리: 첫 요청이 10초씩 걸린 적이 있다면요?** 지도를 받을 때 Lettuce는 시도한 노드 모두의 결과를 기다리고, 응답 없는 노드에는 연결 타임아웃과, 연결을 맺기 시작할 때부터 명령 타임아웃 값으로 재는 초기화 제한 중 먼저 끝나는 쪽까지 기다립니다. 두 값을 모두 비워 두면 연결 타임아웃 기본 10초가 먼저 끝나므로, 시드 목록에 사라진 서버가 있으면 그만큼 기다립니다. 둘 중 하나를 서비스 기한에 맞게 정하고 시드 목록을 점검합니다(코드 경로 판단이며 재현하지 않았습니다).

**Q. 연결이 많으면 Redis 메모리는 얼마나 더 쓰나요?**

놀고 있는 연결은 상태 구조체와 응답 버퍼(16KB로 시작해 쓰지 않으면 정기 작업이 줄임) 정도라 작지만 0은 아닙니다. 정확한 크기는 빌드와 할당기에 따라 달라서 `mem_clients_normal` ÷ `connected_clients`로 잽니다. 더 큰 위험은 큰 응답이나 느린 소비로 출력 버퍼가 쌓이는 연결이고, 일반 연결의 출력 버퍼에는 기본 한도가 없습니다. 이 메모리도 `maxmemory` 판단에 들어갑니다("메모리: 놀고 있는 연결은 작고, 응답을 쌓는 연결은 큽니다").

- **꼬리: `maxmemory-clients`를 켜면 해결되나요?** 일반·Pub/Sub 연결 메모리 합에 상한이 생길 뿐, 넘으면 많이 쓰는 연결부터 끊습니다. 끊기는 것은 노드 하나로의 TCP 연결 하나라서, 그 연결이 공유 연결의 노드 연결이었다면 그 인스턴스에서 그 노드로 가는 명령이 재연결을 기다리게 됩니다. 다른 노드로 가는 명령은 각자의 연결로 계속 나갑니다. 켜기 전에 어떤 연결이 끊길 수 있는지, 끊겨도 회복하는지 봅니다.

**Q. 헬스 체크가 왜 복제본에까지 연결을 만드나요?**

Lettuce를 쓰면 Reactor가 늘 클래스패스에 있어서 Spring Boot가 리액티브 Redis 헬스 체크를 등록하고, 그 헬스 체크는 클러스터에서 활성 노드 중 무작위 하나에 `CLUSTER INFO`를 보냅니다. 활성 노드에는 복제본도 들어 있으므로, 여러 번 불리고 나면 리액티브 공유 연결 하나가 모든 노드에 연결을 갖습니다. 이것은 자동 구성 조건과 소스를 읽어 판단한 것이며 실행해 확인하지는 않았습니다("Spring 앱 한 대는 클러스터 연결을 몇 개 만드는가").

- **꼬리: 그게 문제가 될 만큼 큰가요?** 인스턴스 하나에 노드 수 + 1개라, 공유 연결을 쓰는 기본 구성에서는 작습니다. 다만 인스턴스가 수백 대이고 노드가 많은 클러스터라면 노드마다 인스턴스 수만큼 더해지므로 예산에 넣습니다. 헬스 체크에 Redis가 필요 없다면 끌 수 있습니다.
