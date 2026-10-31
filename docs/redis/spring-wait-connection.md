# Spring에서 쓰기와 WAIT을 같은 연결로 보내기: 어느 호출 경로가 같은 소켓을 보장하는가

> **핵심 질문**: Spring Data Redis에서 쓰기 뒤에 `WAIT`을 붙이면, 두 명령이 Redis 서버의 같은 소켓 연결로 나가는가? 어떤 호출 경로와 연결 공유 설정에서 같은 소켓이 보장되고, 어디서 어긋나는가? `WAIT`이 요구한 수보다 작은 값을 돌려주면 앱은 무엇을 해야 하는가?
>
> **범위**: Spring Boot + Spring Data Redis + Lettuce로 주 서버 하나(복제본이 붙은 단독 구성, 또는 `readFrom` 없이 쓰는 Sentinel 구성)에 연결하는 경우를 다룹니다. Redis Cluster 연결과 `readFrom`으로 복제본 읽기를 켠 연결은 주 서버로 가는 소켓을 고르는 방식이 달라 다루지 않습니다. `WAIT`이 무엇을 세는지와 그 보장의 한계는 [Redis 복제](./replication.md)의 "WAIT은 기다려서, min-replicas는 거부해서 잃는 구간을 줄입니다"를 전제로 하고, 여기서는 연결을 고르는 문제만 다룹니다.

## 배경 지식

### 이 문서의 예시: 주문 C의 `DECR`과 `WAIT 1 100`

[Redis 복제](./replication.md)의 재고 장면을 그대로 이어 씁니다.

- 주 서버 M과 복제본 R이 있습니다. 주문 C가 `DECR stock:item:1`로 재고를 8에서 7로 줄였고, M은 실행한 쓰기를 하나의 바이트 흐름(복제 흐름)으로 순서대로 R에 보내고, R은 "이 흐름의 몇 번째 바이트까지 처리했다"(오프셋)를 M에 보고합니다. C의 `DECR`은 이 흐름에서 1099번째 바이트까지를 차지합니다. 그 순간 R은 1066까지만 처리했습니다(아직 재고 8). 흐름은 순서대로 처리되므로, R이 1099 이상을 보고하면 C의 `DECR`과 그 앞의 쓰기를 모두 처리했다는 뜻입니다. 복제 흐름과 오프셋은 [Redis 복제](./replication.md)의 "오프셋: 흐름의 몇 번째 바이트까지 처리했는가"에서 다룹니다.
- 주문 C를 처리하는 요청 스레드는 주문 확정이라 이 쓰기를 잃고 싶지 않습니다. 그래서 `DECR` 뒤에 `WAIT 1 100`, 곧 "복제본 1대가 이 쓰기를 처리했다고 보고할 때까지 기다리되, 기한은 100밀리초로 해 달라"를 보냅니다.
- 주문 API는 요청 스레드 200개로 동시에 요청을 처리합니다. 같은 순간 다른 요청 스레드 D는 `GET stock:item:1`로 재고를 읽고 있습니다.
- 연결 공유(`shareNativeConnection`)는 Spring Data Redis 기본값대로 켜져 있습니다. 연결 풀은 꺼져 있다고 합시다. Spring Boot는 풀 설정을 적지 않으면 클래스패스에 `commons-pool2`가 있을 때 풀을 켜므로, 이 예시는 그 라이브러리가 없는 경우입니다.

질문은 하나입니다. **C의 `DECR`과 `WAIT`이 M의 같은 소켓 연결에 도착하는가?** 도착하면 `WAIT`은 1099를 기준으로 셉니다. 다른 소켓에 도착하면 `WAIT`은 엉뚱한 위치를 기준으로 세며, 그 값을 믿은 앱은 확인되지 않은 쓰기를 확인됐다고 판단할 수 있습니다.

오프셋(1066, 1099)과 요청 수는 계산을 보여 주려고 정한 **가상의 숫자**입니다. 이 문서에서 실행해 잰 값은 없습니다.

### "연결"이라는 말이 가리키는 세 가지

이 문서에서 가장 잘못 읽기 쉬운 지점입니다. 코드에서 "연결"이라고 부르는 것이 세 층에 있고, `WAIT`이 기준으로 삼는 것은 맨 아래 하나뿐입니다.

| 층 | 무엇인가 | 얼마나 오래 사는가 |
| --- | --- | --- |
| Spring의 `RedisConnection`(Lettuce 연동에서는 `LettuceConnection`) | 명령을 어느 Lettuce 연결로 보낼지 고르는 가벼운 객체. 스레드 사이에 공유하지 않음 | `RedisTemplate` 호출 하나, 또는 콜백 하나가 끝날 때까지. `@Transactional`에 묶이면 그 트랜잭션이 끝날 때까지 |
| Lettuce 연결 객체(`StatefulRedisConnection`) | TCP 소켓 하나를 감싸고 명령 전송과 응답 짝짓기를 맡는 객체. 여러 스레드가 함께 쓸 수 있음 | 공유 연결은 앱이 닫을 때까지. 전용 연결은 돌려줄 때 닫히거나(풀 없음) 풀로 돌아가 풀 정책에 따라 유지됨. 살아 있는 동안 끊기면 **같은 객체가 새 소켓으로** 다시 연결함 |
| 서버 쪽 소켓 연결(Redis의 클라이언트 하나) | M에 열린 TCP 연결 하나. `CLIENT LIST`의 한 줄, `CLIENT ID`의 값 하나에 해당 | 그 TCP 연결이 끊길 때까지 |

- **한 줄**: Redis가 "이 연결"이라고 할 때는 언제나 세 번째, 서버에 열린 TCP 소켓 연결 하나입니다.
- **비유**: 콜센터 상담원(서버)은 걸려 온 통화(소켓 연결)마다 메모를 남깁니다. 앱 쪽의 `RedisConnection`은 "이번 용건은 어느 전화기로 걸까"를 정하는 비서이고, Lettuce 연결 객체는 전화기입니다. 같은 비서가 다른 전화기를 집으면 상담원은 다른 통화로 봅니다. 같은 전화기라도 한 번 끊고 다시 걸면 새 통화입니다.
- **실제로는**: 비유와 다른 점이 둘 있습니다. 첫째, Lettuce 연결 객체 하나(전화기 하나)를 여러 요청 스레드가 동시에 씁니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "일반 명령은 공유 연결 하나로 나갑니다"). 둘째, Lettuce는 끊긴 연결을 앱 모르게 다시 맺으므로, 앱이 보기에는 같은 연결 객체인데 서버가 보기에는 새 소켓 연결일 수 있습니다.
- **이 주제에서는**: `RedisConnection`이 같거나 `RedisTemplate`이 같다는 것은 같은 소켓을 뜻하지 않습니다. 아래에서 호출 경로마다 "결국 어느 Lettuce 연결 객체로 나가는가"를 따라가고, 마지막에 "그 객체의 소켓이 그사이 바뀌었는가"를 따로 봅니다.

### 서버는 소켓 연결마다 마지막 쓰기의 위치를 기억합니다

