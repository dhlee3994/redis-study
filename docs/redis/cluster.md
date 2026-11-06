# Redis Cluster: 데이터를 여러 노드에 나누면 무엇이 안 되고, 클라이언트는 맞는 노드를 어떻게 찾는가

> **핵심 질문**: 데이터를 Redis 노드 여러 대에 나눠 담으면, 노드 한 대에서 되던 일 중 무엇이 안 되는가? 클라이언트는 키를 들고 어느 노드로 가야 하는지 어떻게 알고, 슬롯이 옮겨질 때는 어떻게 따라가는가? 그 결과 원자성과 분산 락의 보장은 어떻게 달라지는가?
>
> **범위**: Redis Cluster의 키 배치(해시 슬롯, 해시 태그), 여러 키를 다루는 명령의 제약, 리샤딩 중의 리다이렉트(MOVED, ASK, TRYAGAIN), Lettuce와 Spring의 토폴로지 갱신을 다룹니다. 장애를 판정하고 복제본을 승격하는 절차와 장애 조치 때 클라이언트가 겪는 일은 한 줄 언급에 그칩니다. 장애 조치 때 Sentinel 구성과 클러스터에서 클라이언트가 겪는 일은 [Redis 장애 조치와 클라이언트](./failover-client.md)에서 다룹니다. 클러스터를 만들고 노드를 더하는 운영 절차, 복제본 읽기 설정은 다루지 않습니다. 노드를 더하고 슬롯을 옮길 때 앱이 준비할 것은 [클러스터 리샤딩](./cluster-resharding.md)에서, 클러스터를 도입할지 판단하는 기준은 [클러스터 도입 판단](./cluster-adoption.md)에서 다룹니다.

## 배경 지식

### 이 문서의 예시: 주 서버 3대에 "1인당 1개" 주문

문서 끝까지 다음 장면 하나를 씁니다. [Redis 원자성](./atomicity.md)의 재고 예시를 클러스터로 옮긴 것입니다.

- Redis Cluster는 주 서버(마스터) 3대 A, B, C로 이루어져 있습니다. 각자 복제본을 하나씩 두지만 이 문서에서는 대부분 생략합니다.
- 상품 1의 재고는 String 키에, 이 상품을 산 사용자 목록은 Set 키에 있습니다.
- 주문 API(Spring Boot, Lettuce)는 "재고가 남았고, 이 사용자가 아직 사지 않았으면 재고를 1 줄이고 사용자를 목록에 넣는다"를 Lua 스크립트 하나로 실행합니다. 두 키를 한 번에 다뤄야 합니다.
- 나중에 특가 행사로 상품 1에 요청이 몰리고, 운영자가 노드 D를 더해 슬롯 일부를 D로 옮깁니다.

```text
 주문 API (JVM, Lettuce)
 ┌──────────────────────────┐
 │ 슬롯 지도(클라이언트 사본) │
 │  0~5460     → A          │──┐
 │  5461~10922 → B          │──┼──▶ 키의 슬롯을 계산해 맡은 노드로 직접 보냄
 │  10923~16383 → C         │──┘
 └──────────────────────────┘
        A (0~5460)     B (5461~10922)     C (10923~16383)
           ▲  ▲           ▲  ▲               ▲  ▲
           └──┴─── 노드끼리 클러스터 버스로 상태를 주고받음 ───┘
```

화살표 중 위쪽은 명령이 가는 방향이고, 아래쪽은 노드끼리 주고받는 상태 정보입니다. 복제본, 연결 풀, 노드 D는 그림에서 뺐습니다. 복제본은 "장애 조치는 지도가 바뀌는 또 하나의 경우입니다"에서, 노드 D는 "리샤딩 중에는 ASK와 TRYAGAIN이 생깁니다"에서 다룹니다. 슬롯 범위(0~5460, 5461~10922, 10923~16383)는 `redis-cli --cluster create`로 주 서버 3대를 만들 때 슬롯을 고르게 나눈 결과입니다.

이 문서에 나오는 슬롯 번호는 이 문서가 공식 문서의 정의(아래 "해시 함수와 해시 슬롯")대로 계산한 값입니다. Redis에서는 `CLUSTER KEYSLOT <키>`로 확인할 수 있습니다.

### 샤딩: 데이터를 나눠 담아 메모리와 쓰기를 나눕니다

- **한 줄**: 샤딩은 전체 데이터를 여러 서버에 겹치지 않게 나눠 담는 방식입니다. 서버마다 데이터의 일부만 갖습니다.
- **비유**: 도서관 한 곳에 책이 넘쳐 분관 세 곳을 만들고, 책을 분관마다 나눠 둡니다. 같은 책이 여러 분관에 있지 않으므로, 책을 찾으려면 "이 책은 어느 분관에 있는가"를 먼저 알아야 합니다.
- **실제로는**: Redis Cluster는 키를 단위로 나눕니다. 키 하나는 주 서버 하나에만 있고, 그 주 서버의 복제본이 사본을 갖습니다. 그래서 메모리와 쓰기가 주 서버 수만큼 나뉩니다. 이것은 [Redis 복제](./replication.md)와 다릅니다. 복제는 같은 데이터를 여러 서버에 두어 읽기와 장애 대비를 얻지만, 쓰기는 여전히 주 서버 한 대가 모두 받습니다. **비유와 다른 점**: 도서관은 책 한 권을 한 분관에 두지만, Redis가 나누는 단위는 키이지 키 안의 원소가 아닙니다. 원소가 100만 개인 Hash 키 하나는 통째로 한 노드에 있습니다([Hot Key와 Big Key](./hot-key-big-key.md)).
- **이 주제에서는**: 재고 키와 구매자 키가 서로 다른 노드에 놓이면, 한 노드의 메인 스레드가 둘을 함께 처리할 방법이 없어집니다. 이 문서의 대부분은 이 사실에서 나옵니다.

### 해시 함수와 해시 슬롯

- **한 줄**: 해시 함수는 임의 길이의 바이트를 정해진 범위의 숫자로 바꾸는 계산입니다. Redis Cluster는 키 이름을 16,384개 칸(해시 슬롯, 이하 슬롯) 중 하나의 번호로 바꾸고, 슬롯마다 맡을 노드를 정해 둡니다.
- **비유**: 우편번호입니다. 주소(키)만 보면 우편번호(슬롯)가 정해지고, 우편번호마다 담당 우체국(노드)이 있습니다. 담당 우체국을 바꿀 때는 우편번호 구역을 통째로 넘깁니다.
- **실제로는**: 슬롯 번호는 키 바이트의 해시 값을 16,384로 나눈 나머지입니다(공식 문서 cluster-spec, "Key distribution model"). 같은 바이트에는 항상 같은 값이 나옵니다. 계산에는 키 이름만 쓰므로 노드 수가 바뀌어도 키의 슬롯은 그대로입니다. 바뀌는 것은 "그 슬롯을 어느 노드가 맡는가"뿐입니다. Kotlin의 `HashMap`이 키의 `hashCode()`로 버킷을 고르는 것과 닮았지만, `HashMap`은 버킷 수를 늘리면 키가 다른 버킷으로 다시 흩어지고, Redis Cluster는 슬롯 수를 16,384로 고정해 두고 슬롯과 노드의 짝만 바꾼다는 점이 다릅니다.
- **이 주제에서는**: 예시의 두 키는 다음과 같이 다른 노드에 놓입니다.

| 키 | 슬롯 | 맡은 노드 |
| --- | --- | --- |
| `stock:item:1` | 8466 | B |
| `buyers:item:1` | 13216 | C |

### 메인 스레드, 원자성, 왕복

- **메인 스레드와 원자성**: Redis 노드 하나는 메인 스레드 하나가 명령을 하나씩 끝까지 실행하므로, 명령 하나나 Lua 스크립트 하나는 그 노드 안에서 다른 명령과 섞이지 않습니다. 이 성질이 노드 하나 안에서만 성립한다는 것이 이 문서의 출발점입니다. 원리는 [Redis 원자성](./atomicity.md)과 [경쟁 상태와 원자성](../cs/race-condition-and-atomicity.md)에서 다룹니다.
- **왕복**: 명령 하나를 보내고 응답을 받기까지의 과정입니다. 클라이언트가 틀린 노드로 보내면 "저쪽으로 가라"는 응답을 받고 다시 보내야 하므로 왕복이 하나 더 듭니다.

### 비동기 복제와 장애 조치

- **한 줄**: 주 서버는 복제본이 쓰기를 받았다는 확인을 기다리지 않고 클라이언트에 응답합니다(비동기 복제). 주 서버가 죽으면 클러스터는 그 복제본 하나를 새 주 서버로 올립니다(장애 조치).
- **이 주제에서는**: 장애 조치가 일어나면 "어느 노드가 그 슬롯을 맡는가"가 바뀌므로, 클라이언트의 슬롯 지도도 바뀌어야 합니다. 또 복제되기 전에 응답한 쓰기는 사라질 수 있습니다. 원리와 유실 범위는 [Redis 복제](./replication.md)의 "장애 조치 때 무엇을 잃는가"에서 다룹니다.

## 키는 어떻게 노드 하나에 정해지는가

### 슬롯 수를 고정하고, 슬롯을 노드에 맡깁니다

키를 노드에 나누는 가장 단순한 방법은 "해시값 mod 노드 수"입니다. 노드가 3대면 해시값을 3으로 나눈 나머지로 노드를 고릅니다. 문제는 노드를 4대로 늘리는 순간입니다. 나누는 수가 바뀌므로 대부분의 키가 다른 노드를 가리키게 되고, 그만큼의 데이터를 옮겨야 합니다.

Redis Cluster는 해시값을 노드 수가 아니라 고정된 16,384로 나눕니다. 노드 수와 상관없이 키의 슬롯은 변하지 않습니다. 노드 D를 더할 때는 A, B, C가 맡던 슬롯 일부를 D에 넘기고, 넘긴 슬롯에 든 키만 옮깁니다. 노드를 빼거나 부하를 다시 맞출 때도 같은 동작, 즉 "슬롯을 다른 노드로 옮기기"입니다(공식 문서 cluster-spec, "Live reconfiguration"). 이렇게 슬롯을 옮기는 작업을 리샤딩(resharding)이라고 합니다. 공식 문서는 이 방식이 일관된 해싱(consistent hashing)이 아니라고 밝힙니다(공식 문서 scaling, "Redis Cluster data sharding").

대가는 옮기는 단위가 슬롯이라는 점입니다. 슬롯 하나에 든 키들은 한 노드에서 다른 노드로 함께 옮겨질 뿐, 둘로 나뉘지 않습니다. 슬롯 하나의 부하가 크면 노드를 늘려도 그 부하는 나뉘지 않습니다("해시 태그는 슬롯을 맞추는 대신 부하를 한 노드에 모읍니다").

### 노드는 요청을 대신 전달하지 않습니다

슬롯과 노드의 짝(이하 슬롯 지도)은 노드들이 클러스터 버스로 서로 주고받으며 맞춥니다. 클러스터 버스는 노드끼리만 쓰는 별도 TCP 포트로, 기본값은 클라이언트 포트에 10,000을 더한 번호입니다(6379면 16379). 클라이언트는 이 포트에 접속하지 않습니다(공식 문서 scaling, "Redis Cluster TCP ports").

노드는 자기가 맡지 않은 슬롯의 요청을 받으면, 그 요청을 맡은 노드에 대신 전달하지 않고 "그 슬롯은 저 노드가 맡는다"는 에러로 답합니다(공식 문서 cluster-spec, "Client and Server roles in the Redis cluster protocol"). 중간에 프록시가 없으므로 정상일 때 요청은 왕복 한 번에 끝납니다. 대신 **맞는 노드를 고르는 책임은 클라이언트에** 있습니다. 그래서 클러스터를 쓰려면 클러스터를 이해하는 클라이언트(Lettuce의 `RedisClusterClient`)가 필요하고, 클라이언트는 슬롯 지도의 사본을 들고 다닙니다. 사본이 낡았을 때 무슨 일이 생기는지는 "클라이언트는 슬롯 지도를 들고 직접 노드를 고릅니다"에서 다룹니다.

