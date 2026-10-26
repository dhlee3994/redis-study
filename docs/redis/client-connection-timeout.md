# Spring 앱의 Redis 연결과 명령 타임아웃: 무엇을 기준으로 정하는가

> **핵심 질문**: Spring Boot 앱은 Redis와 연결을 어떻게 나눠 쓰는가? 연결 풀은 언제 필요하며, 명령 타임아웃은 무엇을 기준으로 정하는가? 타임아웃이 나거나 연결이 끊겼을 때 그 명령은 서버에서 어떻게 되었는가?
>
> **범위**: Spring Boot의 기본 클라이언트인 Lettuce와 Redis 서버 한 대를 다룹니다. Jedis, 클러스터의 토폴로지 갱신, 복제본 읽기는 다루지 않습니다. 클러스터의 토폴로지 갱신은 Redis Cluster에 있습니다. 복제본 읽기와 장애 조치를 따라가는 연결은 [Redis 복제](./replication.md)에 있습니다. 장애 조치 뒤 연결이 새 주 서버로 옮겨 가기까지의 시간은 [Redis 장애 조치와 클라이언트](./failover-client.md)에 있습니다. 오류 종류(`LOADING`, `MISCONF` 등)별 재시도 정책은 이 문서의 범위 밖입니다.

## 배경 지식

### 이 문서의 예시: 주문 API 한 대, Redis 한 대

문서 끝까지 다음 장면 하나를 씁니다.

- 주문 API는 Spring Boot 앱이고, `spring-boot-starter-data-redis`를 쓰며 Redis 설정은 주소만 적었습니다. 요청 스레드는 200개입니다.
- 사용자 요청은 초당 500건입니다. 요청마다 `GET stock:1`로 재고를 읽고, 주문이 성립하면 `DECR stock:1`로 재고를 줄입니다.
- 같은 Redis를 쓰는 관리자 배치가 가끔 `HGETALL user:all`을 보냅니다. 필드가 100만 개인 해시라 메인 스레드를 800ms 붙잡습니다([Redis 이벤트 루프](./event-loop.md)의 예시와 같은 명령입니다).
- 주문 API 안의 알림 작업자는 `BLPOP notify:queue 5`로 알림을 최대 5초 기다리며 꺼냅니다.
- 평소 주문 API에서 잰 Redis 명령의 응답 시간은 p99.9가 5ms입니다. p99.9는 "1,000건 중 999건이 이 시간 안에 끝났다"는 값으로, 평균이 감추는 느린 꼬리를 보여 줍니다. 사용자 요청 하나에 쓸 수 있는 시간(바깥 기한)은 1초입니다.

```text
사용자 500건/초 ─▶ 주문 API (요청 스레드 200개)
                      │  GET stock:1, DECR stock:1 ──┐
                      │  알림 작업자: BLPOP ─────────┼─▶ Redis 서버 ── 메인 스레드
관리자 배치 ──────────────── HGETALL user:all ───────┘
```

화살표는 명령이 가는 방향입니다. 주문 API 안에서 명령이 어느 연결로 나가는지는 그림에서 뺐습니다. 그 구분이 이 문서의 본론이라 아래 "기본 구성: 연결 하나를 모든 요청 스레드가 나눠 씁니다"에서 따로 그립니다.

시간과 요청 수(200개, 500건, 800ms, 5ms, 1초)는 계산을 보여 주기 위한 **가상의 숫자**이며, 이 문서에서 재지 않았습니다. 기본값과 동작은 `근거` 절의 Spring Boot 3.5.16, Spring Data Redis 3.5.13, Lettuce 6.6.0.RELEASE 소스로 확인했습니다.

### 타임아웃과 붙잡힌 자원

- **한 줄**: 타임아웃은 응답을 기다리는 시간의 상한입니다. 기다리는 동안 요청 스레드는 일하지 않아도 자리를 차지합니다. 상대가 멈추면 묶이는 스레드는 "최근 상한 시간 동안 들어온 요청"이므로, 고르게 들어온다면 약 "초당 요청 수 × 상한(초)"입니다. 요청이 몰리면 이보다 많아집니다.
- **이 주제에서는**: Redis가 멈추면 상한이 짧을수록 주문 API의 요청 스레드가 덜 묶입니다. 대신 타임아웃은 기다리기를 그만둘 뿐 명령을 취소하지 않습니다. 원리와 값을 정하는 두 경계(정상 응답의 꼬리, 자원과 바깥 기한)는 [타임아웃](../cs/timeout.md)에서 다룹니다.

### 응답을 받지 못한 요청과 멱등성

- **한 줄**: 응답을 받지 못한 요청은 실행됐는지 알 수 없습니다. 같은 요청을 여러 번 실행해도 한 번 실행한 것과 효과가 같은 성질을 멱등성이라고 합니다.
- **이 주제에서는**: `GET stock:1`은 두 번 실행돼도 재고를 바꾸지 않습니다(그사이 다른 주문이 재고를 바꿨다면 돌려받는 값은 다를 수 있습니다). `DECR stock:1`은 두 번 실행되면 재고가 두 번 줄어듭니다. 이 문서는 "언제 두 번 실행될 수 있는가"를 다루고, 막는 방법은 [Redis 원자성](./atomicity.md)의 "원자성이 보장하지 않는 것"과 [경쟁 상태와 원자성](../cs/race-condition-and-atomicity.md)에 있습니다.

### Redis 메인 스레드와 이벤트 루프

- **한 줄**: Redis는 메인 스레드 하나가 이벤트 루프를 돌며 모든 연결의 명령을 하나씩 끝까지 실행합니다.
- **이 주제에서는**: `HGETALL user:all`이 도는 800ms 동안 주문 API의 `GET`도 서버에서 기다립니다. 주문 API가 어떤 연결 구성을 쓰든 이 기다림은 사라지지 않습니다. 반면 빈 리스트에 보낸 `BLPOP`은 메인 스레드를 붙잡지 않습니다. 서버는 그 연결을 "기다리는 중"으로 표시해 두고 다른 연결의 명령을 계속 처리합니다. 멈추는 것은 그 연결 하나입니다. 원리는 [Redis 이벤트 루프](./event-loop.md)에 있습니다.

### TCP 연결은 양쪽이 각자 기억하는 상태입니다

- **한 줄**: 주문 API와 Redis 사이의 TCP 연결은 양쪽 운영체제가 "이어져 있다"는 상태를 각자 기억하고 바이트를 주고받는 약속입니다. 받은 쪽 운영체제는 "여기까지 받았다"는 확인 패킷(ACK)을 돌려보내는데, 이것은 Redis의 명령 응답과 다릅니다.
- **이 주제에서는**: 상대가 "닫는다"는 패킷 없이 사라지면 이쪽은 연결이 살아 있다고 계속 믿습니다. 그래서 명령이 실패하는 것과 연결이 끊긴 것을 아는 것은 별개의 일입니다. 원리는 [타임아웃](../cs/timeout.md)의 "연결: 양쪽이 각자 기억하는 상태입니다"에 있습니다.

### 한 연결 위의 명령은 보낸 순서대로 응답이 옵니다

이 문서의 연결 구성을 이해하는 열쇠입니다.

- **한 줄**: Redis는 한 연결에서 받은 명령을 받은 순서대로 실행하고, 응답도 그 순서대로 그 연결에 씁니다. 그래서 클라이언트는 응답을 기다리지 않고 명령을 이어 보낸 뒤, 돌아오는 응답을 보낸 순서대로 짝지을 수 있습니다.
- **비유**: 번호표 없는 창구 하나에 서류를 차례로 밀어 넣는 것과 같습니다. 창구는 받은 순서대로 처리해 같은 순서로 돌려주므로, 보낸 사람은 "세 번째로 돌아온 결과는 세 번째로 낸 서류의 것"이라고 압니다.
- **실제로는**: Redis 서버는 연결마다 입력 버퍼의 명령을 차례로 실행하고, 응답을 그 연결의 응답 버퍼 뒤에 붙입니다. Lettuce는 보낸 명령을 연결마다 큐에 넣어 두고, 응답이 오면 큐의 맨 앞 명령과 짝짓습니다. **비유와 다른 점**: 창구는 한 사람의 서류만 받지만, 이 연결에는 여러 스레드의 명령이 섞여 들어갑니다. 짝짓기는 순서로만 하므로, 앞 명령의 응답이 늦으면 뒤 명령은 자기 응답이 서버에서 이미 만들어졌어도 받을 수 없습니다.
- **이 주제에서는**: 한 연결을 여러 요청 스레드가 함께 쓸 수 있는 이유이자, 그 연결 위의 큰 응답 하나가 뒤의 모든 응답을 늦추는 이유입니다.

### 세 라이브러리는 각각 무엇을 맡는가

| 라이브러리 | 맡는 일 | 예시에서 |
| --- | --- | --- |
| Spring Boot | `spring.data.redis.*` 설정을 읽어 아래 둘을 구성 | `timeout`을 적지 않았으므로 기본값이 그대로 감 |
| Spring Data Redis | `RedisTemplate`, `LettuceConnectionFactory`. 명령마다 어느 연결을 쓸지 고름 | `GET`과 `BLPOP`이 다른 연결로 나감 |
| Lettuce | 실제 TCP 연결, 명령 전송과 응답 짝짓기, 다시 연결하기, 명령 타임아웃. 소켓 입출력은 네트워크 라이브러리 Netty에 맡김 | 응답이 없을 때 언제 실패시킬지 정함 |

## 기본 구성: 연결 하나를 모든 요청 스레드가 나눠 씁니다

### 일반 명령은 공유 연결 하나로 나갑니다

Spring Data Redis의 `LettuceConnectionFactory`는 기본으로 Lettuce 연결 하나를 모든 스레드가 공유합니다(`shareNativeConnection` 기본 true). 예시의 요청 스레드 200개가 보내는 `GET stock:1`, `DECR stock:1`은 모두 이 연결 하나로 나갑니다.