- **한 줄**: M은 소켓 연결마다 "이 연결이 마지막으로 실행한 쓰기가 복제 흐름의 몇 번째 바이트까지였는가"를 하나 기억하고, `WAIT`은 그 값 이상을 보고한 복제본 수를 셉니다.
- **비유**: 상담원이 통화마다 "이 통화에서 마지막으로 접수한 건의 접수 번호"를 적어 두고, "몇 개 지점이 그 번호까지 받았는가"를 세 주는 것과 같습니다.
- **실제로는**:
  - 값을 바꾸는 명령이 복제 흐름을 늘릴 때만 이 값이 갱신됩니다. `WAIT` 자체와 `GET` 같은 읽기는 보통 복제 흐름을 늘리지 않으므로 이 값을 바꾸지 않습니다. 그래서 같은 연결에서 `WAIT`을 두 번 보내면 두 번 다 같은 쓰기를 기준으로 셉니다.
  - **새로 맺은 소켓 연결은 이 값이 0입니다.** 복제본이 보고한 위치는 모두 0 이상이므로, 쓰기를 한 번도 하지 않은 연결에서 보낸 `WAIT`은 정상 연결된 복제본을 모두 "확인됨"으로 셉니다. 그 수가 요구한 수 이상이면 기다리지 않고 곧바로 돌려주며, 무엇도 확인하지 않은 채 "확인됐다"처럼 보이는 숫자가 나옵니다. 정상 연결된 복제본이 요구한 수보다 적을 때만 제한 시간까지 기다립니다.
  - 공유 연결처럼 여러 스레드의 쓰기가 한 소켓으로 나가면, 이 값은 "그 소켓에서 누군가 마지막으로 한 쓰기"입니다. 내 쓰기 뒤에 같은 소켓으로 다른 스레드의 쓰기가 실행됐다면 `WAIT`은 그 더 뒤의 위치까지 기다립니다. 내 쓰기도 그 안에 들어 있으므로 확인은 맞지만, 필요한 것보다 더 기다릴 수 있습니다.
- **이 주제에서는**: C의 `DECR`이 실행된 소켓에서는 이 값이 1099입니다. `WAIT`이 다른 소켓에 도착하면 그 소켓의 값(새 연결이면 0, 다른 용도로 쓰던 연결이면 예전 어느 쓰기의 위치)을 기준으로 셉니다. R이 1066까지만 처리했어도 그 기준이 1066 이하라면 `WAIT`은 기다리지 않고 1을 돌려줍니다.

### WAIT은 그 소켓 연결 하나를 멈춰 세웁니다

- **한 줄**: 지정한 수의 복제본이 아직 보고하지 않았으면, M은 `WAIT`을 보낸 소켓 연결을 "기다리는 중"으로 표시하고 그 연결에서 다음 명령을 꺼내 실행하지 않습니다.
- **비유**: 상담원이 한 통화를 보류 상태로 두고 다른 통화를 계속 받는 것과 같습니다. 보류된 통화에서 고객이 다음 용건을 말해도 보류가 풀릴 때까지 처리하지 않습니다.
- **실제로는**: M의 메인 스레드는 멈추지 않고 다른 소켓 연결의 명령을 계속 처리합니다. 멈추는 것은 그 소켓 하나입니다. 보고가 모이거나 제한 시간이 지나면 `WAIT`이 응답을 돌려주고, 그 소켓에 쌓여 있던 다음 명령부터 다시 처리합니다. `BLPOP` 같은 기다리는 명령과 같은 방식입니다([Redis 이벤트 루프](./event-loop.md)). 다만 `MULTI`로 묶은 트랜잭션이나 Lua 스크립트 안처럼 기다리는 것이 허용되지 않는 곳에서는 기다리지 않고, 그 순간까지 확인된 수를 바로 돌려줍니다.
- **이 주제에서는**: `WAIT`이 어느 소켓으로 나가느냐는 "무엇을 기준으로 세는가"뿐 아니라 "누가 함께 멈추는가"도 정합니다. 그 소켓을 다른 요청 스레드와 함께 쓰고 있다면 그 스레드들의 명령도 함께 멈춥니다.

### 공유 연결과 전용 연결

- **한 줄**: Spring Data Redis의 Lettuce 연동은 기본으로 Lettuce 연결 하나를 모든 요청 스레드가 함께 쓰고(공유 연결), 트랜잭션·파이프라인·기다리는 명령처럼 공유하면 안 되는 작업에만 그 작업 동안 쓰는 연결을 따로 꺼냅니다(전용 연결). 풀이 없으면 전용 연결은 쓸 때마다 새로 맺고 닫습니다.
- **트랜잭션과 파이프라인의 차이**: 둘 다 "명령 여러 개를 묶는다"로 읽히지만 서버에서 일어나는 일이 다릅니다. `MULTI`/`EXEC` 트랜잭션은 `MULTI` 뒤의 명령을 서버가 실행하지 않고 쌓아 두었다가 `EXEC`에서 한꺼번에 이어서 실행하며, 이 동안에는 기다리는 명령이 기다리지 못합니다. 파이프라인은 클라이언트가 응답을 기다리지 않고 명령을 연달아 보내는 전송 방식일 뿐이라, 서버는 명령을 하나씩 평소처럼 실행하고 `WAIT`도 평소처럼 기다립니다. Spring Data Redis는 두 경우 모두 다른 스레드의 명령이 끼어들지 않도록 전용 연결을 씁니다.
- **이 주제에서는**: `WAIT`이 공유 연결로 가면 같은 소켓을 쓰는 다른 요청을 함께 멈추고, 전용 연결로 가면 쓰기와 같은 전용 연결인지가 문제가 됩니다. 연결 구성과 풀의 대가는 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에서 다룹니다.

## Spring Data Redis는 명령의 성질이 아니라 호출 방식으로 연결을 고릅니다

`LettuceConnection`이 명령을 보낼 Lettuce 연결을 고르는 규칙은 다음 순서입니다.

1. **트랜잭션 중(`multi()` 뒤 `exec()` 전)이거나 파이프라인 중이면** 전용 연결로 보냅니다.
2. **`bLPop`처럼 기다리는 명령으로 정해 둔 API 메서드**는 공유 여부와 상관없이 전용 연결로 보냅니다.
3. 그 밖의 명령은 **공유가 켜져 있으면 공유 연결**, 꺼져 있으면 전용 연결로 보냅니다.

전용 연결은 `LettuceConnection` 하나 안에서 처음 필요할 때 한 번 꺼내고, 그 `LettuceConnection`이 닫힐 때까지 다시 씁니다. 닫힐 때 연결 공급자에게 돌려줍니다(풀이 있으면 풀로, 없으면 닫음). 그리고 `LettuceConnection`은 템플릿 호출 하나가 끝나면 닫힙니다. 콜백(`execute(SessionCallback)`, `execute(RedisCallback)`)을 쓰면 콜백이 끝날 때까지 같은 `LettuceConnection`을 씁니다.

**`WAIT`은 2번에 들어가지 않습니다.** Spring Data Redis에는 `WAIT`을 보내는 전용 API가 없습니다. 그래서 `RedisConnection.execute("WAIT", ...)`로 원시 명령을 보내거나, `getNativeConnection()`으로 얻은 Lettuce 명령 객체의 `waitForReplication`을 부릅니다. 둘 다 3번 규칙을 따릅니다. 곧 **`WAIT`이 서버에서 연결을 멈추는 명령이라는 사실은 연결 선택에 아무 영향을 주지 않습니다.** 공유가 켜진 기본 구성에서 `WAIT`은 공유 연결로 나갑니다.

`RedisConnection.execute("WAIT", ...)`에는 따로 걸리는 점이 하나 있습니다. 이 메서드는 `WAIT`의 정수 응답을 바이트 응답으로 받으려다 그 명령을 예외로 끝냅니다. 그래서 서버에서는 `WAIT`이 정상 실행됐는데 앱에는 예외가 올라올 것으로 보입니다. 이 결론은 소스의 해석 경로를 따라가 얻은 것이며 **실행해 확인하지는 않았습니다**. 반환값이 필요하므로 정수로 해석하는 경로, 곧 Lettuce의 `waitForReplication`을 쓰는 편이 안전합니다. `getNativeConnection()`이 돌려주는 것은 비동기 명령 객체라 이 메서드는 숫자가 아니라 나중에 숫자가 채워지는 `RedisFuture<Long>`을 돌려줍니다. Spring은 이 결과를 대신 기다려 주지 않으므로, 결과를 받을 때까지 콜백 안에서 기다려야 합니다. 콜백이 먼저 끝나면 연결이 돌려진 뒤에 `WAIT`의 응답이 오게 됩니다.