## 한 노드에서 되던 일 중 무엇이 안 되는가

### 한 번의 실행이 다루는 키는 모두 같은 슬롯이어야 합니다

예시의 스크립트를 그대로 보내면 다음 에러를 받습니다.

```text
EVAL "<재고·구매자 스크립트>" 2 stock:item:1 buyers:item:1 user:42
-CROSSSLOT Keys in request don't hash to the same slot
```

`stock:item:1`(슬롯 8466, B)과 `buyers:item:1`(슬롯 13216, C)이 다른 슬롯이기 때문입니다. 노드 하나의 메인 스레드는 다른 노드의 키를 실행 도중에 읽거나 바꿀 수 없으므로, 클러스터는 한 번의 실행(명령 하나, `MULTI`/`EXEC` 하나, 스크립트 하나)이 다루는 키가 모두 같은 슬롯에 있을 때만 받습니다. 검사 기준은 노드가 아니라 슬롯입니다. 두 키가 지금 같은 노드에 있어도 슬롯이 다르면 거부됩니다. 슬롯은 언제든 다른 노드로 옮겨질 수 있으므로, "지금 같은 노드"는 오래 보장되지 않기 때문입니다.

이 규칙의 세부(실행 단위마다 검사하는 시점, 스크립트가 선언하지 않은 키에 접근할 때 중간 실패가 남는 경우)는 [Redis 원자성](./atomicity.md)의 "여러 키를 함께 다루면 클러스터에서는 같은 슬롯에 있어야 합니다"에서 다룹니다. 여기서는 그 결론만 씁니다. **스크립트가 다루는 키는 모두 `KEYS`로 넘기고, 같은 슬롯에 둡니다.** 같은 슬롯에 두는 방법은 해시 태그입니다("해시 태그는 슬롯을 맞추는 대신 부하를 한 노드에 모읍니다").

### 그 밖에 달라지는 것

| 노드 한 대에서 | 클러스터에서 | 이유 |
| --- | --- | --- |
| `SELECT 1`로 다른 DB 번호를 씀 | DB 0만 씁니다. `SELECT`에 0이 아닌 번호를 주면 "SELECT is not allowed in cluster mode" 에러 | 공식 문서 cluster-spec, "Implemented subset" |
| `KEYS`, `SCAN`이 전체 키를 훑음 | 접속한 노드의 키만 훑습니다. 전체를 보려면 모든 주 서버에 따로 보내야 합니다 | 노드는 자기 슬롯의 키만 가짐 |
| 키 이벤트 알림(keyspace notification)을 한 연결로 모두 받음 | 이벤트는 그 키를 가진 노드에서만 나옵니다. 전부 받으려면 모든 노드를 구독해야 합니다 | 공식 문서 keyspace-notifications, "Events in a cluster" |
| `PUBLISH`가 그 서버의 구독자에게 감 | 일반 `PUBLISH`는 클러스터 버스로 모든 노드에 퍼지므로, 노드를 늘려도 메시지 하나를 모든 노드가 받아야 하는 부담은 나뉘지 않습니다. `SPUBLISH`(샤드 Pub/Sub)는 채널을 슬롯에 배정해 그 샤드, 즉 그 슬롯을 맡은 주 서버와 그 복제본들 안에서만 퍼뜨립니다 | 공식 문서 pubsub, "Sharded Pub/Sub" |

키 이벤트 알림의 차이는 만료 이벤트로 후속 작업을 돌리는 코드에서 특히 문제가 됩니다. Spring Data Redis 참조 문서는 클러스터에서 키 이벤트에 의존하지 말라고 경고합니다. Pub/Sub 구독이 임의의 노드 하나에 붙어 그 샤드의 이벤트만 받기 때문입니다(Spring Data Redis 참조 문서 cluster).

### 클라이언트가 나눠 보내면 에러는 사라지지만 원자성도 사라집니다

`CROSSSLOT`이 나지 않는다고 안전한 것은 아닙니다. Lettuce는 일부 여러 키 명령을 받으면 키를 슬롯별로 나눠 각 노드에 따로 보내고 결과를 합칩니다. Lettuce 6.6.0에서 이렇게 나누는 명령은 `MGET`, `MSET`, `MSETNX`, `DEL`, `UNLINK`, `EXISTS`, `TOUCH`입니다. `KEYS`와 `SCAN`은 모든 주 서버에 보내 결과를 모읍니다.

나눠 보낸 명령은 각각 다른 노드의 다른 실행입니다. 그래서 노드 한 대에서 그 명령이 주던 보장이 사라집니다.

- **`MSET`이 일부만 반영될 수 있습니다.** 노드 한 대에서 `MSET`은 모든 키를 한 번에 씁니다. 나눠 보내면 한 노드의 `MSET`은 성공하고 다른 노드의 `MSET`은 타임아웃이나 연결 오류로 실패할 수 있고, 다른 클라이언트는 그 사이에 일부만 바뀐 상태를 볼 수 있습니다.
- **`MSETNX`의 "모두 없을 때만"이 슬롯 묶음마다 따로 판정됩니다.** 노드 한 대라면 키 하나라도 있으면 아무것도 쓰지 않습니다. 나눠 보내면 키가 없는 슬롯의 묶음은 쓰이고, 키가 있는 슬롯의 묶음만 쓰이지 않습니다. Lettuce는 묶음 중 하나라도 실패하면 `false`를 돌려주지만, 이미 쓰인 묶음을 되돌리지는 않습니다.
- **`MGET`의 결과가 한 시점의 값이 아닙니다.** 슬롯마다 다른 순간에 읽은 값이 합쳐집니다.

Spring Data Redis도 비슷한 일을 합니다. 클러스터 연결에서 두 키의 슬롯이 다른 `rename`은 `DUMP`로 값을 꺼내 `RESTORE`로 새 키에 넣고 옛 키를 `DEL`하는 세 명령으로 바꿔 실행합니다. 중간에 실패하면 두 키가 모두 남을 수 있습니다.

이 동작은 "여러 키를 편하게 읽고 쓰는 도구"로는 쓸모 있지만, 원자성이 필요한 곳에는 맞지 않습니다. 그런 곳은 해시 태그로 슬롯을 맞추고 Lua 스크립트로 묶습니다.

## 해시 태그는 슬롯을 맞추는 대신 부하를 한 노드에 모읍니다

### 규칙: 첫 `{`와 그 뒤의 첫 `}` 사이만 계산합니다

키 이름에 `{…}`가 있으면 중괄호 안의 문자열만으로 슬롯을 계산합니다. 정확한 규칙은 다음과 같습니다(공식 문서 cluster-spec, "Hash tags").

1. 키에서 처음 나오는 `{`를 찾습니다. 없으면 키 전체를 계산합니다.
2. 그 뒤에서 처음 나오는 `}`를 찾습니다. 없거나, `{` 바로 뒤가 `}`라서 사이가 비어 있으면 키 전체를 계산합니다.
3. 둘 다 있으면 그 사이의 바이트만 계산합니다.

예시에 적용하면 다음과 같습니다.

| 키 | 계산에 쓰는 부분 | 슬롯 | 노드 |
| --- | --- | --- | --- |
| `stock:{item:1}` | `item:1` | 13307 | C |
| `buyers:{item:1}` | `item:1` | 13307 | C |
| `stock:{item:2}` | `item:2` | 920 | A |
| `foo{}{bar}` | 키 전체(첫 쌍이 비어 있음) | 8363 | B |

두 키를 `stock:{item:1}`, `buyers:{item:1}`로 바꾸면 같은 슬롯 13307에 놓이므로, 예시의 스크립트가 실행됩니다. 공식 문서의 예처럼 `foo{bar}{zap}`은 `bar`만, `foo{{bar}}zap`은 `{bar`를 계산합니다. 첫 쌍에서 멈추기 때문입니다.

잘못 읽기 쉬운 점이 둘 있습니다.

- **계산은 클라이언트가 보낸 바이트로 합니다.** Spring의 `RedisTemplate`이 키 직렬화기를 거쳐 만든 바이트가 그대로 계산 대상입니다. 키를 JSON 객체로 직렬화하면 직렬화기가 넣은 `{`가 해시 태그의 시작으로 읽혀, 의도하지 않은 슬롯 묶음이 생길 수 있습니다. Spring Data Redis 참조 문서도 JSON 키 직렬화기의 구조가 바뀌면 슬롯 계산이 곧바로 바뀐다고 경고합니다(cluster, "Working with RedisTemplate and ClusterOperations"). 키에는 문자열 직렬화기(`StringRedisSerializer`)를 쓰고, `redis-cli`의 `CLUSTER KEYSLOT`으로 실제 키의 슬롯을 확인합니다. 직렬화기 선택은 [Redis 직렬화](./serializer.md)에서 다룹니다.
- **태그가 같으면 이름이 달라도 같은 슬롯입니다.** 같은 태그의 키는 몇 개든, 어떤 자료형이든 모두 한 슬롯, 한 노드에 놓입니다. 다음 절의 비용이 여기서 나옵니다.

### 태그 하나는 노드 하나입니다

해시 태그는 "함께 다룰 키를 한 노드에 모으는 장치"입니다. 모은 만큼 나뉘지 않습니다.

특가 행사로 상품 1에 초당 수만 건의 주문이 몰린다고 합시다. 재고 키와 구매자 키만이 아닙니다. [Redis 분산 락](./distributed-lock.md)의 락 키 `lock:{item:1}`과 펜싱 카운터 `fence:{item:1}`(락을 얻을 때마다 1씩 늘려 락 보유자에게 번호를 주는 키로, DB가 낡은 보유자의 늦은 쓰기를 번호로 거절하게 합니다. 락과 번호가 어긋나지 않게 락을 거는 스크립트에서 함께 바꿉니다), 재시도를 막으려고 스크립트에서 함께 기록하는 주문 처리 기록 키처럼, 상품 1과 함께 원자적으로 다루려고 `{item:1}`을 붙인 키는 모두 슬롯 13307, 노드 C에 있습니다. 상품 1의 모든 주문은 C의 메인 스레드 하나가 줄 세워 처리합니다. 노드를 10대로 늘려도 슬롯 13307은 어느 한 노드에 통째로 있을 뿐이므로, 이 부하는 나뉘지 않습니다. 키 하나에 몰린 부하가 노드를 늘려도 나뉘지 않는 이유는 [Hot Key와 Big Key](./hot-key-big-key.md)의 "노드를 늘려도 키 하나의 부하는 나뉘지 않습니다"와 같습니다. 해시 태그는 그 범위를 키 하나에서 태그 하나로 넓힙니다.

그래서 태그의 단위가 설계의 핵심입니다.

| 태그 단위 | 예 | 결과 |
| --- | --- | --- |
| 함께 원자적으로 다뤄야 하는 가장 작은 묶음 | `{item:1}` | 상품마다 슬롯이 흩어집니다. 한 상품에 몰린 부하만 한 노드에 남습니다 |
| 너무 큰 묶음 | 모든 상품에 `{stock}` | 전체 재고가 한 슬롯, 한 노드에 모입니다. 클러스터를 쓰는 의미가 사라집니다 |
| 태그 없음 | `stock:item:1` | 고르게 흩어지지만, 한 스크립트로 묶을 수 없습니다 |

태그를 정하는 질문은 "이 키들을 한 번의 실행으로 함께 다뤄야 하는가"입니다. 함께 바꾸는 경우가 대표적이고, 여러 값을 다른 쓰기가 끼지 않은 한 시점의 값으로 함께 읽어야 하는 경우도 여기에 듭니다(같은 슬롯의 `MGET` 하나는 한 번의 실행이지만, 슬롯별로 나눈 `MGET`은 아닙니다). 단지 같은 화면에 함께 보여 준다는 이유로, 한 시점의 값일 필요도 없는데 태그를 붙이면 부하만 한 노드에 모읍니다. 그런 키는 태그 없이 두고 Lettuce가 나눠 보내게 하는 편이 낫습니다.

