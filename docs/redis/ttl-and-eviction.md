# TTL과 축출: 키는 언제, 누구의 판단으로 지워지는가

> **핵심 질문**: Redis에서 키가 개발자의 `DEL` 없이 사라지는 길은 무엇이며, 각각 누가 언제 결정하는가? 메모리 한도에 닿았을 때 어떤 키를 잃을지는 무엇이 정하고, 백엔드 개발자는 TTL과 축출 정책을 어떻게 골라야 하는가?
>
> **범위**: 단일 Redis 서버(주 서버)의 키 만료와 메모리 한도 축출을 다룹니다. 주기적 만료 정리가 메인 스레드를 얼마나 쓰는지는 [Redis 이벤트 루프](./event-loop.md)에서, 복제본이 만료·축출을 다루는 방식은 Redis 복제에서, 만료 판정에 쓰는 시계의 문제는 [Redis 분산 락](./distributed-lock.md)에서 다룹니다.

## 배경 지식

### 이 문서의 예시: 한 Redis에 섞여 사는 세 종류의 키

문서 끝까지 다음 장면 하나를 씁니다.

- 커머스 서비스가 Redis 서버 **한 대**를 씁니다. 메모리 한도(`maxmemory`)는 **1GB**로 정해 두었습니다.
- 이 서버에 성격이 다른 키 세 종류가 함께 있습니다.

| 키 | 하는 일 | 만든 방법 | TTL | 사라지면 |
| --- | --- | --- | --- | --- |
| `product:1001` 등 | 상품 상세 캐시 | Spring `@Cacheable`(기본 설정) | 없음 | DB에서 다시 읽으면 됨 |
| `session:42` 등 | 로그인 세션 | 로그인할 때 30분 TTL로 저장 | 30분 | 사용자가 로그아웃됨 |
| `stock:1001` 등 | 재고 카운터 | 운영자가 넣고 주문마다 `DECR` | 없음 | 재고 수량을 잃음. 원본이 Redis뿐이면 복구할 수 없음 |

- 이벤트로 상품 조회가 늘어 `product:*` 캐시가 계속 쌓이고, 메모리가 1GB에 닿습니다.

이 문서의 질문은 이 순간 **어떤 키가 사라지거나, 어떤 요청이 실패하는가**입니다. 답은 설정 하나(`maxmemory-policy`)와 키마다 TTL이 있는지에 따라 달라집니다. 메모리 크기와 키 개수는 흐름을 보여 주기 위한 **가상의 숫자**이고, 이 문서에서 잰 값이 아닙니다.

### 키가 사라지는 세 길을 구분합니다

이 문서에서 가장 헷갈리기 쉬운 지점이라 처음에 나눠 둡니다. 세 길은 결정하는 주체와 시점이 다릅니다.

| 길 | 누가 결정하는가 | 언제 결정하는가 | 예시에서 |
| --- | --- | --- | --- |
| **삭제** | 개발자가 보낸 명령(`DEL`, `UNLINK`) | 명령을 보낸 순간 | 관리자가 캐시를 비울 때 |
| **만료** | 개발자가 키에 미리 붙여 둔 TTL | 키를 만들 때 정해 둔 시각이 지나면 | `session:42`가 30분 뒤에 사라짐 |
| **축출(eviction)** | Redis가 설정된 정책으로 | 메모리가 한도를 넘은 상태에서 명령이 올 때 | 메모리가 1GB에 닿은 뒤 어떤 키가 사라짐 |

만료는 "언제 사라질지"를 개발자가 정하고, 축출은 "무엇이 사라질지"를 Redis가 정합니다. 둘은 별개의 장치이지만 하나로 이어집니다. 일부 축출 정책은 **TTL이 있는 키만** 후보로 삼기 때문에, TTL을 붙였는지가 축출 대상을 정합니다. 이 연결이 이 문서의 핵심입니다.

### 메인 스레드

- **한 줄**: Redis 서버에서 키를 읽고 바꾸는 명령은 메인 스레드 하나가 하나씩 끝까지 실행합니다.
- **이 주제에서는**: 만료된 키를 지우는 일과 축출로 키를 지우는 일도 같은 메인 스레드가 합니다. 그래서 지우는 일이 길어지면 그동안 다른 명령이 기다립니다. 원리는 [Redis 이벤트 루프](./event-loop.md)와 [프로세스와 스레드](../cs/process-and-thread.md)에 있습니다.

### Redis가 세는 메모리: `used_memory`와 `maxmemory`

- **한 줄**: `used_memory`는 Redis가 메모리 할당기를 통해 할당받은 바이트 수이고, `maxmemory`는 축출을 시작하는 기준으로 개발자가 정하는 바이트 수입니다. 둘 다 `INFO memory`로 볼 수 있습니다.
- **비유**: 창고의 적재 한도입니다. 한도선(`maxmemory`)을 넘으면 관리인(Redis)이 정해 둔 규칙(정책)대로 짐을 내다 버립니다.
- **실제로는**:
  - `used_memory`에는 키와 값만이 아니라 Redis가 할당한 거의 모든 것이 들어갑니다. 연결마다의 입력·응답 버퍼(받은 명령이 처리를 기다리거나, 만든 응답이 전송을 기다리는 동안 머무는 메모리), 내부 구조, 아직 지우지 않은 만료된 키도 포함됩니다. 할당기가 운영체제에서 받아 둔 메모리와는 다르며, 그 차이는 이 문서에서 다루지 않습니다.
  - 축출을 판단할 때 Redis는 `used_memory`에서 두 버퍼를 뺀 값을 `maxmemory`와 비교합니다. 하나는 복제본(주 서버의 변경을 받아 같은 데이터를 유지하는 다른 Redis 서버)에 보낼 변경을 담는 복제 버퍼 중, 설정된 복제 백로그 크기와 그 관리 비용 추정치를 넘는 부분입니다. 복제 백로그는 복제본이 잠깐 끊겼다 돌아왔을 때 이어 보내려고 최근 변경을 보관해 두는 기록으로, 복제 버퍼와 같은 메모리를 씁니다. 백로그는 정해진 크기로 늘 남아 있으므로 그만큼은 세고, 그보다 더 쌓인 부분만 뺍니다. 다른 하나는 AOF(실행한 쓰기 명령을 파일에 덧붙여 남기는 영속화 방식)에 쓸 명령을 모아 두는 AOF 버퍼입니다. 축출로 키를 지우면 그 삭제도 복제본과 AOF에 전할 변경이라 이 버퍼에 쌓입니다. 이 버퍼를 세면 삭제가 버퍼를 키우고, 그래서 다시 축출하는 악순환이 생길 수 있기 때문입니다.
  - `maxmemory`의 기본값은 0이고, 64비트 서버에서 0은 한도가 없다는 뜻입니다. 이때 Redis는 축출하지 않고 운영체제가 줄 수 있는 만큼 메모리를 씁니다.