## 호출 경로마다 쓰기와 WAIT은 같은 소켓으로 가는가

예시의 C가 `DECR`과 `WAIT`을 보내는 방법을 다섯 가지로 나눠, 공유 설정에 따라 두 명령이 어느 Lettuce 연결로 가는지 정리합니다. "같은 소켓"은 그사이 다시 연결되지 않았다는 전제입니다(이 전제가 깨지는 경우는 "연결이 다시 맺어지면 확인이 거짓이 됩니다"에서 다룹니다).

| 호출 경로 | 공유 켬(기본) | 공유 끔 |
| --- | --- | --- |
| ① 템플릿 호출 두 번: `opsForValue().decrement(...)` 뒤 따로 `execute { WAIT }` | 둘 다 공유 연결. **같은 소켓**이지만 `WAIT`이 공유 연결을 멈춤 | 호출마다 전용 연결을 따로 꺼냄. **다른 소켓일 수 있음** |
| ② 콜백 하나 안에서 쓰기와 `WAIT`(`execute(SessionCallback)` 또는 `execute(RedisCallback)`) | 둘 다 공유 연결. **같은 소켓**이지만 공유 연결을 멈춤 | 둘 다 그 콜백의 전용 연결. **같은 소켓**, 다른 요청에 영향 없음 |
| ③ 콜백 안에서 `multi()`·쓰기·`exec()` 뒤에 `WAIT` | 트랜잭션은 전용 연결, `exec()` 뒤 `WAIT`은 공유 연결. **다른 소켓** | 둘 다 그 콜백의 전용 연결. **같은 소켓** |
| ④ `multi()`와 `exec()` 사이에 `WAIT` | 같은 소켓이지만 **기다리지 않음** | 같은 소켓이지만 **기다리지 않음** |
| ⑤ `executePipelined` 안에서 쓰기와 `WAIT` | 둘 다 전용 연결. **같은 소켓**, 다른 요청에 영향 없음 | 둘 다 전용 연결. **같은 소켓**, 다른 요청에 영향 없음 |

### 공유를 끄면 따로 부른 WAIT이 거짓 확인을 돌려줍니다

공유를 끄고 풀도 없으면, ①에서 `DECR`은 새로 맺은 연결 X로 나가고 X는 호출이 끝나며 닫힙니다. `WAIT`은 또 새로 맺은 연결 Y로 나갑니다. Y는 쓰기를 한 적이 없어 기준이 0입니다. R이 정상 연결돼 있으면 1066까지만 처리했어도 1066 ≥ 0이므로 M은 기다리지 않고 1을 돌려줍니다. C의 `DECR`(1099)은 R에 없는데 앱은 "복제본 1대 확인"으로 읽습니다.

풀을 켜도 같습니다. 어느 연결을 빌려줄지는 풀이 정하므로, `WAIT`은 예전에 다른 용도로 쓰던 연결로 갈 수 있고, 그 연결의 기준은 예전 어느 쓰기의 위치입니다. 반대로 방금 돌려준 연결을 다시 빌리면 우연히 같은 소켓으로 가므로, 시험에서 맞게 동작했다는 것만으로 같은 소켓이 보장된다고 볼 수 없습니다. 어느 쪽이 얼마나 자주 일어나는지는 풀 설정과 동시 요청 수에 달려 있습니다.

### 트랜잭션 뒤의 WAIT은 공유 연결로 빠져나갑니다

③은 겉으로 안전해 보여 특히 헷갈립니다. 콜백 하나 안에서 같은 `RedisConnection`을 썼는데도, `exec()`가 끝나면 "트랜잭션 중"이 아니게 되어 `WAIT`이 3번 규칙대로 공유 연결로 갑니다. 트랜잭션은 전용 연결에서 실행됐으므로, 공유 연결의 기준(다른 스레드가 마지막으로 한 쓰기, 또는 0)으로 셉니다. 공유를 끈 구성에서는 전용 연결 하나만 있으므로 이 문제가 없습니다.

### 트랜잭션 안의 WAIT은 기다리지 않습니다

`MULTI` 안에 넣은 명령은 `EXEC` 때 한꺼번에 실행되는데, 이때 M은 기다리는 명령을 허용하지 않습니다. 그래서 `WAIT`은 지금까지 확인된 수를 바로 돌려줍니다. 같은 이유로 Spring의 `@Transactional`과 템플릿의 `enableTransactionSupport`를 함께 켠 메서드 안에서도 `WAIT`은 쓸모가 없습니다. 이 조합에서는 읽기 전용이 아닌 트랜잭션 안에서 템플릿이 처음 Redis 연결을 얻을 때 그 연결에 `MULTI`가 걸리고, 이후의 쓰기와 원시 `WAIT`은 쌓여 있다가 커밋 때 `EXEC`로 함께 실행됩니다. 그래서 `WAIT`은 ④처럼 기다리지 않고 지금까지 확인된 수를 돌려줍니다.

### 파이프라인은 두 설정 모두에서 전용 연결 하나를 씁니다

파이프라인 중에는 1번 규칙이 공유 설정보다 먼저 적용되므로, 쓰기와 `WAIT`이 모두 그 파이프라인의 전용 연결로 갑니다. 파이프라인은 `MULTI`가 아니므로 `WAIT`은 서버에서 정상적으로 기다립니다. 대가는 둘입니다. 풀이 없으면 호출마다 TCP 연결을 새로 맺습니다. 그리고 결과를 받는 방식이 연결 선택과 따로 놉니다. Spring을 거쳐 보낸 원시 `WAIT`은 파이프라인 결과 목록에 들어가지만 위에서 말한 응답 해석 문제를 그대로 안고 있고, `getNativeConnection()`으로 부른 `waitForReplication`은 목록에 들어가지 않아 그 `RedisFuture`를 앱이 직접 기다려야 합니다.

## 같은 소켓이어도 공유 연결 위의 WAIT은 다른 요청을 함께 멈춥니다

기본 구성(공유 켬)에서 ①·②는 같은 소켓을 보장하므로 기준은 맞습니다. 대신 `WAIT`이 그 공유 소켓을 멈춰 세웁니다. 예시에서 C의 `WAIT 1 100`이 기다리는 동안, 같은 공유 연결로 나간 D의 `GET stock:item:1`은 M의 입력 버퍼에 도착해 있어도 실행되지 않습니다. 주문 API 인스턴스 하나의 요청 스레드 200개가 일반 명령을 모두 같은 공유 연결로 보내므로, `WAIT` 하나가 그 인스턴스의 일반 명령 전체를 `WAIT`이 끝날 때까지 늦춥니다. 트랜잭션·파이프라인·`BLPOP`처럼 전용 연결로 가는 명령은 이 소켓에 줄 서지 않습니다.