### 몰린 슬롯은 슬롯 통계로 찾습니다

Redis 8.2의 `CLUSTER SLOT-STATS`는 그 노드가 맡은 슬롯별 통계를 돌려줍니다. 키 수(`key-count`)는 항상 나옵니다. 슬롯별 CPU 시간(`cpu-usec`)과 네트워크 입출력 바이트(`network-bytes-in`, `network-bytes-out`)는 `cluster-slot-stats-enabled`를 켜야 집계되며, 기본값은 꺼짐입니다. 소스 주석은 키 수 말고의 통계에 성능 비용이 따른다고 적습니다. `CLUSTER SLOT-STATS ORDERBY cpu-usec LIMIT 10`처럼 정렬해 보면, 한 노드만 바쁠 때 그 노드에서 어느 슬롯이 원인인지 좁힐 수 있습니다. 단, 다른 슬롯 접근을 허용하는 스크립트의 실행은 슬롯 통계에 들어가지 않습니다. CPU 시간도, 네트워크 바이트도 그 슬롯에 더해지지 않습니다. 첫 줄에 `#!lua`처럼 실행 조건을 선언하는 줄(shebang)이 없는 `EVAL` 스크립트가 여기에 해당합니다. 이런 스크립트는 같은 노드 안이면 다른 슬롯의 키에도 접근할 수 있는 호환 모드로 실행되기 때문입니다([Redis 원자성](./atomicity.md)의 "Lua는 실행 중에도 검사하므로 중간에 실패할 수 있습니다"). 예시의 주문 스크립트가 shebang 없이 쓰였다면 상품 1의 슬롯은 CPU 순위에 나타나지 않을 수 있으므로, 통계가 낮다는 이유로 그 슬롯을 배제하지 않습니다. 슬롯이 정해지면 그 슬롯에 들어가는 태그나 키를 애플리케이션의 키 규칙에서 찾습니다. 슬롯 통계는 노드마다 따로 보내야 하고, 키 이름까지 알려 주지는 않습니다.

## 클라이언트는 슬롯 지도를 들고 직접 노드를 고릅니다

### MOVED: 그 슬롯은 이제 저 노드가 맡습니다

클라이언트가 틀린 노드에 명령을 보내면 노드는 이렇게 답합니다.

```text
EVAL "<스크립트>" 2 stock:{item:1} buyers:{item:1} user:42   (A에 보냄)
-MOVED 13307 10.0.0.3:6379
```

에러에는 슬롯 번호와 그 슬롯을 지금 맡은 노드의 주소가 들어 있습니다. 클라이언트는 같은 명령을 그 주소로 다시 보냅니다. MOVED는 "이 슬롯은 앞으로도 저 노드가 맡는다"는 뜻이므로, 클라이언트는 지도를 고쳐 다음부터 바로 그쪽으로 보내는 것이 맞습니다(공식 문서 cluster-spec, "MOVED Redirection"). 공식 문서는 MOVED를 받으면 슬롯 하나만 고치기보다 `CLUSTER SHARDS`(또는 이전 방식의 `CLUSTER SLOTS`)로 지도 전체를 다시 받는 편이 낫다고 권합니다. 리다이렉트가 생겼다면 슬롯 여러 개가 한꺼번에 바뀌었을 가능성이 높기 때문입니다. 장애 조치로 복제본이 승격되면 옛 주 서버가 맡던 슬롯이 모두 바뀝니다.

### 리샤딩 중에는 ASK와 TRYAGAIN이 생깁니다

슬롯을 옮기는 동안에는 키가 두 노드에 나뉘어 있습니다. 슬롯 13307을 C에서 새 노드 D로 옮긴다고 합시다. 옮기는 도구(`redis-cli --cluster reshard`)는 다음 순서로 일합니다(공식 문서 cluster-spec, "Live reconfiguration").

1. D에 "13307을 C에서 받는 중(IMPORTING)", C에 "13307을 D로 보내는 중(MIGRATING)"이라고 표시합니다.
2. C에 있는 슬롯 13307의 키를 몇 개씩 골라 `MIGRATE`로 D에 보냅니다. `MIGRATE`는 키를 D에 넣고, D가 받았다고 답하면 C에서 지웁니다. 그래서 정상적으로 끝난 이동에서 클라이언트가 보는 키는 C와 D 중 한쪽에만 있습니다. 다만 D가 키를 넣었는데 C가 응답을 받지 못하면(타임아웃이나 연결 오류) 양쪽에 키가 남을 수 있습니다(공식 문서 migrate).
3. 다 옮기면 슬롯의 주인을 D로 바꾸고 표시를 지웁니다.

1단계 표시가 붙은 뒤로 예시의 주문 스크립트(`stock:{item:1}`, `buyers:{item:1}`)를 C에 보내면, C는 두 키가 자기에게 있는지 보고 다음처럼 답합니다. 표시가 붙기 전에는 키가 있든 없든 C가 그대로 실행합니다.

| 단계 | C에 있는 키 | D에 있는 키 | C의 응답 | 클라이언트가 할 일 |
| --- | --- | --- | --- | --- |
| 표시 후 키를 옮기기 전, 구매자가 이미 있음 | 둘 다 | 없음 | 실행 | 없음 |
| 표시 후 키를 옮기기 전, 첫 구매(구매자 키가 아직 없음) | `stock` | 없음 | `-TRYAGAIN` | 잠시 뒤 다시 보냄 |
| `stock`만 옮겨짐 | `buyers` | `stock` | `-TRYAGAIN` | 잠시 뒤 다시 보냄 |
| 둘 다 옮겨짐 | 없음 | 둘 다 | `-ASK 13307 <D의 주소>` | D에 `ASKING`을 보낸 뒤 같은 명령을 보냄 |
| 슬롯 주인이 D로 바뀜 | 없음 | 둘 다 | `-MOVED 13307 <D의 주소>` | D로 다시 보내고 지도를 고침 |

- **ASK는 "이번 한 번만 저쪽에 물어보라"입니다.** C가 ASK를 주는 조건은 "요청한 키를 C에서 하나도 찾지 못했다"입니다. 키가 D로 갔을 수도 있고 아직 어디에도 없는 새 키일 수도 있으며, C는 D에 키가 있는지 확인하지 않습니다. 슬롯의 주인은 아직 C입니다. 다음 명령은 아직 C에 있는 키일 수 있으므로, 클라이언트는 지도를 고치지 않고 다음에도 C에 먼저 보냅니다(공식 문서 cluster-spec, "ASK redirection").
- **`ASKING`은 받는 쪽의 문을 한 번 여는 명령입니다.** D는 IMPORTING인 슬롯의 명령을 `ASKING` 바로 뒤에 올 때만 실행하고, 아니면 아직 주인인 C로 `MOVED`를 돌려줍니다. 지도를 너무 일찍 고친 클라이언트가 D에 직접 보내도 C로 돌려보내지는 이유입니다. `ASKING`의 효과는 다음 명령 하나(트랜잭션 안이면 그 트랜잭션)가 끝나면 사라집니다.
- **TRYAGAIN은 키가 여럿일 때만 생기며, C와 D 양쪽에서 나옵니다.** 키가 하나면 C에 있으면 C가, 없으면 D가 처리하면 됩니다. 키가 여럿이면 두 곳에서 검사합니다. C(보내는 쪽)는 일부는 있고 일부는 없을 때 거부합니다. D(받는 쪽)는 `ASKING`이 붙어 왔더라도 서로 다른 키 중 하나라도 자기에게 없으면 거부합니다. 키가 아직 없는 경우도 "없는 키"로 셉니다. 첫 구매에서 이 두 검사가 이어지는 순서는 이렇습니다. 재고 키가 C에 남아 있는 동안은 구매자 키가 없어 C가 거부하고, 재고 키가 D로 간 뒤에는 C가 두 키를 모두 못 찾아 ASK를 주지만 D에도 구매자 키가 없어 D가 다시 거부합니다. 그래서 새 키를 만드는 스크립트는 슬롯을 옮기는 내내 실패할 수 있습니다. 공식 문서도 리샤딩 중에는 여러 키 연산을 한동안 쓸 수 없고 키 하나짜리 연산은 늘 쓸 수 있다고 적습니다(cluster-spec, "Implemented subset", "Multi-keys operations").

TRYAGAIN은 명령을 실행하기 전에 거부한 것이므로, 같은 명령을 다시 보내도 그 전송 때문에 두 번 실행되지는 않습니다. 다만 그 요청에 결과를 모르는 앞선 전송(타임아웃, 연결이 끊긴 뒤 Lettuce의 자동 재전송)이 있었다면, 마지막 TRYAGAIN만 보고 한 번도 실행되지 않았다고 판단할 수 없습니다([클러스터 리샤딩](./cluster-resharding.md)). 한편 Lettuce는 MOVED와 ASK만 따라가고 TRYAGAIN은 다시 보내지 않으므로, 애플리케이션에 명령 실행 예외로 올라옵니다. 리샤딩이 있는 클러스터에서 여러 키 스크립트를 쓴다면 TRYAGAIN에 짧은 지연 후 재시도를 붙여 둡니다. 빅키가 있는 슬롯은 `MIGRATE` 한 번이 오래 걸려 TRYAGAIN 구간도 길어집니다([Hot Key와 Big Key](./hot-key-big-key.md)).

### Lettuce는 지도를 언제 받고, 언제 고치는가

Lettuce의 클러스터 클라이언트가 하는 일을 순서대로 보면 다음과 같습니다.

1. **처음 연결할 때** 설정에 적은 노드(시드 노드)에 `CLUSTER NODES`를 보내 슬롯 지도를 받습니다. Spring Boot에서는 `spring.data.redis.cluster.nodes`에 적은 주소가 시드 노드입니다. 노드 전체를 적을 필요는 없지만, 하나만 적으면 그 노드가 내려가 있을 때 처음 연결에서 지도를 받지 못해 실패합니다.
2. **명령을 보낼 때** 명령의 첫 번째 키로 슬롯을 계산해 지도에서 노드를 찾고, 그 노드의 연결로 보냅니다. 키가 없는 명령은 기본 연결로 보냅니다.
3. **MOVED나 ASK를 받으면** 응답의 주소로 같은 명령을 다시 보냅니다. ASK면 앞에 `ASKING`을 붙입니다. 따라가는 횟수의 상한은 기본 5번이고, 넘으면 에러로 끝냅니다. Spring Boot의 `spring.data.redis.cluster.max-redirects`가 이 값입니다.

여기서 중요한 점이 하나 있습니다. **Lettuce는 MOVED를 따라가기만 할 뿐, 그 응답으로 지도를 고치지 않습니다.** 지도를 다시 받는 일은 따로 켜야 하는 "토폴로지 갱신"이 맡고, Lettuce 6.6.0의 기본값은 주기 갱신도, 사건에 반응하는 갱신도 모두 꺼짐입니다. Spring Boot도 설정하지 않으면 켜지 않습니다. 그래서 기본 설정에서는 처음 받은 지도가 계속 쓰입니다.

예시에서 슬롯 13307이 D로 옮겨진 뒤를 따라가 보면 다음과 같습니다.