```text
요청 스레드 1 ── GET ──┐
요청 스레드 2 ── DECR ─┼─▶ 공유 연결 1개 ──▶ Redis
   ...                 │   (Lettuce가 보낸 순서대로 큐에 두고, 응답을 순서대로 짝지음)
요청 스레드 200 ─ GET ─┘

알림 작업자 ── BLPOP ──▶ 전용 연결 ──▶ Redis
```

화살표는 명령이 가는 방향입니다. 응답은 같은 연결로 반대 방향으로 옵니다.

연결 하나로 충분한 이유는 둘입니다.

- **Lettuce는 응답을 기다리며 연결을 막지 않습니다.** 위 "한 연결 위의 명령은 보낸 순서대로 응답이 옵니다" 덕분에, 앞 명령의 응답을 받기 전에 다음 명령을 보낼 수 있습니다. 응답을 기다리는 것은 요청 스레드이고, 연결은 계속 다른 스레드의 명령을 실어 나릅니다.
- **Redis 쪽에서 연결을 늘려도 실행은 한 줄입니다.** 메인 스레드가 명령을 하나씩 실행하므로, 연결을 10개로 늘려도 서버가 동시에 실행하는 명령 수는 늘지 않습니다. 연결 수가 늘면 서버가 연결마다 두는 버퍼와 관리 비용은 늘어납니다. 응답 전송과 해석이 병목인 경우라면 연결을 늘려 얻는 것이 있을 수 있지만, 그 폭은 환경마다 달라 재 봐야 합니다.

### 공유 연결의 대가: 앞 응답이 늦으면 뒤 응답도 늦습니다

공유 연결 위의 한 명령이 큰 응답을 받으면, 그 응답을 다 받아 해석할 때까지 뒤에 보낸 명령의 응답을 짝지을 수 없습니다. 예를 들어 주문 API가 공유 연결로 큰 해시를 `HGETALL`하면, 같은 연결의 `GET stock:1`은 서버에서 이미 실행됐더라도 그 큰 응답 뒤에 줄을 섭니다. 이 지연은 서버 메인 스레드의 기다림([Redis 이벤트 루프](./event-loop.md))과 별도로 더해집니다.

판단은 다음과 같습니다. 짧은 명령만 보낸다면 공유 연결의 대가는 작습니다. 큰 응답을 받는 명령이 섞인다면, 연결을 늘리기 전에 명령 자체를 나눠 읽기로 바꾸는 것이 먼저입니다. 서버의 기다림은 연결 구성으로 풀 수 없기 때문입니다.

### 전용 연결을 쓰는 명령: 기다리는 명령, 트랜잭션, 파이프라인, 구독

공유하면 안 되는 명령은 그 작업 동안만 쓰는 전용 연결로 나갑니다.

| 명령 | 왜 공유하면 안 되는가 |
| --- | --- |
| `BLPOP`, `XREAD ... BLOCK` 같은 기다리는 명령 | 기다리는 동안 서버가 그 연결의 다음 명령을 처리하지 않습니다. 메인 스레드는 다른 연결을 계속 처리하므로, 연결만 따로 두면 다른 요청은 영향을 받지 않습니다 |
| `MULTI`/`EXEC` 트랜잭션 | `MULTI`부터 `EXEC`까지의 명령이 한 연결에 모여야 합니다. 다른 스레드의 명령이 끼어들면 그 명령도 트랜잭션에 들어갑니다 |
| 파이프라인(`executePipelined`) | 모아 보낸 명령의 응답을 한꺼번에 받아야 합니다 |
| Pub/Sub 구독 | 구독한 연결로는 서버가 보내는 메시지가 언제든 들어옵니다. Spring Data Redis는 구독에 Pub/Sub 전용 연결을 쓰고, 구독하는 동안 그 연결을 계속 차지합니다. `RedisMessageListenerContainer`는 여러 채널과 리스너를 연결 하나에 모아 구독하므로, 연결 수는 채널 수가 아니라 구독 연결 수로 셉니다 |

**풀이 없으면 전용 연결은 쓸 때마다 새로 맺고 닫습니다.** 전용 연결을 내주는 쪽은 기본으로 연결을 새로 만들고, 다 쓰면 닫습니다. 여기서 "쓸 때"는 `RedisTemplate` 호출 한 번입니다. 콜백 하나 안에서 명령 여러 개를 보내면 그동안은 같은 연결을 씁니다. 예시의 알림 작업자가 `BLPOP`을 부를 때마다 TCP 연결을 새로 맺고, 연결을 준비하는 명령(프로토콜 협상, 설정에 따라 인증이나 데이터베이스 선택 등)을 주고받은 뒤 `BLPOP`을 보내고 닫습니다. 연결 맺기에는 왕복 시간이 들고, 서버에는 연결이 열리고 닫히는 일이 그만큼 생깁니다. 다음 절의 연결 풀이 이 비용을 줄입니다.

## 연결 풀은 언제 필요하고, 무엇을 대가로 치르는가

### Spring Boot는 commons-pool2가 있으면 풀을 저절로 켭니다

Spring Boot 3.5.16은 `spring.data.redis.lettuce.pool.enabled`를 적지 않았을 때, 클래스패스에 `commons-pool2`가 있으면 풀을 켭니다. 다른 의존성이 `commons-pool2`를 끌어와도 켜지므로, 의도하지 않게 풀을 쓰고 있을 수 있습니다.

**공유 연결을 켠 상태에서 일반 명령은 풀을 거치지 않습니다.** 이 점을 잘못 읽기 쉽습니다. 풀을 켜면 공유 연결 자체도 풀에서 하나를 빌려 와 계속 붙잡고, `GET stock:1` 같은 일반 명령은 모두 그 연결 하나로 나갑니다. 풀의 나머지 연결은 위 표의 기다리는 명령, 트랜잭션, 파이프라인이 그때그때 빌려 씁니다(Pub/Sub 구독은 연결 종류가 달라 별도로 빌립니다). "풀 크기를 늘리면 일반 명령의 처리량이 는다"는 기대는 기본 구성에서는 맞지 않습니다.

### 풀의 기본값: 최대 8개, 기다림에 상한 없음

| 설정(`spring.data.redis.lettuce.pool.*`) | 3.5.16 기본값 | 뜻 |
| --- | --- | --- |
| `max-active` | 8 | 동시에 빌려줄 수 있는 연결 수. 공유 연결이 하나를 계속 차지함 |
| `max-idle` | 8 | 놀고 있어도 닫지 않고 남겨 둘 연결 수 |
| `min-idle` | 0 | 놀고 있는 연결을 최소 몇 개 유지할지. 0이면 미리 맺어 두지 않음 |
| `max-wait` | -1 | 풀이 바닥났을 때 빌리기를 기다리는 시간. **음수면 무한정 기다림** |

예시의 알림 작업자를 스레드 10개로 늘리면 `BLPOP`이 동시에 10개까지 돕니다. 각자 최대 5초씩 전용 연결을 붙잡습니다. `max-active` 8 중 하나는 공유 연결이 차지하므로 전용 연결로 쓸 수 있는 것은 7개이고, 나머지 세 스레드는 풀 앞에서 기다립니다. `max-wait`가 기본값 -1이면 이 기다림에 상한이 없습니다. 같은 풀을 트랜잭션이나 파이프라인을 쓰는 요청 스레드도 함께 쓴다면, 그 요청 스레드들도 상한 없이 묶입니다. 명령 타임아웃을 아무리 짧게 정해도, 명령을 보내기 전 풀 앞의 기다림은 그 타임아웃에 들어가지 않습니다.

그래서 풀을 쓴다면 두 가지를 정합니다.

- **`max-active`는 공유 연결 하나 + 동시에 필요한 전용 연결 수로 정합니다.** 리틀의 법칙으로 "전용 연결이 필요한 작업의 초당 횟수 × 한 번에 붙잡는 시간"을 계산합니다. 이 값은 평균이므로 요청이 몰리는 순간을 감안해 여유를 둡니다. 기다리는 명령처럼 오래 붙잡는 작업은 동시에 돌 수 있는 최대치를 셉니다. 늘린 만큼 Redis 서버의 연결 수도 늘어나며, 주문 API 인스턴스 수를 곱해 서버의 `maxclients`(8.2.10 기본 10000) 안에 드는지 봅니다.
- **`max-wait`에 상한을 둡니다.** 바깥 기한 안에 들도록 정합니다. 바닥났을 때 빨리 실패하는 편이 요청 스레드를 지킵니다.

### 공유를 끄면 모든 명령이 풀을 거칩니다

`LettuceConnectionFactory`의 `shareNativeConnection`을 false로 바꾸면 일반 명령도 풀(또는 매번 새 연결)에서 빌린 연결로 나갑니다. 큰 응답 하나가 같은 연결의 다른 응답을 늦추는 일은 줄어듭니다. 대신 요청 스레드마다 연결을 빌리고 돌려주는 비용이 생기고, 동시에 Redis를 쓰는 요청 수만큼 연결이 필요해져 풀 크기와 서버 연결 수가 커집니다. 풀이 바닥나면 위의 `max-wait` 문제가 일반 명령 전체로 번집니다.

이 문서는 **기본값인 공유 연결을 유지하는 쪽**을 권합니다. Redis 서버가 명령을 한 줄로 실행하므로 연결을 늘려 얻는 처리량이 작고, 큰 응답 문제는 명령을 바꿔 푸는 것이 근본 대책이기 때문입니다. 공유를 끄는 쪽은 큰 응답을 피할 수 없는 명령이 일반 명령과 섞여 있고, 그 명령만 따로 떼어 낼 수 없을 때 검토합니다.