- **평소에는 짧게 끝날 수 있습니다.** `WAIT`이 걸리면 M은 복제본들에게 곧바로 보고를 요청하므로, R이 정상이고 가깝다면 기다림은 보고가 오가는 시간 정도에 그칩니다. 얼마인지는 네트워크와 부하에 따라 다르므로 재 봐야 합니다.
- **복제본이 느리거나 없으면 매번 제한 시간을 다 씁니다.** R이 재시작 중이라 보고하는 복제본이 0대라면, 모든 `WAIT 1 100`이 제한 시간이 다 될 때까지 기다립니다. 100밀리초는 서버가 정한 기한이고, 실제로 풀리는 시각과 앱이 보는 지연은 이보다 조금 길 수 있습니다. 단순하게 모두 100밀리초씩 기다린다고 보면, 주문 확정이 초당 20건(가상)일 때 공유 소켓은 1초에 20 × 100ms = 2초 동안 멈춰 있어야 합니다. 1초보다 길므로 `WAIT`끼리도 같은 소켓에서 줄을 서고, 뒤에 선 일반 명령의 기다림은 계속 길어지다 명령 타임아웃으로 실패합니다. 줄이 길어지는 동안 요청 스레드도 응답을 기다리며 묶이므로, 복제본 하나의 문제가 주문 API 전체의 Redis 장애처럼 보이게 됩니다. 실제로 얼마나 번지는지는 재 봐야 합니다.

그래서 공유 연결 위의 `WAIT`은 "확인이 맞는가"는 통과하지만 "다른 요청에 영향이 없는가"를 통과하지 못합니다.

## 권하는 경로: WAIT을 붙이는 쓰기에는 공유를 끈 연결 구성을 따로 둡니다

이 문서는 **공유를 끈 `LettuceConnectionFactory`를 하나 더 두고, `WAIT`을 붙이는 쓰기는 그 팩토리의 템플릿으로 콜백 하나 안에서 쓰기와 `waitForReplication`을 함께 보내는 방법(② + 공유 끔)**을 권합니다.

- **같은 소켓**: 콜백 하나 동안 전용 연결 하나를 쓰므로, 트랜잭션을 섞어도(③) 쓰기와 `WAIT`이 같은 소켓으로 갑니다. 단, `waitForReplication`의 `RedisFuture`는 콜백 안에서 결과가 올 때까지 기다립니다. 이때 기한을 주고 기다리고, 시간 초과나 예외로 끝나면 0을 받은 것과 같이 "확인되지 않음"으로 보고 쓰기를 다시 보내지 않습니다("반환값이 요구한 수보다 작으면 무엇을 하는가"의 순서를 그대로 따릅니다). 기한을 주지 않아도 이 팩토리의 명령 타임아웃이 지나면 Lettuce가 그 명령을 예외로 끝내지만, 직접 만든 팩토리에서 명령 타임아웃을 정하지 않으면 그 값은 60초입니다.
- **다른 요청에 영향 없음**: 일반 요청은 기존 팩토리의 공유 연결을 그대로 쓰므로, 복제본이 느려져도 서버에서 멈추는 것은 `WAIT`을 붙인 요청의 연결뿐입니다.
- **타임아웃을 따로 정할 수 있음**: `WAIT`의 제한 시간과 이 팩토리의 명령 타임아웃을 일반 요청과 따로 맞출 수 있습니다. [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "기다리는 명령은 명령 타임아웃보다 짧게 기다려야 합니다"에서 기다리는 작업에 연결 구성을 따로 두라고 한 것과 같은 판단입니다.

대가와 주의점은 다음과 같습니다.

- **풀 크기를 정해야 합니다.** 이 팩토리의 연결 하나는 쓰기부터 `WAIT`이 끝날 때까지 붙잡힙니다. 복제본이 제때 보고하면 짧게 끝나지만, 복제본이 보고하지 못하는 동안에는 제한 시간 전체를 붙잡습니다. 그 최악을 가정하면 "`WAIT`을 붙이는 쓰기의 초당 수 × 제한 시간"이 평균적으로 붙잡히는 연결 수입니다. 예시의 초당 20건 × 100ms면 평균 2개이고, 쓰기 자체의 시간과 요청이 몰리는 순간을 감안해 여유를 둡니다. 풀의 최대 연결 수(`max-active`)에 닿으면 다음 요청은 빌리기를 기다리므로 `max-wait`에도 상한을 둡니다. 풀이 없으면 쓰기마다 TCP 연결을 새로 맺습니다.
- **Spring Boot 자동 구성이 물러납니다.** Spring Boot 3.5.16의 자동 구성은 `RedisConnectionFactory` 빈이 없을 때만 기본 팩토리를 만듭니다. 두 번째 팩토리를 빈으로 등록하면 기본 팩토리가 만들어지지 않으므로, 일반 요청용 팩토리도 직접 정의해야 합니다. 이때 기본 템플릿(`redisTemplate`, `stringRedisTemplate`)은 팩토리 후보가 하나이거나 `@Primary`로 하나가 정해질 때만 만들어지므로, 일반 요청용 팩토리에 `@Primary`를 붙이거나 템플릿도 직접 정의합니다.
- **`WAIT`의 제한 시간은 명령 타임아웃보다 확실히 짧게, 0은 쓰지 않습니다.** 명령 타임아웃은 클라이언트가 기다리기를 그만둘 뿐 서버의 `WAIT`을 취소하지 않습니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "타임아웃이 난 명령은 서버에서 어떻게 되었는가"). 그러면 서버에서 아직 멈춰 있는 소켓이 풀로 돌아가고, 다음에 그 연결을 빌린 요청의 명령이 남은 `WAIT` 뒤에서 기다립니다. 제한 시간 0은 "무한히 기다림"이라 이 상황을 끝없이 만듭니다.

**고르지 않은 대안**: 같은 팩토리에서 ⑤ `executePipelined`로 보내면 팩토리를 하나 더 두지 않아도 됩니다. 하지만 파이프라인 결과 목록에서 `WAIT`의 값을 꺼내는 방식을 따로 맞춰야 하고, 기본 팩토리의 풀을 기다리는 명령·트랜잭션과 나눠 써야 해 풀 크기 계산이 섞입니다. `WAIT`을 붙이는 쓰기가 아주 드물어 팩토리를 하나 더 두는 관리 비용이 더 크다면 이쪽을 검토합니다.

## 연결이 다시 맺어지면 확인이 거짓이 됩니다

위 표의 "같은 소켓"은 쓰기와 `WAIT` 사이에 Lettuce 연결 객체의 소켓이 바뀌지 않았다는 전제 위에 있습니다. 이 전제는 앱이 고르는 호출 경로로 지킬 수 없습니다.