- 기본 설정: 상품 1의 주문은 지도대로 C에 갔다가 `MOVED 13307 <D의 주소>`를 받습니다. 그런데 D는 Lettuce가 처음 지도를 받은 뒤에 더한 노드라 지도에 없습니다. Lettuce는 기본으로 지도에 없는 노드로의 연결을 거부하므로(`validateClusterNodeMembership` 기본 `true`), 이 주문은 "This partition is not known in the cluster view"라는 `UnknownPartitionException`으로 실패합니다. 앱을 재시작하거나 갱신이 일어나기 전까지 **상품 1의 주문이 모두** 실패합니다. 이 동작은 로컬 실험에서 확인됐고, 실패는 슬롯의 주인이 바뀐 뒤가 아니라 키를 옮기기 시작한 `ASK` 단계부터 나타났습니다. 실험 조건과 결과는 [클러스터 리샤딩](./cluster-resharding.md)에 있습니다.
- 슬롯이 이미 지도에 있는 노드 사이에서 옮겨졌다면(예: C에서 A로) 연결 거부는 없습니다. 대신 주문마다 C를 거쳐 `MOVED`를 받고 A로 다시 가므로, 결과는 맞지만 주문마다 왕복이 하나 더 들고 C는 자기가 맡지도 않은 요청에 계속 답합니다.
- 갱신을 켠 설정: `MOVED`나 지도에 없는 노드가 갱신을 일으키고, 새 지도를 받은 뒤로는 D로 바로 갑니다. 갱신이 끝나기 전에 D로 가려던 명령은 여전히 연결 거부로 실패할 수 있습니다(로컬 실험에서 재현됐습니다. 직전에 다른 갱신이 있었다면 사건 기반 갱신은 그 뒤 30초 동안 신호를 버리므로 실패가 이어질 수 있습니다).

Spring Boot 3.5에서 갱신을 켜는 설정은 다음과 같습니다. 이 설정은 `spring.data.redis.cluster.*`로 클러스터를 설정했을 때만 적용됩니다.

| 설정 | 기본값 | 켜면 |
| --- | --- | --- |
| `spring.data.redis.lettuce.cluster.refresh.adaptive` | `false` | Lettuce의 사건 기반 갱신을 모두 켭니다. MOVED·ASK를 받거나, 한 노드에 다시 연결하기를 5번 이상 반복하거나, 지도에 없는 슬롯·노드를 만나면 지도를 다시 받습니다. 사건이 몰려도 30초에 한 번까지만 갱신합니다(Lettuce 기본값이며 Spring Boot 설정으로는 바꿀 수 없습니다) |
| `spring.data.redis.lettuce.cluster.refresh.period` | 없음(꺼짐) | 정한 주기마다 지도를 다시 받습니다. 사건이 없어도 바뀐 지도를 따라잡습니다 |
| `spring.data.redis.lettuce.cluster.refresh.dynamic-refresh-sources` | `true` | 갱신할 때 지도에서 알게 된 모든 노드에 묻습니다. `false`면 시드 노드에만 묻습니다 |
| `spring.data.redis.cluster.max-redirects` | 없음(Lettuce 기본 5) | 한 명령이 따라갈 리다이렉트 수의 상한 |

`adaptive`와 `period`는 둘 다 켜는 것이 일반적인 선택입니다. 사건 기반 갱신은 빠르게 반응하지만 사건이 있어야 움직이고, 주기 갱신은 사건을 놓쳐도 결국 따라잡습니다. 대가는 갱신 요청입니다. 갱신 한 번에 Lettuce는 묻는 노드마다 `CLUSTER NODES`와 `INFO`를 보냅니다. 앱 인스턴스가 100대, 노드가 6대, 주기가 30초라면(가상 예시) 클러스터 전체가 30초마다 1,200개의 갱신 명령을 받습니다. 노드가 많은 큰 클러스터에서 `dynamic-refresh-sources`를 `false`로 두는 이유입니다. 대신 시드 노드가 모두 내려가면 지도를 받을 곳이 없어집니다.

### 장애 조치는 지도가 바뀌는 또 하나의 경우입니다

주 서버 C가 죽고 복제본이 새 주 서버로 승격되면, 슬롯 10923~16383의 주인이 바뀝니다. 지도를 고쳐야 한다는 점은 리샤딩과 같지만, 다른 점이 있습니다. 리샤딩에서는 옛 노드가 살아 있어 `MOVED`로 알려 주지만, 장애 조치에서는 옛 노드가 응답하지 않으므로 `MOVED`를 받을 기회 자체가 없습니다. 기본 설정의 Lettuce라면 그 슬롯의 명령은 죽은 노드의 연결로 가서 다시 연결하기를 기다리며 실패하거나 명령 타임아웃까지 기다릴 것입니다. 이는 위의 Lettuce 동작(지도는 갱신으로만 바뀜)에서 끌어낸 이 문서의 추론이며, 재현하지 않았습니다. 다시 연결하는 동안 명령이 어떻게 기다리는지는 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에서, 장애 판정과 승격 절차 및 그동안 클라이언트가 겪는 일은 이 문서의 범위 밖입니다. 여기서는 **클러스터를 쓴다면 토폴로지 갱신을 켜는 것이 장애 조치를 따라가기 위한 전제**라는 점만 기억합니다.

## 원자성과 분산 락의 보장은 어떻게 달라지는가

### 원자성: 같은 슬롯 안에서만, Spring에서는 사실상 Lua로

노드 한 대의 원자성(명령 하나, `MULTI`/`EXEC`, Lua 스크립트가 다른 명령과 섞이지 않음)은 클러스터에서도 노드 하나 안에서 그대로 성립합니다. 클러스터가 더하는 조건은 "다루는 키가 모두 같은 슬롯"이라는 것입니다. 서버가 실제로 막는 범위는 조금 더 좁아서, shebang 없는 스크립트나 `allow-cross-slot-keys` 플래그를 준 스크립트는 같은 노드 안이면 슬롯이 다른 키에도 접근할 수 있습니다. 하지만 그 슬롯이 다른 노드로 옮겨지는 순간 실패하므로 설계 규칙은 "같은 슬롯"으로 둡니다([Redis 원자성](./atomicity.md)의 "Lua는 실행 중에도 검사하므로 중간에 실패할 수 있습니다"). 다른 노드에 있는 키 사이의 원자성은 클러스터 어디에도 없습니다. 노드끼리 실행을 맞추는 장치(두 단계 커밋 같은)가 없기 때문입니다.

Spring에서는 선택지가 하나 더 줄어듭니다. Spring Data Redis 3.5.13의 Lettuce 클러스터 연결은 `multi()`, `watch()`, `unwatch()`를 부르면 "MULTI is currently not supported in cluster mode" 같은 `InvalidDataAccessApiUsageException`을 던집니다. Lettuce도 `MULTI`, `EXEC`, `DISCARD`에는 키가 없어 노드를 정할 수 없으므로 클러스터 연결에서 트랜잭션 명령을 쓰지 말라고 문서에 적습니다. 트랜잭션은 `MULTI`부터 `EXEC`까지 한 노드의 한 연결로 가야 의미가 있는데, 클러스터 연결은 명령마다 키로 노드를 고릅니다. 키가 없는 `MULTI`는 기본 연결로 가고, 뒤의 `DECR stock:{item:1}`은 슬롯 13307을 맡은 노드의 연결로 가므로 둘이 같은 연결이라는 보장이 없습니다. 그래서 다음은 클러스터에서 동작하지 않습니다.

- `RedisTemplate.execute(SessionCallback)` 안에서 `multi()`/`exec()`로 묶는 코드
- `WATCH`로 낙관적 재시도를 하는 코드
- 템플릿의 트랜잭션 지원(`setEnableTransactionSupport(true)`)을 켜고 `@Transactional` 안에서 쓰는 코드. 이 경우 Spring Data Redis가 연결에 `multi()`를 먼저 부르므로 같은 예외가 납니다(이 문서가 코드 경로로 판단한 것이며 실행해 보지 않았습니다)

Redis 서버는 같은 슬롯의 `MULTI`/`EXEC`를 받지만, Spring + Lettuce 클러스터 구성에서 여러 키를 원자적으로 다루는 실용적인 길은 **해시 태그로 슬롯을 맞춘 Lua 스크립트**입니다. 단일 서버에서 `MULTI`/`EXEC`나 `WATCH`로 짠 코드가 있다면, 클러스터로 옮기기 전에 스크립트로 바꿔야 합니다.

### 분산 락: 키 하나는 주 서버 하나에 있고, 노드가 많다고 Redlock이 되지 않습니다

[Redis 분산 락](./distributed-lock.md)의 `SET … NX PX` 락은 클러스터에서도 키 하나, 즉 주 서버 하나에 걸립니다. 그래서 그 문서의 보장과 한계가 그대로 옮겨 옵니다.

- **장애 조치로 락을 잃는 경우가 그대로입니다.** 클러스터의 주 서버도 비동기로 복제하므로, 복제되기 전에 주 서버가 죽으면 새 주 서버에는 락이 없습니다(분산 락 문서의 "장애 조치: 복제되기 전의 락은 새 주 서버에 없습니다"). 클러스터라서 더 안전해지지 않습니다.
- **락 키와 펜싱 카운터 키는 같은 태그로 묶어야 합니다.** 한 스크립트에서 함께 다루기 때문입니다(`lock:{item:1}`, `fence:{item:1}`). 그 결과 두 키는 같은 주 서버에 있고, 그 주 서버의 장애 조치는 둘을 함께 되돌릴 수 있습니다. 카운터가 되돌아가면 이미 나간 번호가 다시 나오므로, 번호로 늦은 쓰기를 거절하는 DB 쪽 비교가 엄격해야 합니다(분산 락 문서의 "Redis 카운터로 토큰을 만들 때의 조건").
- **클러스터의 주 서버 여러 대는 Redlock의 "독립된 주 서버 N대"가 아닙니다.** Redlock은 복제로 이어지지 않은 독립 서버 여러 대에 **같은 키**를 거는 알고리즘입니다(분산 락 문서의 "Redlock: 서로 독립된 주 서버 여러 대의 과반에 락을 겁니다"). 클러스터에서 같은 키는 늘 같은 슬롯, 같은 주 서버로 가므로 키 하나로는 여러 주 서버에 걸 수 없습니다. 또 클러스터의 주 서버마다 복제본이 있고 장애 조치가 일어나므로, Redlock이 피하려던 상황을 그대로 갖고 있습니다. 이 판단은 두 문서의 설명에서 끌어낸 이 문서의 추론입니다. Redlock이 필요하다면 클러스터와 별도인 독립 인스턴스를 씁니다.
- **인기 상품의 락은 한 노드에 몰립니다.** 락 키 하나는 노드 하나의 메인 스레드가 처리하므로, 대기 중인 재시도도 모두 그 노드로 갑니다.

### 쓰기 유실: 비동기 복제에 네트워크 분할이 더해집니다

공식 문서는 Redis Cluster가 강한 일관성을 보장하지 않으며, 클라이언트가 확인 응답을 받은 쓰기도 잃을 수 있다고 밝힙니다(공식 문서 cluster-spec, "Write safety"; scaling, "Redis Cluster consistency guarantees"). 원인은 둘입니다.

1. **비동기 복제**: 단일 주 서버와 복제본 구성과 같은 원인입니다. 범위는 [Redis 복제](./replication.md)의 "장애 조치 때 무엇을 잃는가"에 있습니다.
2. **소수 쪽에 남은 주 서버**: 네트워크가 갈려 주 서버 C가 다른 주 서버 다수와 통신하지 못하는데, 주문 API 일부는 여전히 C에 닿는다고 합시다. C는 한동안 쓰기를 받습니다. 그사이 다수 쪽이 C의 복제본을 새 주 서버로 올리면, 분할이 풀린 뒤 C는 복제본으로 돌아가며 그동안 받은 쓰기를 버립니다. 이 구간의 길이를 좌우하는 것이 `cluster-node-timeout`(기본 15,000밀리초)입니다. 주 서버는 다수와 대략 이 시간 이상 연락이 끊겼다고 판단하면 쓰기를 거부하기 시작하고, 다수 쪽도 이 시간이 지나야 장애 조치를 시작합니다. 그래서 소수 쪽에서 잃을 수 있는 쓰기는 대략 이 시간만큼입니다. 연락이 끊겼다는 판단은 주기적인 검사로 내리므로, 이 값이 분할 순간부터 정확히 몇 초 뒤에 쓰기가 막힌다는 엄격한 상한은 아닙니다(공식 문서 cluster-spec, "Failure detection").