## 타임아웃은 네 군데에 있고, 기본값은 서비스 기한보다 깁니다

예시의 주문 API처럼 Redis 설정에 주소만 적으면 다음 값이 그대로 적용됩니다.

| 기다리는 것 | 설정 | 적지 않았을 때 |
| --- | --- | --- |
| TCP 연결 맺기 | `spring.data.redis.connect-timeout` | Lettuce 기본 10초 |
| 명령의 응답 | `spring.data.redis.timeout` | Lettuce 기본 60초 |
| 풀에서 전용 연결 빌리기 | `spring.data.redis.lettuce.pool.max-wait` | 무한정(-1) |
| 기다리는 명령의 대기 | 명령의 인자(예: `BLPOP`의 5초) | 명령마다 정함 |

예시의 바깥 기한은 1초입니다. 앞의 세 기본값이 모두 이보다 깁니다. **기본값 그대로는 Redis가 멈췄을 때 주문 API의 요청 스레드를 지키지 못합니다.**

`spring.data.redis.timeout`의 설명은 "Read timeout"이지만, 실제로는 Lettuce의 **명령 타임아웃**으로 들어갑니다. 소켓에서 바이트를 읽는 시간의 상한이 아니라, 명령 하나를 맡긴 뒤 응답을 받기까지의 상한입니다. Spring Boot는 Lettuce의 명령 만료 기능도 켭니다(`TimeoutOptions.enabled()`). 그래서 이 값은 동기 API로 결과를 기다리는 시간과, Lettuce가 응답 없는 명령을 실패로 끝내는 시간 양쪽에 쓰입니다.

**명령 타임아웃의 시계는 명령을 Lettuce에 맡긴 순간부터 갑니다.** 연결이 끊겨 아직 보내지 못한 명령도 시계가 돕니다. 그래서 이 값은 "서버가 처리하는 시간"이 아니라 "주문 API가 그 명령 하나에 내줄 수 있는 시간 전체"로 정해야 합니다.

서버 쪽에도 연결과 관련된 설정이 둘 있습니다. 판단에 영향을 주는 만큼만 적습니다.

- `timeout`(8.2.10 기본 0, 꺼짐): 한동안 아무것도 보내지 않은 클라이언트 연결을 서버가 닫습니다. 켜면 오래 노는 풀의 연결이나 공유 연결이 서버 쪽에서 닫힐 수 있습니다. 서버가 닫으면 "닫는다"는 패킷이 오므로 Lettuce는 이를 알아채고 자동 재연결을 시작합니다. 공식 문서는 Pub/Sub 연결에는 적용되지 않는다고 적습니다.
- `tcp-keepalive`(8.2.10 기본 300초): 서버가 응답 없는 클라이언트를 알아내는 데 씁니다. 조용한 연결에 주기적으로 패킷을 보내므로, 놀고 있는 연결을 중간 장비가 지우는 일을 막는 데도 도움이 됩니다. **서버 쪽 설정이라 클라이언트가 죽은 서버를 알아내는 데는 도움이 되지 않습니다.** 그 문제는 아래 "조용히 끊긴 연결: 명령마다 실패하는데 다시 연결하지 않습니다"에서 다룹니다.

## 명령 타임아웃 값은 무엇을 기준으로 정하는가

[타임아웃](../cs/timeout.md)의 두 경계를 예시에 대입합니다.

- **하한**: 주문 API에서 잰 Redis 응답의 꼬리 위에 둡니다. 예시의 p99.9는 5ms입니다. 서버의 SLOWLOG에 찍히는 실행 시간이 아니라, 네트워크와 서버의 기다림이 모두 들어간 **클라이언트에서 잰 시간**이어야 합니다.
- **상한**: Redis가 멈췄을 때 Redis를 기다리느라 묶여도 되는 요청 스레드 수로 정합니다. Redis에 50개까지만 내주겠다면 50 ÷ 500 = 0.1초입니다. 바깥 기한 1초 안에 Redis를 두 번(`GET`, `DECR`) 부르고 다른 일도 해야 하므로, 이 조건으로도 1초보다 충분히 짧아야 합니다. Redis 호출이 DB 트랜잭션 안에 있으면 묶이는 것은 요청 스레드만이 아닙니다. 트랜잭션이 잡은 DB 연결은 커밋이나 롤백까지 풀로 돌아가지 않으므로, Redis를 기다리는 동안 DB 연결도 붙잡힙니다(JDBC 트랜잭션 관리자 기준이며, JPA에서 연결을 돌려주는 시점은 이 문서에서 확인하지 않았습니다). Redis를 기다리며 쥐고 있는 DB 연결 수는 요청 스레드처럼 "초당 요청 수 × 기다리는 시간"으로 늘어나므로, 이 값이 DB 연결 풀 크기에 닿으면 Redis와 무관한 DB 요청도 연결을 기다리게 됩니다. 그 연결 수가 풀 크기에 닿는 시점은 요청 스레드가 바닥나는 시점보다 이를 수 있습니다. DB 연결 풀이 요청 스레드보다 작다면 같은 유입에서 DB 연결 쪽이 먼저 한계에 닿기 때문입니다. Redis 호출을 트랜잭션 밖으로 빼는 것이 먼저이고, 뺄 수 없다면 상한을 "Redis에 내줘도 되는 DB 연결 수"로도 계산합니다.

예시라면 5ms와 100ms 사이에서 고릅니다. 꼬리 위에 여유를 두어 정상 요청이 실패하지 않게 하되, 상한을 넘지 않게 합니다.

### 예시의 세 사건에 대입하면

| 사건 | 기본값 60초일 때 | 100ms일 때 |
| --- | --- | --- |
| `HGETALL user:all`이 메인 스레드를 800ms 붙잡음 | 0.4초 만에 요청 스레드 200개가 모두 Redis를 기다림(약 200 ÷ 500초). 800ms 뒤 차례로 풀림. 그동안 Redis와 무관한 API도 스레드를 못 받음 | 요청이 고르게 들어오면 묶이는 스레드는 약 50개(500 × 0.1). 그 800ms 동안 Redis를 쓰는 요청은 끝 무렵 도착한 일부를 빼고 대부분 실패하지만, 무관한 API는 계속 처리됨 |
| Redis 재시작으로 연결이 10초 끊김 | 끊긴 동안 보낸 명령은 Lettuce 안에서 대기. 0.4초 만에 스레드가 바닥나고 다시 연결될 때까지 회복하지 않음 | 명령마다 100ms 뒤 실패. 스레드는 약 50개만 묶임 |
| 네트워크 장비 고장으로 연결이 조용히 끊김 | 명령마다 60초 기다린 뒤 실패. 스레드가 바닥난 채 오래 유지됨 | 명령마다 100ms 뒤 실패. 하지만 연결은 다시 맺어지지 않음(아래 절) |

첫 줄이 보여 주듯 **타임아웃은 멈춤보다 짧을 때만 스레드를 지킵니다.** 800ms 멈춤에 1초 타임아웃은 아무것도 지키지 못합니다. 반대로 100ms는 800ms 동안 Redis를 쓰는 요청 대부분을 실패시킵니다. 또 50개는 평균 유입으로 낸 값이라, 요청이 몰리면 더 묶입니다. 어느 쪽이 나은지는 "Redis 요청 일부가 실패하는 것"과 "서비스 전체가 멈추는 것" 중 무엇을 감당할 수 있는가로 정합니다. 대부분의 서비스에서는 앞쪽입니다.

### 기다리는 명령은 명령 타임아웃보다 짧게 기다려야 합니다

Spring Boot가 켠 명령 만료는 기다리는 명령에도 같은 명령 타임아웃을 적용합니다. `TimeoutOptions.enabled()`는 명령 종류와 상관없이 연결에 설정한 명령 타임아웃(`spring.data.redis.timeout`)을 씁니다. Spring Data Redis의 동기 API도 같은 값만큼만 결과를 기다립니다.

그래서 명령 타임아웃을 100ms로 줄이면 예시의 `BLPOP notify:queue 5`는 5초를 기다리지 못하고 100ms 뒤 클라이언트 쪽에서 타임아웃으로 실패합니다. 알림이 없는 정상 상황인데도 계속 예외가 납니다. Spring Data Redis의 `StreamMessageListenerContainer`도 기본으로 2초씩 기다리는 읽기(소비자 그룹이면 `XREADGROUP`, 아니면 `XREAD`에 `BLOCK`)를 보내므로([Redis Streams](./streams.md)의 "Spring에서 확인할 것") 같은 문제를 만납니다.

선택지는 두 가지입니다.

- **기다리는 시간을 명령 타임아웃보다 짧게 줄입니다.** 다만 Spring Data Redis의 `leftPop(key, timeout)`은 대기 시간을 초 단위로 바꾸고, 1초 미만은 1초로 올립니다. 그래서 이 API로는 1초보다 짧게 기다릴 수 없고, 명령 타임아웃이 1초보다 길어야 합니다. 대기를 줄이면 빈 대기열에도 요청이 자주 오가 Redis 명령 수가 늘어납니다.
- **기다리는 작업에는 연결 구성을 따로 둡니다.** 명령 타임아웃을 길게 잡은 `LettuceConnectionFactory`와 템플릿을 하나 더 만들어 알림 작업자만 쓰게 합니다. 요청 경로의 짧은 타임아웃을 지키면서 대기를 길게 둘 수 있습니다. 대신 연결 구성이 둘이 되어 설정과 관찰할 대상이 늘어납니다.

예시라면 뒤쪽을 고릅니다. 요청 경로와 작업자는 기다림의 성질이 달라 한 값으로 둘 다 맞추기 어렵기 때문입니다.

## 타임아웃이 난 명령은 서버에서 어떻게 되었는가