- **쓰기 응답을 받은 뒤 끊기고 다시 연결된 경우**: Lettuce는 같은 연결 객체로 새 소켓을 맺습니다. 그 뒤에 보낸 `WAIT`은 새 소켓에 도착하고, 새 소켓의 기준은 0이므로, 정상 연결된 복제본이 요구한 수 이상이면 곧바로 그 수를 돌려줍니다. 앱은 같은 연결 객체를 썼으므로 이 일을 알아채지 못합니다.
- **장애 조치로 주 서버가 바뀐 경우**: 다시 연결한 곳이 새 주 서버라면, 그 서버의 새 소켓에서도 기준은 0입니다. 옛 주 서버에서 실행한 쓰기가 새 주 서버에 없는데도 `WAIT`은 새 주 서버의 복제본 수를 돌려줄 수 있습니다.
- **쓰기 응답을 받기 전에 끊긴 경우**: Lettuce의 기본 동작은 응답을 못 받은 쓰기를 새 소켓으로 다시 보내는 것이므로([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "연결이 끊기면 Lettuce가 응답 못 받은 명령을 다시 보냅니다"), 그 경우 기준은 새 소켓에서 다시 실행된 쓰기의 위치가 됩니다. 대신 쓰기가 두 번 실행됐을 수 있다는 별도의 문제가 생깁니다.

정리하면 `WAIT`의 1은 "**이 소켓이** 마지막으로 한 쓰기를 복제본 1대가 처리했다"입니다. 그사이 소켓이 바뀌지 않았다는 것까지는 보장하지 않습니다. 연결이 끊긴 시각 근처의 확인은 그만큼 덜 믿어야 하고, 이것이 아래에서 확인 결과와 별도로 대조 수단을 두라고 하는 이유 중 하나입니다.

## 반환값이 요구한 수보다 작으면 무엇을 하는가

C의 `WAIT 1 100`이 0을 돌려줬다고 합시다. 먼저 사실 두 가지를 분명히 합니다.

- **쓰기는 이미 M에서 실행됐습니다.** `WAIT`은 쓰기를 되돌리지 않습니다. M의 재고는 7입니다.
- **0은 "R에 없다"가 아니라 "100밀리초 안에 확인되지 않았다"입니다.** 응답 직후 R이 1099까지 처리했을 수도 있습니다.

그래서 다음 순서로 판단합니다.

1. **쓰기를 다시 보내지 않습니다.** `DECR`을 다시 보내면 M의 재고가 한 번 더 줄 뿐, 첫 쓰기가 복제되는 것을 돕지 않습니다.
2. **남은 시간 예산이 있으면 같은 연결로 `WAIT`만 다시 보낼 수 있습니다.** `WAIT`은 기준 위치를 바꾸지 않으므로 같은 소켓에서 다시 보내면 같은 쓰기(1099)를 기준으로 셉니다. 이 방법은 같은 콜백 안, 곧 같은 전용 연결 안에서만 성립합니다. 콜백이 끝나 연결을 돌려준 뒤 다시 빌린 연결로 보내면 ①과 같은 거짓 확인이 됩니다.
3. **그래도 부족하면 보상 쓰기로 되돌리지 말고, "복제 미확인"으로 기록합니다.** `INCR`로 재고를 되돌리는 보상 자체도 복제가 확인되지 않은 쓰기입니다. 그리고 그사이 장애 조치가 일어나 원래 `DECR`이 없는 복제본이 새 주 서버가 된 뒤에 보상이 실행되면, 없어진 차감을 한 번 더 되돌려 재고가 실제보다 늘어납니다. 앱은 보상을 보내는 순간 어느 서버의 어느 상태에 보내는지 알 수 없습니다. 대신 주문 ID를 "복제 미확인"으로 DB의 주문 행처럼 Redis 밖에 남깁니다. Redis에 남기면 장애 조치 때 차감과 함께 사라질 수 있기 때문입니다. 다만 요구한 수를 채운 쓰기도 장애 조치 뒤 사라질 수 있으므로(아래 문단), 장애 조치가 일어나면 미확인 주문만이 아니라 원본 기록(예: DB의 주문 테이블)에 확정된 주문 전체를 Redis와 대조해 재고를 다시 맞춥니다. 실제로는 예시의 단독 `DECR` 대신 차감과 주문 ID 기록을 함께 하는 스크립트를 쓰고, `WAIT`은 그 스크립트 뒤에 같은 연결로 보냅니다. 그 장치와 대조를 언제 돌리는지는 [Redis 재고를 DB 기준으로 다시 맞추기](./stock-reconciliation.md)의 "주문 ID로 맞추면 읽는 시점이 결과를 바꾸지 않습니다"와 "언제 맞추는가: 손실 신호, 그리고 주기 점검"에서 다룹니다. 사용자에게 주문을 성공으로 알릴지 실패로 알릴지는 업무가 정할 일이지만, 어느 쪽이든 대조할 원본이 있어야 결정이 의미가 있습니다.
4. **부족이 반복되면 복제본 상태를 봅니다.** 하나의 실패가 아니라 비율이 오르면 R의 연결 상태와 복제 지연을 확인합니다. 복제본이 없을 때 쓰기 자체를 받지 않는 쪽을 원하면 `WAIT` 대신 `min-replicas-to-write`가 맞는 도구입니다([Redis 복제](./replication.md)).

`WAIT`이 요구한 수를 채웠더라도 장애 조치 뒤 그 쓰기가 남는다는 보장은 없습니다(공식 문서 wait, "Consistency and WAIT"). 잃으면 안 되는 사실의 원본은 Redis 밖에 두고, `WAIT`은 잃는 확률을 줄이는 장치로 씁니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 본문 |
| --- | --- | --- |
| 같은 `RedisTemplate`(또는 같은 `RedisConnection`)으로 보내면 같은 연결이다 | Redis가 세는 단위는 서버 쪽 소켓 연결이고, 템플릿·`RedisConnection`은 그보다 위의 층입니다 | "연결"이라는 말이 가리키는 세 가지 |
| `WAIT`은 기다리는 명령이니 Spring이 전용 연결로 보낸다 | Spring Data Redis는 명령의 성질이 아니라 호출 방식으로 고르며, `WAIT`은 일반 명령처럼 공유 연결로 갑니다 | Spring Data Redis는 명령의 성질이 아니라 호출 방식으로 연결을 고릅니다 |
| 공유를 끄면 연결이 섞이지 않아 더 안전하다 | 쓰기와 `WAIT`을 따로 부르면 다른 소켓으로 갈 수 있고, 그러면 거짓 확인이 날 수 있습니다 | 공유를 끄면 따로 부른 WAIT이 거짓 확인을 돌려줍니다 |
| 콜백 하나 안에서 트랜잭션 뒤에 `WAIT`을 보내면 같은 연결이다 | 공유가 켜져 있으면 `exec()` 뒤의 `WAIT`은 공유 연결로 갑니다 | 트랜잭션 뒤의 WAIT은 공유 연결로 빠져나갑니다 |
| 같은 소켓이면 공유 연결 위의 `WAIT`도 문제없다 | 기준은 맞지만 그 공유 연결로 나가는 일반 명령을 모두 함께 멈춥니다 | 같은 소켓이어도 공유 연결 위의 WAIT은 다른 요청을 함께 멈춥니다 |
| `WAIT`이 1을 돌려주면 내 쓰기가 복제본에 있다 | 그사이 다시 연결됐다면 새 소켓의 기준 0으로 센 값일 수 있습니다 | 연결이 다시 맺어지면 확인이 거짓이 됩니다 |
| `WAIT`이 부족하면 쓰기를 다시 보내면 된다 | 쓰기는 이미 실행됐고, 다시 보내면 두 번 반영됩니다 | 반환값이 요구한 수보다 작으면 무엇을 하는가 |

## 근거

**Spring Data Redis**(태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13), 커밋 `42619b203148bae01235fe14f3cff3e171d93ebf`)