예시로 옮기면, 분할된 C에서 성공한 재고 차감과 구매자 추가가 사라져 재고가 되살아나고 같은 사용자가 다시 살 수 있습니다. 결제처럼 되돌릴 수 없는 일의 근거를 Redis에만 두지 않아야 하는 이유입니다. `WAIT`으로 복제본 확인을 기다리면 잃을 가능성은 줄지만, 공식 문서는 그래도 강한 일관성이 되지는 않는다고 밝힙니다(scaling, "Redis Cluster consistency guarantees").

### 슬롯 하나가 비면 전체가 멈출 수 있습니다

`cluster-require-full-coverage`의 기본값은 `yes`입니다. 이 설정에서는 16,384개 슬롯 중 하나라도 맡은 노드가 없거나 그 노드가 실패 상태면 클러스터 전체가 실패 상태가 됩니다. 그동안 노드는 키를 다루는 명령에 대개 `-CLUSTERDOWN The cluster is down`으로 답합니다(실패 상태가 반영되기 전에 맡은 노드가 없는 슬롯을 만나면 `-CLUSTERDOWN Hash slot not served`, 읽기만 허용한 상태의 쓰기에는 "only accepts read commands"가 붙은 문구입니다). 예를 들어 주 서버 C와 그 복제본이 함께 죽으면, A와 B에 있는 상품 2의 주문도 실패합니다. `cluster-allow-reads-when-down`(기본 `no`)을 켜면 이 상태에서도 읽기는 받습니다.

`cluster-require-full-coverage no`로 두면 슬롯이 비었다는 이유로는 멈추지 않으므로, 살아 있는 슬롯의 요청은 계속 받습니다. 단, 슬롯을 맡은 주 서버의 과반에 닿지 못하는 노드는 이 설정과 상관없이 실패 상태가 됩니다. 예시에서 C가 죽어도 A, B는 셋 중 둘로 과반이라 계속 동작합니다. 대신 일부 키만 응답하는 상태가 되므로, 애플리케이션이 "일부 상품만 실패"를 견딜 수 있을 때만 맞습니다. 이 선택은 운영팀과 함께 정할 일입니다.

## 노드와 요청이 늘면 무엇이 달라지는가

- **노드를 늘리면 늘어나는 것은 메모리와 "흩어진 키들의" 처리량입니다.** 키가 고르게 흩어져 있다면 주 서버 N대는 대략 N배의 메모리와 처리량을 줍니다(공식 문서 cluster-spec, "Performance"). 한 키나 한 태그에 몰린 부하는 늘지 않습니다.
- **해시 태그를 많이 쓸수록 분산이 줄어듭니다.** 태그 하나에 묶인 키 수와 요청 수가 커질수록 그 슬롯이 든 노드가 먼저 한계에 닿습니다. 태그의 단위가 데이터 증가와 함께 커지지 않는지(예: "사용자별"이던 태그가 "조직별"로 바뀌지 않는지) 설계 때 확인합니다.
- **여러 키 명령을 나눠 보내면 슬롯 묶음 수만큼 요청이 늘어납니다.** 슬롯이 모두 다른 키 100개의 `MGET`은 100개의 슬롯 묶음, 즉 `MGET` 100개로 나뉘어 여러 노드에 갑니다. 한 노드가 맡은 슬롯끼리도 따로 보냅니다. 각 요청은 짧지만, 가장 늦은 응답이 전체 지연을 정합니다.
- **리샤딩이 잦을수록 리다이렉트와 TRYAGAIN이 늘어납니다.** 슬롯을 옮기는 동안의 여러 키 명령은 실패할 수 있고, 지도가 낡은 클라이언트는 옮긴 뒤에도 요청마다 왕복이 하나 더 듭니다.
- **앱 인스턴스가 늘수록 토폴로지 갱신 요청이 늘어납니다.** 주기 갱신만 보면 갱신 명령 수는 "인스턴스 수 × 묻는 노드 수 × 2(`CLUSTER NODES`와 `INFO`) ÷ 주기"입니다.
- **노드와 인스턴스가 늘면 연결도 곱해집니다.** Spring의 연결 하나가 클러스터에서는 주 서버 수 + 1개의 TCP 연결이 되고, 연결 한도(`maxclients`)는 노드마다 따로 걸립니다. 계산과 정할 것은 [Redis Cluster의 연결 수](./cluster-connections.md)에서 다룹니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 본문 |
| --- | --- | --- |
| 같은 노드에 있는 키라면 함께 다룰 수 있다 | 기준은 노드가 아니라 슬롯입니다. 같은 노드라도 슬롯이 다르면 `CROSSSLOT`이고, 슬롯은 다른 노드로 옮겨질 수 있습니다 | 한 번의 실행이 다루는 키는 모두 같은 슬롯이어야 합니다 |
| `CROSSSLOT`이 안 나면 원자적으로 실행된 것이다 | Lettuce는 `MSET`, `DEL` 같은 명령을 슬롯별로 나눠 보내므로 에러 없이 일부만 반영될 수 있습니다 | 클라이언트가 나눠 보내면 에러는 사라지지만 원자성도 사라집니다 |
| 해시 태그는 성능 최적화다 | 함께 실행하기 위한 장치이고, 대가로 부하를 한 노드에 모읍니다 | 태그 하나는 노드 하나입니다 |
| 클라이언트가 MOVED를 받으면 지도도 고쳐진다 | Lettuce는 따라가기만 하고, 지도는 토폴로지 갱신을 켜야 바뀝니다. 기본값은 꺼짐입니다 | Lettuce는 지도를 언제 받고, 언제 고치는가 |
| ASK도 MOVED처럼 지도를 고쳐야 한다 | ASK는 이번 한 번만입니다. 슬롯의 주인은 아직 옛 노드입니다 | 리샤딩 중에는 ASK와 TRYAGAIN이 생깁니다 |
| 클러스터의 주 서버 여러 대로 Redlock이 된다 | 같은 키는 한 주 서버에만 가고, 주 서버마다 복제와 장애 조치가 있습니다 | 분산 락: 키 하나는 주 서버 하나에 있고, 노드가 많다고 Redlock이 되지 않습니다 |
| 클러스터는 확인 응답한 쓰기를 잃지 않는다 | 비동기 복제와 소수 쪽 분할 때문에 잃을 수 있습니다 | 쓰기 유실: 비동기 복제에 네트워크 분할이 더해집니다 |

## 근거