- **이 주제에서는**: 예시의 1GB는 키 데이터만의 한도가 아닙니다. 연결 버퍼와 아직 지우지 않은 만료 키도 같은 1GB 안에서 셉니다.

## TTL은 키에 붙은 절대 시각입니다

`SET session:42 ... EX 1800`이나 `EXPIRE session:42 1800`으로 TTL을 주면, Redis는 "지금부터 1800초 뒤"를 밀리초 단위의 절대 시각(유닉스 시각)으로 바꿔 키에 붙여 둡니다. 남은 시간을 세는 타이머가 아니라 시각이므로, 서버 시계가 그 시각을 지나면 키는 없는 것으로 취급됩니다. 공식 문서는 시계를 바꾸면 만료도 함께 움직인다고 경고합니다(공식 문서 expire, "Expires and persistence"). 시계가 어긋날 때 생기는 문제는 [Redis 분산 락](./distributed-lock.md)에서 다룹니다.

TTL을 붙이면 키마다 만료 시각을 담는 메모리가 조금 더 듭니다(공식 문서 expire, "Keys with an expire").

### TTL은 언제 유지되고, 언제 사라지는가

TTL은 키의 **값을 통째로 바꾸는** 명령에서 사라지고, 값을 **고치는** 명령에서는 유지됩니다(공식 문서 expire, 본문 첫 부분).

| 명령 | TTL은 | 예시에서 |
| --- | --- | --- |
| `SET`(옵션 없이), `GETSET`, `*STORE` 계열, `DEL` | 사라짐. 덮어쓴 키는 TTL 없는 키가 됨 | `session:42`를 `SET`으로 덮어쓰면 30분 TTL이 없어짐 |
| `SET ... KEEPTTL` | 유지 | 세션 값을 바꾸되 남은 시간은 그대로 둠 |
| `INCR`, `LPUSH`, `HSET` 등 값을 고치는 명령 | 유지 | `stock:1001`에 `DECR`해도 TTL 상태는 그대로 |
| `EXPIRE`를 다시 보냄 | 새 값으로 바뀜 | 요청마다 `EXPIRE session:42 1800`이면 마지막 요청부터 30분 |
| `PERSIST` | 사라짐 | |
| `RENAME a b` | `a`의 TTL이 `b`로 옮겨감 | |
| `EXPIRE`에 0 이하, `EXPIREAT`에 지난 시각 | 키가 바로 삭제됨 | |

가장 흔한 함정은 첫 줄입니다. TTL이 있던 키를 옵션 없는 `SET`으로 덮어쓰면, 의도와 달리 **영원히 남는 키**가 됩니다. Spring에서 `opsForValue().set(key, value)`는 옵션 없는 `SET`을 보내므로, 세션처럼 TTL이 필요한 키를 이 메서드로 갱신하면 TTL이 사라집니다. 시간을 함께 넘기는 오버로드를 쓰거나, 남은 시간을 유지하려면 `KEEPTTL`을 써야 합니다.

TTL은 키 하나에 붙는 것이 기본입니다. Hash는 필드마다 만료를 거는 명령(`HEXPIRE` 등)도 있으며, 그 선택은 [객체 하나를 String에 담을까, Hash에 담을까](./string-vs-hash.md)에서 다룹니다.

## 만료된 키는 언제 메모리에서 사라지는가

만료 시각이 지났다고 키가 그 순간 메모리에서 지워지지는 않습니다. 주 서버는 두 길로 지웁니다(공식 문서 expire, "How Redis expires keys").

- **접근할 때**: 명령이 그 키를 읽거나 쓰려 할 때 만료됐는지 확인하고, 만료됐으면 그때 지웁니다.
- **주기적으로**: 메인 스레드가 TTL 있는 키를 일부 골라 만료된 것을 지웁니다. 한 번에 쓰는 시간에 한도가 있어, 만료된 키가 많으면 여러 주기에 걸쳐 지웁니다. 시간 예산과 그 대가는 [Redis 이벤트 루프](./event-loop.md)의 "만료 정리도 메인 스레드가 합니다"에 있습니다.

판단에 필요한 결과는 셋입니다.

- **만료됐지만 아직 지우지 않은 키도 메모리를 차지합니다.** 아무도 다시 읽지 않는 만료 키는 주기적 정리가 찾을 때까지 남습니다. 그동안 `used_memory`와 `INFO keyspace`의 `keys` 수에 들어갑니다. 그래서 TTL을 붙였다고 메모리가 정확히 그 시각에 줄지는 않습니다.
- **만료로 지우는 일도 비용입니다.** 원소를 따로따로 담은 큰 컬렉션이 만료되면, 기본 설정(`lazyfree-lazy-expire no`)에서는 메인 스레드가 원소를 하나씩 해제합니다. 작은 컬렉션과 String처럼 값을 한 덩어리에 담은 키는 크기와 상관없이 한 번에 해제합니다. 이 설정을 켜도 해제 작업량이 큰 값만 보조 스레드로 넘어갑니다. 이 작업량은 바이트가 아니라 저장 구조에 따라 센 해제 작업의 양입니다. 기준은 [Redis 이벤트 루프](./event-loop.md)의 "원소 수에 비례하는 명령"에 있습니다.
- **기본 설정의 복제본(읽기 전용)은 스스로 지우지 않습니다.** 읽기에는 없는 것으로 답하지만, 실제 삭제는 주 서버가 보내는 삭제를 따릅니다(Redis 복제).

## 메모리 한도에 닿으면 축출이 일어납니다

### 언제: 한도를 넘은 상태에서 명령이 올 때마다