- [`LettuceConnection.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnection.java)
  - `getAsyncConnection`: 트랜잭션 중(`isQueueing`)이거나 파이프라인 중이면 전용 연결, 아니면 공유 연결이 있을 때 공유 연결, 없으면 전용 연결(연결 선택 규칙 1·3번)
  - `getOrCreateDedicatedConnection`: 전용 연결을 처음 필요할 때 한 번 꺼내 `LettuceConnection` 안에 두고 다시 씀. `reset`(`close`에서 호출): 전용 연결을 연결 공급자에게 돌려줌
  - `execute(String, byte[]...)`, `execute(String, CommandOutput, byte[]...)`: 원시 명령을 `invoke()`, 곧 `getAsyncConnection`으로 보냄. 응답 형태 힌트가 없으면 `TypeHints.getTypeHint`가 목록(`COMMAND_OUTPUT_TYPE_MAPPING`)에 없는 명령에 `ByteArrayOutput`을 씀. 이 목록에 `WAIT`이 없음
  - `getNativeConnection`: 구독 중이 아니면 `getAsyncConnection()`의 Lettuce 비동기 명령 객체를 돌려줌
  - `isQueueing`은 `isMulti`이고 `exec`·`discard`가 이를 false로 되돌림(③에서 `exec()` 뒤 공유 연결로 가는 근거)
  - 클래스 설명: `LettuceConnection`은 스레드에 안전하지 않아 스레드 사이에 공유하지 않음(`LettuceConnectionFactory` 클래스 설명)
- [`LettuceListCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceListCommands.java): `bLPop`·`bRPop`이 `getAsyncDedicatedConnection()`을 직접 씀(규칙 2번, 기다리는 명령은 API 메서드별로 정해 둠)
- [`LettuceConnectionFactory.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionFactory.java): 클래스 설명("a shared connection will be used for regular operations and a LettuceConnectionProvider will be used to select a connection for blocking and tx operations only ... If native connection sharing is disabled, new (or pooled) connections will be used for all operations"), `getConnection`(호출마다 새 `LettuceConnection`), `getSharedConnection`(`shareNativeConnection`이 false면 null), `pipeliningFlushPolicy` 기본 `flushEachCommand`, 클라이언트 설정의 `ClientOptions`를 `RedisClient`에 적용(`setOptions`)하고 명령 타임아웃을 연결 주소에 넣음(`setTimeout`)
- [`LettuceClientConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClientConfiguration.java): 빌더 기본값 `ClientOptions.builder().timeoutOptions(TimeoutOptions.enabled())`, 명령 타임아웃 기본 `RedisURI.DEFAULT_TIMEOUT`(직접 만든 팩토리도 명령 만료가 켜지고 기본 60초라는 근거)
- [`core/RedisTemplate.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisTemplate.java): `execute(RedisCallback, boolean, boolean)`(연결 하나를 얻어 콜백 동안 쓰고 끝나면 `releaseConnection`), `execute(SessionCallback)`(`bindConnection`으로 콜백 동안 같은 연결을 스레드에 묶음), `executePipelined`
- [`core/RedisConnectionUtils.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisConnectionUtils.java): `doGetConnection`(묶인 연결이 있으면 다시 씀, 없으면 `factory.getConnection()`), `potentiallyRegisterTransactionSynchronisation`(활성 트랜잭션 + 트랜잭션 지원에서 연결을 처음 얻을 때, 읽기 전용이 아니면 `multi()`), `ConnectionSplittingInterceptor`(읽기 전용으로 알려진 명령이 아니면 묶인 연결로 보냄. `execute`는 알려진 명령이 아니므로 묶인 연결, 곧 `MULTI` 안으로 감)

**Lettuce**(태그 [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE), 커밋 `643bd4709ce2c7a2c263dd50ed0320ce0f972c22`)

- [`api/async/BaseRedisAsyncCommands.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/api/async/BaseRedisAsyncCommands.java): `waitForReplication(int replicas, long timeout)`가 `RedisFuture<Long>`을 돌려줌. `RedisClusterAsyncCommands`가 이 인터페이스를 상속하므로 `getNativeConnection()`의 반환 객체에서 부를 수 있음
- [`TimeoutOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/TimeoutOptions.java), [`RedisURI.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisURI.java): `enabled()`는 명령 만료를 켜고 연결의 타임아웃을 적용함, `DEFAULT_TIMEOUT = 60`(초)
- [`output/ByteArrayOutput.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/output/ByteArrayOutput.java), [`output/CommandOutput.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/output/CommandOutput.java): `ByteArrayOutput`은 `set(ByteBuffer)`만 구현하고, 기본 `set(long)`은 `UnsupportedOperationException`을 던짐
- [`protocol/RedisStateMachine.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/RedisStateMachine.java), [`protocol/CommandHandler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/CommandHandler.java): 정수 응답은 `handleInteger` → `safeSet`이 출력의 `set(long)`을 부르고 예외를 오류 처리기로 넘김. `CommandHandler.decode`가 그 처리기로 `command::completeExceptionally`를 줌(원시 `WAIT`이 예외로 끝난다는 추론의 근거. 실행 확인 안 함)

**Spring Boot**(태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16))

- [`LettuceConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/LettuceConnectionConfiguration.java): `redisConnectionFactory`에 `@ConditionalOnMissingBean(RedisConnectionFactory.class)`
- [`RedisConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisConnectionConfiguration.java): `isPoolEnabled`(풀 설정이 없으면 `commons-pool2` 존재 여부, `COMMONS_POOL2_AVAILABLE`)
- [`RedisAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisAutoConfiguration.java): `redisTemplate`·`stringRedisTemplate`에 `@ConditionalOnSingleCandidate(RedisConnectionFactory.class)`

**Redis**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/replication.c`](https://github.com/redis/redis/blob/8.2.10/src/replication.c): `waitCommand`(그 연결의 `woff` 이상을 보고한 복제본 수를 먼저 세고, 모자라고 `CLIENT_DENY_BLOCKING`이 아니면 `blockForReplication`으로 그 연결을 멈춘 뒤 `replicationRequestAckFromSlaves`로 보고 요청), `replicationCountAcksByOffset`(`online` 상태 복제본 중 `repl_ack_off >= offset`인 수. 연결이 끊긴 복제본은 기준이 0이어도 세지 않음)
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c): `call`에서 명령 실행으로 `master_repl_offset`이 바뀐 경우에만 `c->woff`를 갱신("Remember the replication offset of the client, right after its last command that resulted in propagation")
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `createClient`에서 `woff = 0`, `processInputBuffer`에서 `CLIENT_BLOCKED`인 연결은 입력 버퍼의 다음 명령을 처리하지 않음
- [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c): `expireIfNeeded`가 만료된 키를 `deleteExpiredKeyAndPropagate`로 지우고 그 삭제를 복제 흐름에 넣음(읽기도 이 경우 연결의 기준 위치를 바꿀 수 있는 근거)
- [`src/multi.c`](https://github.com/redis/redis/blob/8.2.10/src/multi.c): `execCommand`가 실행 동안 `CLIENT_DENY_BLOCKING`을 켬(트랜잭션 안의 `WAIT`이 기다리지 않는 근거)

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`)

- [wait](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/wait.md): 반환값은 성공·제한 시간 모두 확인된 복제본 수이며 앱이 요구한 수와 비교해야 함(4번 설명), `MULTI`·스크립트처럼 기다릴 수 없는 곳에서는 바로 돌려줌(2번), 0은 무한히 기다림(3번), "Consistency and WAIT", "Implementation"(클라이언트마다 쓰기 오프셋을 기억)

## 실무 포인트

### 중요도별 점검 항목

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | 쓰기와 `WAIT`을 한 콜백 안에서 보내고, 공유를 끈 연결로 보낸다 | 따로 부르면 다른 소켓으로 가 거짓 확인이 날 수 있고, 공유 연결로 보내면 그 인스턴스의 일반 명령이 함께 멈춥니다 |
| 필수 | `WAIT`의 반환값을 요구한 수와 비교하고, 부족할 때 쓰기를 다시 보내지 않는다 | `WAIT`은 실패를 오류로 알리지 않고 숫자로만 알립니다. 쓰기는 이미 실행됐습니다 |
| 필수 | `WAIT`의 제한 시간을 명령 타임아웃보다 충분히 짧게 두고 0을 쓰지 않는다 | 클라이언트가 포기해도 서버의 `WAIT`은 남아 그 소켓을 계속 멈춥니다 |
| 알면 유리 | `@Transactional` + 트랜잭션 지원 메서드 안에서는 `WAIT`을 쓰지 않는다 | 쓰기가 커밋 때 실행되므로 메서드 안의 `WAIT`은 확인할 쓰기가 없습니다 |
| 알면 유리 | 두 번째 팩토리를 등록할 때 기본 팩토리와 템플릿도 직접 정의한다 | Spring Boot 자동 구성이 물러나 기존 템플릿 주입이 깨질 수 있습니다 |
| 참고 | 원시 `execute("WAIT", ...)` 대신 Lettuce의 `waitForReplication`을 쓴다 | 원시 명령은 정수 응답을 바이트로 해석하려다 예외로 끝날 수 있습니다(소스 추론) |