**Redis 소스**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/cluster.h`](https://github.com/redis/redis/blob/8.2.10/src/cluster.h): `keyHashSlot`(CRC16 결과에 `& 0x3FFF`, 첫 `{`와 그 뒤 첫 `}` 사이만 계산, 짝이 없거나 사이가 비면 키 전체)
- [`src/cluster.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster.c):
  - `getNodeByQuery`: 첫 키의 슬롯과 다른 키의 슬롯을 비교해 `CROSSSLOT`, `EXEC`이면 대기열 전체의 키를 검사, MIGRATING인 슬롯에서 키가 일부만 있으면 `TRYAGAIN`·모두 없으면 ASK, IMPORTING인 슬롯은 `ASKING`이 있을 때만 실행하고 서로 다른 여러 키 중 없는 키가 있으면 `TRYAGAIN`, `ASKING`이 없으면 주인 노드로 MOVED, 클러스터가 실패 상태면 `cluster-allow-reads-when-down`에 따라 거부
  - `clusterRedirectClient`: `-CROSSSLOT Keys in request don't hash to the same slot`, `-TRYAGAIN Multiple keys request during rehashing of slot`, `-CLUSTERDOWN The cluster is down`, `-CLUSTERDOWN Hash slot not served`, `-CLUSTERDOWN The cluster is down and only accepts read commands`, `-MOVED <슬롯> <주소>:<포트>`, `-ASK …` 응답 형식
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c): `processCommand`(클러스터 모드에서 키가 있는 명령만 `getNodeByQuery`로 검사하고, 리다이렉트할 때는 명령을 실행하지 않고 돌려보냄)
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `resetClient`(MULTI 안이 아니고 방금 실행한 것이 `ASKING`이 아니면 `ASKING` 표시를 지움)
- [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c): `selectCommand`(클러스터 모드에서 0이 아닌 DB면 "SELECT is not allowed in cluster mode")
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `cluster-node-timeout` 기본 15000, `cluster-require-full-coverage` 기본 yes, `cluster-allow-reads-when-down` 기본 no, `cluster-slot-stats-enabled` 기본 no, `cluster-port` 기본 0
- [`src/cluster_legacy.h`](https://github.com/redis/redis/blob/8.2.10/src/cluster_legacy.h): `CLUSTER_PORT_INCR`(클러스터 버스 포트 = 클라이언트 포트 + 10000), [`src/cluster_legacy.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster_legacy.c): `clusterUpdateState`(`cluster-require-full-coverage`이면 맡은 노드가 없거나 실패한 슬롯이 하나라도 있을 때 클러스터를 실패 상태로, 이 설정과 별도로 슬롯을 맡은 주 서버의 과반에 닿지 못하면 실패 상태로), `clusterCron`(노드 응답 지연을 주기적으로 검사해 `cluster-node-timeout`을 넘으면 의심 상태로 표시)
- [`src/commands/cluster-slot-stats.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/cluster-slot-stats.json): `key-count`, `cpu-usec`, `network-bytes-in`, `network-bytes-out`, `ORDERBY … LIMIT`, [`src/cluster_slot_stats.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster_slot_stats.c): `clusterSlotStatsInvalidateSlotIfApplicable`(다른 슬롯 접근을 허용한 스크립트는 호출한 연결의 슬롯을 지워 집계에서 뺌), `canAddNetworkBytesOut`·`canAddCpuDuration`(슬롯이 지워진 명령은 집계하지 않음), [`src/script.c`](https://github.com/redis/redis/blob/8.2.10/src/script.c): `scriptPrepareForRun`(shebang 없는 호환 모드와 `allow-cross-slot-keys`에 다른 슬롯 접근 허용 표시), `key-count` 말고는 `cluster-slot-stats-enabled`일 때만 집계("Any additional metrics aside from key-count come with a performance trade-off")
- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): `cluster-slot-stats-enabled` 설명(슬롯 통계로 핫·콜드 슬롯을 파악, 기본 no), `cluster-compatibility-sample-ratio` 설명(0~100 비율로 명령을 표본 검사, 기본 0, 비율이 높으면 성능 비용), "CLUSTER DOCKER/NAT support" 절(주소가 NAT되거나 포트가 전달되는 환경에서는 노드 주소 탐지가 실패하므로 `cluster-announce-ip`·`cluster-announce-port`·`cluster-announce-bus-port`로 각 노드가 자기 공개 주소를 알게 함, 이 정보는 클러스터 버스로 다른 노드에 전해져 그 노드의 주소로 쓰임)
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c)의 `processCommand`와 INFO 생성부: 클러스터가 아닌 서버에서 표본으로 고른 명령의 키가 같은 슬롯이 아니면 `stat_cluster_incompatible_ops` 증가, `# Stats` 절에 `cluster_incompatible_ops` 출력(비율이 0이 아닐 때만). [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c): `SELECT`의 0이 아닌 DB, `MOVE`, `SWAPDB`도 이 수에 더함
- [`src/redis-cli.c`](https://github.com/redis/redis/blob/8.2.10/src/redis-cli.c): `clusterManagerCommandCreate`(주 서버 수로 16384를 나눠 `lround`로 범위를 정함. 주 서버 3대면 0–5460, 5461–10922, 10923–16383)

**Redis 공식 문서**(저장소 [`redis/docs`](https://github.com/redis/docs/tree/e53775dde0ee7e39ebe327e7ffff583caa9fbd65), 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`)

- [cluster-spec](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/reference/cluster-spec.md): "Implemented subset"(DB 0만, 리샤딩 중 여러 키 연산 불가), "Client and Server roles…"(노드는 프록시하지 않고 MOVED·ASK로 리다이렉트, 클러스터 버스), "Write safety"(확인 응답한 쓰기 유실, 소수 쪽과 `NODE_TIMEOUT`), "Performance"(N배 확장), "Key distribution model"(CRC16 XMODEM, 16384), "Hash tags"(규칙과 예), "MOVED Redirection"(지도 전체를 다시 받는 권고), "Live reconfiguration"(MIGRATING·IMPORTING, `MIGRATE`), "ASK redirection", "Multi-keys operations"(`TRYAGAIN`), "Failure detection"(node timeout 기반 판정)
- [migrate](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/migrate.md): 성공하면 원본에서 지우고, 타임아웃 오류가 나지 않는 한 키는 어느 한쪽에만 있음
- [scaling](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/scaling.md): "Redis Cluster TCP ports", "Redis Cluster data sharding"(일관된 해싱이 아님), "Redis Cluster consistency guarantees"(`WAIT`으로도 강한 일관성이 아님, 분할 시 유실과 node timeout), "Redis Cluster configuration parameters"(`cluster-require-full-coverage`, `cluster-allow-reads-when-down`)
- [keyspace-notifications](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/pubsub/keyspace-notifications.md): "Events in a cluster"(이벤트는 노드별)
- [pubsub](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/pubsub/_index.md): "Sharded Pub/Sub"(일반 Pub/Sub은 모든 노드에 전파, 샤드 Pub/Sub은 샤드 안에서만)

**Lettuce**(태그 [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE). 아래 Spring Boot 3.5.16이 지정한 버전)

- [`RedisClusterClient.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/RedisClusterClient.java): 클래스 설명(첫 키로 라우팅, 클러스터 연결에서 `MULTI`·`EXEC`·`DISCARD`를 쓰지 말 것), `loadPartitionsAsync`, `refreshPartitionsAsync`(지도를 다시 받는 경로)
- [`ClusterDistributionChannelWriter.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterDistributionChannelWriter.java): `doWrite`(첫 키의 슬롯으로 연결 선택, MOVED·ASK면 응답의 주소로 다시 보내고 ASK면 `ASKING`을 앞에 붙임, MOVED에서는 `onMovedRedirection` 알림만 하고 지도를 직접 고치지 않음, `MovedRedirectionEvent`·`AskRedirectionEvent` 발행, 키가 없는 명령은 기본 연결로)
- [`ClusterCommand.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterCommand.java): `complete`(MOVED·ASK일 때만 상한까지 다시 보냄. TRYAGAIN은 다시 보내지 않음)
- [`ClusterClientOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterClientOptions.java): `DEFAULT_MAX_REDIRECTS = 5`, `DEFAULT_VALIDATE_CLUSTER_MEMBERSHIP = true`
- [`PooledClusterConnectionProvider.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/PooledClusterConnectionProvider.java): `beforeGetConnection`(지도에 없는 노드면 `onUnknownNode`를 알리고, 멤버십 검사가 켜져 있으면 `connectionAttemptRejected`로 `UnknownPartitionException` "… This partition is not known in the cluster view."), `getWriteConnection`(슬롯의 주 서버를 지도에서 찾아 그 노드 정보의 host·port로 연결)
- [`ClusterPartitionParser.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/models/partitions/ClusterPartitionParser.java): `parseNode`(`CLUSTER NODES`의 `host:port`, 알린 호스트 이름이 있으면 그 이름으로 노드 주소를 만듦)
- [`MappingSocketAddressResolver.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/resource/MappingSocketAddressResolver.java): 클래스 설명(연결할 때 `RedisURI`의 host·port를 매핑 함수로 바꿔 다른 주소로 접속)
- [`ClusterTopologyRefreshOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterTopologyRefreshOptions.java): 주기 갱신 기본 꺼짐(`DEFAULT_PERIODIC_REFRESH_ENABLED = false`), 사건 기반 갱신 기본 없음(`DEFAULT_ADAPTIVE_REFRESH_TRIGGERS` 빈 집합), 사건 기반 갱신 제한 30초, `DEFAULT_DYNAMIC_REFRESH_SOURCES = true`, 다시 연결하기 5번(`DEFAULT_REFRESH_TRIGGERS_RECONNECT_ATTEMPTS`), `RefreshTrigger`(MOVED_REDIRECT, ASK_REDIRECT, PERSISTENT_RECONNECTS, UNCOVERED_SLOT, UNKNOWN_NODE)
- [`ClusterTopologyRefreshScheduler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterTopologyRefreshScheduler.java): `onMovedRedirection` 등은 해당 트리거가 켜졌을 때만 갱신을 예약, `activateTopologyRefreshIfNeeded`는 주기 갱신이 켜졌을 때만 일정을 잡음
- [`topology/Connections.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/Connections.java), [`topology/DefaultClusterTopologyRefresh.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/DefaultClusterTopologyRefresh.java): 갱신할 때 노드마다 `CLUSTER NODES`와 `INFO`를 보냄
- [`RedisAdvancedClusterAsyncCommandsImpl.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/RedisAdvancedClusterAsyncCommandsImpl.java): `del`, `unlink`, `exists`, `touch`, `mget`, `mset`, `msetnx`가 `SlotHash.partition`으로 슬롯별로 나눠 실행, `msetnx`는 묶음 중 하나라도 실패하면 `false`, `keys`·`scan`은 모든 주 서버에 실행

**Spring Data Redis**(태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13), 커밋 `42619b203148bae01235fe14f3cff3e171d93ebf`)

- [`LettuceClusterConnection.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClusterConnection.java): `multi`, `watch`, `unwatch`가 `InvalidDataAccessApiUsageException`("MULTI is currently not supported in cluster mode" 등), `select`는 0이 아니면 예외
- [`LettuceClusterKeyCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClusterKeyCommands.java): `rename`은 슬롯이 다르면 `dump`·`restore`·`del`
- [`LettuceConnectionFactory.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionFactory.java): `getClusterClientOptions`(설정의 `maxRedirects`를 Lettuce에 넘김)
- [`RedisConnectionUtils.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisConnectionUtils.java): 트랜잭션 지원을 켠 템플릿이 묶인 연결에 `multi()` 호출
- 참조 문서 [cluster.adoc](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/antora/modules/ROOT/pages/redis/cluster.adoc): 클러스터에서 키 이벤트에 의존하지 말 것, JSON 키 직렬화기가 슬롯 계산에 영향

**Spring Boot**(태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16), 커밋 `0566f6933049aca6bc5ffc6d559fffade9cd2e0c`)

- [`RedisProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisProperties.java): `Cluster.nodes`, `Cluster.maxRedirects`, `Lettuce.Cluster.Refresh`(`dynamicRefreshSources = true`, `period` 기본 없음, `adaptive` 기본 false)
- [`LettuceConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/LettuceConnectionConfiguration.java): `initializeClientOptionsBuilder`(클러스터 속성이 있을 때만 갱신 옵션 구성, `period`가 있으면 `enablePeriodicRefresh`, `adaptive`면 `enableAllAdaptiveRefreshTriggers`)
- [`RedisConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisConnectionConfiguration.java): `max-redirects`를 `RedisClusterConfiguration`에 설정

**이 문서가 계산·추론한 것**

- 슬롯 번호(8466, 13216, 13307, 920, 8363)는 cluster-spec의 CRC16 정의(XMODEM, `"123456789"` → `0x31C3`로 확인)와 `keyHashSlot` 규칙으로 직접 계산했습니다. 실제 서버에서 `CLUSTER KEYSLOT`으로 확인하지는 않았습니다.
- 장애 조치 때 기본 설정의 Lettuce가 죽은 노드로 계속 보낸다는 설명, 트랜잭션 지원을 켠 템플릿이 클러스터에서 예외를 낸다는 설명, 클러스터 주 서버가 Redlock의 전제를 채우지 못한다는 판단은 위 소스와 문서에서 끌어낸 추론이며 재현하지 않았습니다.

## 실무 포인트

### 클러스터로 옮기기 전에 확인할 것

| 중요도 | 항목 | 이유와 선택 |
| --- | --- | --- |
| 필수 | 여러 키를 한 번에 다루는 코드 목록 | 스크립트, `MULTI`/`EXEC`, `WATCH`, `MSET`·`RENAME` 같은 여러 키 명령을 찾습니다. 원자성이 필요한 것은 해시 태그 + Lua로 바꾸고, 필요 없는 것은 태그 없이 두고 클라이언트가 나눠 보내게 합니다. 공식 문서도 클러스터로 옮길 때 가장 중요한 것은 여러 키 연산을 어떻게 쓰는지라고 적습니다(scaling, "Migrate to Redis Cluster") |
| 필수 | Spring 트랜잭션 코드 제거 | `SessionCallback`의 `multi()`, `WATCH`, 템플릿 트랜잭션 지원은 Lettuce 클러스터 연결에서 예외가 납니다 |
| 필수 | 해시 태그의 단위 | "함께 바꿔야 하는 가장 작은 묶음"으로 정합니다. 그 묶음 하나에 몰릴 최대 요청량이 노드 하나가 감당할 수 있는 양인지 확인합니다 |
| 필수 | Lettuce 토폴로지 갱신 켜기 | `refresh.adaptive=true`와 `refresh.period`를 함께 켭니다. 꺼 두면 새로 더한 노드로 옮긴 슬롯의 요청이 연결 거부로 실패하고, 기존 노드 사이의 이동에서도 요청마다 왕복이 늘며, 장애 조치를 따라가지 못할 수 있습니다 |
| 알면 유리 | 앱에서 노드 주소에 닿는지 | Lettuce는 시드 노드에서 받은 지도에 적힌 노드 주소, 곧 각 노드가 `CLUSTER NODES`로 알리는 주소로 직접 접속합니다. Docker·Kubernetes·NAT처럼 노드가 알리는 주소와 앱이 닿을 수 있는 주소가 다르면, 시드 연결은 되는데 명령은 실패합니다. 앱이 도는 네트워크에서 그 주소마다 접속되는지 미리 확인합니다 |
| 알면 유리 | 키 직렬화기 | 키는 문자열 직렬화기로 둡니다. 직렬화기가 넣은 바이트가 슬롯을 바꿉니다 |
| 알면 유리 | `DB 0`, 키 이벤트, `KEYS` 의존 제거 | DB 번호로 데이터를 나눴다면 키 접두사로 바꿉니다. 만료 이벤트에 의존하는 기능은 모든 노드를 구독하거나 다른 방법으로 바꿉니다 |
| 알면 유리 | TRYAGAIN 재시도 | 리샤딩 중 여러 키 스크립트가 실패합니다. 실행 전에 거부된 것이므로, 결과를 모르는 앞선 전송이 없었다면 짧은 지연 후 재시도해도 두 번 실행되지 않습니다 |
| 참고 | 옮기기 전 위반 건수 재기 | 클러스터가 아닌 지금의 서버에 `cluster-compatibility-sample-ratio`(0~100, 기본 0)를 주면, 그 비율만큼 표본으로 고른 명령이 클러스터 규칙(슬롯이 다른 키 등)을 어기는지 세어 `INFO`의 `cluster_incompatible_ops`로 보여 줍니다. 0이 아닌 DB의 `SELECT`, `MOVE`, `SWAPDB`도 셉니다. 어느 코드인지는 알려 주지 않고 건수만 알려 주며, 비율을 높이면 검사 비용이 늘어납니다. 운영에서 써 보지는 않았고 소스로만 확인했습니다 |
| 참고 | `cluster-require-full-coverage` | 기본 `yes`면 샤드 하나의 상실이 전체 장애가 됩니다. 일부 실패를 견디는 서비스라면 `no`를 검토합니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 키 이름과 해시 태그의 단위 | 노드별 CPU·네트워크 사용량의 치우침 |
| 여러 키 작업을 Lua로 묶을지, 나눠 보낼지 | `CLUSTER SLOT-STATS ORDERBY cpu-usec`로 본 상위 슬롯(통계를 켰을 때. shebang 없는 스크립트의 실행은 빠짐) |
| 토폴로지 갱신 설정(`adaptive`, `period`, `dynamic-refresh-sources`) | 애플리케이션 로그·예외의 `MOVED`·`ASK`·`TRYAGAIN`·`CROSSSLOT`·`CLUSTERDOWN` 빈도 |
| TRYAGAIN 재시도 정책 | 리샤딩·장애 조치 시각과 Redis 오류율이 함께 움직이는지 |
| 시드 노드 목록 | 앱 시작 시 토폴로지를 받지 못해 실패하는지 |

Lettuce는 리다이렉트를 따라갈 때 이벤트 버스에 `MovedRedirectionEvent`, `AskRedirectionEvent`를 냅니다. MOVED 이벤트가 계속 나온다면 지도가 낡은 상태가 이어진다는 신호입니다. ASK는 지도가 정확해도 슬롯을 옮기는 동안 정상적으로 나오므로 지도의 낡음으로 읽지 않습니다.

### 증상과 확인 순서

**증상 1: 클러스터로 옮긴 뒤 일부 API만 `CROSSSLOT` 에러를 냅니다.**

1. 에러가 난 명령의 키를 모아 `CLUSTER KEYSLOT`으로 슬롯을 봅니다.
2. 직렬화된 키 바이트가 예상과 같은지 봅니다(직렬화기가 붙인 바이트, 태그 오타, `{}`처럼 빈 태그).
3. 원자성이 필요하면 같은 태그로 바꿉니다. **부작용**: 키 이름이 바뀌므로 기존 데이터는 새 이름으로 옮기거나 다시 만들어야 하고, 그 태그의 부하가 한 노드에 모입니다.

**증상 2: 노드를 더하고 슬롯을 옮긴 뒤, 특정 키들의 요청이 `UnknownPartitionException`("not known in the cluster view")으로 실패하거나, 실패는 없는데 응답 시간이 늘고 옛 노드의 명령 수가 줄지 않습니다.**

1. 예외 메시지와 앱 로그·Lettuce 이벤트의 MOVED 빈도를 봅니다. 연결 거부는 새 노드가 지도에 없다는 뜻이고, MOVED가 계속 나오면 지도가 낡은 것입니다.
2. 토폴로지 갱신 설정이 꺼져 있는지 확인합니다.
3. 갱신을 켜고 배포하거나, 당장은 앱을 재시작해 지도를 다시 받게 합니다. **부작용**: 재시작은 연결을 모두 다시 맺게 하므로 순차로 합니다.

**증상 3: 특가 행사 때 한 노드만 CPU가 100%이고, 그 노드의 다른 키를 쓰는 API까지 느립니다.**

1. 그 노드에서 슬롯 통계로 상위 슬롯을 찾습니다. 주문 처리가 shebang 없는 스크립트라면 그 실행은 슬롯 통계(CPU와 네트워크 모두)에 잡히지 않으므로, 애플리케이션에서 센 키별·태그별 요청 수를 함께 봅니다. 스크립트 첫 줄에 `#!lua`를 두면 집계에 들어갑니다.
2. 그 슬롯에 들어가는 태그를 키 규칙에서 찾습니다. 예시라면 `{item:1}`입니다.
3. 그 태그에 원자적으로 함께 다룰 필요가 없는 키까지 묶여 있다면, 그 키를 태그에서 빼 다른 슬롯으로 흩습니다. 태그가 이미 함께 다뤄야 하는 가장 작은 묶음이라면 태그를 더 나누는 것은 한 스크립트로 판단하는 범위를 바꾸는 설계 변경이므로, 먼저 Redis까지 오는 요청을 줄입니다([Redis 원자성](./atomicity.md)의 "요청과 데이터가 늘면 무엇이 달라지는가"). 키 자체를 나누는 방법과 대가는 [Hot Key와 Big Key](./hot-key-big-key.md)의 "쓰기 핫키는 캐시로 막을 수 없습니다"에 있습니다. 그 슬롯만 다른 노드로 옮기면 함께 있던 다른 슬롯의 피해는 줄지만, 몰린 슬롯 자체의 한계는 그대로입니다.

**증상 4: 앱이 시드 노드에는 연결되는데, 명령은 연결 실패로 끝납니다.**

1. 시드 노드에 `CLUSTER NODES`를 보내 각 노드가 알리는 주소와 포트를 보고, 앱이 도는 곳(컨테이너, 파드)에서 그 주소로 접속되는지 확인합니다.
2. 닿지 않는다면 지도가 낡은 문제가 아니라 주소 문제입니다. 토폴로지 갱신을 켜도 같은 주소를 다시 받을 뿐이라 풀리지 않습니다.
3. 노드마다 `cluster-announce-ip`, `cluster-announce-port`로 앱이 닿을 수 있는 주소를 알리게 합니다. Redis 설정 파일이 Docker·NAT 환경을 위해 설명하는 설정입니다. 노드가 `cluster-announce-hostname`으로 호스트 이름도 알리고 있다면 Lettuce는 IP 대신 그 이름으로 접속하므로, 그 이름이 앱에서 풀리고 닿는지도 봅니다. **부작용**: 알린 주소는 다른 노드도 그 노드의 주소로 쓰므로, 노드끼리도 그 주소(와 클러스터 버스 포트)로 닿아야 합니다. 클라이언트 쪽에서 Lettuce의 `MappingSocketAddressResolver`로 주소를 바꿔 끼우는 방법도 있지만, 노드별 대응표를 앱 설정으로 들고 있다면 노드 주소가 바뀔 때마다 그 표도 고쳐야 합니다.

### Spring에서 확인할 것

- `spring.data.redis.cluster.*`를 설정하면 `LettuceConnectionFactory`가 클러스터 연결을 만듭니다. `spring.data.redis.lettuce.cluster.refresh.*`는 이 `cluster.*` 속성이 있을 때만 적용됩니다. `RedisClusterConfiguration` 빈을 직접 만든다면 `LettuceClientConfigurationBuilderCustomizer`나 직접 만든 `ClusterClientOptions`로 갱신 옵션을 넣었는지 확인합니다.
- `RedisTemplate`에 `RedisScript`로 스크립트를 실행할 때 키 목록 인자에 모든 키를 넘깁니다. Lettuce는 첫 키로 노드를 고르고, 서버는 선언된 키끼리 슬롯을 검사합니다.
- `opsForValue().multiSet()`, `delete(Collection)`은 키의 슬롯이 여럿이면 클러스터에서 에러 없이 슬롯별로 나뉘어 실행됩니다. 키가 모두 한 슬롯이면 명령 하나로 갑니다. 슬롯이 갈린 키로 원자성이 필요한 곳에 쓰지 않습니다.
- 명령 타임아웃(`spring.data.redis.timeout`)은 리다이렉트를 따라간 시간까지 포함한 한 명령의 기한으로 정합니다. 값을 정하는 기준은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에 있습니다.

## 말로 설명하기

### 30초 버전

Redis Cluster는 키 이름의 해시 값을 16,384로 나눈 나머지, 곧 슬롯으로 키를 나누고, 슬롯마다 주 서버 하나가 맡습니다. 그래서 메모리와 쓰기가 노드 수만큼 나뉘지만, 노드 한 대의 원자성은 그 노드 안에서만 성립합니다. 한 명령, 한 트랜잭션, 한 스크립트가 다루는 키는 모두 같은 슬롯이어야 하고, 해시 태그로 맞추면 그 키들의 부하가 한 노드에 모입니다. 클라이언트는 슬롯 지도를 들고 직접 노드를 고르고, 틀리면 MOVED나 ASK로 안내받습니다. Lettuce는 기본으로 지도를 갱신하지 않으므로 토폴로지 갱신을 켜야 합니다.

### 2분 버전

노드 한 대에서는 재고 키와 구매자 키를 Lua 스크립트 하나로 묶으면 메인 스레드가 끊지 않고 처리합니다. 클러스터에서는 두 키가 다른 노드에 있을 수 있습니다. 키의 슬롯은 키 이름만으로 정해지고(`stock:item:1`은 8466, `buyers:item:1`은 13216), 슬롯은 주 서버에 나뉘어 있기 때문입니다. 노드는 다른 노드의 키를 실행 중에 다룰 수 없으므로, 클러스터는 슬롯이 다른 키를 한 번에 다루는 요청을 `CROSSSLOT`으로 거부합니다.

해결은 해시 태그입니다. `stock:{item:1}`, `buyers:{item:1}`처럼 중괄호 안을 같게 하면 둘 다 슬롯 13307에 놓여 스크립트가 실행됩니다. 대가는 그 태그의 모든 키가 한 노드에 모인다는 점입니다. 특가 상품처럼 한 태그에 요청이 몰리면 노드를 늘려도 그 슬롯의 부하는 나뉘지 않으므로, 태그는 함께 바꿔야 하는 가장 작은 묶음에만 붙입니다.

노드는 요청을 대신 전달하지 않으므로 클라이언트가 슬롯 지도로 노드를 고릅니다. 슬롯이 옮겨졌으면 노드가 `MOVED`로 새 주인을 알려 주고, 옮기는 중이면 `ASK`로 이번 한 번만 새 노드에 물어보게 하며, 여러 키가 한 노드에 다 있지 않으면 `TRYAGAIN`으로 거부합니다. Lettuce는 MOVED와 ASK를 자동으로 따라가지만 기본으로는 지도를 고치지 않고, 지도에 없는 새 노드로는 연결도 거부합니다. 그래서 Spring Boot의 `lettuce.cluster.refresh.adaptive`와 `period`를 켭니다.

Spring에서 Lettuce 클러스터 연결은 `MULTI`와 `WATCH`를 거부하고, `MSET`·`DEL` 같은 명령은 키의 슬롯이 여럿이면 에러 없이 슬롯별로 나눠 보냅니다. 그래서 여러 키의 원자성은 해시 태그와 Lua로 얻습니다. 분산 락은 클러스터에서도 주 서버 하나에 걸리므로 장애 조치로 잃는 문제가 그대로이고, 클러스터의 주 서버 여러 대로 Redlock이 되지도 않습니다. 비동기 복제와 네트워크 분할 때문에 확인 응답을 받은 쓰기도 잃을 수 있습니다.

### 예상 질문과 꼬리 질문

**Q. 왜 노드 수로 나누지 않고 16,384개 슬롯을 두나요?**
노드 수로 나누면 노드를 하나 더할 때 나누는 수가 바뀌어 대부분의 키가 다른 노드를 가리키게 됩니다. 슬롯 수를 고정하면 키의 슬롯은 변하지 않고, 노드를 더할 때는 일부 슬롯의 주인만 바꾸고 그 슬롯의 키만 옮깁니다. 대가는 옮기는 단위가 슬롯이라 슬롯 하나의 부하는 나눌 수 없다는 점입니다. (본문: "슬롯 수를 고정하고, 슬롯을 노드에 맡깁니다")

- **꼬리: 그럼 일관된 해싱과 같은 건가요?**
  같은 목적(노드 변화 때 옮길 키를 줄이기)을 다른 방법으로 이룹니다. 공식 문서는 Redis Cluster가 일관된 해싱을 쓰지 않는다고 밝힙니다. 키를 고정된 슬롯에 넣고, 슬롯과 노드의 짝을 명시적으로 관리하고 옮깁니다.

**Q. 두 키가 같은 노드에 있으면 함께 다룰 수 있지 않나요?**
아닙니다. 검사 기준은 슬롯입니다. 같은 노드라도 슬롯이 다르면 `CROSSSLOT`을 받습니다. 슬롯은 리샤딩이나 장애 조치로 다른 노드로 옮겨질 수 있으므로, "지금 같은 노드"에 기대는 코드는 언제든 깨질 수 있습니다. 실행 단위별 세부 규칙은 [Redis 원자성](./atomicity.md)에 있습니다. (본문: "한 번의 실행이 다루는 키는 모두 같은 슬롯이어야 합니다")

**Q. 해시 태그를 쓰면 무엇을 잃나요?**
분산입니다. 같은 태그의 키는 모두 한 슬롯, 한 노드에 있으므로 그 태그에 몰린 요청은 노드 하나의 메인 스레드가 모두 처리합니다. 예시라면 상품 1의 재고, 구매자, 락, 펜싱 카운터가 모두 노드 C에 있습니다. 노드를 늘려도 이 부하는 나뉘지 않습니다. (본문: "태그 하나는 노드 하나입니다")

- **꼬리: 그럼 태그를 아예 안 쓰면 되지 않나요?**
  태그 없는 키도 우연히 같은 슬롯일 수는 있지만, 여러 키가 같은 슬롯에 있음을 이름으로 보장하는 방법은 태그뿐입니다. 반대로 한 번의 실행이 필요 없는 키에 태그를 붙이면 얻는 것 없이 부하만 모읍니다. 기준은 "한 번의 실행으로 함께 다뤄야 하는가"이고, 함께 바꾸는 경우와 한 시점의 값으로 함께 읽어야 하는 경우가 여기에 듭니다.
- **꼬리: 태그를 붙였는데도 의도와 다른 슬롯에 갑니다. 무엇을 의심하나요?**
  계산은 클라이언트가 보낸 바이트로 하므로 키 직렬화기를 먼저 봅니다. JSON 직렬화기가 넣은 `{`나 따옴표가 태그를 바꿀 수 있습니다. 첫 `{`와 그 뒤 첫 `}` 사이만 쓴다는 규칙, 첫 쌍이 `{}`처럼 비어 있으면 뒤의 태그를 보지 않고 키 전체를 계산한다는 규칙도 확인합니다. `CLUSTER KEYSLOT`으로 실제 키를 확인합니다. (본문: "규칙: 첫 `{`와 그 뒤의 첫 `}` 사이만 계산합니다")

**Q. MOVED와 ASK는 무엇이 다른가요?**
MOVED는 "그 슬롯의 주인이 바뀌었다"이므로 지도를 고칩니다. ASK는 슬롯을 옮기는 중에 "요청한 키를 옛 노드에서 찾지 못했으니 이번 한 번만 새 노드에 물어보라"이므로 지도를 고치지 않고 다음에도 옛 노드에 먼저 보냅니다. 새 노드에 키가 있다는 보장은 아니며, 아직 없는 새 키일 수도 있습니다. 슬롯의 주인은 아직 옛 노드이고, 다음 키는 아직 옛 노드에 있을 수 있기 때문입니다. ASK를 따라갈 때는 `ASKING`을 먼저 보내야 새 노드가 실행합니다. (본문: "리샤딩 중에는 ASK와 TRYAGAIN이 생깁니다")

- **꼬리: ASK를 받고 지도를 고쳐 버리면 어떻게 되나요?**
  다음 요청을 새 노드에 `ASKING` 없이 보내게 되고, 새 노드는 아직 주인이 아니므로 옛 노드로 `MOVED`를 돌려줍니다. 결과는 맞지만 왕복이 늘어납니다.
- **꼬리: TRYAGAIN은 언제 나오고 어떻게 다루나요?**
  리샤딩 중인 슬롯에서 여러 키를 다룰 때, 보내는 쪽 노드에 키가 일부만 있거나, `ASKING`으로 받은 쪽 노드에 키 중 하나라도 없을 때입니다. 아직 없는 키도 "없는 키"로 세므로, 새 키를 만드는 스크립트는 옮기는 내내 실패할 수 있습니다. 실행 전에 거부된 것이라 짧은 지연 후 재시도해도 그 전송 때문에 두 번 실행되지는 않습니다. 다만 앞서 타임아웃이나 자동 재전송으로 결과를 모르는 전송이 있었다면 이미 실행됐을 수 있습니다. Lettuce는 TRYAGAIN을 다시 보내지 않으므로 애플리케이션이 재시도를 맡습니다.

**Q. Lettuce는 클러스터 구성이 바뀐 것을 어떻게 아나요?**
처음 연결할 때 시드 노드에서 `CLUSTER NODES`로 지도를 받고, 그 뒤에는 토폴로지 갱신을 켰을 때만 다시 받습니다. Lettuce 6.6.0의 기본값은 주기 갱신과 사건 기반 갱신이 모두 꺼짐이고, MOVED는 따라가기만 할 뿐 지도를 고치지 않습니다. 그래서 기본 설정에서는 리샤딩 뒤 옮겨진 슬롯의 요청이 매번 MOVED를 거칩니다. 기존 노드 사이의 이동이면 왕복이 하나 더 들 뿐이지만, 새로 더한 노드로 옮겼다면 그 노드가 지도에 없어 연결이 거부되고 요청이 실패합니다. Spring Boot에서는 `spring.data.redis.lettuce.cluster.refresh.adaptive`와 `period`로 켭니다. (본문: "Lettuce는 지도를 언제 받고, 언제 고치는가")

- **꼬리: 주기 갱신만 켜면 충분하지 않나요?**
  주기 사이의 변화는 다음 주기까지 따라잡지 못합니다. 주기를 짧게 하면 갱신 요청이 "인스턴스 수 × 노드 수"만큼 늘어납니다. 사건 기반 갱신은 MOVED나 반복된 재연결 같은 신호에 바로 반응하므로 둘을 함께 켭니다. 사건 기반 갱신은 30초에 한 번으로 제한되어 사건이 몰려도 갱신 폭주는 막힙니다.
- **꼬리: 장애 조치 때는 MOVED가 신호가 되나요?**
  옛 주 서버가 응답하지 않으므로 MOVED를 받을 기회가 없을 수 있습니다. 반복된 재연결 신호나 주기 갱신이 필요한 이유입니다. 장애 조치 때 클라이언트가 겪는 일의 세부는 이 문서의 범위 밖이고, 이 부분은 소스에서 끌어낸 추론입니다. (본문: "장애 조치는 지도가 바뀌는 또 하나의 경우입니다")

**Q. Spring에서 클러스터로 바꿨더니 트랜잭션 코드가 예외를 냅니다. 왜인가요?**
Spring Data Redis 3.5.13의 Lettuce 클러스터 연결은 `multi()`, `watch()`를 부르면 `InvalidDataAccessApiUsageException`을 던집니다. `MULTI`와 `EXEC`에는 키가 없어 어느 노드로 보낼지 정할 수 없기 때문에, Lettuce도 클러스터 연결에서 트랜잭션 명령을 쓰지 말라고 합니다. 여러 키를 원자적으로 다뤄야 한다면 해시 태그로 슬롯을 맞추고 Lua 스크립트로 바꿉니다. (본문: "원자성: 같은 슬롯 안에서만, Spring에서는 사실상 Lua로")

- **꼬리: `multiSet()`은 예외 없이 되던데, 그건 원자적인가요?**
  키의 슬롯이 여럿이면 아닙니다. Lettuce가 키를 슬롯별로 나눠 `MSET`을 따로 보내므로, 일부만 성공할 수 있습니다. 키가 모두 한 슬롯이면 `MSET` 하나로 갑니다. `MSETNX`도 "모두 없을 때만"이 슬롯 묶음마다 따로 판정됩니다. 에러가 없다는 것이 원자적이라는 뜻은 아닙니다. (본문: "클라이언트가 나눠 보내면 에러는 사라지지만 원자성도 사라집니다")

**Q. 클러스터의 주 서버가 여러 대이니 분산 락이 더 안전해지나요?**
아닙니다. 락 키 하나는 슬롯 하나, 주 서버 하나에 있으므로 그 주 서버의 장애 조치로 락을 잃는 문제가 그대로입니다. Redlock은 복제로 이어지지 않은 독립 서버 여러 대에 같은 키를 거는 알고리즘인데, 클러스터에서 같은 키는 한 주 서버로만 가고 주 서버마다 복제와 장애 조치가 있습니다. 클러스터 주 서버가 Redlock의 전제를 채우지 못한다는 것은 이 문서의 추론입니다. (본문: "분산 락: 키 하나는 주 서버 하나에 있고, 노드가 많다고 Redlock이 되지 않습니다")

- **꼬리: 그럼 펜싱 토큰 카운터는 어디에 두나요?**
  락 키와 한 스크립트에서 다루므로 같은 태그로 묶어 같은 슬롯에 둡니다. 그러면 둘은 같은 주 서버에 있고, 그 주 서버의 장애가 둘을 함께 되돌릴 수 있다는 점까지 [Redis 분산 락](./distributed-lock.md)의 분석이 그대로 적용됩니다.

**Q. 클러스터는 확인 응답한 쓰기를 잃을 수 있나요? 얼마나요?**
잃을 수 있습니다. 하나는 비동기 복제로, 응답한 쓰기가 복제본에 닿기 전에 주 서버가 죽는 경우입니다. 다른 하나는 네트워크 분할로, 다수와 끊긴 주 서버가 받은 쓰기를 다수 쪽이 장애 조치로 무시하는 경우입니다. 후자의 구간은 대략 `cluster-node-timeout`(기본 15초)만큼입니다. 다수와 그만큼 연락이 끊겼다고 판단하면 소수 쪽 주 서버가 쓰기를 거부하기 때문입니다. 판단은 주기적인 검사로 내리므로 정확한 상한은 아닙니다. `WAIT`으로 줄일 수는 있지만 강한 일관성은 아닙니다. (본문: "쓰기 유실: 비동기 복제에 네트워크 분할이 더해집니다")

- **꼬리: `cluster-node-timeout`을 줄이면 되지 않나요?**
  잃을 수 있는 구간은 줄지만, 잠깐의 네트워크 지연도 장애로 판정해 불필요한 장애 조치가 늘어납니다. 장애 조치 자체가 지도 변경과 유실 구간을 만들므로 공짜가 아닙니다.

**Q. 주 서버 하나와 그 복제본이 모두 죽으면 다른 노드의 키는 쓸 수 있나요?**
기본 설정(`cluster-require-full-coverage yes`)에서는 아닙니다. 맡은 노드가 없는 슬롯이 하나라도 있으면 클러스터 전체가 실패 상태가 되어 키를 다루는 명령에 `CLUSTERDOWN`으로 답합니다. `no`로 바꾸면 주 서버의 과반에 닿는 한 살아 있는 슬롯은 계속 쓰지만, 서비스가 "일부 키만 실패"를 견딜 수 있어야 합니다. (본문: "슬롯 하나가 비면 전체가 멈출 수 있습니다")

**Q. 단일 서버에서 클러스터로 옮길 때 무엇부터 보나요?**
여러 키를 한 번에 다루는 코드입니다. 스크립트, `MULTI`/`EXEC`, `WATCH`, 여러 키 명령을 찾아, 원자성이 필요한 것은 해시 태그와 Lua로 바꾸고 필요 없는 것은 그대로 둡니다. 그다음 DB 번호, 키 이벤트, `KEYS` 의존을 없애고, Lettuce 토폴로지 갱신을 켭니다. (본문: "클러스터로 옮기기 전에 확인할 것")

- **꼬리: 코드를 다 읽지 않고 찾을 방법은 없나요?**
  지금의 서버에 `cluster-compatibility-sample-ratio`를 주면 표본으로 고른 명령 중 클러스터 규칙을 어기는 건수가 `INFO`의 `cluster_incompatible_ops`로 나옵니다. 건수만 알려 주고 어느 코드인지는 알려 주지 않으므로, 건수가 0이 아닐 때 코드를 찾는 출발점으로 씁니다. 비율을 높이면 검사 비용이 늘어납니다. (본문: "클러스터로 옮기기 전에 확인할 것")