Lettuce의 명령 타임아웃은 클라이언트 쪽에서 그 명령을 예외로 끝낼 뿐, 서버에는 아무것도 보내지 않습니다. Spring Data Redis는 일반 명령의 이 예외를 `QueryTimeoutException`으로 바꿔 던집니다. 파이프라인에서는 `RedisPipelineException`에 감싸여 올 수 있습니다. 타임아웃이 연결을 끊지도 않습니다. 이때 명령은 다음 중 하나입니다.

| 상태 | 예시에서 | 서버에서는 |
| --- | --- | --- |
| 아직 보내지 못함 | 연결이 끊겨 Lettuce 안에서 대기하던 `DECR` | 실행되지 않음. 이미 끝난 명령은 다시 연결돼도 보내지 않음 |
| 보냈고 실행 대기 중 | `HGETALL user:all` 뒤에서 기다리던 `DECR` | **차례가 오면 실행됨**. 응답은 이미 끝난 명령에 도착해 버려짐 |
| 보냈고 실행됨 | 실행 직후 응답이 늦은 `DECR` | 이미 실행됨 |

주문 API는 `QueryTimeoutException`만 보고 셋을 구분하지 못합니다. 그래서 다음 두 가지가 따라옵니다.

- **`GET`은 다시 보내도 재고를 바꾸지 않지만, `DECR`은 다릅니다.** 타임아웃 뒤 `DECR`을 다시 보내면 재고가 두 번 줄 수 있습니다. 쓰기를 다시 보내려면 멱등성을 먼저 확보합니다. 스크립트 하나 안에서 "이 주문 ID를 이미 처리했는가"를 확인하고, 처리했으면 차감하지 않고, 처음이면 차감과 처리 기록을 함께 남깁니다. 여기서 스크립트는 앱이 아니라 Redis 서버가 실행하는 Lua 스크립트이고, 메인 스레드가 스크립트 하나를 끝까지 실행하는 동안 다른 연결의 명령은 끼어들지 못합니다. 앱 코드에서 명령 여러 개를 함수 하나로 묶는 것과는 다릅니다. 그 함수의 명령들 사이에는 다른 요청의 명령이 끼어들 수 있습니다. 확인과 차감 사이에 다른 명령이 끼어들면 두 요청이 모두 "처음"으로 판단할 수 있으므로 한 스크립트로 묶고, 기록은 다시 보낼 수 있는 기간보다 오래 남깁니다([Redis 원자성](./atomicity.md)의 "원자성이 보장하지 않는 것").
- **긴 명령은 타임아웃이 나도 서버에서 끝까지 돕니다.** 관리자 배치가 `HGETALL user:all`의 타임아웃을 보고 다시 보내면, 메인 스레드를 800ms 붙잡는 명령이 또 실행됩니다. 기다리기만 한 주문 API의 `GET`을 바로 다시 보내는 것도 이미 밀린 줄에 요청을 더 얹습니다.

## 연결이 끊기면 Lettuce가 응답 못 받은 명령을 다시 보냅니다

이 절은 앱이 재시도 코드를 하나도 쓰지 않아도 생기는 중복 실행을 다룹니다.

### 기본 동작: 끊긴 동안 쌓아 두고, 다시 연결되면 보냅니다

Lettuce는 기본으로 자동 재연결을 켜 둡니다(`autoReconnect` true). 이때 연결은 "최소 한 번(at-least-once)" 방식으로 동작합니다. 이름과 달리 반드시 한 번 실행된다는 보장이 아닙니다. "끊겨도 버리지 않고 다시 보내므로, 두 번 실행될 수는 있다"는 재전송 방식의 이름입니다. 타임아웃으로 먼저 끝난 명령은 한 번도 실행되지 않을 수 있습니다. 예시의 Redis가 재시작되어 공유 연결이 끊기면 다음이 일어납니다.

1. **보냈지만 응답을 받지 못한 명령을 버리지 않습니다.** 연결이 끊긴 것을 알아채면, 응답을 기다리던 명령들을 꺼내 "끊긴 동안의 버퍼"로 옮깁니다.
2. **끊긴 동안 새로 들어온 명령도 받아 둡니다.** 자동 재연결이 켜져 있으면 기본 동작(`DisconnectedBehavior.DEFAULT`)은 명령을 거절하지 않고 버퍼에 쌓습니다. 이 버퍼의 기본 크기 상한은 사실상 없습니다.
3. **다시 연결되면 버퍼의 명령을 보냅니다.** 아직 끝나지 않은 명령만 보냅니다. 명령 타임아웃으로 이미 실패 처리된 명령은 보내지 않습니다.

### 왜 중복 실행이 되는가

1번에서 옮긴 명령에는 **서버가 이미 실행한 명령**이 섞여 있을 수 있습니다. 예시의 `DECR stock:1`을 서버가 실행하고 응답을 보낸 직후 연결이 끊기면, 주문 API는 응답을 받지 못했으므로 그 `DECR`을 버퍼로 옮깁니다. 다시 연결되면 Lettuce가 같은 `DECR`을 한 번 더 보냅니다. 재고는 두 번 줄고, 주문 API는 두 번째 실행의 응답 하나만 받습니다. **앱 코드에는 재시도가 없는데도 중복이 생깁니다.**

명령 타임아웃과의 관계는 다음과 같습니다. 3번에서 보면 타임아웃이 먼저 난 명령은 다시 보내지 않습니다. 그래서 명령 타임아웃이 짧을수록 다시 연결되기 전에 실패 처리되는 명령이 늘어, 재전송되는 명령이 줄어듭니다. 기본값 60초라면 대부분의 짧은 재시작은 60초 안에 다시 연결되므로 응답 못 받은 명령이 거의 그대로 다시 나갑니다. 다만 타임아웃이 짧아도 다시 연결이 그보다 빠르면 재전송은 일어나므로, **짧은 타임아웃이 중복을 막아 주지는 않습니다.**

### 선택지

| 방법 | 얻는 것 | 대가 |
| --- | --- | --- |
| 쓰기를 멱등하게 만든다(권장) | 누가 다시 보내든 한 번만 반영됩니다. Lettuce의 재전송과 앱의 재시도를 함께 막습니다 | 처리 기록 키와 그 만료 시간을 관리해야 합니다 |
| `replayFilter`로 다시 보내지 않을 명령을 고른다 | 끊긴 동안 버퍼에 들어가는 명령과 소켓 쓰기에 실패한 명령 중 고른 것은 다시 보내지 않습니다(기본은 아무것도 거르지 않음) | **이미 보내고 응답을 기다리던 명령의 재전송은 막지 못합니다.** 그 명령들을 버퍼로 옮기는 경로는 필터를 보지 않습니다. 이 절의 `DECR` 사례에는 효과가 없습니다 |
| 자동 재연결을 끈다(`autoReconnect` false) | "최대 한 번(at-most-once)" 방식이 되어 재전송이 없습니다 | 끊기면 연결이 돌아오지 않으므로, 다시 연결하는 책임이 앱으로 옵니다. 공유 연결 하나를 쓰는 구성에서는 부담이 큽니다 |
| 끊긴 동안 새 명령을 거절한다(`DisconnectedBehavior.REJECT_COMMANDS`) | 끊긴 동안 명령이 쌓이지 않고 바로 실패하므로 요청 스레드가 묶이지 않습니다 | 자동 재연결이 켜진 동안에는 이미 보낸 명령의 재전송은 그대로입니다. 이미 보낸 명령까지 거절하는 것은 자동 재연결을 끈 경우뿐입니다 |

Spring Boot에서는 `LettuceClientOptionsBuilderCustomizer` 빈으로 `ClientOptions`를 바꿀 수 있습니다. 예시라면 `DECR`을 주문 ID 기록과 함께 스크립트로 묶어 멱등하게 만들고, 나머지 설정은 기본값을 유지합니다. 설정으로는 응답을 기다리던 명령의 재전송을 자동 재연결을 끄지 않는 한 막을 수 없고, 재전송돼도 안전한 쓰기는 앱의 재시도까지 함께 해결하기 때문입니다.

## 조용히 끊긴 연결: 명령마다 실패하는데 다시 연결하지 않습니다

예시에서 Redis와 주문 API 사이의 네트워크 장비가 고장 나, 끊는다는 패킷 없이 연결이 사라졌다고 합시다. Redis 서버는 살아 있습니다.

1. 주문 API의 운영체제는 공유 연결이 살아 있다고 기억합니다. Lettuce도 끊긴 것을 모릅니다.
2. `GET stock:1`을 보내면 응답이 오지 않습니다. 명령 타임아웃 100ms가 지나면 실패합니다. **명령 타임아웃은 연결을 끊지 않으므로** 다음 `GET`도 같은 연결로 나가 또 100ms 뒤 실패합니다.
3. 운영체제가 재전송을 포기해 연결을 끊어야 Lettuce가 끊긴 것을 알고 다시 연결합니다. Linux 기본값에서 이 시간은 15분이 넘습니다([타임아웃](../cs/timeout.md)의 "조용히 끊긴 연결은 타임아웃 없이는 오래 남습니다").

그동안 공유 연결로 나가는 모든 명령, 곧 주문 API의 일반 명령 전부가 실패합니다. 짧은 명령 타임아웃 덕분에 스레드는 지키지만, **Redis 서버가 멀쩡한데도 15분 넘게 Redis를 못 쓰는** 상태가 됩니다. 장애 조치로 주 서버가 바뀐 경우에도, 옛 주 서버로 맺어 둔 연결이 조용히 죽어 있다면 같은 일이 생깁니다.

Lettuce 6.6.0의 기본값은 이 상황을 감지하지 않습니다.