### 관찰할 것

- **서버에서 멈춘 연결 수**: `INFO clients`의 `blocked_clients`, `CLIENT LIST`의 `flags`에 `b`가 있고 `cmd=wait`인 연결. 공유 연결의 주소(앱 인스턴스의 IP·포트)가 이 목록에 자주 보이면 `WAIT`이 공유 연결로 나가고 있다는 뜻입니다.
- **`WAIT` 반환값 분포**: 요구한 수보다 작은 비율과 `WAIT`에 걸린 시간을 앱에서 지표로 남깁니다. 비율이 오르면 복제본 상태, 시간이 제한 시간에 붙어 있으면 복제본이 보고하지 못하는 상황입니다.
- **일반 명령의 지연이 `WAIT`과 함께 오르는가**: 복제본 점검·재시작 시각에 주문 API 전체의 Redis 지연이 오르면, `WAIT`이 공유 연결을 멈추고 있는지부터 의심합니다.
- **시험에서 확인할 것**: 쓰기와 `WAIT`을 보내는 바로 그 콜백 안에서, 쓰기 직전과 `WAIT` 직전에 각각 `CLIENT ID`를 보내 두 값을 비교하면 그 호출 경로가 같은 소켓을 쓰는지 직접 확인할 수 있습니다. ID를 별도 템플릿 호출로 얻으면 그 호출이 다른 연결을 쓸 수 있어 확인이 되지 않습니다. 두 값이 다르면 다른 소켓으로 갔거나 그사이 다시 연결된 것입니다. 같아도 마지막 측정과 `WAIT` 사이의 재연결까지 배제하지는 못합니다. 동시 요청을 걸어 풀 연결이 섞이는 상황에서도 확인합니다.

### 증상과 확인 순서

| 증상 | 의심할 원인 | 확인 순서 | 대응과 부작용 |
| --- | --- | --- | --- |
| 복제본이 크게 뒤처져 있는데 `WAIT`이 기다리지 않고 곧바로 요구한 수를 돌려줌 | 쓰기와 `WAIT`이 다른 소켓(①·③), 또는 그사이 다시 연결됨 | 호출 경로와 공유 설정을 표에 대입 → 쓰기 직전과 `WAIT` 직전의 `CLIENT ID` 비교 → 앱 로그에서 재연결 시각 확인 | 콜백 하나 + 공유 끈 연결로 옮김. 팩토리가 하나 늘어남 |
| 복제본 재시작 때마다 주문 API 전체의 Redis 응답이 느려지거나 타임아웃 | `WAIT`이 공유 연결을 멈춤 | `CLIENT LIST`에서 공유 연결이 `cmd=wait`로 멈췄는지 확인 | `WAIT`을 따로 둔 연결 구성으로 옮김. 그 구성의 풀 크기를 정해야 함 |
| `WAIT`을 붙인 쓰기 뒤 다른 요청이 그 연결에서 이유 없이 느려짐 | 명령 타임아웃이 `WAIT`보다 짧거나 `WAIT 0`이라, 서버에서 멈춘 소켓이 풀로 돌아감 | `WAIT` 제한 시간과 명령 타임아웃 비교 | 제한 시간을 줄이고 0을 없앰. 확인이 부족한 비율이 늘 수 있음 |
| `WAIT` 호출에서 응답 형식 관련 예외가 나는데 서버에는 `WAIT`이 실행됨 | 원시 `execute("WAIT")`의 응답을 바이트로 해석 | 호출 방식 확인 | `waitForReplication`으로 바꿈 |

## 말로 설명하기

### 30초 버전

`WAIT`은 "이 소켓 연결이 마지막으로 한 쓰기"를 복제본 몇 대가 받았는지 세므로, 쓰기와 같은 소켓으로 보내야 의미가 있습니다. Spring Data Redis는 `WAIT`을 기다리는 명령으로 따로 다루지 않아, 기본 구성에서는 공유 연결로 보냅니다. 그러면 기준은 맞지만 그 공유 연결로 나가는 일반 명령이 모두 함께 멈춥니다. 반대로 공유를 끄고 쓰기와 `WAIT`을 따로 부르면 다른 소켓으로 갈 수 있고, 그러면 기다리지도 않고 거짓 확인이 나올 수 있습니다. 그래서 공유를 끈 연결 구성을 따로 두고, 콜백 하나 안에서 쓰기와 `WAIT`을 함께 보냅니다.

### 2분 버전

Redis 주 서버는 소켓 연결마다 마지막 쓰기의 복제 위치를 기억하고, `WAIT`은 그 위치 이상을 보고한 복제본 수를 셉니다. 새 소켓은 그 위치가 0이라, 쓰기 없는 연결에서 보낸 `WAIT`은 정상 연결된 복제본이 요구한 수 이상이면 기다리지 않고 그 수를 돌려줍니다. 그리고 `WAIT`은 기다리는 동안 그 소켓의 다음 명령을 처리하지 않습니다.

Spring Data Redis는 명령을 어느 Lettuce 연결로 보낼지 호출 방식으로 정합니다. 트랜잭션·파이프라인 중이거나 `BLPOP` 같은 정해진 API면 전용 연결, 나머지는 공유가 켜져 있으면 공유 연결입니다. `WAIT`은 전용 API가 없어 나머지에 들어갑니다. 그래서 기본 구성에서 쓰기와 `WAIT`은 같은 공유 소켓으로 가 기준은 맞지만, 복제본이 느려지면 `WAIT`마다 그 공유 연결의 일반 명령이 제한 시간 동안 멈춥니다. 공유를 끄면 템플릿 호출마다 연결을 따로 꺼내므로, 쓰기와 `WAIT`을 따로 부르면 다른 소켓으로 갈 수 있습니다. 공유를 켠 채 콜백 안에서 트랜잭션을 쓰고 `exec()` 뒤 `WAIT`을 보내도 `WAIT`만 공유 연결로 빠져나갑니다.

그래서 `WAIT`을 붙이는 쓰기는 공유를 끈 팩토리를 따로 두고, 콜백 하나 안에서 쓰기와 `waitForReplication`을 보내고 결과를 콜백 안에서 기다립니다. 대가는 팩토리 하나와 풀 크기 관리입니다. 그래도 그사이 연결이 다시 맺어지면 확인이 거짓일 수 있고, 반환값이 부족해도 쓰기는 이미 실행됐습니다. 부족하면 쓰기를 다시 보내지 말고 같은 연결로 `WAIT`만 다시 보내거나 "복제 미확인"으로 기록합니다. 확인된 쓰기도 장애 조치 뒤 사라질 수 있으므로, 장애 조치 뒤에는 미확인 주문만이 아니라 확정 주문 전체를 원본과 대조합니다.

### 예상 질문과 꼬리 질문

- **Q. 같은 `RedisTemplate`으로 보내면 같은 연결 아닌가요?**
  아닙니다. `WAIT`이 세는 단위는 서버에 열린 소켓 연결 하나입니다. 템플릿은 호출마다 `RedisConnection`을 새로 얻고, 그것이 어느 Lettuce 연결로 보낼지는 공유 설정과 트랜잭션·파이프라인 여부로 정해집니다("연결"이라는 말이 가리키는 세 가지).
  - **꼬리: 그럼 공유를 켠 기본 구성이면 늘 같은 소켓이니 괜찮은 것 아닌가요?**
    호출을 따로 해도 같은 공유 소켓으로 가는 것은 맞습니다. 하지만 `WAIT`이 그 소켓을 멈추므로 같은 공유 연결로 나가는 일반 명령이 모두 함께 기다립니다. 복제본이 내려가 있으면 `WAIT`마다 제한 시간까지 기다리고, 초당 `WAIT` 수 × 제한 시간이 1초를 넘으면 공유 소켓의 줄이 계속 길어집니다(같은 소켓이어도 공유 연결 위의 WAIT은 다른 요청을 함께 멈춥니다).
  - **꼬리: 트랜잭션 뒤에 보내면요? 같은 콜백 안이잖아요.**
    공유가 켜져 있으면 `exec()` 뒤에는 트랜잭션 중이 아니므로 `WAIT`이 공유 연결로 갑니다. 트랜잭션은 전용 연결에서 실행됐으니 다른 소켓입니다(트랜잭션 뒤의 WAIT은 공유 연결로 빠져나갑니다).