축출은 백그라운드에서 미리 일어나지 않습니다. `maxmemory`가 정해져 있으면, Redis는 **명령을 실행하기 전마다** 메모리가 한도를 넘었는지 확인하고, 넘었으면 그 자리에서 키를 지웁니다. 확인하는 명령은 쓰기만이 아닙니다. `GET`이 와도 그 전에 축출이 돕니다.

그래서 메모리는 잠시 한도를 넘을 수 있습니다. 명령 하나가 실행되는 동안 쓴 메모리는 다음 명령이 올 때 정리되기 때문입니다. 공식 문서도 큰 집합 연산 결과를 저장하는 것처럼 데이터를 많이 더하는 명령은 한도를 눈에 띄게 넘길 수 있다고 적습니다(공식 문서 eviction, 첫 부분).

연결의 응답 버퍼도 `used_memory`에 들어가고 축출 판단에서 빠지지 않습니다. 이것은 소스에서 나온 해석입니다. 큰 응답이 버퍼에 쌓이면, 키 데이터는 그대로인데 메모리가 한도를 넘어 키가 축출될 수 있습니다. Hot Key와 Big Key의 큰 응답이 이 경로로 다른 키를 밀어낼 수 있습니다.

### 얼마나: 한도 아래로 내려갈 때까지, 단 한 번에 오래는 아니게

축출은 한도 아래로 내려갈 만큼 키를 지웁니다. 다만 한 번에 메인 스레드를 오래 잡지 않도록 시간 한도가 있습니다. 이 시간 한도는 `maxmemory-eviction-tenacity`로 정하며, 기본 설정에서 한 번에 약 500마이크로초를 쓰고, 그래도 모자라면 남은 축출을 타이머 작업으로 넘겨 이어서 합니다. 타이머 작업은 보조 스레드가 아니라 같은 메인 스레드가 이벤트 루프의 정해진 시점에 다시 돌리는 일이라, 이어지는 축출도 그동안 다른 명령을 기다리게 합니다. 시간은 키를 몇 개 지울 때마다 확인하므로 정확한 상한은 아닙니다. 특히 원소가 많은 키를 축출하면, 기본 설정(`lazyfree-lazy-eviction no`)에서는 그 키를 해제하는 동안 메인 스레드가 묶입니다.

### 무엇을: 정책이 후보 범위와 기준을 정합니다

`maxmemory-policy`는 여덟 가지이고, 두 질문의 조합으로 읽으면 됩니다.

| | LRU: 오래 안 쓴 것 | LFU: 덜 자주 쓴 것 | random: 무작위 | ttl: 만료가 가까운 것 |
| --- | --- | --- | --- | --- |
| **allkeys-**: 모든 키가 후보 | `allkeys-lru` | `allkeys-lfu` | `allkeys-random` | (없음) |
| **volatile-**: TTL 있는 키만 후보 | `volatile-lru` | `volatile-lfu` | `volatile-random` | `volatile-ttl` |

그리고 **`noeviction`**(기본값)은 아무것도 지우지 않습니다.

- **LRU**(Least Recently Used)는 마지막으로 쓴 지 가장 오래된 키를, **LFU**(Least Frequently Used)는 최근 접근 빈도가 가장 낮은 키를 고릅니다. LFU 빈도 카운터가 어떻게 오르고 줄어드는지는 Hot Key와 Big Key에 있습니다.
- **LRU·LFU·`volatile-ttl`은 정확한 순위가 아니라 표본에서 고릅니다.** Redis는 모든 키를 정렬하지 않습니다. 키를 `maxmemory-samples`개(기본 5개)씩 무작위로 뽑고, 뽑을 때마다 지울 만한 후보 몇 개를 추려 두었다가 그중 가장 지울 만한 키를 지웁니다. 공식 문서는 표본 수를 늘리면 진짜 LRU에 가까워지는 대신 CPU를 더 쓴다고 적습니다(공식 문서 eviction, "Approximated LRU algorithm"). 그래서 "가장 오래 안 쓴 키가 반드시 먼저 사라진다"고 기대하면 안 됩니다. `random` 정책은 표본을 비교하지 않고 후보 범위에서 무작위로 하나를 바로 고릅니다.
- **후보가 없으면 `noeviction`처럼 됩니다.** `volatile-*` 정책에서 TTL 있는 키가 하나도 없으면 지울 것이 없습니다. 공식 문서도 "The `volatile-xxx` policies behave like `noeviction` if no keys have an associated expiration"이라고 적습니다(공식 문서 eviction, "Eviction policies").

### 지우지 못하면: 쓰기 명령이 OOM 에러를 받습니다

`noeviction`이거나 후보가 없어 메모리를 한도 아래로 내리지 못하면, 메모리를 더 쓸 수 있는 명령(Redis가 `denyoom`으로 표시한 `SET`, `HSET`, `LPUSH` 등)은 실행되지 않고 다음 에러를 받습니다.

```text
-OOM command not allowed when used memory > 'maxmemory'.
```

`GET`이나 `DEL`처럼 메모리를 늘리지 않는 명령은 계속 실행됩니다. 그래서 장애의 모양은 "Redis가 죽었다"가 아니라 **"읽기는 되는데 쓰기만 실패한다"** 입니다.

축출로 지운 키는 주 서버가 복제본과 AOF에 삭제로 전합니다. 기본 설정에서는 `DEL`, `lazyfree-lazy-eviction`을 켜면 `UNLINK`입니다(Redis 복제). 복제본은 기본으로 `maxmemory`를 무시하고 주 서버의 삭제를 따릅니다(`replica-ignore-maxmemory yes`).

## 예시에 정책을 대 보면

메모리가 1GB에 닿은 순간, 정책마다 무슨 일이 생기는지 봅니다. 예시의 키 상태를 다시 적으면 `product:*`는 TTL 없음, `session:*`는 TTL 30분, `stock:*`는 TTL 없음입니다.

| 정책 | 후보 | 실제로 생기는 일 |
| --- | --- | --- |
| `noeviction`(기본값) | 없음 | 아무 키도 잃지 않습니다. 대신 새 로그인의 세션 저장, 새 상품 캐시 저장이 OOM 에러를 받습니다. 기존 키를 읽는 요청은 됩니다 |
| `allkeys-lru` | 모든 키 | 오래 안 쓴 키부터 사라집니다. LRU는 키의 역할을 모르므로, 잘 팔리지 않는 상품의 `stock:*`도 오래 안 쓰였으면 `product:*`와 똑같이 후보입니다. 재고 수량을 잃을 수 있습니다 |
| `volatile-lru` | `session:*`만 | 캐시를 지우고 싶었는데, TTL이 없는 `product:*`는 후보가 아닙니다. 사라지는 것은 세션뿐이라 사용자가 로그아웃됩니다. 세션을 다 지워도 한도 아래로 내려가지 않으면, 그때부터는 `noeviction`처럼 쓰기가 OOM 에러를 받습니다 |
| `volatile-ttl` | `session:*`만 | 만료가 가까운 세션부터 사라집니다. 결과는 `volatile-lru`와 비슷합니다 |