| Lettuce 설정(`SocketOptions`) | 기본값 | 켜면 |
| --- | --- | --- |
| TCP keepalive | 꺼짐 | 한동안 조용한 연결에 확인 패킷을 보냅니다. 운영체제 기본값으로는 첫 확인까지 2시간이라, 간격(`KeepAliveOptions`의 idle·interval·count)을 함께 줄여야 의미가 있습니다 |
| TCP user timeout | 꺼짐 | 보낸 데이터의 확인 패킷이 정해진 시간 동안 오지 않으면 운영체제가 연결을 끊고, Lettuce가 다시 연결합니다. 명령을 보냈는데 경로가 끊겨 확인 패킷이 오지 않는 이 상황에 직접 들어맞습니다 |

두 설정이 감지하는 것은 TCP 수준의 무응답입니다. `HGETALL user:all` 때문에 Redis가 바쁜 경우에는 Redis 서버의 운영체제가 확인 패킷을 정상으로 돌려보내므로, 명령 응답이 늦어도 두 설정은 동작하지 않습니다. 그 경우를 맡는 것은 명령 타임아웃입니다.

두 설정에는 조건이 있습니다.

- **전송 방식에 따라 적용되지 않을 수 있습니다.** Netty는 소켓을 다루는 방법으로 JDK 기본 방식(NIO)과, 운영체제 전용 기능을 직접 부르는 네이티브 전송(Linux의 epoll·io_uring)을 고를 수 있습니다. 같은 소켓 설정이라도 어느 방식을 쓰느냐에 따라 운영체제에 전달되지 않을 수 있습니다. Lettuce 설명에 따르면 keepalive 간격 설정은 Java 11 이상의 NIO 소켓이나 epoll·io_uring 소켓에서만, TCP user timeout은 epoll·io_uring 네이티브 전송에서만 적용됩니다. epoll 전송은 Netty의 `netty-transport-native-epoll`이 실행 시 클래스패스에 있고 운영체제·CPU에 맞는 네이티브 라이브러리를 불러올 수 있을 때만 쓰입니다. Lettuce는 이 의존성을 선택(optional)으로만 두므로 직접 추가하거나 다른 의존성을 통해 들어와야 합니다. 적용됐는지는 운영 환경에서 확인해야 하며, 이 문서에서는 확인하지 않았습니다.
- **Spring Boot의 `connect-timeout`과 같은 객체를 씁니다.** Spring Boot는 `connect-timeout`을 `SocketOptions`에 담아 넘기고, 그다음 `LettuceClientOptionsBuilderCustomizer`를 부릅니다. 커스터마이저에서 `SocketOptions`를 새로 만들어 넣으면 `connect-timeout`이 사라지므로, 새 `SocketOptions`에 연결 타임아웃도 함께 넣습니다.

감지 시간을 너무 짧게 잡으면 잠깐의 네트워크 지연에도 멀쩡한 공유 연결을 끊습니다. 공유 연결은 주문 API의 일반 명령 전부가 쓰는 연결이라, 다시 연결하는 동안 그 명령들이 대기하거나 실패합니다. 평소 네트워크 지연의 꼬리보다 넉넉하게, 그러나 서비스가 Redis 없이 버틸 수 있는 시간보다는 짧게 둡니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

- **요청이 늘면 같은 타임아웃이 더 많은 스레드를 묶습니다.** 묶이는 스레드는 "초당 요청 수 × 타임아웃"이므로, 초당 500건에서 50개였던 것이 1,000건이 되면 100개입니다. 타임아웃 값은 트래픽이 늘 때 다시 계산해야 합니다.
- **데이터가 늘면 하한이 올라갑니다.** 키의 원소 수가 늘어 긴 명령이 생기면 그 명령을 기다리는 다른 명령의 꼬리 응답 시간도 늘어납니다. 정상 요청의 실패가 늘어난다면 타임아웃을 늘리기 전에 긴 명령부터 찾습니다([Redis 이벤트 루프](./event-loop.md)의 "무엇이 메인 스레드를 오래 잡는가").
- **인스턴스가 늘면 서버의 연결 수가 곱해집니다.** 풀을 쓰면 공유 연결을 포함한 풀 연결과 구독 연결이 인스턴스마다 생깁니다. 오토스케일링으로 인스턴스가 늘 때 "인스턴스 수 × (풀 최대치 + 구독 연결 수)"가 서버의 `maxclients` 안에 드는지 봅니다. 클러스터에서는 연결 하나가 주 서버 수 + 1개의 TCP 연결이 됩니다. Redis Cluster의 연결 수에서 다룹니다.
- **끊긴 동안 쌓이는 명령도 요청에 비례합니다.** 기본 설정에서 끊긴 동안의 버퍼에는 상한이 없습니다. 여기서 세는 것을 둘로 나눠야 합니다. **버퍼에 남은 항목 수**는 끊긴 시간 동안 들어온 요청 수만큼 늘어납니다. 명령 타임아웃으로 실패 처리된 명령도 버퍼에서 바로 빠지지 않고, 다시 연결돼 버퍼를 비울 때에야 걸러지기 때문입니다. 반면 **아직 끝나지 않은 명령 수**는 동기 API라면 요청 스레드마다 하나씩 기다리므로 대략 스레드 수까지이고, 비동기 API로 보내는 명령은 그 제한이 없습니다. 다시 연결되면 이 끝나지 않은 명령들이 한꺼번에 전송 경로로 넘어가, 막 재시작한 Redis가 이 명령들을 짧은 사이에 몰아서 받습니다. 명령 타임아웃이 길수록 끝나지 않은 채 남는 명령이 많아집니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 바로잡은 곳 |
| --- | --- | --- |
| 풀 크기를 늘리면 일반 명령이 빨라진다 | 공유 연결을 켠 기본 구성에서 일반 명령은 공유 연결 하나로 나가고, 풀의 나머지는 전용 연결에 쓰입니다. 서버도 명령을 한 줄로 실행합니다 | Spring Boot는 commons-pool2가 있으면 풀을 저절로 켭니다 |
| 자동 재연결은 "최소 한 번" 방식이니 명령은 반드시 실행된다 | 끊겨도 버리지 않고 다시 보낸다는 뜻일 뿐입니다. 두 번 실행될 수 있고, 타임아웃으로 먼저 끝난 명령은 한 번도 실행되지 않을 수 있습니다 | 연결이 끊기면 Lettuce가 응답 못 받은 명령을 다시 보냅니다 |
| 연결이 하나뿐이면 요청이 하나씩만 나간다 | 응답을 기다리지 않고 이어 보내고, 응답을 순서로 짝짓습니다 | 한 연결 위의 명령은 보낸 순서대로 응답이 옵니다 |
| `spring.data.redis.timeout`은 소켓 읽기 시간이다 | Lettuce의 명령 타임아웃으로 들어가, 명령을 맡긴 순간부터 응답까지를 셉니다 | 타임아웃은 네 군데에 있고, 기본값은 서비스 기한보다 깁니다 |
| 명령 타임아웃만 짧게 하면 요청 스레드가 안전하다 | 풀 앞의 기다림(`max-wait` 기본 무한)은 명령 타임아웃에 들어가지 않습니다 | 풀의 기본값: 최대 8개, 기다림에 상한 없음 |
| 타임아웃이 난 명령은 실행되지 않았다 | 서버에서 이미 실행됐거나, 줄에서 기다리다 나중에 실행될 수 있습니다 | 타임아웃이 난 명령은 서버에서 어떻게 되었는가 |
| 앱에 재시도 코드가 없으면 쓰기는 한 번만 반영된다 | Lettuce가 다시 연결한 뒤 응답 못 받은 명령을 스스로 다시 보냅니다 | 연결이 끊기면 Lettuce가 응답 못 받은 명령을 다시 보냅니다 |
| 명령 타임아웃이 나면 Lettuce가 연결을 다시 맺는다 | 타임아웃은 명령만 실패시키고 연결은 그대로 둡니다 | 조용히 끊긴 연결: 명령마다 실패하는데 다시 연결하지 않습니다 |
| Redis의 `tcp-keepalive`가 켜져 있으니 죽은 연결은 감지된다 | 서버 쪽 설정이라 클라이언트가 죽은 서버를 알아내는 데는 쓰이지 않습니다. 놀고 있는 연결을 중간 장비가 지우는 일을 막는 데는 도움이 됩니다 | 타임아웃은 네 군데에 있고, 기본값은 서비스 기한보다 깁니다 |
| `BLPOP` 대기 시간은 명령 타임아웃과 무관하다 | 같은 명령 타임아웃이 적용되어, 더 길게 기다리면 클라이언트 쪽에서 실패합니다 | 기다리는 명령은 명령 타임아웃보다 짧게 기다려야 합니다 |

## 근거

**Spring Boot**(태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16), 커밋 `0566f6933049aca6bc5ffc6d559fffade9cd2e0c`. `spring-boot-dependencies`가 Spring Data BOM 2025.0.13과 Lettuce 6.6.0.RELEASE를 지정하고, Spring Data BOM 2025.0.13이 Spring Data Redis 3.5.13을 지정)