- **Q. `WAIT`은 블로킹 명령인데 Spring이 알아서 전용 연결로 보내지 않나요?**
  보내지 않습니다. 전용 연결로 보내는 기다리는 명령은 `bLPop` 같은 API 메서드별로 정해 두었고, `WAIT`은 Spring Data Redis에 API가 없어 원시 명령이나 Lettuce 명령 객체로 보냅니다. 둘 다 일반 명령과 같은 규칙으로 연결을 고릅니다(Spring Data Redis는 명령의 성질이 아니라 호출 방식으로 연결을 고릅니다).
  - **꼬리: 공유를 끄면 해결되나요?**
    공유를 끄는 것만으로는 안 됩니다. 쓰기와 `WAIT`을 템플릿 호출 두 번으로 나누면 호출마다 전용 연결을 따로 꺼내므로 다른 소켓으로 갈 수 있습니다. 풀이 없으면 새 연결이라 기준이 0이고, 정상 연결된 복제본이 요구한 수 이상이면 `WAIT`은 기다리지 않고 그 수를 돌려줍니다. 콜백 하나 안에서 함께 보내야 같은 전용 연결을 씁니다.
  - **꼬리: 그럼 기존 팩토리의 공유를 꺼 버리면 안 되나요?**
    그러면 일반 명령도 모두 전용 연결을 빌려 쓰게 되어 동시에 Redis를 쓰는 요청 수만큼 연결이 필요해집니다. 풀의 최대 연결 수를 그만큼 키우거나, 아니면 바닥난 풀 앞에서 요청이 기다립니다. `WAIT`을 붙이는 쓰기는 일부이므로, 그 쓰기만 공유를 끈 팩토리로 보내는 편이 일반 요청의 구성을 건드리지 않습니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "공유를 끄면 모든 명령이 풀을 거칩니다").

- **Q. 왜 파이프라인이 아니라 팩토리를 하나 더 두나요?**
  파이프라인도 두 설정 모두에서 쓰기와 `WAIT`을 같은 전용 연결로 보냅니다. 다만 원시 `WAIT`은 결과 목록에 들어가도 응답 해석 문제가 있고 `waitForReplication`은 목록에 들어가지 않아 따로 기다려야 하며, 기본 팩토리의 풀을 다른 기다리는 명령·트랜잭션과 나눠 써 풀 계산이 섞입니다. 따로 둔 팩토리는 `WAIT`용 명령 타임아웃과 풀을 독립적으로 정할 수 있습니다. `WAIT`을 붙이는 쓰기가 아주 드물면 파이프라인 쪽이 관리 비용이 작습니다.
  - **꼬리: 팩토리를 하나 더 빈으로 등록하면 무엇이 깨지나요?**
    Spring Boot 자동 구성은 `RedisConnectionFactory` 빈이 없을 때만 기본 팩토리를 만들고, 기본 템플릿은 팩토리 후보가 하나로 정해질 때만 만듭니다. 그래서 일반 요청용 팩토리도 직접 정의하고 `@Primary`를 붙이거나 템플릿도 직접 정의합니다.

- **Q. 같은 소켓으로 보냈다면 `WAIT`의 1은 믿어도 되나요?**
  그사이 소켓이 바뀌지 않았다면 "그 소켓의 마지막 쓰기를 복제본 1대가 메모리에 반영했다"는 뜻입니다. 하지만 쓰기 응답을 받은 뒤 연결이 끊겨 Lettuce가 다시 맺었다면 `WAIT`은 기준이 0인 새 소켓에서 세므로 거짓 확인이 됩니다. 장애 조치 뒤 새 주 서버에 다시 붙은 경우도 같습니다(연결이 다시 맺어지면 확인이 거짓이 됩니다).
  - **꼬리: 그럼 확인이 맞아도 장애 조치 뒤에 그 쓰기가 남나요?**
    보장되지 않습니다. 장애 조치는 가장 나은 복제본을 고르려고 최선을 다할 뿐이어서 확인된 쓰기도 잃을 수 있습니다(공식 문서 wait, "Consistency and WAIT"). 그래서 잃으면 안 되는 사실의 원본은 Redis 밖에 둡니다.

- **Q. `WAIT`이 0을 돌려주면 어떻게 하나요?**
  쓰기는 이미 주 서버에서 실행됐고 `WAIT`은 되돌리지 않습니다. 쓰기를 다시 보내면 두 번 반영되므로 보내지 않습니다. 시간 예산이 남으면 같은 연결로 `WAIT`만 다시 보냅니다. `WAIT`은 기준 위치를 바꾸지 않으므로 같은 쓰기를 기준으로 다시 셉니다(반환값이 요구한 수보다 작으면 무엇을 하는가).
  - **꼬리: 그래도 부족하면 `INCR`로 되돌리면 되지 않나요?**
    보상 쓰기도 복제가 확인되지 않은 쓰기입니다. 그리고 그사이 장애 조치로 원래 `DECR`이 없는 복제본이 새 주 서버가 된 뒤에 보상이 실행되면, 없어진 차감을 한 번 더 되돌려 재고가 실제보다 늘어납니다. 주문을 "복제 미확인"으로 기록하고, 장애 조치가 일어나면 원본 기록과 대조해 재고를 다시 맞춥니다.
  - **꼬리: 그럼 장애 조치 뒤에는 "복제 미확인" 주문만 대조하면 되나요?**
    아닙니다. `WAIT`이 요구한 수를 채운 쓰기도 장애 조치 뒤 사라질 수 있고, 그사이 다시 연결됐다면 확인 자체가 거짓일 수 있습니다. 그래서 원본 기록에 확정된 주문 전체를 주문 ID로 Redis와 대조합니다. 미확인 기록은 차이가 날 가능성이 큰 주문을 알려 줄 뿐 대조 범위를 좁히지 않습니다(반환값이 요구한 수보다 작으면 무엇을 하는가).
  - **꼬리: 다른 연결로 `WAIT`을 다시 보내면요?**
    콜백이 끝나 연결을 돌려준 뒤 다시 빌린 연결은 다른 소켓일 수 있고, 그러면 기준이 달라 거짓 확인이 됩니다. 다시 보내는 `WAIT`도 같은 콜백 안에서 보냅니다.

- **Q. `WAIT`의 제한 시간은 어떻게 정하나요?**
  그 요청의 바깥 기한 안에 들고, 이 연결 구성의 명령 타임아웃보다 확실히 짧게 정합니다. 명령 타임아웃이 먼저 나면 클라이언트만 포기하고 서버의 `WAIT`은 남아 그 소켓을 계속 멈추며, 그 연결이 풀로 돌아가 다음 요청이 그 뒤에서 기다립니다. 0은 무한히 기다리므로 쓰지 않습니다.
  - **꼬리: 짧게 잡으면 무엇을 잃나요?**
    복제본이 조금만 늦어도 요구한 수를 못 채우는 비율이 오릅니다. 그 비율만큼 "복제 미확인" 기록이 늘어납니다. 제한 시간과 미확인 비율을 함께 지표로 보고 정합니다.