어느 정책도 이 서버의 요구를 만족하지 못합니다. **원인은 정책이 아니라, 잃어도 되는 키와 잃으면 안 되는 키가 한 서버에 섞여 있는 것입니다.** `volatile-lru`가 캐시를 지키고 세션을 지운 이유는, Spring `@Cacheable`의 기본 설정이 캐시 항목에 TTL을 붙이지 않기 때문입니다. Spring Data Redis의 `RedisCacheConfiguration.defaultCacheConfig()`는 TTL 없음이 기본이고, `entryTtl(...)`로 정해야 붙습니다.

### 그래서 어떻게 나누는가

선택은 "잃어도 되는가"로 키를 가르는 데서 시작합니다.

**① 서버를 나누기(권장)**: 캐시 전용 Redis와 데이터 전용 Redis를 따로 둡니다.

- 캐시 서버: `allkeys-lru`나 `allkeys-lfu`. 모든 키가 다시 만들 수 있는 사본이므로 무엇이 지워져도 됩니다. 캐시 항목의 TTL은 축출과 상관없이 "얼마나 낡은 값을 견딜까"로 정합니다.
- 데이터 서버(세션, 재고): `noeviction`. 메모리가 차면 키를 잃는 대신 쓰기가 실패하므로, 실패를 알아챌 수 있고 데이터는 남습니다. 대신 메모리 사용량을 감시해 한도에 닿기 전에 늘려야 합니다.
- 대가: 서버와 연결 설정이 둘이 됩니다. 두 서버의 키를 한 명령이나 스크립트로 묶을 수 없습니다.

**② 한 서버를 계속 써야 한다면**: 캐시 항목에는 모두 TTL을 붙이고(`entryTtl`), 정책은 `volatile-lru`, 잃으면 안 되는 키(`stock:*`)에는 TTL을 붙이지 않습니다. 그러면 재고는 후보에서 빠집니다. 하지만 세션도 TTL이 있으므로 캐시와 함께 후보가 됩니다. 세션을 잃으면 안 되는 서비스라면 이 방법으로는 지킬 수 없고, ①로 가야 합니다.

`allkeys-*` 정책 아래에서는 TTL을 붙이지 않아도 지켜지지 않는다는 점이 ②와 반대입니다. 이 차이를 정책을 바꾸는 사람이 알아야 합니다. 데이터 키가 섞인 서버의 정책을 `allkeys-*`로 바꾸는 것은, 데이터를 잃어도 된다고 선언하는 것과 같습니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

- **한도 근처에서는 명령마다 축출이 붙습니다.** 쓰기가 빠르게 들어오는 동안 메모리가 한도에 붙어 있으면, 명령을 실행하기 전마다 축출이 돕니다. 한 번은 짧아도 모든 요청의 지연에 조금씩 더해지고, 큰 키를 축출하는 순간에는 크게 더해집니다. 한도를 넘은 상태가 얼마나 이어졌는지는 `INFO stats`의 `current_eviction_exceeded_time`과 `total_eviction_exceeded_time`(밀리초)으로 봅니다.
- **캐시 서버에서는 축출이 적중률로 돌아옵니다.** 캐시 대상이 메모리보다 커지면, 지워진 키를 다시 읽을 때 미스가 나서 원본 DB로 갑니다. 그 결과를 다시 캐시에 넣으면 또 다른 키가 축출됩니다. `INFO stats`의 `evicted_keys`로 축출 수를 봅니다. 같은 절의 `keyspace_hits`와 `keyspace_misses`는 캐시 적중률이 아닙니다. 서버 전체의 읽기 조회를 재시작이나 `CONFIG RESETSTAT` 이후 누적해 세므로 캐시가 아닌 키와 `EXISTS`·`TTL` 같은 명령의 조회가 섞이고, `SET`·`INCR` 같은 쓰기 명령의 조회는 빠집니다. 캐시마다의 적중률은 캐시 효과 재기에서 다룹니다.
- **축출은 복제 흐름에도 실립니다.** 축출된 키마다 삭제가 복제본과 AOF로 갑니다. 축출이 많으면 복제 흐름도 그만큼 바빠집니다.
- **같은 TTL을 한꺼번에 주면 만료도 몰립니다.** 많은 키가 같은 시각에 만료되면 주기적 정리가 시간 한도까지 돌고, 캐시라면 원본 DB로도 미스가 몰립니다. TTL에 작은 무작위 값을 더해 흩습니다. 대가는 만료 시각이 정확하지 않다는 것입니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 바로잡은 곳 |
| --- | --- | --- |
| 만료와 축출은 같은 것이다 | 만료는 개발자가 정한 시각에, 축출은 메모리 한도에서 Redis가 정책으로 지웁니다 | 키가 사라지는 세 길을 구분합니다 |
| TTL을 붙이면 그 시각에 메모리가 줄어든다 | 접근하거나 주기적 정리가 찾을 때 지워지므로, 그때까지 메모리를 차지합니다 | 만료된 키는 언제 메모리에서 사라지는가 |
| `SET`으로 값만 바꾸면 TTL은 그대로다 | 옵션 없는 `SET`은 TTL을 지웁니다. 유지하려면 `KEEPTTL`을 씁니다 | TTL은 언제 유지되고, 언제 사라지는가 |
| TTL 없는 키는 축출되지 않는다 | `allkeys-*` 정책에서는 모든 키가 후보입니다 | 무엇을: 정책이 후보 범위와 기준을 정합니다 |
| `volatile-lru`면 캐시가 먼저 지워진다 | TTL 없는 캐시 항목은 후보가 아닙니다. Spring `@Cacheable`의 기본 설정이 그렇습니다 | 예시에 정책을 대 보면 |
| LRU 정책은 가장 오래 안 쓴 키를 지운다 | 표본에서 고르는 근사입니다 | 무엇을: 정책이 후보 범위와 기준을 정합니다 |
| `maxmemory`를 넘는 일은 없다 | 다음 명령 전에 정리하므로 잠시 넘을 수 있습니다 | 언제: 한도를 넘은 상태에서 명령이 올 때마다 |
| `noeviction`에서 메모리가 차면 Redis가 멈춘다 | 메모리를 늘리는 명령만 OOM 에러를 받고 읽기는 됩니다 | 지우지 못하면: 쓰기 명령이 OOM 에러를 받습니다 |