- [`RedisProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisProperties.java): 접두사 `spring.data.redis`, `timeout`("Read timeout", 기본 없음), `connectTimeout`(기본 없음), `Pool`의 `enabled`("Enabled automatically if commons-pool2 is available"), `maxIdle` 8, `minIdle` 0, `maxActive` 8, `maxWait` -1("Use a negative value to block indefinitely"), `Lettuce.shutdownTimeout` 100ms
- [`RedisConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisConnectionConfiguration.java): `isPoolEnabled`(설정이 없으면 `commons-pool2` 존재 여부)
- [`LettuceConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/LettuceConnectionConfiguration.java): `applyProperties`(`timeout`이 있을 때만 `commandTimeout`), `createClientOptions`(`connectTimeout`을 `SocketOptions`로, `TimeoutOptions.enabled()`, 그 뒤 `LettuceClientOptionsBuilderCustomizer` 적용), `PoolBuilderFactory`

**Spring Data Redis**(태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13))

- [`LettuceConnectionFactory.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionFactory.java): 클래스 설명("a shared connection will be used for regular operations and a LettuceConnectionProvider will be used to select a connection for blocking and tx operations only"), `shareNativeConnection = true`, `createConnectionProvider`(풀 설정이면 `LettucePoolingConnectionProvider`), `SharedConnection.getNativeConnection`(공유 연결도 연결 공급자에게서 빌림), RedisURI에 `commandTimeout` 적용
- [`LettuceConnection.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnection.java): `getAsyncConnection`(트랜잭션·파이프라인이면 전용 연결), 구독은 Pub/Sub 전용 연결(`switchToPubSub`), `await`(`LettuceFutures.awaitOrCancel`에 명령 타임아웃), `closePipeline`(파이프라인 예외를 `RedisPipelineException`으로 감쌈)
- [`core/TimeoutUtils.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/TimeoutUtils.java), [`core/DefaultListOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/DefaultListOperations.java): `leftPop(key, timeout, unit)`이 대기를 초로 바꾸고 1초 미만 양수는 1초로 올림(`toSeconds`, `roundUpIfNecessary`)
- [`listener/RedisMessageListenerContainer.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/listener/RedisMessageListenerContainer.java): 여러 리스너의 구독을 연결 하나로 다중화(클래스 설명)
- [`LettuceConnectionProvider.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionProvider.java): `release`·`releaseAsync` 기본은 연결 닫기
- [`LettuceClientConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClientConfiguration.java): 명령 타임아웃 기본값 `RedisURI.DEFAULT_TIMEOUT`
- [`LettuceExceptionConverter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceExceptionConverter.java): `RedisCommandTimeoutException` → `QueryTimeoutException`, 연결 예외 → `RedisConnectionFailureException`

**Spring Framework**(태그 [`v6.2.19`](https://github.com/spring-projects/spring-framework/tree/v6.2.19). Spring Boot 3.5.16의 `gradle.properties`가 `springFrameworkVersion=6.2.19`를 지정)

- [`DataSourceTransactionManager.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-jdbc/src/main/java/org/springframework/jdbc/datasource/DataSourceTransactionManager.java): `doBegin`에서 DB 연결을 얻고 `doCleanupAfterCompletion`에서 트랜잭션이 끝난 뒤 돌려줌(트랜잭션이 잡은 DB 연결은 커밋이나 롤백까지 풀로 돌아가지 않음)
- [`JpaTransactionManager.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-orm/src/main/java/org/springframework/orm/jpa/JpaTransactionManager.java): `doCleanupAfterCompletion`에서 JDBC 연결의 반환을 `JpaDialect`에 맡김(`DefaultJpaDialect`는 아무것도 하지 않고 JPA 구현에 맡김). JPA에서 연결을 얻고 돌려주는 정확한 시점은 이 문서에서 확인하지 않음

**Lettuce**(태그 [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE))

- [`RedisURI.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisURI.java): `DEFAULT_TIMEOUT = 60`(초)
- [`SocketOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/SocketOptions.java): `DEFAULT_CONNECT_TIMEOUT = 10`(초), `DEFAULT_SO_KEEPALIVE = false`, `DEFAULT_TCP_USER_TIMEOUT_ENABLED = false`, `KeepAliveOptions`(간격 설정이 적용되는 전송 방식), `TcpUserTimeoutOptions`("only supported with epoll and io_uring native transports")
- [`ClientOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ClientOptions.java): `DEFAULT_AUTO_RECONNECT = true`, `DEFAULT_REPLAY_FILTER`(아무것도 거르지 않음), `DEFAULT_DISCONNECTED_BEHAVIOR = DEFAULT`, `DisconnectedBehavior`("Accept commands when auto-reconnect is enabled"), `DEFAULT_REQUEST_QUEUE_SIZE = Integer.MAX_VALUE`
- [`TimeoutOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/TimeoutOptions.java): `enabled()`는 명령 만료를 켜고 연결의 타임아웃을 적용
- [`protocol/CommandExpiryWriter.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/CommandExpiryWriter.java): 클래스 설명("Command timeout starts at the time the command is written"), `potentiallyExpire`(연결 타임아웃을 모든 명령에 적용, 시간이 되면 명령만 예외로 완료하고 버퍼에서 빼지는 않음)
- [`protocol/DefaultEndpoint.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/DefaultEndpoint.java): 자동 재연결이면 `Reliability.AT_LEAST_ONCE`, `notifyDrainQueuedCommands`(응답 못 받은 명령을 끊긴 동안의 버퍼로, 거절 분기는 at-most-once일 때만), `notifyChannelActive`·`flushCommands`·`drainCommands`(버퍼를 비울 때 끝난 명령을 걸러 보내지 않음), `replayFilter`는 끊긴 동안의 버퍼에 넣을 때(`writeToDisconnectedBuffer`)와 쓰기 실패 뒤 다시 넣을 때(`potentiallyRequeueCommands`)만 검사하고 `notifyDrainQueuedCommands`에서는 검사하지 않음
- [`protocol/ConnectionWatchdog.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/ConnectionWatchdog.java): `channelInactive`에서 재연결 예약(`scheduleReconnect`)
- [`protocol/CommandHandler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/CommandHandler.java): 보낸 명령을 담는 `stack`, 응답을 맨 앞 명령과 짝지음(`decode`에서 `stack.peek()` 후 `complete`. 타임아웃으로 이미 끝난 명령도 `stack`에 남아 늦은 응답과 짝지어짐), `channelInactive`에서 `notifyDrainQueuedCommands` 호출
- [`resource/EpollProvider.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/resource/EpollProvider.java), [`pom.xml`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/pom.xml): epoll 전송은 `netty-transport-native-epoll`이 있을 때만 사용, 이 의존성은 선택(optional)

**Redis**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10))

- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `timeout` 0, `tcp-keepalive` 300, `maxclients` 10000
- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): `tcp-keepalive` 설명(죽은 상대 감지, 중간 장비의 연결 유지)
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `processInputBuffer`(한 연결의 명령을 차례로 실행)

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`)

- [clients](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/reference/clients.md): "Client Timeouts"(Pub/Sub 클라이언트에는 적용 안 됨, 정확하지 않음), "TCP keepalive"

## 실무 포인트

### 설정 점검 표

| 중요도 | 항목 | 이유와 선택 |
| --- | --- | --- |
| 필수 | `spring.data.redis.timeout`을 명시한다 | 적지 않으면 60초입니다. 클라이언트에서 잰 Redis 응답의 꼬리를 하한으로, "Redis에 내줄 요청 스레드 수 ÷ 초당 요청 수"와 바깥 기한을 상한으로 정합니다. |
| 필수 | 풀을 쓰면 `max-wait`에 상한을 둔다 | 기본 -1은 무한정 기다립니다. 명령 타임아웃은 풀 앞의 기다림을 막지 못합니다. 풀을 쓰는지 모르겠다면 `commons-pool2`가 클래스패스에 있는지부터 봅니다. |
| 필수 | 타임아웃 뒤 다시 나갈 수 있는 쓰기를 멱등하게 만든다 | 앱이 재시도하지 않아도 Lettuce가 다시 연결한 뒤 응답 못 받은 명령을 다시 보냅니다. |
| 필수 | 기다리는 명령의 대기 시간을 명령 타임아웃보다 짧게 두거나, 연결 구성을 분리한다 | 같은 명령 타임아웃이 적용되어 정상 대기도 실패합니다. `StreamMessageListenerContainer`의 기본 2초 대기도 해당합니다. |
| 알면 유리 | `spring.data.redis.connect-timeout`을 명시한다 | 기본 10초입니다. 다시 연결하는 동안 명령은 버퍼에서 명령 타임아웃까지 기다리므로, 연결 맺기가 길면 그만큼 실패로 끝나는 명령이 늘어납니다. |
| 알면 유리 | 조용히 끊긴 연결을 감지하는 설정을 검토한다 | 기본값에서는 운영체제가 끊을 때까지(Linux 기본으로 15분 이상) 공유 연결로 나가는 일반 명령이 모두 실패할 수 있습니다. 전송 방식 조건과 `connect-timeout`이 덮어써지는 문제를 함께 확인합니다. |
| 알면 유리 | `spring.data.redis.client-name`을 정한다 | 서버의 `CLIENT LIST`와 SLOWLOG에 연결 이름이 남아, 긴 명령을 보낸 앱을 찾기 쉽습니다. |
| 참고 | 공유 연결을 끌지 | 대부분은 기본값(공유)이 낫습니다. 큰 응답을 피할 수 없는 명령이 섞여 있을 때만 검토합니다. |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 명령 타임아웃, 연결 타임아웃, 풀의 최대치와 `max-wait` | 클라이언트에서 잰 Redis 명령 응답 시간의 p99·p99.9 |
| 기다리는 명령의 대기 시간, 작업자용 연결 구성의 분리 | `QueryTimeoutException`과 `RedisConnectionFailureException`의 발생 수와 시각 |
| 쓰기의 멱등성, 자동 재연결 같은 재전송 설정 | Lettuce의 재연결 로그, 서버의 `INFO clients`의 `connected_clients` |
| keepalive와 TCP user timeout | 요청 스레드 풀의 사용 중인 수, 풀이 있다면 빌리기 대기 시간 |

### 증상과 확인 순서

**증상 1**: "Redis가 잠깐 느려졌을 뿐인데 주문 API 전체가 응답하지 않았다."

1. 요청 스레드 풀이 최대치에 붙었는지 봅니다. 붙었다면 무언가를 기다리느라 스레드가 묶인 것이지만, 아직 Redis라고 단정할 수는 없습니다.
2. 스레드 덤프에서 스레드들이 무엇을 기다리는지 봅니다. 대부분이 Lettuce의 결과 대기라면 명령 타임아웃, 풀의 빌리기 대기라면 `max-wait`를 봅니다. DB나 외부 API를 기다리고 있다면 Redis 밖을 먼저 의심하되, 덤프가 보여 주는 것은 지금 기다리는 대상이지 처음 느려진 원인이 아닙니다. DB 연결 풀에서 연결 빌리기를 기다리고 있다면, 같은 덤프에서 연결을 쥔 스레드가 무엇을 기다리는지 이어서 봅니다. DB 잠금을 기다리고 있다면 잠금을 쥔 쪽은 덤프가 아니라 DB의 잠금 대기·보유 세션 정보로 찾습니다. 다른 앱의 트랜잭션일 수도 있습니다. 연결이나 잠금을 쥔 스레드가 트랜잭션 안에서 Redis 응답을 기다리고 있다면, Redis 호출의 대기가 DB 자원을 붙잡고 있는 것입니다("명령 타임아웃 값은 무엇을 기준으로 정하는가"). Redis 호출 경로의 어느 구간이 늦었는지는 아래 4·5단계로 좁힙니다.
3. 적용된 실제 값을 확인합니다. 설정 파일에 없으면 60초와 무한정입니다.
4. 같은 시각의 SLOWLOG로 메인 스레드를 붙잡은 명령을 찾습니다([Redis 이벤트 루프](./event-loop.md)의 "증상과 확인 순서").
5. SLOWLOG에 그 시각의 긴 명령이 없다면, 먼저 기록 기준 시간과 보관 개수 때문에 그 시각의 기록이 빠지거나 밀려나지 않았는지 확인합니다. 기록이 제대로 남는데도 없다면 긴 명령 하나가 메인 스레드를 붙잡은 경우는 아닐 가능성이 큽니다. SLOWLOG는 명령의 실행 시간만 재므로, 같은 공유 연결로 큰 응답을 받는 명령이 있었는지("공유 연결의 대가: 앞 응답이 늦으면 뒤 응답도 늦습니다"), 그다음 네트워크 경로를 봅니다.

**대응과 부작용**: 명령 타임아웃을 줄이면 그 멈춤 동안 Redis를 쓰는 요청이 실패로 바뀝니다. 실패를 받는 쪽(캐시라면 DB로 우회, 재고라면 주문 거절)을 함께 정해 둡니다. 용도마다 이를 정하는 기준은 Redis 장애 시 우회에서 다룹니다.

**증상 2**: "Redis 재시작 뒤 재고가 주문 수보다 많이 줄었다."

1. 재시작 시각에 `QueryTimeoutException`이나 재연결 로그가 있는지 봅니다.
2. 그 시각 전후의 차감 요청과 주문 기록을 맞춰 봅니다. 주문 하나에 차감이 두 번이면 응답 유실 뒤의 재전송을 의심합니다. 사용자의 중복 요청이나 업무 로직의 중복 호출도 같은 모양이므로 요청 로그로 구분합니다.
3. 앱에 재시도 코드가 없고 중복 요청도 아니라면 Lettuce의 재전송을 의심합니다.

**대응과 부작용**: 차감을 주문 ID 기록과 함께 스크립트로 묶습니다. 기록 키가 늘어나므로 만료 시간을 정합니다. `autoReconnect` false로 재전송을 막는 방법도 있지만, 끊긴 연결을 되살리는 일이 앱으로 오고 앱의 재시도로 생기는 중복은 남습니다. `replayFilter`는 응답을 기다리던 명령의 재전송을 막지 못합니다.

**증상 3**: "Redis는 정상인데 특정 인스턴스만 수십 분 동안 Redis 타임아웃을 냈고, 재시작하니 풀렸다."

1. 그 인스턴스에서 `QueryTimeoutException`만 나고 `RedisConnectionFailureException`이나 재연결 로그가 없는지 봅니다. 그렇다면 조용히 죽은 연결에 명령을 계속 보냈을 가능성이 큽니다. Lettuce의 재연결 로그가 기록되는 로그 수준인지도 확인합니다.
2. 장애가 이어지는 동안 서버의 `CLIENT LIST`에서 그 인스턴스의 연결을 찾습니다. `addr`는 서버가 본 상대 주소라 NAT나 프록시를 거치면 인스턴스의 주소와 다를 수 있고, `name`은 `client-name` 설정값이라 같은 앱의 인스턴스끼리 겹칠 수 있습니다. 두 값으로 대상 연결을 정확히 가려냈는데도 없다면 서버는 이미 연결을 잊었는데 클라이언트만 기억하는 상태일 가능성이 큽니다.
3. 그 시각에 네트워크 장비 교체, 방화벽의 유휴 연결 정리, 장애 조치가 있었는지 봅니다.

**대응과 부작용**: TCP user timeout이나 keepalive 간격을 켭니다. 너무 짧으면 일시적 지연에도 공유 연결이 끊겨 그 인스턴스의 일반 명령이 잠시 멈춥니다.

## 말로 설명하기

### 30초 버전

Spring Boot의 Lettuce 구성은 일반 명령을 연결 하나로 모든 스레드가 나눠 쓰고, 기다리는 명령과 트랜잭션, 파이프라인만 전용 연결을 씁니다. 풀을 켜도 일반 명령은 공유 연결 하나로 나가므로, 풀은 사실상 전용 연결을 위한 것입니다. 명령 타임아웃 기본값은 60초라, Redis가 멈추면 요청 스레드가 금방 바닥납니다. 그래서 클라이언트에서 잰 응답의 꼬리보다는 길고, Redis에 내줄 스레드와 요청 기한으로 계산한 상한보다는 짧게 정합니다. 타임아웃은 명령을 취소하지 않고, Lettuce는 다시 연결되면 응답 못 받은 명령을 스스로 다시 보내므로, 쓰기는 멱등하게 만들어야 합니다.

### 2분 버전

Redis는 한 연결의 명령을 받은 순서대로 실행해 같은 순서로 응답합니다. Lettuce는 이를 이용해 응답을 기다리지 않고 명령을 이어 보내고, 응답을 순서대로 짝짓습니다. 그래서 Spring Data Redis는 기본으로 연결 하나를 모든 요청 스레드가 공유합니다. 서버도 명령을 한 줄로 실행하므로 연결을 늘려도 서버의 실행 병렬성은 늘지 않습니다. 기다리는 명령, 트랜잭션, 파이프라인, 구독만 전용 연결을 씁니다. `commons-pool2`가 있으면 Spring Boot가 풀을 켜는데, 공유 연결이 그중 하나를 계속 차지하고 나머지가 전용 연결에 쓰입니다. 풀의 `max-wait` 기본값은 무한정이라 상한을 둬야 합니다.

명령 타임아웃은 기본 60초입니다. 초당 500건을 받는 서버에서 Redis가 800ms 멈추면, 묶이는 스레드는 500 × 0.4초 만에 200개에 닿습니다. 100ms로 두면 50개 정도에서 멈춥니다. 하한은 클라이언트에서 잰 꼬리 응답 시간이고, 상한은 Redis에 내줄 스레드 수 ÷ 초당 요청 수와 요청 기한입니다. 이 타임아웃은 기다리는 명령에도 적용되므로 `BLPOP`이나 스트림 읽기의 대기는 그보다 짧게 두거나 연결 구성을 따로 둡니다.

타임아웃이 난 명령은 실행됐을 수도, 서버의 줄에서 나중에 실행될 수도 있습니다. 또 Lettuce는 기본으로 연결이 끊기면 응답 못 받은 명령을 버퍼에 옮겼다가 다시 연결되면 다시 보냅니다. 서버가 이미 실행한 `DECR`이라면 두 번 반영됩니다. 그래서 쓰기는 주문 ID 기록과 함께 스크립트로 묶어 멱등하게 만듭니다. 마지막으로, 연결이 조용히 끊기면 명령 타임아웃은 명령만 실패시키고 연결은 그대로 두므로, 운영체제가 끊을 때까지 실패가 이어집니다. TCP user timeout이나 keepalive로 감지 시간을 줄입니다.

### 예상 질문과 꼬리 질문

**Q. 요청 스레드가 200개인데 Redis 연결이 하나면 병목 아닌가요?**
Lettuce는 응답을 기다리며 연결을 막지 않습니다. Redis가 한 연결의 응답을 보낸 순서대로 돌려주므로, 여러 스레드의 명령을 이어 보내고 응답을 순서로 짝짓습니다. 서버 쪽도 메인 스레드 하나가 명령을 차례로 실행하므로, 연결을 늘려도 서버가 동시에 실행하는 명령 수는 늘지 않습니다. (본문: "기본 구성: 연결 하나를 모든 요청 스레드가 나눠 씁니다")

- **꼬리: 그럼 연결 하나의 단점은 없나요?**
  있습니다. 응답을 순서로 짝지으므로, 그 연결 위의 큰 응답 하나를 다 받을 때까지 뒤의 응답은 서버에서 이미 만들어졌어도 받을 수 없습니다. 다만 근본 원인은 큰 응답을 만드는 명령이고, 그 명령은 서버의 메인 스레드도 붙잡으므로 연결을 늘리기보다 명령을 나눠 읽기로 바꾸는 것이 먼저입니다.
- **꼬리: 공유 연결이 끊기면 모든 요청이 영향을 받지 않나요?**
  받습니다. 그래서 끊긴 것을 빨리 알아채고 다시 맺는 것이 중요합니다. 끊는다는 패킷 없이 사라지면 기본 설정으로는 운영체제가 판단할 때까지 오래 걸리므로, TCP user timeout이나 keepalive 간격으로 감지 시간을 줄입니다.

**Q. 연결 풀을 넣으면 성능이 좋아지나요?**
기본 구성에서는 일반 명령에 영향이 없습니다. 공유 연결이 켜져 있으면 공유 연결이 풀에서 하나를 빌려 계속 쓰고, 일반 명령은 모두 그 하나로 나갑니다. 풀의 나머지는 기다리는 명령, 트랜잭션, 파이프라인이 빌려 씁니다. 풀이 없으면 이 전용 연결을 쓸 때마다 새로 맺고 닫으므로, 이런 작업이 잦다면 풀이 연결 맺기 비용을 줄여 줍니다. (본문: "연결 풀은 언제 필요하고, 무엇을 대가로 치르는가")

- **꼬리: 풀을 쓰면 무엇을 조심해야 하나요?**
  Spring Boot의 `max-wait` 기본값 -1은 풀이 바닥났을 때 무한정 기다립니다. 명령 타임아웃은 명령을 Lettuce에 맡긴 뒤부터 세는데, 풀에서 연결을 빌리는 일은 그보다 앞이라 풀 앞의 기다림을 막지 못합니다. 상한을 두고, `max-active`는 공유 연결 하나와 동시에 필요한 전용 연결 수로 정합니다. 인스턴스 수를 곱한 연결 수가 서버의 `maxclients` 안에 드는지도 봅니다.
- **꼬리: 우리는 풀 설정을 안 했는데 풀을 쓰고 있을 수도 있나요?**
  네. `pool.enabled`를 적지 않으면 Spring Boot는 `commons-pool2`가 클래스패스에 있을 때 풀을 켭니다. 다른 의존성이 끌어온 경우에도 그렇습니다.

**Q. 명령 타임아웃은 얼마로 정하나요?**
두 경계 사이에서 고릅니다. 하한은 클라이언트에서 잰 Redis 응답의 꼬리(예시 p99.9 5ms)이고, 상한은 Redis가 멈췄을 때 묶여도 되는 요청 스레드 수 ÷ 초당 요청 수(예시 50 ÷ 500 = 0.1초)와 요청 기한입니다. 기본값 60초는 대부분의 웹 요청 기한보다 깁니다. (본문: "명령 타임아웃 값은 무엇을 기준으로 정하는가")

- **꼬리: 하한을 SLOWLOG로 정하면 안 되나요?**
  안 됩니다. SLOWLOG는 명령의 실행 시간만 잽니다. 다른 명령을 기다린 시간, 네트워크, 공유 연결에서 앞 응답을 기다린 시간이 빠지므로 클라이언트가 실제로 기다리는 시간보다 짧습니다.
- **꼬리: 800ms 멈춤이 자주 있다면 타임아웃을 1초로 늘려야 하지 않나요?**
  1초로 늘리면 그 멈춤 동안 스레드를 하나도 지키지 못합니다. 타임아웃은 멈춤보다 짧을 때만 스레드를 지킵니다. 정상 요청의 실패가 문제라면 타임아웃을 늘리기 전에 800ms를 만드는 긴 명령을 찾아 바꿉니다.
- **꼬리: DB 트랜잭션 안에서 Redis를 부르면 무엇이 달라지나요?**
  Redis를 기다리는 동안 트랜잭션이 잡은 DB 연결도 커밋이나 롤백까지 돌아가지 않습니다. Redis가 멈춘 동안 Redis를 기다리며 쥐고 있는 DB 연결이 풀 크기에 닿으면 Redis와 무관한 DB 요청도 멈춥니다. DB 연결 풀이 요청 스레드보다 작다면 같은 유입에서 DB 연결 쪽이 먼저 이 한계에 닿습니다. 스레드 덤프에서는 DB 연결을 기다리는 것으로 보이므로, 연결을 쥔 스레드가 무엇을 기다리는지까지 봐야 합니다. Redis 호출을 트랜잭션 밖으로 빼고, 뺄 수 없다면 Redis에 내줘도 되는 DB 연결 수로도 상한을 정합니다.

**Q. 타임아웃을 짧게 했더니 `BLPOP`이 계속 실패합니다. 왜인가요?**
Spring Boot가 켠 Lettuce의 명령 만료는 기다리는 명령에도 같은 명령 타임아웃을 적용하고, Spring Data Redis의 동기 API도 같은 값만큼만 기다립니다. `BLPOP`이 5초를 기다려도 100ms 뒤 클라이언트가 실패로 끝냅니다. 대기 시간을 타임아웃보다 짧게 줄이거나, 작업자에게 타임아웃을 길게 잡은 연결 구성을 따로 줍니다. (본문: "기다리는 명령은 명령 타임아웃보다 짧게 기다려야 합니다")

- **꼬리: 대기를 짧게 줄이면 무엇을 잃나요?**
  빈 대기열에도 요청이 자주 오가 Redis 명령 수가 늘어납니다. 연결 구성을 나누면 명령 수는 그대로지만 설정과 관찰 대상이 둘이 됩니다.

**Q. 타임아웃이 났으니 재고 차감은 안 된 거죠?**
알 수 없습니다. Lettuce의 타임아웃은 클라이언트 쪽에서 명령을 예외로 끝낼 뿐 서버에 아무것도 보내지 않습니다. 이미 실행됐을 수도, 긴 명령 뒤에서 기다리다 나중에 실행될 수도 있습니다. 그래서 차감을 다시 보내려면 주문 ID 기록과 함께 묶어 멱등하게 만듭니다. (본문: "타임아웃이 난 명령은 서버에서 어떻게 되었는가")

- **꼬리: 그럼 재시도만 안 하면 중복은 없나요?**
  아닙니다. Lettuce는 기본으로 자동 재연결을 켜고 "최소 한 번" 방식으로 동작합니다. 연결이 끊기면 응답 못 받은 명령을 버퍼에 옮겼다가, 다시 연결되면 아직 끝나지 않은 명령을 다시 보냅니다. 서버가 이미 실행한 `DECR`이 끊기기 직전이었다면 두 번 반영됩니다. (본문: "연결이 끊기면 Lettuce가 응답 못 받은 명령을 다시 보냅니다")
- **꼬리: 타임아웃을 짧게 하면 재전송이 안 되지 않나요?**
  다시 연결되기 전에 타임아웃이 난 명령은 다시 보내지 않으므로 줄어듭니다. 하지만 다시 연결이 타임아웃보다 빠르면 재전송은 일어납니다. 타임아웃 길이로 중복을 막을 수는 없습니다.
- **꼬리: 재전송을 아예 끄면 안 되나요?**
  `autoReconnect`를 끄면 재전송이 없어지지만, 끊긴 연결을 되살리는 일이 앱으로 옵니다. `replayFilter`는 끊긴 동안 새로 들어온 명령에는 적용되지만, 이미 보내고 응답을 기다리던 명령의 재전송은 막지 못합니다. 앱 쪽 재시도의 중복도 남습니다. 멱등한 쓰기는 이 모든 경로를 한 번에 막습니다.

**Q. Redis는 정상인데 한 인스턴스만 계속 Redis 타임아웃이 납니다. 무엇을 의심하나요?**
조용히 끊긴 공유 연결입니다. 중간 장비가 연결을 지우면 끊는다는 패킷이 오지 않아 클라이언트는 연결이 살아 있다고 믿습니다. 명령 타임아웃은 명령만 실패시키고 연결을 끊지 않으므로, 운영체제가 재전송을 포기할 때까지(Linux 기본값에서 15분 넘게) 같은 연결로 계속 보내고 실패합니다. 연결 실패 예외나 재연결 로그 없이 타임아웃만 나고, 서버의 `CLIENT LIST`에서 그 인스턴스의 연결을 가려냈는데 없다면 이 경우일 가능성이 큽니다. (본문: "조용히 끊긴 연결: 명령마다 실패하는데 다시 연결하지 않습니다")

- **꼬리: Redis 서버의 `tcp-keepalive`가 300초로 켜져 있지 않나요?**
  그것은 서버가 죽은 클라이언트를 알아내는 설정입니다. 주기적으로 패킷을 보내 중간 장비가 놀고 있는 연결을 지우는 일을 막는 데는 도움이 되지만, 이미 끊긴 것을 클라이언트에 알려 주지는 않습니다. 클라이언트가 죽은 서버나 끊긴 경로를 알아내려면 클라이언트 쪽 소켓 설정이 필요한데, Lettuce 6.6.0은 keepalive와 TCP user timeout이 모두 기본으로 꺼져 있습니다.
- **꼬리: TCP user timeout을 켜면 끝인가요?**
  조건이 있습니다. Lettuce에서는 epoll·io_uring 네이티브 전송에서만 적용되고, epoll은 Netty의 네이티브 의존성이 클래스패스에 있고 운영체제에 맞아야 쓰입니다. 또 Spring Boot에서 커스터마이저로 `SocketOptions`를 새로 넣으면 `connect-timeout` 설정이 사라지므로 함께 넣어야 합니다. 감지 시간을 너무 짧게 잡으면 일시적 지연에도 공유 연결이 끊겨 그 인스턴스의 모든 Redis 요청이 잠시 멈춥니다.

**Q. 트래픽이 두 배가 되면 이 설정을 다시 봐야 하나요?**
봐야 합니다. Redis가 멈췄을 때 묶이는 스레드는 고르게 들어온다면 약 "초당 요청 수 × 타임아웃"이라, 같은 타임아웃이 두 배의 스레드를 묶습니다. 인스턴스를 늘렸다면 서버 연결 수도 곱해집니다. 또 끊긴 동안의 버퍼에는 상한이 없어, 아직 끝나지 않은 명령이 다시 연결되는 순간 막 재시작한 Redis에 한꺼번에 나갑니다. 동기 API라면 그 수는 대략 요청 스레드 수까지이고, 비동기 API는 그 제한이 없습니다. (본문: "데이터와 요청이 늘면 무엇이 달라지는가")