## 근거

**소스**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10))

- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `maxmemory_policy_enum`(정책 8가지), `maxmemory` 기본 0, `maxmemory-policy` 기본 `noeviction`, `maxmemory-samples` 기본 5, `maxmemory-eviction-tenacity` 기본 10, `replica-ignore-maxmemory` 기본 yes, `lazyfree-lazy-expire`·`lazyfree-lazy-eviction` 기본 no
- [`src/evict.c`](https://github.com/redis/redis/blob/8.2.10/src/evict.c): `EVPOOL_SIZE 16`, `evictionPoolPopulate`(`maxmemory_samples`개를 무작위로 뽑아 후보 풀 갱신, `volatile-ttl`은 만료 시각 기준), `freeMemoryGetNotCountedMemory`(복제 백로그 크기와 관리 비용 추정치를 넘는 복제 버퍼, 그리고 AOF 버퍼만 빼며, 빼는 이유를 주석으로 설명), `getMaxmemoryState`(`maxmemory` 0이면 한도 없음), `performEvictions`(`random` 정책은 후보 풀 없이 무작위로 하나를 고름, `noeviction`이면 실패, 후보가 없으면 실패, 키 16개마다 시간 한도 확인 뒤 `startEvictionTimeProc`로 넘김, `lazyfree_lazy_eviction`), `evictionTimeLimitUs`(tenacity 10 이하는 `50 × tenacity`마이크로초), `isSafeToPerformEvictions`(복제본은 `replica-ignore-maxmemory`면 축출 안 함)
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c): `processCommand`(`maxmemory`가 있으면 명령마다 `performEvictions`, 실패하고 `denyoom` 명령이면 `shared.oomerr`로 거부), `shared.oomerr`의 문구, `initServer`(32비트이고 `maxmemory`가 0이면 3GB와 `noeviction`으로 바꿈), `INFO`의 `used_memory`·`maxmemory`·`maxmemory_policy`·`expired_keys`·`evicted_keys`·`total_eviction_exceeded_time`·`current_eviction_exceeded_time`·`keyspace_hits`·`keyspace_misses`, `INFO keyspace`의 `keys`·`expires`, `resetServerStats`(`CONFIG RESETSTAT`이 `keyspace_hits`·`keyspace_misses`를 0으로 되돌림)
- [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c): `propagateDeletion`(지연 해제면 `UNLINK`, 아니면 `DEL`로 전파), `setKey`(`SETKEY_KEEPTTL`이 없으면 기존 TTL을 지움), `expireIfNeeded`(접근 시 만료 확인, 복제본은 강제 삭제 조건이 아니면 지우지 않음), `lookupKey`(`LOOKUP_WRITE`인 쓰기용 조회는 `keyspace_hits`·`keyspace_misses`를 세지 않음), `existsCommand`(읽기 조회라 이 카운터에 들어감)
- [`src/expire.c`](https://github.com/redis/redis/blob/8.2.10/src/expire.c): `ttlGenericCommand`(키가 없으면 -2, 키는 있고 만료 시각이 없으면 -1)
- [`src/lazyfree.c`](https://github.com/redis/redis/blob/8.2.10/src/lazyfree.c): `lazyfreeGetFreeEffort`, `freeObjAsync`(해제 작업량 추정치가 64를 넘을 때만 보조 스레드로 넘김)
- [`src/t_string.c`](https://github.com/redis/redis/blob/8.2.10/src/t_string.c): `setGenericCommand`(`KEEPTTL`이나 만료 옵션이 있을 때만 `SETKEY_KEEPTTL`), `parseExtendedStringArgumentsOrReply`(`KEEPTTL` 옵션)

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`. 특정 Redis 버전 전용 문서가 아니므로 소스와 다르면 소스를 따릅니다)

- [expire](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/expire.md): 본문 첫 부분(TTL을 지우는 명령과 유지하는 명령, `PERSIST`, `RENAME`, 0 이하 값은 삭제), "Refreshing expires", "Keys with an expire"(추가 메모리), "Expire accuracy"(0~1밀리초), "Expires and persistence"(밀리초 절대 유닉스 시각, 시계 변경의 영향), "How Redis expires keys"(접근할 때와 주기적으로)
- [eviction](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/reference/eviction/index.md): 첫 부분(명령이 데이터를 더할 때 확인, 큰 명령은 한도를 잠시 넘길 수 있음), "Using the `maxmemory` configuration directive", "Eviction policies"(`volatile-xxx`는 TTL 있는 키가 없으면 `noeviction`처럼 동작, 다른 이유가 없으면 `allkeys-lru`가 기본 선택), "Using the `INFO` command", "Approximated LRU algorithm"(표본 수와 정확도·CPU의 맞바꿈), "LFU eviction"(자주 쓰는 키가 남을 가능성이 높아 일부 경우 적중률이 더 나음)

**Spring Data Redis**(태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13))

- [`RedisCacheConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheConfiguration.java): `defaultCacheConfig`(`TtlFunction.persistent()`, 곧 TTL 없음), `entryTtl`, `enableTimeToIdle`, [`RedisCache.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCache.java)의 `lookup`과 [`DefaultRedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/DefaultRedisCacheWriter.java)의 `shouldExpireWithin`(TTI가 켜지고 TTL이 0이 아닐 때만 읽기에 `GETEX`)
- [`RedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheWriter.java): `TtlFunction.persistent()`는 `Duration.ZERO`(만료 없음)
- [`RedisCacheManager.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheManager.java): `create`와 빌더의 기본 설정이 `defaultCacheConfig()`
- [`DefaultValueOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/DefaultValueOperations.java): `set(key, value)`는 옵션 없는 `SET`

## 실무 포인트

### 무엇을 미리 정할까

| 중요도 | 항목 | 이유와 선택 |
| --- | --- | --- |
| 필수 | 키를 "잃어도 되는가"로 나누고, 섞지 않기 | 정책은 서버 단위라, 섞여 있으면 어떤 정책도 둘 다 지키지 못합니다. 캐시와 데이터를 다른 서버에 둡니다. |
| 필수 | 캐시 서버에 `maxmemory`와 `allkeys-lru`(또는 `allkeys-lfu`) 정하기 | `maxmemory`를 정하지 않으면 축출 자체가 일어나지 않습니다. 기본 정책 `noeviction`의 캐시 서버는 메모리가 차면 캐시 저장이 실패합니다. 둘 중에는 다른 이유가 없으면 `allkeys-lru`로 시작하고, 자주 읽히는 키를 더 잘 남겨야 할 때 `allkeys-lfu`를 검토합니다(공식 문서 eviction, "Eviction policies", "LFU eviction"). 바꾼 뒤에는 부하가 비슷한 구간끼리 `evicted_keys`의 증가 속도(누적값이므로 값 자체가 아니라)와 앱의 캐시 미스율을 바꾸기 전과 비교합니다. |
| 필수 | 데이터 서버는 `noeviction` + 메모리 감시 | 키를 잃는 대신 쓰기 실패로 드러나게 합니다. `used_memory`가 `maxmemory`에 다가가면 경보를 울립니다. |
| 필수 | Spring 캐시에 `entryTtl` 정하기 | 기본은 TTL 없음입니다. TTL은 "얼마나 낡은 값을 견딜까"로 정하고, 서버를 나누지 못해 `volatile-*`를 쓴다면 TTL이 축출 후보 자격도 됩니다. |
| 필수 | TTL이 필요한 키를 옵션 없는 `SET`으로 덮어쓰지 않기 | TTL이 사라져 영원히 남습니다. `opsForValue().set(key, value)`가 그렇습니다. |
| 알면 유리 | TTL에 무작위 값을 더해 흩기 | 한꺼번에 만료되어 메인 스레드와 원본 DB에 부하가 몰리는 것을 줄입니다. 얼마나 줄지는 키 수와 흩는 폭에 달려 있습니다. |
| 알면 유리 | 큰 키가 축출·만료될 수 있으면 `lazyfree-lazy-eviction`, `lazyfree-lazy-expire` 검토 | 해제 작업량이 큰 값의 해제를 보조 스레드로 넘깁니다. 대가는 메모리가 늦게 돌아오는 것이라, 축출 중에는 한도 아래로 내려가는 데 시간이 더 걸릴 수 있습니다. |
| 참고 | `maxmemory-samples` | 늘리면 LRU·LFU가 정확해지는 대신 축출마다 CPU를 더 씁니다. 기본값으로 시작합니다. |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 키마다 TTL을 붙일지, 얼마로 할지 | `INFO keyspace`의 `keys`와 `expires`(TTL 있는 키 수) |
| 서버를 나누는 방식, `maxmemory`, `maxmemory-policy` | `INFO memory`의 `used_memory`와 `maxmemory`, `INFO stats`의 `evicted_keys`, `expired_keys` 증가 속도(서버 전체 합계) |
| 값을 덮어쓸 때 `KEEPTTL`을 쓸지 | 갱신한 키에 보낸 `TTL`의 결과. -1은 지금 TTL이 없다는 뜻이라, 갱신 전에 TTL이 있던 키에서 나오면 갱신이 TTL을 지운 것입니다 |
| Spring 캐시의 `entryTtl` | 캐시마다의 적중률(앱 지표). `keyspace_hits`·`keyspace_misses`는 서버 전체의 읽기 조회라 추세로만 봅니다 |
| `lazyfree-*` 설정 | `current_eviction_exceeded_time`, 쓰기의 OOM 에러 수 |

### 증상과 확인 순서

**증상 1**: "로그인이 갑자기 풀린다는 문의가 늘었다."

1. `INFO stats`의 `evicted_keys`가 늘고 있는지 봅니다. 이 값은 서버 전체의 축출 수라 어느 키가 지워졌는지는 알려 주지 않습니다. 늘고 있다면 세션이 만료가 아니라 축출로 사라졌을 가능성을 후보로 둡니다.
2. `CONFIG GET maxmemory-policy`로 정책을 봅니다. `volatile-*`라면 TTL 있는 키만 후보이므로, TTL이 있는 세션도 그 후보입니다.
3. `INFO keyspace`의 `keys`와 `expires`를 비교합니다. 둘은 개수일 뿐 메모리 비율이 아니지만, `keys`가 `expires`보다 훨씬 많다면 TTL 없는 키가 많다는 신호입니다. 키 종류별로 표본 키에 `TTL`을 보내 -1(TTL 없음)이 나오는 종류와 TTL이 있는 종류를 나눕니다. 예시에서는 `@Cacheable`의 `product:*`가 TTL 없음이고, TTL이 있는 것은 세션뿐입니다. 큰 키는 `MEMORY USAGE`로 크기를 확인합니다.
4. 대응: 급하면 메모리를 늘립니다. 근본 대응은 서버를 나누는 것입니다. 캐시에 `entryTtl`을 주면 캐시도 후보가 되어 세션이 덜 지워질 수는 있지만, 세션이 후보에서 빠지지는 않습니다. TTL 없는 캐시 키를 지우는 작업은 큰 키가 섞여 있으면 `UNLINK`로 합니다.

**증상 2**: "읽기는 되는데 쓰기만 `OOM command not allowed` 에러로 실패한다."

1. 축출로 한도 아래로 내려가지 못했습니다. `noeviction`이거나, `volatile-*`인데 TTL 있는 키를 다 지워도 모자랐거나, 지울 키가 남지 않을 만큼 키 아닌 메모리가 컸을 수 있습니다. `maxmemory_policy`와 `expires` 수를 봅니다.
2. `used_memory`가 `maxmemory`를 넘었는지, 키 데이터가 늘었는지 아니면 연결 버퍼가 늘었는지 봅니다. 큰 응답을 받는 연결이 있으면 응답 버퍼가 메모리를 차지할 수 있습니다.
3. 대응: 데이터 서버라면 메모리를 늘리거나 데이터를 줄입니다. 정책을 `allkeys-*`로 바꾸면 지울 후보가 늘어, 늘어난 후보에서 초과분을 회수할 수 있는 만큼 에러가 사라집니다. 대신 데이터를 잃기 시작합니다. 지울 수 있는 키를 모두 지워도 메모리가 한도를 넘는다면, 예를 들어 연결 버퍼가 그만큼 크다면, 정책을 바꿔도 풀리지 않습니다. 캐시만 있는 서버가 아니라면 바꾸지 않습니다.

### Spring에서 확인할 것

- **`RedisCacheManager`의 기본 설정은 TTL 없음입니다.** `RedisCacheManager.create(...)`와 빌더 모두 `defaultCacheConfig()`를 씁니다. `cacheDefaults(RedisCacheConfiguration.defaultCacheConfig().entryTtl(...))`처럼 TTL을 정했는지 확인합니다. 캐시마다 TTL을 정하는 방법과 그 함정은 Spring `@Cacheable`의 함정에서 다룹니다.
- **`entryTtl`과 함께 `enableTimeToIdle()`을 켜면 읽을 때마다 TTL을 늘립니다.** TTL이 정해져 있으면 캐시를 읽을 때 `GETEX`로 TTL을 다시 정하므로(TTL이 없는 기본 설정에서는 그냥 `GET`입니다), 자주 읽히는 항목은 계속 살아남습니다. 원본이 바뀌어도 자주 읽히는 캐시는 만료로 갱신되지 않으므로, 갱신은 따로 무효화해야 합니다.
- **`opsForValue().set(key, value)`는 TTL을 지웁니다.** TTL이 필요한 키는 시간을 함께 넘기는 오버로드를 씁니다.

## 말로 설명하기

### 30초 버전

Redis에서 키가 `DEL` 없이 사라지는 길은 만료와 축출입니다. 만료는 개발자가 키에 붙인 시각에, 축출은 메모리가 `maxmemory`를 넘었을 때 Redis가 정책으로 지웁니다. `volatile-*` 정책은 TTL 있는 키만 지우므로, TTL을 붙였는지가 축출 대상을 정합니다. 그래서 잃어도 되는 캐시와 잃으면 안 되는 데이터를 한 서버에 두면 어떤 정책도 둘 다 지키지 못합니다. 캐시 서버는 `allkeys-lru`, 데이터 서버는 `noeviction`과 메모리 감시로 나눕니다.

### 2분 버전

TTL은 키에 붙인 밀리초 단위의 절대 시각입니다. 값을 통째로 바꾸는 명령, 예를 들어 옵션 없는 `SET`은 TTL을 지우고, `INCR`이나 `HSET`처럼 값을 고치는 명령은 TTL을 유지합니다. 만료 시각이 지나도 바로 메모리에서 지워지지는 않고, 접근할 때나 메인 스레드의 주기적 정리가 찾을 때 지워집니다. 그동안은 메모리를 차지합니다.

축출은 `maxmemory`가 정해져 있을 때만 일어납니다. Redis는 명령을 실행하기 전마다 메모리가 한도를 넘었는지 보고, 넘었으면 정책에 따라 키를 지웁니다. 정책은 후보 범위(모든 키냐, TTL 있는 키만이냐)와 고르는 기준(LRU, LFU, 무작위, 만료 임박)의 조합이고, LRU·LFU·만료 임박은 정확한 순위가 아니라 표본에서 고르는 근사입니다. 지울 후보를 다 지워도 한도 아래로 못 내려가거나 `noeviction`이면 메모리를 늘리는 명령만 OOM 에러를 받고, 읽기는 계속됩니다.

실무에서 흔한 실수는 Spring `@Cacheable`의 기본 설정이 TTL을 붙이지 않는다는 점과 겹칩니다. 캐시와 세션이 한 서버에 있고 정책이 `volatile-lru`면, TTL 없는 캐시는 후보가 아니어서 세션만 지워집니다. `allkeys-lru`로 바꾸면 재고 같은 데이터 키까지 후보가 됩니다. 그래서 키를 잃어도 되는가로 서버를 나누는 것이 먼저이고, 캐시의 TTL은 축출과 별개로 얼마나 낡은 값을 견딜지로 정합니다.

### 예상 질문과 꼬리 질문

**Q. 만료와 축출은 무엇이 다른가요?**
만료는 개발자가 정한 시각이 지나면 키가 없어지는 것이고, 축출은 메모리가 한도를 넘었을 때 Redis가 정책에 따라 키를 골라 지우는 것입니다. 만료는 "언제"를 개발자가, 축출은 "무엇을"을 Redis가 정합니다. 다만 `volatile-*` 정책은 TTL 있는 키만 후보로 삼으므로, 둘은 TTL을 통해 연결됩니다. (본문: "키가 사라지는 세 길을 구분합니다")

- **꼬리: 그럼 TTL 없는 키는 안전한가요?**
  정책에 따라 다릅니다. `volatile-*`에서는 후보가 아니지만, `allkeys-*`에서는 모든 키가 후보입니다. 정책은 서버 설정이라 나중에 누가 바꿀 수 있으므로, 잃으면 안 되는 키는 TTL 유무에 기대기보다 `noeviction` 서버에 둡니다.

**Q. TTL이 지나면 메모리가 바로 줄어드나요?**
아닙니다. 주 서버는 그 키에 접근할 때나 주기적 정리가 찾았을 때 지웁니다. 주기적 정리는 한 번에 쓰는 시간에 한도가 있어 만료된 키가 많으면 여러 주기에 걸칩니다. 그동안 만료된 키도 `used_memory`와 `keys` 수에 들어갑니다. (본문: "만료된 키는 언제 메모리에서 사라지는가")

- **꼬리: 복제본에서는요?**
  기본 설정의 읽기 전용 복제본은 만료된 키를 읽을 때 없는 것으로 답하지만 스스로 지우지 않고, 주 서버가 보내는 삭제를 따릅니다. 그래서 복제본의 키 수와 메모리가 주 서버보다 늦게 줄 수 있습니다.

**Q. 세션 값을 갱신했더니 TTL이 사라졌습니다. 왜 그런가요?**
옵션 없는 `SET`은 값을 통째로 바꾸는 명령이라 TTL을 지웁니다. Spring의 `opsForValue().set(key, value)`가 이 명령을 보냅니다. 시간을 함께 넘기는 오버로드를 쓰거나, 남은 시간을 그대로 두려면 `SET ... KEEPTTL`을 씁니다. `INCR`이나 `HSET`처럼 값을 고치는 명령은 TTL을 유지합니다. (본문: "TTL은 언제 유지되고, 언제 사라지는가")

**Q. 축출은 언제 일어나나요? 백그라운드에서 미리 지워 두나요?**
미리 지우지 않습니다. `maxmemory`가 정해져 있을 때, 명령을 실행하기 전마다 메모리가 한도를 넘었는지 확인하고 넘었으면 그 자리에서 지웁니다. `GET`이 와도 그 전에 돕니다. 한 번에 쓰는 시간에 한도가 있어, 기본 설정에서 약 500마이크로초를 넘으면 남은 일을 타이머 작업으로 넘깁니다. 그래서 메모리는 잠시 한도를 넘을 수 있습니다. (본문: "언제: 한도를 넘은 상태에서 명령이 올 때마다")

- **꼬리: 축출이 지연을 만드나요?**
  한도 근처에서 쓰기가 계속 들어오면 명령마다 축출이 붙어 지연이 조금씩 더해집니다. 원소를 따로 담은 큰 컬렉션을 축출하면 기본 설정에서는 메인 스레드가 그 키를 해제하는 동안 다른 명령이 기다립니다. `lazyfree-lazy-eviction`을 켜면 해제 작업량이 큰 값의 해제를 보조 스레드로 넘기는 대신 메모리가 늦게 돌아옵니다.
- **꼬리: 키 데이터는 그대로인데 축출이 일어날 수 있나요?**
  소스를 보면 축출 판단에서 빼는 것은 복제 버퍼 일부와 AOF 버퍼뿐이고, 연결의 응답 버퍼는 셉니다. 그래서 큰 응답이 버퍼에 쌓이면 키 데이터가 늘지 않아도 한도를 넘어 키가 축출될 수 있다고 해석합니다.

**Q. LRU 정책이면 가장 오래 안 쓴 키가 먼저 지워지나요?**
꼭 그렇지는 않습니다. Redis는 모든 키를 정렬하지 않고, 기본 5개씩 무작위로 뽑은 표본들 중에서 가장 지울 만한 키를 지웁니다. 표본 수를 늘리면 정확해지는 대신 CPU를 더 씁니다. (본문: "무엇을: 정책이 후보 범위와 기준을 정합니다")

**Q. 캐시와 세션이 한 Redis에 있습니다. `volatile-lru`면 캐시부터 지워지지 않나요?**
캐시 항목에 TTL이 없으면 후보가 아니므로 세션만 지워집니다. Spring `@Cacheable`의 기본 설정은 TTL을 붙이지 않습니다. 세션을 다 지워도 한도 아래로 내려가지 않으면 그 뒤로는 `noeviction`처럼 쓰기가 OOM 에러를 받습니다. (본문: "예시에 정책을 대 보면")

- **꼬리: 그럼 `allkeys-lru`로 바꾸면 되나요?**
  캐시는 지워지지만 TTL 없는 재고 같은 데이터 키도 후보가 됩니다. 잘 안 팔리는 상품의 재고 키가 오래 안 쓰였다면 사라질 수 있습니다. 데이터가 섞인 서버를 `allkeys-*`로 바꾸는 것은 데이터를 잃어도 된다고 정하는 것과 같습니다.
- **꼬리: 캐시에 TTL을 다 붙이면 해결되나요?**
  `volatile-lru`에서 캐시가 후보가 되고 TTL 없는 재고는 빠집니다. 하지만 세션도 TTL이 있어서 캐시와 함께 후보입니다. 세션을 잃으면 안 된다면 서버를 나누는 수밖에 없습니다.

**Q. 데이터 서버를 `noeviction`으로 두면 메모리가 찼을 때 어떻게 되나요?**
키를 잃지 않는 대신 `SET`, `HSET`처럼 메모리를 늘릴 수 있는 명령이 `OOM command not allowed` 에러를 받습니다. `GET`이나 `DEL`은 계속 됩니다. 그래서 "읽기는 되는데 쓰기만 실패"하는 모양으로 드러납니다. `used_memory`가 `maxmemory`에 다가가면 경보를 울려 미리 늘리는 운영이 함께 필요합니다. (본문: "지우지 못하면: 쓰기 명령이 OOM 에러를 받습니다")

- **꼬리: `maxmemory`를 아예 안 정하면요?**
  64비트 서버에서 기본값 0은 한도가 없다는 뜻이라 축출도 OOM 에러도 없습니다. Redis는 운영체제가 줄 수 있는 만큼 메모리를 쓰고, 그 끝에서 무슨 일이 생기는지는 운영체제와 환경에 달려 있습니다. 한도를 정해 Redis 안에서 실패가 드러나게 하는 편이 원인을 찾기 쉽습니다.

**Q. 로그인이 자꾸 풀린다는 문의가 왔습니다. 무엇부터 보나요?**
`INFO stats`의 `evicted_keys`가 늘고 있으면 세션이 축출로 사라졌을 가능성을 후보로 둡니다. 이 값은 서버 전체의 축출 수라 어느 키가 지워졌는지는 알려 주지 않습니다. 정책이 `volatile-*`인지, `INFO keyspace`에서 `keys`가 `expires`보다 훨씬 많은지 봅니다. 그다음 키 종류별 표본에 `TTL`을 보내, TTL이 있는 종류가 세션뿐인지 봅니다. 표본은 단서일 뿐이라, 후보가 세션뿐이라고 확정하려면 각 키를 만들고 갱신하는 코드가 TTL을 어떻게 거는지까지 확인합니다. (본문: "증상과 확인 순서")

**Q. Spring 캐시의 TTL은 어떤 기준으로 정하나요?**
"원본이 바뀐 뒤 얼마나 낡은 값을 보여 줘도 되는가"로 정합니다. 축출을 위해 TTL을 붙이는 것은 서버를 나누지 못해 `volatile-*`를 쓸 때의 부수 효과입니다. 많은 항목이 같은 시각에 만료되지 않도록 무작위 값을 더해 흩고, `entryTtl`과 함께 `enableTimeToIdle()`을 쓰면 자주 읽히는 항목은 만료되지 않으므로 원본 변경 시 무효화를 따로 해야 합니다. (본문: "Spring에서 확인할 것")
