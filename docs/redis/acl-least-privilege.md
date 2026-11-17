# Spring 앱의 Redis 계정 최소 권한: ACL을 무엇에 맞춰 정하는가

> **핵심 질문**: Spring 앱이 쓰는 Redis 계정에 어떤 명령·키·채널만 열어 줘야 하며, 앱이 실제로 보내는 명령은 어떻게 알아내는가? 분류 단위와 명령 단위의 규칙은 각각 무엇을 얻고 잃으며, 권한이 모자라면 앱에는 어떻게 드러나는가?
>
> **범위**: 단일 Redis 서버에 Lettuce로 붙는 Spring Boot 앱을 기준으로 합니다. 클러스터는 연결과 헬스 체크에 더 필요한 명령만 다룹니다. 비밀번호 보관·교체, TLS, 네트워크 격리, Sentinel 계정, 관리형 서비스의 자체 권한 체계는 다루지 않습니다.

## 배경 지식

### 이 문서의 예시: 상품 서비스의 `product-api` 계정

문서 끝까지 다음 앱 하나의 계정을 설계합니다.

- Spring Boot 앱 `product-api`가 Lettuce로 Redis 서버 한 대(DB 0번)에 붙습니다. Redis에는 다른 팀의 키도 함께 들어 있습니다.
- `@Cacheable("product")`로 상품 상세를 캐시합니다. `getProduct(42)`는 키 `product::42`에 TTL 10분으로 값을 넣습니다.
- 관리 화면의 "캐시 비우기" 버튼은 `@CacheEvict(cacheNames = ["product"], allEntries = true)`로 `product` 캐시 전체를 지웁니다.
- 주문 시 재고 차감은 Lua 스크립트로 합니다. 스크립트는 `KEYS[1]`인 `stock:item:1`을 `GET`으로 읽고, 남아 있으면 `DECR`합니다. `KEYS[1]`은 스크립트를 부를 때 넘기는 키 목록의 첫 항목이며, 뒤에 나오는 키 이름 검색 명령 `KEYS`와는 이름만 같습니다. 원자성을 위해 왜 Lua를 쓰는지는 [Redis 원자성](./atomicity.md)에서 다룹니다. 스크립트는 키를 문자열 그대로 보내는 `StringRedisTemplate`으로 실행합니다. 키 패턴은 직렬화를 거친 뒤의 바이트와 비교하므로 이 전제가 중요합니다. Spring Boot가 기본으로 만드는 `RedisTemplate<Object, Object>`는 키에도 JDK 직렬화를 써서, `stock:item:1`을 넘겨도 서버에는 다른 바이트가 도착하고 `~stock:item:*`에 맞지 않습니다. `@Cacheable`의 Redis 캐시는 기본으로 키를 문자열로 보냅니다.
- Actuator의 `/actuator/health`가 Redis 상태를 함께 보고합니다.

목표는 이 앱이 정상 동작에 필요한 일은 모두 하고, 그 밖의 일(다른 팀 키 읽기, `FLUSHALL`, 서버 설정 바꾸기)은 서버가 거부하게 만드는 것입니다.

```text
 product-api (JVM)                         Redis server
+------------------------------+        +-----------------------------+
| Lettuce connection setup  ---|--(1)-->| 1. who are you? (AUTH)      |
| @Cacheable / @CacheEvict  ---|--(2)-->| 2. may this user run this   |
| RedisTemplate.execute(lua)---|--(2)-->|    command on these keys?   |
| Actuator health           ---|--(2)-->|    no -> NOPERM error       |
|                              |<-reply-|    yes -> run command       |
+------------------------------+        +-----------------------------+
```

화살표는 네트워크로 명령이 가는 방향입니다. (1)은 연결을 맺을 때 한 번, (2)는 명령마다 일어납니다. 연결 풀, 복제본, 다른 팀의 앱은 그림에서 뺐습니다. 이 문서의 판단은 모두 오른쪽 2단계, 곧 명령마다 하는 권한 검사에 관한 것입니다.

### 인증과 인가, 그리고 최소 권한

- **한 줄**: 인증은 "누구인가"를 확인하는 일이고, 인가는 "그 사람이 이 일을 해도 되는가"를 판정하는 일입니다. 최소 권한은 인가 규칙을 "필요한 일만 허용"으로 정하는 원칙입니다.
- **비유**: 회사 출입증입니다. 정문에서 사원증을 찍는 것이 인증이고, 사원증마다 열리는 층과 방이 정해져 있는 것이 인가입니다. 최소 권한은 상품팀 사원증으로 상품팀 층만 열리게 하는 것입니다. 예시의 `product-api`는 상품 캐시 층과 재고 층만 열리고, 다른 팀 층과 전산실(서버 설정)은 열리지 않아야 합니다.
- **실제로는**: Redis의 인증은 연결 단위입니다. 연결을 맺은 뒤 사용자 이름과 비밀번호를 한 번 보내면, 그 연결로 오는 명령은 모두 그 사용자의 권한으로 판정됩니다. 비유와 달리 문을 지날 때마다 사원증을 찍지 않고, 연결이 이미 사원증을 들고 있는 셈입니다. 그리고 권한 판정은 명령이 실행되기 **전에** 서버가 하므로, 거부된 명령은 아무것도 바꾸지 않습니다.
- **이 주제에서는**: 최소 권한의 대가는 "필요한 일"의 목록을 정확히 알아야 한다는 것입니다. 목록보다 넓으면 사고를 막지 못하고, 좁으면 기능이 깨집니다. 이 문서의 대부분은 그 목록을 어떻게 알아내고 검증하는가에 관한 것입니다.

### Redis ACL은 사용자마다 명령·키·채널 세 가지를 제한합니다

ACL(Access Control List, 접근 제어 목록)은 Redis가 사용자마다 허용 범위를 적어 두는 규칙입니다. 규칙은 짧은 단어를 공백으로 이어 쓴 문자열이며, 서버는 **적힌 순서대로 왼쪽부터** 적용합니다.

| 무엇을 | 규칙 예 | 뜻 |
| --- | --- | --- |
| 명령 | `+get`, `-flushall`, `+client\|setinfo` | 명령(또는 하위 명령) 하나를 허용·금지 |
| 명령 분류 | `+@read`, `-@dangerous`, `+@all`, `-@all` | 서버가 정해 둔 분류에 든 명령 전체를 허용·금지 |
| 키 | `~product::*`, `%R~report:*`, `%W~log:*` | 이 패턴에 맞는 키만 명령 인자로 쓸 수 있음. `%R`은 읽기만, `%W`는 쓰기만 |
| Pub/Sub 채널 | `&__keyevent@*__:expired` | 이 패턴에 맞는 채널만 구독·발행. 채널은 값을 저장하는 키가 아니라, 한쪽이 발행(`PUBLISH`)한 메시지를 구독(`SUBSCRIBE`)한 연결들에 전달하는 이름 붙은 통로라 키와 따로 제한합니다 |
| 상태 | `on`, `off`, `>비밀번호` | 사용자 활성화, 비밀번호 추가 |

- **명령과 하위 명령**: `CLIENT SETINFO`, `CONFIG GET`처럼 명령 이름 뒤에 동작 이름이 붙는 명령이 있습니다. 뒤쪽을 하위 명령이라 하고, ACL에서는 `client|setinfo`처럼 `|`로 이어 씁니다. `+client`는 `CLIENT`의 모든 하위 명령을 엽니다.
- **키 패턴**: `*`(아무 문자열)와 `?`(아무 한 글자)를 쓰는 glob 패턴입니다. Kotlin의 정규식보다 단순한, 파일 이름 와일드카드와 같은 문법입니다.
- **새 사용자의 기본값**: `ACL SETUSER`로 처음 만든 사용자는 꺼져 있고(`off`), 비밀번호가 없으며, 명령·키·채널을 하나도 쓸 수 없습니다. 아무것도 없는 상태에서 하나씩 여는 방식이 기본입니다. 기존 사용자에게 `ACL SETUSER`를 다시 보내면 처음부터 다시 만드는 것이 아니라, 지금 규칙 위에 새 규칙을 덧붙입니다.
- **기본 사용자 `default`**: 설정 없이 띄운 Redis에는 `default` 사용자 하나만 있고, 모든 명령·키·채널이 열려 있으며 비밀번호도 없습니다. 비밀번호 하나만 쓰는 예전 방식(`requirepass`)도 이 `default` 사용자의 비밀번호를 정하는 것입니다. 앱이 사용자 이름 없이 비밀번호만 보내면 `default`로 인증됩니다. 그래서 **앱 전용 사용자를 만들고 Spring Boot에 `spring.data.redis.username`을 지정하는 것**이 최소 권한의 출발점입니다.

### 서버는 명령마다 세 가지를 차례로 검사합니다

명령이 도착하면 서버는 명령을 실행하기 전에 다음을 검사합니다.

1. **명령 검사**: 이 사용자에게 이 명령(하위 명령 포함)이 허용되어 있는가? 아니면 거부 사유는 `command`입니다.
2. **키 검사**: 명령의 인자 가운데 **키로 정의된 인자**마다, 허용된 키 패턴 가운데 하나에 맞는가? 하나라도 안 맞으면 거부 사유는 `key`입니다.
3. **채널 검사**: 구독·발행할 채널이 허용된 채널 패턴에 맞는가? 아니면 거부 사유는 `channel`입니다.

어느 인자가 키인지는 명령마다 서버가 정의해 둡니다(명령 정의의 key specs). 예를 들어 `GET product::42`의 `product::42`는 키이지만, `KEYS product::*`의 `product::*`는 키가 아니라 검색 패턴입니다. 이 차이가 "키 패턴 제한이 막는 것과 못 막는 것"에서 중요해집니다.

예외가 하나 있습니다. `AUTH`와 `HELLO`처럼 인증에 쓰는 명령은 명령 검사를 받지 않습니다. 인증하기 전에도 보낼 수 있어야 하기 때문입니다.

## 앱이 실제로 보내는 명령은 직접 쓴 코드보다 많습니다

최소 권한 목록을 만들 때 가장 흔한 실수는 직접 쓴 `RedisTemplate` 호출만 세는 것입니다. 예시의 `product-api`가 보내는 명령을 출처별로 모으면 다음과 같습니다. 각 출처는 기준 버전 소스(근거 절)로 확인한 것입니다.

| 출처 | 언제 | 보내는 명령 | 필요한 권한 | 없으면 |
| --- | --- | --- | --- | --- |
| Lettuce 연결 수립 | 연결을 맺을 때마다 | `HELLO 3 AUTH product-api <비밀번호>` | 없음(항상 허용) | — |
| Lettuce 연결 정보 | 연결을 맺은 직후 | `CLIENT SETINFO lib-name Lettuce`, `CLIENT SETINFO lib-ver <버전>` | `+client\|setinfo` | 실패를 무시하고 연결은 정상. 거부 기록은 쌓임 |
| Lettuce DB 선택 | `spring.data.redis.database`가 0이 아닐 때 | `SELECT <번호>` | `+select` | **연결 자체가 실패** |
| `@Cacheable` 조회 | 캐시 읽기 | `GET product::42` | `+get`, 키 읽기 | 캐시 에러 |
| `@Cacheable` 저장 | 미스 뒤 저장 | `SET product::42 <값> PX 600000` | `+set`, 키 쓰기 | 캐시 에러 |
| `@CacheEvict` 한 건 | 상품 수정 시 | `DEL product::42` | `+del`, 키 쓰기 | 캐시 에러 |
| `@CacheEvict(allEntries = true)` | 캐시 비우기 버튼 | `KEYS product::*` 뒤 `DEL <찾은 키들>` | `+keys`, `+del` | 캐시 에러 |
| Lua 재고 차감 | 주문 시 | `EVALSHA <sha1> 1 stock:item:1`, 서버의 스크립트 캐시에 없으면 `EVAL <본문> 1 stock:item:1`. 스크립트 캐시는 상품 캐시와 무관한, 서버가 한 번 받은 스크립트 본문을 SHA1 해시값(`<sha1>`)을 이름으로 보관하는 곳이며 재시작 등으로 비워질 수 있습니다 | `+evalsha`, `+eval`, 키 읽기·쓰기 | 주문 실패 |
| 스크립트 안 | 스크립트 실행 중 | `GET stock:item:1`, `DECR stock:item:1` | `+get`, `+decr` | 스크립트 중간 실패 |
| Actuator 헬스 체크 | `/actuator/health` 호출마다 | `INFO` | `+info` | 헬스 DOWN |

이 표에서 짚을 것이 세 가지 있습니다.

**첫째, 연결을 맺을 때 보내는 명령은 실패해도 되는 것과 안 되는 것으로 나뉩니다.** Lettuce는 연결 직후의 명령을 두 묶음으로 보냅니다. `SELECT`(DB 번호가 0이 아닐 때)와 `READONLY`(클러스터 복제본에서 읽게 설정했을 때)는 실패하면 연결이 맺어지지 않은 것으로 봅니다. 반면 `CLIENT SETINFO` 같은 연결 정보 명령은 서버 종류나 버전마다 지원이 달라 실패해도 연결을 계속 씁니다. 그래서 `+client|setinfo`를 빼도 앱은 멀쩡하지만, 연결을 맺을 때마다 서버의 거부 기록과 거부 카운터가 늘어 진짜 권한 문제를 가립니다. 이 문서의 예시는 이 소음을 없애려고 `+client|setinfo`를 엽니다. 연결 정보를 기록하는 명령일 뿐이고 데이터나 다른 연결에 영향을 주지 않기 때문입니다.

**둘째, 같은 기능도 설정에 따라 명령이 달라집니다.**

- 캐시 비우기의 기본 방식은 `KEYS`입니다. 캐시 설정에서 `BatchStrategies.scan(n)`을 쓰면 `SCAN`으로 바뀌고, 필요한 권한도 `+keys` 대신 `+scan`이 됩니다. `KEYS`는 키 전체를 한 번에 훑는 동안 서버를 막으므로, 키가 많은 Redis에서는 `SCAN` 방식이 낫습니다. `n`은 `SCAN` 한 번에 주는 작업량 힌트(`COUNT`, 정확한 개수나 시간 상한이 아님)이자, 찾은 키를 지우는 `DEL` 한 번에 담는 최대 키 수입니다. 키 전체를 훑는 일은 줄지 않고, 한 명령에 몰렸던 일을 여러 명령으로 나눠 그 사이에 다른 요청이 실행될 수 있게 합니다. 그 대가는 [Redis 이벤트 루프](./event-loop.md)의 "SCAN 계열의 대가"에서 다룹니다.
- `RedisCacheManager`의 기본 캐시 작성기는 잠금을 쓰지 않습니다. 잠금을 쓰는 작성기(`lockingRedisCacheWriter`)로 바꾸면, 캐시 이름마다 `product~lock`이라는 잠금 키에 `SET ... NX`, `EXISTS`, `DEL`을 보냅니다. 이 키는 `product::`로 시작하지 않으므로 키 패턴이 `~product::*`뿐이면 잠금이 거부됩니다.
- 접근할 때마다 TTL을 늘리는 설정(time-to-idle)을 켜면 캐시 조회가 `GET` 대신 `GETEX`가 됩니다. `GETEX`는 키를 읽고 만료 시간도 바꾸므로 키 읽기와 쓰기 권한이 함께 필요합니다.

**셋째, 시작할 때 한 번만 보내는 명령은 테스트에서 놓치기 쉽습니다.** 예시에는 없지만, Spring Data Redis의 키 만료 이벤트 리스너(`KeyExpirationEventMessageListener`)를 빈으로 등록하면 빈을 초기화할 때 `CONFIG GET notify-keyspace-events`를 보내고, 값이 비어 있으면 `CONFIG SET`으로 켭니다. `CONFIG`는 서버 설정 명령이라 앱 계정에 열어 주기 어렵습니다. 이때는 운영자가 서버 설정에서 `notify-keyspace-events`를 켜 두고, 리스너의 `keyspaceNotificationsConfigParameter`를 빈 문자열로 정해 이 명령을 아예 보내지 않게 합니다. 리스너가 구독하는 채널(`__keyevent@*__:expired`)도 따로 열어야 하는데, 이 조건은 "Pub/Sub 채널도 따로 엽니다"에서 다룹니다.

클러스터에 붙으면 명령이 더 늘어납니다. Lettuce는 클러스터 구성을 알아내려고 노드마다 `CLUSTER NODES`와 `INFO`를 보냅니다. `INFO`가 거부되면 그 정보 없이 계속하지만, `CLUSTER NODES`가 모든 노드에서 실패하면 구성을 얻지 못해 연결을 시작할 수 없습니다. 헬스 체크도 클러스터에서는 `INFO` 대신 `CLUSTER INFO`를 보냅니다. 그래서 클러스터라면 `+cluster|nodes +cluster|info`가 더 필요합니다. 클러스터의 다른 동작은 [Redis Cluster](./cluster.md)에서 다룹니다.

## 명령 목록은 앱 쪽과 서버 쪽 두 곳에서 모읍니다

위 표를 처음부터 소스를 읽어 만들 수는 없습니다. 라이브러리를 바꾸거나 설정 하나만 고쳐도 명령이 바뀌기 때문입니다. 실제로는 **스테이징에서 앱을 돌리며 관찰**해서 목록을 만들고, 그 목록으로 만든 ACL을 다시 스테이징에서 검증합니다.

### 앱 쪽: Lettuce 명령 지표

Spring Boot는 Actuator와 Micrometer 레지스트리가 있으면 Lettuce에 명령 지연 기록기를 붙입니다. 그러면 `lettuce.command.completion`이라는 타이머가 명령 이름마다(`command` 태그) 생깁니다. 이 지표는 **그 앱이 보낸 명령만** 셉니다. 같은 Redis를 쓰는 다른 앱의 명령이 섞이지 않는다는 것이 장점입니다.

한계도 분명합니다. 태그는 명령 이름 단위라서 `CLIENT SETINFO`와 `CLIENT KILL`이 모두 `CLIENT`로 묶입니다. 하위 명령까지 알아야 ACL을 쓸 수 있으므로, 이 지표는 "어떤 명령 계열을 쓰는가"의 첫 목록으로만 씁니다.

### 서버 쪽: `INFO commandstats`

서버는 명령마다 호출 횟수를 셉니다. `INFO commandstats`의 `cmdstat_client|setinfo:calls=...`처럼 하위 명령까지 나뉘어 나옵니다. 서버 전체를 세므로 **다른 앱이 함께 쓰는 Redis에서는 누구의 명령인지 알 수 없습니다.** 앱 하나만 붙은 스테이징 Redis에서 `CONFIG RESETSTAT`으로 숫자를 0으로 돌린 뒤, 앱을 띄우고 테스트를 돌려 0이 아니게 된 명령을 모으면 됩니다.

명령과 인자를 실시간으로 보여 주는 `MONITOR`도 있지만, 받은 명령을 모두 출력하느라 부하가 크고 관리 명령이라 스테이징에서만 씁니다. 그리고 `CONFIG GET` 같은 관리 명령은 출력에서 빠지므로, 시작 때 보내는 관리 명령을 찾는 데는 맞지 않습니다.

### 어느 쪽이든 '실행된 경로'만 보입니다

관찰은 테스트가 지나간 코드 경로만 보여 줍니다. 예시에서 캐시 비우기 버튼을 누르지 않았으면 `KEYS`는 목록에 없습니다. 빠뜨리기 쉬운 경로는 다음과 같습니다.

- 관리 기능, 배치, 드물게 쓰는 API(예: 캐시 비우기)
- 앱 시작과 종료(빈 초기화 때 보내는 명령)
- 연결이 끊긴 뒤 다시 맺는 연결(연결 수립 명령이 다시 나감)
- 스크립트 캐시가 비워진 뒤(`EVALSHA` 실패 후 `EVAL`로 다시 보냄)
- 헬스 체크와 지표 수집

## 분류 단위로 막을까, 명령 단위로 열까

목록을 얻었으면 규칙으로 옮깁니다. 방향은 크게 둘입니다. 모두 열고 위험한 것을 빼는 **거부 목록**과, 모두 막고 필요한 것만 여는 **허용 목록**입니다. 분류(`@...`)는 둘 다에 쓸 수 있습니다.

### 분류는 서버가 정한 묶음이고, 앱의 필요와 맞지 않습니다

분류는 명령의 성격으로 묶은 것이지 "어느 앱에 필요한가"로 묶은 것이 아닙니다. 그래서 분류 하나에 앱이 꼭 쓰는 명령과 절대 쓰면 안 되는 명령이 함께 들어 있습니다.

| 분류 | 앱이 쓰는 명령 | 함께 들어 있는 위험한 명령 |
| --- | --- | --- |
| `@dangerous` | `INFO`(헬스 체크), `KEYS`(캐시 비우기) | `FLUSHALL`, `FLUSHDB`, `SWAPDB`, `CONFIG SET`, `SHUTDOWN`, `ACL SETUSER`, `MONITOR` 등 |
| `@keyspace` | `DEL`, `EXPIRE`, `SCAN`, `KEYS` | `FLUSHALL`, `FLUSHDB`, `SWAPDB` |
| `@write` | `SET`, `DEL`, `DECR` | `FLUSHALL`, `FLUSHDB`, `SWAPDB`, `RESTORE`, `MIGRATE` |
| `@admin` | 없음 | `CONFIG GET`·`CONFIG SET`, `ACL SETUSER`·`ACL LIST`, `SHUTDOWN`, `MONITOR`, `REPLICAOF`, `CLIENT KILL`, `CLIENT LIST`, `SAVE` 등 |

이 표의 `@dangerous` 줄이 [영속성](./persistence.md)에서 `FLUSHALL`·`FLUSHDB`를 분류째가 아니라 명령 단위로 빼라고 한 이유입니다. `-@dangerous`는 헬스 체크와 캐시 비우기를 함께 깨뜨립니다. 반대로 `@keyspace`나 `@write`를 "필요한 분류만 열었다"고 생각하고 열면, 막으려던 `FLUSHALL`이 다시 열립니다. `FLUSHALL`은 키 공간 전체를 쓰는 명령이라 두 분류에 모두 들어 있기 때문입니다.

`ACL CAT <분류>`로 그 서버에서 분류에 실제로 든 명령을 볼 수 있습니다. 분류의 구성원은 서버가 정하므로, 규칙을 쓰기 전에 운영과 같은 버전의 서버에서 확인합니다.

### 세 가지 규칙 수준

예시 앱을 기준으로 세 수준을 비교합니다. 모든 규칙은 `on >비밀번호` 뒤에 붙는다고 봅니다.

| 수준 | 규칙 | 얻는 것 | 잃는 것 |
| --- | --- | --- | --- |
| ① 사고 명령만 빼기 | `~* +@all -flushall -flushdb -swapdb` | 기능이 깨질 일이 거의 없음. 바로 적용 가능 | `CONFIG SET`, `SHUTDOWN`, `ACL SETUSER`가 열려 있음. 계정이 유출되면 자기 권한을 넓히거나 서버를 끌 수 있음. 다른 팀 키도 읽고 지울 수 있음 |
| ② 관리 명령까지 빼기 | `~* +@all -@admin -flushall -flushdb -swapdb` | 서버 설정·계정·종료를 막음. `INFO`, `KEYS`는 `@admin`이 아니라 그대로 동작 | 모든 데이터 명령과 앞으로 생길 명령이 열려 있음. 키 범위 제한 없음 |
| ③ 필요한 것만 열기 | `resetkeys resetchannels clearselectors -@all +get +set +del +keys +evalsha +eval +decr +info +client\|setinfo ~product::* ~stock:item:*` | 쓸 수 있는 명령이 위 표의 명령으로, 닿는 키가 두 접두사로 줄어듦. 다른 팀 키에 닿지 않음 | 코드나 설정이 바뀌어 새 명령이 생기면 배포 뒤 깨질 수 있음. 목록을 관리해야 함 |

**전용 앱 계정에는 ③을 고릅니다.** 이유는 ①·②의 남은 위험이 "사람의 실수"가 아니라 "계정 유출"과 "다른 팀 데이터 침범"이라서, 사고가 나면 되돌리기 어렵기 때문입니다. ③의 대가인 "새 명령 때문에 깨짐"은 다음 절의 방법으로 테스트가 지나간 경로에서는 배포 전에 잡을 수 있고, 잡지 못하더라도 오류가 분명하게 드러나며 규칙 한 줄로 고칠 수 있습니다.

③도 "명령 목록"과 "키 범위"를 따로 제한할 뿐, 둘의 조합까지 정하지는 않습니다. 허용한 명령은 허용한 모든 키에 쓸 수 있으므로, 캐시용으로 연 `SET`·`DEL`로 재고 키 `stock:item:1`을 덮어쓰거나 지울 수도 있습니다. 키 영역마다 허용 명령을 달리하려면 셀렉터로 규칙을 나눠야 하는데, 이때는 "셀렉터는 '키를 나눠 맞추기'를 허용하지 않습니다"의 제약을 함께 따져야 합니다.

②는 여러 앱이 한 계정을 함께 써서 명령 목록을 모을 수 없을 때의 차선입니다. ③을 준비하는 동안 먼저 적용하는 단계로도 씁니다. ①은 [영속성](./persistence.md)이 다룬 "실수로 보낸 `FLUSHALL`"만 막는 최소한의 조치입니다.

### 순서 때문에 다시 열리는 규칙

규칙은 적힌 순서대로 적용되므로, 빼는 규칙은 넓게 여는 규칙 **뒤에** 둬야 합니다. `-flushall +@all`은 `FLUSHALL`을 뺀 뒤 `+@all`로 다시 엽니다. ③처럼 `-@all`에서 시작해 하나씩 여는 규칙은 순서 실수의 여지가 적다는 장점도 있습니다.

또 하나, `ACL SETUSER`는 기존 사용자에게 규칙을 덧붙입니다. 기존에 `~*`나 `+@all`이 있던 사용자에게 `+get ~product::*` 같은 허용 규칙만 보내면 넓은 권한이 그대로 남습니다. ③의 맨 앞 `resetkeys resetchannels -@all`은 괄호 밖 기본 규칙의 키 패턴·채널·명령 허용을 지웁니다. 괄호로 더한 셀렉터는 이것으로 지워지지 않고 `clearselectors`로만 지워지므로 ③에 함께 넣었습니다. `ACL GETUSER product-api`로 적용 결과를 확인합니다.

## 키 패턴 제한이 막는 것과 못 막는 것

### 막는 것: 이름이 인자로 들어가는 키

③의 `~product::* ~stock:item:*`은 키를 인자로 받는 명령에서 그 키가 패턴에 맞는지 봅니다. `GET product::42`는 통과하고, 다른 팀의 `GET order:7`은 거부됩니다.

키를 여럿 받는 명령은 **모든 키가** 맞아야 합니다. 캐시 비우기가 보내는 `DEL product::1 product::2 ...`에 패턴 밖 키가 하나라도 섞이면 명령 전체가 거부되고, 하나도 지워지지 않습니다. 검사가 실행 전에 일어나기 때문입니다.

### 못 막는 것: 키를 인자로 받지 않는 명령

키 패턴은 키 인자가 있는 명령에만 적용됩니다. 키 인자가 없는 명령은 명령 규칙으로만 막을 수 있습니다.

- **`FLUSHALL`, `FLUSHDB`, `SWAPDB`**: DB 전체를 다루고 키 인자가 없습니다. `~product::* +@all`인 사용자도 `FLUSHALL`로 다른 팀 키까지 모두 지울 수 있습니다. 그래서 ①·②에서도 명령으로 따로 뺍니다.
- **`KEYS`, `SCAN`**: `KEYS product::*`의 `product::*`는 키가 아니라 패턴 인자라 검사하지 않습니다. 그래서 `~product::*`인 사용자가 `KEYS *`를 보내면 **DB의 모든 키 이름을 받습니다.** 값은 읽지 못하지만, 키 이름에 사용자 ID나 이메일을 넣는 서비스라면 그 자체가 정보 유출입니다. 예시 앱이 `+keys`(또는 `+scan`)를 여는 것은 이 위험을 받아들이는 선택입니다. 키 이름이 민감하다면 다른 앱과 Redis(또는 DB 번호가 아니라 서버)를 나누는 쪽이 확실합니다.

### 읽기·쓰기를 나눈 패턴은 명령마다 요구가 다릅니다

`%R~`(읽기만)와 `%W~`(쓰기만)로 키 권한을 나눌 수 있습니다. 명령이 그 키를 읽는지 쓰는지는 서버의 명령 정의가 정하는데, 직관과 다른 경우가 있습니다.

| 명령 | 요구하는 키 권한 | 이유 |
| --- | --- | --- |
| `GET` | 읽기 | 값을 돌려줌 |
| `SET k v PX ...` | 쓰기만 | 값을 바꾸지만 돌려주지 않음. `GET` 옵션을 붙이면 읽기도 필요 |
| `GETEX` | 읽기와 쓰기 | 값을 돌려주고 만료 시간도 바꿈 |
| `DEL` | 쓰기 | 지움 |
| `DECR` | 읽기와 쓰기 | 값을 바꾸고 새 값을 돌려줌 |
| `EVAL`, `EVALSHA`의 `KEYS` | 읽기와 쓰기 | 스크립트가 무엇을 할지 실행 전에는 모르므로 둘 다 요구 |
| `EVAL_RO`의 `KEYS` | 읽기 | 읽기 전용 스크립트 |

그래서 "보고서 앱은 `%R~report:*`로 읽기만"이라고 정해 두고 그 앱이 `EVAL`로 읽기만 하는 스크립트를 돌리면 거부됩니다. 이때는 `EVAL_RO`를 쓰거나 쓰기 권한까지 줘야 합니다.

### 셀렉터는 '키를 나눠 맞추기'를 허용하지 않습니다

괄호로 묶은 규칙 묶음(셀렉터)을 더하면, 기본 규칙 또는 셀렉터 가운데 **하나가 명령 전체를** 통과시켜야 허용됩니다. 예를 들어 `+@all ~app1:* (+@read ~app2:*)`인 사용자가 `app2:user`를 `app1:user`로 복사하는 `COPY`를 보내면, 기본 규칙은 명령은 통과하지만 원본 키 `app2:user`가 패턴에 없어 막히고, 셀렉터는 `COPY`가 읽기 분류가 아니라 명령 검사에서 막히므로 거부됩니다. 셀렉터에 `+copy`를 더해도 대상 키 `app1:user`가 셀렉터의 `~app2:*`에 맞지 않아 여전히 거부됩니다. 두 키를 한 규칙 안에서 다 맞춰야 합니다(공식 문서 ACL "Key permissions"의 예).

## Lua 스크립트와 MULTI는 두 번 검사됩니다

스크립트와 트랜잭션은 "명령 하나 안에 다른 명령이 들어 있는" 구조라 검사 시점이 둘입니다. 결과적으로 실패했을 때 남는 상태가 다릅니다.

### Lua: 실행 전에 선언한 키, 실행 중에 명령마다

예시의 재고 스크립트를 따라가 봅니다.

1. **실행 전**: 서버는 `EVALSHA <sha1> 1 stock:item:1` 자체를 검사합니다. `EVALSHA`가 허용되어 있는가, 그리고 `KEYS`로 선언한 `stock:item:1`이 키 패턴에 맞는가(읽기·쓰기 모두)를 봅니다. 여기서 걸리면 스크립트는 시작하지 않습니다.
2. **실행 중**: 스크립트 안의 `redis.call('GET', ...)`과 `redis.call('DECR', ...)`도 **명령마다 같은 사용자 권한으로** 다시 검사합니다. `+evalsha`만 열고 `+decr`을 빼면, 스크립트는 시작하고 `GET`까지 한 뒤 `DECR`에서 실패합니다.

실행 중 실패의 결과가 중요합니다. 스크립트는 그 자리에서 멈추고, **앞에서 이미 한 쓰기는 되돌리지 않습니다**([Redis 원자성](./atomicity.md)의 "대가 2"). 예시 스크립트는 `DECR`이 첫 쓰기라 남는 쓰기가 없지만, "구매자 기록 → 재고 차감" 순서의 스크립트라면 구매자만 기록된 상태가 남습니다.

스크립트 안에서 선언하지 않은 키에 접근하면 그 키는 실행 중에 처음 검사됩니다. 패턴에 맞으면 그대로 통과하지만, 맞지 않으면 같은 중간 실패가 납니다. 셀렉터를 쓰지 않는 사용자라면 키를 모두 `KEYS`로 넘길 때 키 권한 문제가 실행 전에 걸러집니다. 선언한 키는 실행 전에 읽기·쓰기를 모두 검사받기 때문입니다. 셀렉터마다 명령과 키 범위가 다르면 바깥 `EVAL`은 통과하고 안쪽 명령과 키의 조합이 거부될 수 있습니다. 명령 권한 문제는 실행 전에 걸러지지 않으므로, 스크립트가 부르는 명령 목록을 ACL 목록에 함께 적어 둡니다.

### MULTI/EXEC: 대기열에 넣을 때, 그리고 EXEC 때

`MULTI` 뒤에 보낸 명령은 대기열에 넣기 전에 검사됩니다. 하나라도 거부되면 그 명령은 `NOPERM`을 받고, 트랜잭션은 "실패 표시"가 되어 `EXEC`가 `EXECABORT`로 아무것도 실행하지 않고 끝납니다. 대기열의 명령은 `EXEC` 때 한 번 더 검사합니다. 명령을 쌓는 사이에 운영자가 ACL을 바꿨을 수 있기 때문입니다. 이때 거부된 명령만 실행되지 않고 나머지는 실행됩니다.

## 권한이 모자라면 앱에는 이렇게 드러납니다

### 오류의 모양

| 상황 | 서버 응답 | Lettuce 예외 | Spring Data Redis 예외 |
| --- | --- | --- | --- |
| 명령이 막힘 | `NOPERM User product-api has no permissions to run the 'keys' command` | `RedisCommandExecutionException` | `RedisSystemException` |
| 키가 막힘 | `NOPERM No permissions to access a key` | `RedisCommandExecutionException` | `RedisSystemException` |
| 스크립트 안에서 막힘 | `ERR ACL failure in script: ...`로 시작하는 오류 | `RedisCommandExecutionException` | `RedisSystemException` |
| 트랜잭션 안에서 막힘 | 대기열에 넣을 때 막히면 그 명령은 `NOPERM`, `EXEC`는 `EXECABORT`. `EXEC` 때 재검사에서 막히면 그 명령의 결과만 `NOPERM` | `RedisCommandExecutionException` | `RedisSystemException` 등 |
| 비밀번호·사용자 이름 틀림, 사용자 꺼짐 | `WRONGPASS invalid username-password pair or user is disabled.` | 연결 실패(`RedisConnectionException`) | `RedisConnectionFailureException` |
| 연결 직후 `SELECT`가 막힘 | `NOPERM ...` | 연결 실패 | `RedisConnectionFailureException` |

잘못 읽기 쉬운 지점이 셋 있습니다.

- **키 거부 메시지는 어느 키인지 말하지 않습니다.** 명령에 대한 일반 응답에는 키 이름이 없습니다. 어느 키였는지는 서버의 `ACL LOG`에서 봅니다.
- **스크립트 안의 거부는 `NOPERM`이 아니라 `ERR`로 시작합니다.** 로그에서 `NOPERM`만 찾으면 스크립트 안의 권한 문제를 놓칩니다. `ACL failure in script`도 함께 찾습니다.
- **권한 문제가 연결 실패로 보일 수 있습니다.** 연결 직후의 `SELECT`(또는 클러스터 복제본의 `READONLY`)가 거부되면 연결이 맺어지지 않으므로, 앱 로그에는 네트워크 장애와 같은 `RedisConnectionFailureException`이 찍힙니다. 원인 예외의 메시지까지 봐야 구분됩니다.

### 기능별 증상

- **캐시**: `@Cacheable`의 조회·저장·삭제에서 난 예외는 캐시 에러 처리기로 갑니다. 기본 처리기는 예외를 다시 던지므로 그 API가 실패합니다. 에러를 삼키도록 처리기를 바꿨다면 API는 성공하지만 캐시가 늘 미스가 되거나(저장 거부) 지워지지 않습니다(삭제 거부). 겉으로는 "캐시가 효과가 없다"로만 보입니다. 처리기의 선택은 [Spring @Cacheable](./spring-cacheable.md)에서 다룹니다.
- **헬스 체크**: `INFO`가 거부되면 Redis 헬스가 DOWN이 되고, 전체 `/actuator/health`도 DOWN이 됩니다. 부하 분산기나 배포 도구가 이 엔드포인트를 본다면 데이터 명령은 멀쩡한데 인스턴스가 빠지거나 배포가 멈출 수 있습니다.
- **시작 단계**: 빈 초기화 때 보내는 명령(예: 키 만료 리스너의 `CONFIG GET`)이 거부되면 앱이 뜨지 않습니다.

### 재시도로 해결되지 않습니다

권한 거부는 같은 명령을 다시 보내도 같은 결과입니다. 연결 실패나 타임아웃과 달리 일시적인 오류가 아니므로, 재시도 정책에서 `NOPERM`(그리고 스크립트의 `ACL failure`)을 재시도 대상에서 빼야 합니다. 그렇지 않으면 재시도 횟수만큼 지연과 거부 기록만 늘어납니다. Spring Data Redis는 이 오류를 `RedisSystemException` 하나로 감싸므로, 재시도 판단은 예외 종류가 아니라 원인 예외의 메시지로 해야 합니다.

## 배포 전에 빠진 권한을 찾고, 안전하게 바꾸기

### 서버가 남기는 거부 기록: `ACL LOG`

서버는 권한 거부와 인증 실패를 `ACL LOG`에 남깁니다. 항목마다 다음이 있습니다.

| 필드 | 뜻 | 예시 |
| --- | --- | --- |
| `reason` | 무엇이 막혔나: `command`, `key`, `channel`, `auth` | `command` |
| `context` | 어디서: `toplevel`(일반), `multi`(트랜잭션), `lua`(스크립트), `module` | `lua` |
| `object` | 막힌 명령 이름, 키, 채널 | `decr` |
| `username` | 누가 | `product-api` |
| `client-info` | 어느 연결(주소, 이름, 마지막 명령 등) | |
| `count`, `age-seconds` | 같은 거부가 몇 번, 얼마 전에 | |

같은 사유·위치·대상·사용자의 거부가 짧은 간격으로 반복되면 보통 새 항목 대신 `count`가 늘어나지만, 다른 거부가 사이에 많이 끼면 같은 거부도 새 항목이 될 수 있습니다. 항목 수는 `acllog-max-len`(기본 128)까지만 두고 오래된 것부터 버립니다. 인자 없는 `ACL LOG`는 그 가운데 최근 10개만 보여 주므로, 더 보려면 `ACL LOG 128`처럼 개수를 줍니다. 서버 전체의 누적 숫자는 `INFO stats`의 `acl_access_denied_auth`, `acl_access_denied_cmd`, `acl_access_denied_key`, `acl_access_denied_channel`로 볼 수 있어 지표 수집에 붙이기 좋습니다.

### 절차: 스테이징에서 '운영과 같은 ACL'로 돌려 봅니다

1. **목록 만들기**: "명령 목록은 앱 쪽과 서버 쪽 두 곳에서 모읍니다"의 방법으로 명령을 모으고, 스크립트가 부르는 명령과 키 패턴을 더해 ③ 같은 규칙을 씁니다.
2. **스테이징에 적용**: 스테이징 Redis에 운영과 같은 규칙의 `product-api` 사용자를 만들고, 앱이 그 사용자로 붙게 합니다. `ACL LOG RESET`으로 기록을 비웁니다.
3. **경로 돌리기**: 통합 테스트와 함께 앱 시작, 캐시 비우기, 재고 차감, 헬스 체크, Redis 재시작 뒤 재연결(이때 사용자와 규칙이 그대로 남았는지도 `ACL GETUSER`로 확인), `SCRIPT FLUSH` 뒤 스크립트 실행처럼 "어느 쪽이든 '실행된 경로'만 보입니다"의 경로를 일부러 지나게 합니다.
4. **기록 보기**: `ACL LOG`에서 `username`이 `product-api`인 항목을 봅니다. 항목마다 "열어야 할 권한인가, 앱이 보내지 말아야 할 명령인가"를 판단합니다. 예를 들어 `object`가 `keys`면, 열어 줄지 아니면 캐시를 `SCAN` 방식으로 바꿀지를 정합니다.
5. **한 명령씩 확인**: 특정 명령이 허용되는지는 `ACL DRYRUN product-api <명령과 인자>`로 실제로 실행하지 않고 확인할 수 있습니다. 예를 들어 `ACL DRYRUN product-api FLUSHALL`이 `OK`가 아니라 거부 이유를 돌려주는지 봅니다. 다만 `ACL DRYRUN`은 명령 하나의 바깥만 검사합니다. `EVAL`이면 `EVAL`과 선언한 키만 보고, 스크립트 안의 명령은 실행하지 않으므로 검사하지 않습니다. 스크립트는 3번처럼 실제로 돌려 봐야 합니다.

### 운영에 적용할 때

- **바꾼 규칙은 이미 붙어 있는 연결에도 곧바로 적용됩니다**(소스에서 추론, 재현 안 함). 그래서 권한을 좁히는 변경은 앱 재시작 없이 바로 효과가 나고, 잘못 좁혔을 때도 바로 깨집니다. 반대로 되돌리는 것도 규칙 한 줄로 바로 됩니다.
- **규칙 하나라도 틀리면 아무것도 바뀌지 않습니다.** `ACL SETUSER`는 규칙이 모두 올바를 때만 사용자에 반영합니다. ACL 파일(`aclfile`)을 쓰면 `ACL LOAD`도 파일의 모든 사용자가 올바를 때만 새 설정을 적용하고, 아니면 기존 설정을 유지합니다.
- **적용 직후 거부 기록을 봅니다.** 넓은 규칙(②)에서 좁은 규칙(③)으로 바꿀 때는 적용 직후 `acl_access_denied_*` 지표와 `ACL LOG`를 지켜보고, 거부가 생기면 해당 명령을 열거나 직전 규칙으로 돌립니다.
- **앱의 Redis 관련 변경을 리뷰할 때 ACL 목록도 함께 봅니다.** 새 명령을 쓰는 코드, 캐시 작성기·비우기 방식·TTL 갱신 설정의 변경, Redis를 쓰는 라이브러리 추가가 모두 명령 목록을 바꿉니다. 이런 변경을 여러 인스턴스에 차례로 배포할 때는 옛 버전과 새 버전이 함께 돕니다. 그래서 새 권한(예: 캐시 비우기를 `SCAN`으로 바꿀 때 `+scan`)은 배포 전에 먼저 열고, 옛 권한(`+keys`)은 옛 인스턴스가 모두 내려간 뒤에 닫습니다. 대가는 그 사이 두 권한이 함께 열려 있다는 것입니다.
- **바꾼 규칙을 저장해야 재시작 뒤에도 남습니다.** `ACL SETUSER`는 서버 메모리의 규칙만 바꿉니다. Redis는 시작할 때 설정 파일이나 ACL 파일(`aclfile`)에서 사용자를 읽으므로, 저장하지 않으면 재시작 뒤 옛 규칙으로 돌아가 새로 만든 사용자가 없어져 앱이 `WRONGPASS`로 연결하지 못하거나 좁힌 권한이 다시 넓어집니다. `aclfile`을 쓰면 `ACL SAVE`로 저장합니다. 쓰지 않으면 설정 파일을 지정해 띄운 서버에서만 `CONFIG REWRITE`로 설정 파일에 저장할 수 있습니다. `aclfile` 없이 보낸 `ACL SAVE`도, 설정 파일 없이 띄운 서버의 `CONFIG REWRITE`도 거부됩니다.

## Pub/Sub 채널도 따로 엽니다

새 사용자는 채널도 하나도 쓸 수 없습니다(`acl-pubsub-default`의 기본값 `resetchannels`). 그래서 앱이 Pub/Sub을 쓰면 `+subscribe`나 `+psubscribe` 같은 명령과 함께 채널 패턴을 `&`로 열어야 합니다.

채널 패턴은 명령에 따라 맞추는 방식이 다릅니다. `SUBSCRIBE`와 `PUBLISH`의 채널 이름은 ACL의 채널 패턴과 glob으로 맞춥니다. 반면 `PSUBSCRIBE`가 보내는 것은 채널 이름이 아니라 구독 패턴이므로, ACL의 채널 패턴과 **글자 그대로 같아야** 합니다. 모든 채널을 여는 `&*`(`allchannels`)만은 비교 없이 통과합니다. 키 만료 리스너는 `PSUBSCRIBE __keyevent@*__:expired`를 보내므로 ACL에도 `&__keyevent@*__:expired`를 똑같이 적어야 하고, `&__keyevent@*`처럼 더 넓은 패턴으로는 통과하지 않습니다. 패턴을 넓게 열어도 거부되는 이 동작은 직관과 반대이므로, 채널 권한은 반드시 실제 구독으로 확인합니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 본문 |
| --- | --- | --- |
| 키 패턴을 `~product::*`로 좁혔으니 다른 키는 건드릴 수 없다 | 키 인자가 없는 `FLUSHALL`(모든 DB를 비움)·`FLUSHDB`(지금 DB를 비움)·`SWAPDB`(두 DB의 내용을 맞바꿈)는 키 패턴과 상관없이 실행되고, `KEYS *`는 모든 키 이름을 돌려줍니다 | 키 패턴 제한이 막는 것과 못 막는 것 |
| `-@dangerous`면 위험한 명령이 다 막히고 앱은 문제없다 | `INFO`와 `KEYS`도 막혀 헬스 체크와 캐시 비우기가 실패합니다 | 분류는 서버가 정한 묶음이고, 앱의 필요와 맞지 않습니다 |
| `+@write`나 `+@keyspace`만 열면 `FLUSHALL`은 닫혀 있다 | 두 분류 모두 `FLUSHALL`을 포함합니다 | 분류는 서버가 정한 묶음이고, 앱의 필요와 맞지 않습니다 |
| 앱이 쓰는 명령은 `RedisTemplate` 호출을 세면 된다 | 연결 수립, 캐시, 헬스 체크, 리스너 초기화가 명령을 따로 보냅니다 | 앱이 실제로 보내는 명령은 직접 쓴 코드보다 많습니다 |
| `+eval`을 열면 스크립트는 다 돈다 | 스크립트 안의 명령도 하나씩 검사하며, 중간에 막히면 앞의 쓰기가 남습니다 | Lua: 실행 전에 선언한 키, 실행 중에 명령마다 |
| `ACL DRYRUN`이 `OK`면 스크립트도 안전하다 | 스크립트 안의 명령은 검사하지 않습니다 | 절차: 스테이징에서 '운영과 같은 ACL'로 돌려 봅니다 |
| 권한 오류는 로그에서 `NOPERM`으로 찾으면 된다 | 스크립트 안의 거부는 `ERR ACL failure in script`이고, `SELECT` 거부는 연결 실패로 보입니다 | 오류의 모양 |
| 권한 오류도 재시도하면 언젠가 성공한다 | 같은 명령은 같은 결과입니다. 재시도 대상에서 뺍니다 | 재시도로 해결되지 않습니다 |
| 채널을 넓은 패턴으로 열면 좁은 패턴 구독도 된다 | `PSUBSCRIBE`의 패턴은 ACL 패턴과 글자 그대로 같아야 합니다(모든 채널을 여는 `&*`만 예외) | Pub/Sub 채널도 따로 엽니다 |

## 근거

확인한 버전: Redis 태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10)(커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`), 공식 문서 저장소 `redis/docs` 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`, Spring Boot 태그 `v3.5.16`, Spring Data Redis 태그 `3.5.13`, Lettuce 태그 `6.6.0.RELEASE`(Spring Boot 3.5.16의 [`spring-boot-dependencies/build.gradle`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-dependencies/build.gradle)이 고른 버전).

**Redis 소스**

- [`src/acl.c`](https://github.com/redis/redis/blob/8.2.10/src/acl.c)
  - `ACLSelectorCheckCmd`: 명령 비트 검사(`CMD_NO_AUTH` 명령은 건너뜀), 키 인자가 있는 명령만 key specs로 키를 뽑아 하나씩 검사, 이어서 채널 검사 — "서버는 명령마다 세 가지를 차례로 검사합니다", "`AUTH`와 `HELLO`는 명령 검사를 받지 않습니다", "키를 여럿 받는 명령은 모든 키가 맞아야"
  - `ACLSelectorCheckKey`: 키 spec의 `ACCESS`는 읽기, `INSERT`·`DELETE`·`UPDATE`는 쓰기 권한을 요구하고 패턴의 권한이 모두 맞아야 통과 — "읽기·쓰기를 나눈 패턴은 명령마다 요구가 다릅니다"
  - `ACLCheckAllUserCommandPerm`: 셀렉터마다 명령 전체를 검사해 하나라도 통과하면 허용 — "셀렉터는 '키를 나눠 맞추기'를 허용하지 않습니다"
  - `ACLCheckChannelAgainstList`: 패턴 구독은 ACL 패턴과 문자열이 같아야, 일반 채널은 glob 일치 — "Pub/Sub 채널도 따로 엽니다"
  - `getAclErrorMessage`: 명령 거부 "User %S has no permissions to run the '%S' command", 키 거부는 상세 모드가 아니면 "No permissions to access a key" — "오류의 모양", "키 거부 메시지는 어느 키인지 말하지 않습니다"
  - `ACLStringSetUser`: 규칙을 임시 사용자에 모두 적용해 본 뒤 성공하면 `ACLCopyUser`로 기존 사용자 객체에 복사 — "규칙 하나라도 틀리면 아무것도 바뀌지 않습니다", "이미 붙어 있는 연결에도 곧바로 적용"(연결이 가리키는 객체를 고친다는 추론의 근거)
  - `ACLCreateUser`(새 사용자는 `USER_FLAG_DISABLED`, 비밀번호 목록 비어 있음), `ACLCreateSelector`(채널 허용은 `acl-pubsub-default`를 따름), `ACLSetUser`의 `clearselectors`(괄호 밖 기본 규칙은 바꾸지 않고 추가 셀렉터만 지움), 기본 셀렉터에 대한 `resetkeys`·`resetchannels`·`-@all` — 새 사용자의 기본값, "순서 때문에 다시 열리는 규칙"
  - `ACLSetSelector`: `&*`·`allchannels`는 채널 전체 허용 플래그를 세워 채널 검사를 건너뜀 — "Pub/Sub 채널도 따로 엽니다"
  - `addACLLogEntry`(최근 10개 항목에서만 같은 항목을 찾고, 찾으면 마지막 갱신 시각을 바꾸고 `count` 증가), `ACLLogMatchEntry`(`ACL_LOG_GROUPING_MAX_TIME_DELTA 60000`, 사유·위치·대상·사용자가 같아야 함), `aclCommand`의 `log`(인자 없으면 10개 반환), `trimACLLogEntriesToMaxLen`, `aclCommand`의 `log` 출력 필드(`count`, `reason`, `context`, `object`, `username`, `age-seconds`, `client-info` 등) — "`ACL LOG`"
  - `aclCommand`의 `dryrun`: 명령 조회와 인자 수 확인 뒤 `ACLCheckAllUserCommandPerm`만 호출(실행하지 않음) — "`ACL DRYRUN`은 명령 하나의 바깥만 검사"
  - `ACLUpdateInfoMetrics` — `INFO stats`의 `acl_access_denied_*` 카운터
  - `aclCommand`: `aclfile`이 없으면 `ACL LOAD`·`ACL SAVE`를 오류로 거부하며 `ACL SETUSER` 뒤 `CONFIG REWRITE`로 저장하라고 안내. `ACLLoadUsersAtStartup`: 시작할 때 설정 파일의 `user` 줄 또는 `aclfile`에서 사용자를 읽음 — "바꾼 규칙을 저장해야 재시작 뒤에도 남습니다"
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c)
  - `processCommand`: 인증 전에는 `CMD_NO_AUTH` 명령만 허용, 이어서 `ACLCheckAllPerm`, 거부 시 `-NOPERM` 응답과 `ACL LOG` 기록(`MULTI` 중이면 `multi` 위치) — "오류의 모양"
  - `rejectCommand`: `flagTransaction`을 불러 트랜잭션에 실패 표시, `rejected_calls` 증가 — "MULTI/EXEC: 대기열에 넣을 때"
  - `setImplicitACLCategories`: `WRITE` 플래그는 `@write`, `ADMIN` 플래그는 `@admin`과 `@dangerous` — 분류 표
  - `genRedisInfoStringCommandStats`(`cmdstat_<명령|하위 명령>:calls=...,rejected_calls=...`), `genRedisInfoStringACLStats`(`# Stats` 절 안) — "서버 쪽: `INFO commandstats`", "`ACL LOG`"
  - `createSharedObjects`: `EXECABORT Transaction discarded because of previous errors.`
- [`src/eval.c`](https://github.com/redis/redis/blob/8.2.10/src/eval.c): `EVAL` 계열은 `server.c`의 `call`이 아니라 여기서 따로 `replicationFeedMonitors`로 `MONITOR`에 보냄. `server.c`의 `call`은 `CMD_SKIP_MONITOR`·`CMD_ADMIN` 명령을 `MONITOR`에 보내지 않음 — "`CONFIG GET` 같은 관리 명령은 출력에서 빠지므로"
- [`src/multi.c`](https://github.com/redis/redis/blob/8.2.10/src/multi.c): `flagTransaction`, `execCommand`(대기열 명령마다 `ACLCheckAllPerm`을 다시 하고 거부된 명령만 `NOPERM ACLs rules changed ...`로 응답) — "MULTI/EXEC: 대기열에 넣을 때, 그리고 EXEC 때"
- [`src/script.c`](https://github.com/redis/redis/blob/8.2.10/src/script.c): `scriptCall`이 스크립트 안 명령마다 `scriptVerifyACL`을 부름, 거부 시 "ACL failure in script: " 메시지와 `lua` 위치 기록 — "Lua: 실행 전에 선언한 키, 실행 중에 명령마다"
- [`src/script_lua.c`](https://github.com/redis/redis/blob/8.2.10/src/script_lua.c): `luaPushErrorBuff`가 `-`로 시작하지 않는 오류에 `ERR` 코드를 붙임 — "스크립트 안의 거부는 `NOPERM`이 아니라 `ERR`로 시작"
- [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c): `setGetKeys`(`GET` 옵션이 없으면 `OW|UPDATE`, 있으면 `RW|ACCESS|UPDATE`) — "`SET k v PX ...`는 쓰기만"
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `acllog-max-len` 기본 128, `acl-pubsub-default` 기본 `resetchannels`, `rewriteConfigUserOption`(`aclfile`이 없으면 `CONFIG REWRITE`가 현재 사용자 목록을 설정 파일의 `user` 줄로 다시 씀), `configRewriteCommand`(설정 파일 없이 띄운 서버면 오류)
- 명령 정의 JSON(`src/commands/`)
  - [`get.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/get.json)(`RO ACCESS`), [`set.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/set.json)(`VARIABLE_FLAGS`), [`getex.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/getex.json)·[`decr.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/decr.json)(`RW ACCESS UPDATE`), [`del.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/del.json)(`RM DELETE`) — 키 권한 표
  - [`eval.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/eval.json)·[`evalsha.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/evalsha.json)(키 `RW ACCESS UPDATE`), [`eval_ro.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/eval_ro.json)(키 `RO ACCESS`)
  - [`keys.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/keys.json)(`KEYSPACE`, `DANGEROUS`, key specs 없음), [`scan.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/scan.json)(`KEYSPACE`, key specs 없음), [`flushall.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/flushall.json)·[`flushdb.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/flushdb.json)·[`swapdb.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/swapdb.json)(`WRITE`, `KEYSPACE`, `DANGEROUS`, key specs 없음), [`info.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/info.json)(`DANGEROUS`, `ADMIN` 플래그 없음)
  - [`hello.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hello.json)·[`auth.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/auth.json)(`NO_AUTH`), [`client-setinfo.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/client-setinfo.json)(`CONNECTION`, `ADMIN` 플래그 없음), [`config-get.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/config-get.json)(`ADMIN`)
  - 분류 표의 구성원은 8.2.10의 명령 JSON 전체를 읽어 `setImplicitACLCategories` 규칙을 적용해 계산한 결과입니다(`@keyspace`·`@write`에 `flushall`·`flushdb`·`swapdb` 포함, `@dangerous`이면서 `@admin`이 아닌 명령은 `flushall`, `flushdb`, `info`, `keys`, `migrate`, `restore`, `sort`, `swapdb` 등)

**Redis 공식 문서**

- [ACL](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/security/acl.md): 규칙은 왼쪽부터 적용, 기본 사용자 `default`, 새 사용자는 꺼져 있고 권한 없음, `ACL SETUSER`는 덧붙임, "Command categories", "Selectors", "Key permissions"(`COPY` 예, 쓰기 명령은 메타데이터만 돌려주면 쓰기 권한만, 키 인자가 없는 `FLUSHALL`·`FLUSHDB`·`SWAPDB`는 키 패턴으로 제한되지 않음), "Use an external ACL file"(`ACL LOAD`는 모든 사용자가 올바를 때만 적용). 이 문서의 예시 오류 메시지 "this user has no permissions to access one of the keys used as arguments"는 8.2.10 소스의 메시지와 다르므로, 본문은 소스의 메시지를 따릅니다
- [ACL LOG](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/acl-log.md): 항목 필드와 60초 묶음, 인자 없으면 최근 10개 반환
- [ACL DRYRUN](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/acl-dryrun.md): 명령을 실행하지 않고 사용자 권한으로 판정

**Lettuce 6.6.0.RELEASE**

- [`RedisHandshake.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisHandshake.java): 프로토콜을 정하지 않으면 `HELLO 3`(비밀번호가 있으면 `AUTH`, 이름이 있으면 `SETNAME` 포함)을 먼저 보내고, 모르는 명령·`NOPROTO`면 RESP2의 `AUTH`로 물러남. `applyPostHandshake`(`SELECT`, `READONLY`, 실패하면 연결 실패), `applyConnectionMetadataSafely`(`CLIENT SETNAME`은 RESP2일 때, `CLIENT SETINFO lib-name`·`lib-ver`, 실패는 debug 로그만) — 표의 연결 수립 행, "연결을 맺을 때 보내는 명령은 실패해도 되는 것과 안 되는 것으로 나뉩니다"
- [`RedisURI.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisURI.java): `libraryName` 기본값 `LettuceVersion.getName()`("Lettuce"), `libraryVersion` 기본값 jar의 구현 버전
- [`ClientOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ClientOptions.java): `getConfiguredProtocolVersion`(정하지 않으면 `null`이라 HELLO를 시도)
- [`cluster/topology/Connections.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/Connections.java): `requestTopology`(`CLUSTER NODES`), `requestInfo`(`INFO`). [`NodeTopologyView.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/NodeTopologyView.java): `from`이 `CLUSTER NODES` 실패면 노드를 쓸 수 없는 것으로, `INFO` 실패는 `null`로 넘김. [`DefaultClusterTopologyRefresh.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/DefaultClusterTopologyRefresh.java): 쓸 수 있는 노드가 없으면 `CannotRetrieveClusterPartitions` — 클러스터 문단
- [`internal/ExceptionFactory.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/internal/ExceptionFactory.java): `createExecutionException`이 `NOPERM`은 별도 종류 없이 `RedisCommandExecutionException`으로 만듦. [`AbstractRedisClient.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/AbstractRedisClient.java): 연결 실패를 `RedisConnectionException.create`로 감쌈 — "오류의 모양"
- [`metrics/MicrometerCommandLatencyRecorder.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/metrics/MicrometerCommandLatencyRecorder.java): `lettuce.command.completion`, `command` 태그는 명령 종류(`commandType`) — "앱 쪽: Lettuce 명령 지표"

**Spring Data Redis 3.5.13**

- [`cache/DefaultRedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/DefaultRedisCacheWriter.java): `doGet`(TTL 갱신이면 `getEx`, 아니면 `get`), `doPut`(TTL이 있으면 만료 옵션을 붙인 `set`), `putIfAbsent`(`SET NX`), `remove`(`del`), `clean`(배치 전략 호출), `doLock`·`doCheckLock`·`doUnlock`(`<캐시 이름>~lock` 키에 `SET NX`·`EXISTS`·`DEL`), `isLockingCacheWriter`(대기 시간이 0이면 잠금 없음) — 표의 캐시 행, "같은 기능도 설정에 따라 명령이 달라집니다"
- [`cache/BatchStrategies.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/BatchStrategies.java): `Keys`(`KEYS` 뒤 `DEL`), `Scan`(`n`을 `SCAN`의 `COUNT`로 쓰고, 찾은 키를 최대 `n`개씩 묶어 `DEL`). [`cache/RedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheWriter.java): `nonLockingRedisCacheWriter(connectionFactory)`의 기본 배치 전략은 `BatchStrategies.keys()`. [`cache/RedisCacheManager.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheManager.java): `builder(connectionFactory)`는 잠금 없는 작성기를 씀. [`cache/CacheKeyPrefix.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/CacheKeyPrefix.java): 구분자 `::`
- [`core/script/DefaultScriptExecutor.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/script/DefaultScriptExecutor.java): `EVALSHA`가 `NOSCRIPT`면 `EVAL`로 다시 보냄, `keysAndArgs`가 키를 템플릿의 `getKeySerializer()`로 직렬화 — 예시의 `StringRedisTemplate` 전제
- [`core/RedisTemplate.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisTemplate.java): 직렬화기를 정하지 않으면 `JdkSerializationRedisSerializer`를 기본으로 씀. [`cache/RedisCacheConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheConfiguration.java): `defaultCacheConfig`의 키 직렬화는 `RedisSerializer.string()`
- [`listener/KeyspaceEventMessageListener.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/listener/KeyspaceEventMessageListener.java): `afterPropertiesSet`에서 `init`, 설정값(기본 `EA`)이 비어 있지 않으면 `getConfig("notify-keyspace-events")` 뒤 비어 있으면 `setConfig`. [`listener/KeyExpirationEventMessageListener.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/listener/KeyExpirationEventMessageListener.java): 패턴 토픽 `__keyevent@*__:expired`
- [`connection/lettuce/LettuceExceptionConverter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceExceptionConverter.java): `RedisCommandExecutionException`은 `RedisSystemException("Error in execution")`, `RedisConnectionException`은 `RedisConnectionFailureException`

**Spring Boot v3.5.16**

- [`RedisHealthIndicator.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator/src/main/java/org/springframework/boot/actuate/data/redis/RedisHealthIndicator.java): 클러스터면 `clusterGetClusterInfo()`(`CLUSTER INFO`), 아니면 `serverCommands().info()`(`INFO`). [`RedisReactiveHealthIndicator.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator/src/main/java/org/springframework/boot/actuate/data/redis/RedisReactiveHealthIndicator.java): 리액티브는 `info("server")`
- [`LettuceMetricsAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/metrics/redis/LettuceMetricsAutoConfiguration.java): `MeterRegistry` 빈이 있으면 `MicrometerCommandLatencyRecorder`를 붙임
- [`RedisAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisAutoConfiguration.java): `redisTemplate`은 직렬화기를 정하지 않은 `RedisTemplate<Object, Object>`
- [`RedisProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisProperties.java): `username`, `password`, `database`

## 실무 포인트

### 중요도

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | 앱마다 전용 사용자를 만들고 `spring.data.redis.username`을 지정하기. `default` 사용자는 끄거나 강하게 제한하기 | 사용자 이름 없이 붙으면 모든 권한이 열린 `default`로 인증됩니다. 앱별로 나눠야 거부 기록에서 누구인지도 보입니다 |
| 필수 | 어떤 수준이든 `FLUSHALL`·`FLUSHDB`·`SWAPDB`를 명령 단위로 막고, 넓게 여는 규칙 뒤에 두기 | 키 패턴으로는 막히지 않고, `@write`·`@keyspace`에 들어 있어 분류로 열면 다시 열립니다 |
| 필수 | 앱 계정에서 `@admin`(특히 `ACL SETUSER`, `CONFIG SET`, `SHUTDOWN`) 빼기 | 계정이 유출되면 자기 권한을 넓히거나 서버 설정을 바꿀 수 있습니다 |
| 필수 | 권한 거부를 재시도 대상에서 빼기 | 같은 명령은 같은 결과이고 지연만 늘어납니다 |
| 알면 유리 | 전용 계정은 명령 단위 허용 목록(③)으로 운영하기 | 쓸 수 있는 명령과 닿는 키가 앱의 필요 범위로 줄어듭니다. 대가는 목록 관리입니다 |
| 알면 유리 | 스테이징에 운영과 같은 ACL을 두고 `ACL LOG`로 확인하기 | 빠진 권한을 배포 전에 찾는 가장 직접적인 방법입니다 |
| 알면 유리 | 키 이름이 민감하면 `KEYS`·`SCAN`을 여는 계정과 Redis를 나누기 | 키 패턴은 키 이름 목록 조회를 막지 못합니다 |
| 참고 | `+client\|setinfo` 열기 | 막아도 동작은 같지만 연결마다 거부 기록이 쌓여 진짜 문제를 가립니다 |

### 개발자가 정하는 것과 관찰해야 하는 것

| 정하는 것 | 관찰·검증하는 것 |
| --- | --- |
| 사용자 이름과 규칙(명령·키 패턴·채널) | `ACL GETUSER <사용자>`로 본 실제 적용 결과 |
| 캐시 비우기 방식(`KEYS`/`SCAN`), 잠금 사용, TTL 갱신 | `INFO commandstats`, `lettuce.command.completion`의 `command` 태그 |
| 키 이름 규칙(접두사) | 키 패턴 밖 키를 쓰는 코드가 있는지: `ACL LOG`의 `reason=key` |
| 스크립트가 부르는 명령과 `KEYS` 선언 | `ACL LOG`의 `context=lua` |
| 헬스 체크 사용 여부와 방식 | 헬스가 DOWN일 때 원인 예외 메시지 |
| 재시도 대상 오류 | 로그의 `NOPERM`, `ACL failure in script` 반복 |

### 증상과 대응

| 증상 | 의심할 원인 | 확인 순서 | 대응과 부작용 |
| --- | --- | --- | --- |
| 배포 뒤 특정 API만 실패, 로그에 `NOPERM ... run the 'xxx' command` | 새 코드나 설정이 쓰는 명령이 목록에 없음 | `ACL LOG`의 `object`로 명령 확인 → 그 명령을 보내는 코드·설정 확인 | 명령을 열거나 코드를 바꿈. 열기 전에 그 명령이 다른 위험을 여는지(`ACL CAT`) 확인 |
| `NOPERM No permissions to access a key` | 키 접두사가 패턴과 다름(잠금 키 `product~lock` 등) | `ACL LOG`의 `reason=key`, `object`로 키 확인 | 패턴을 더하거나 키 이름을 맞춤. 패턴을 넓히면 다른 앱 키까지 열릴 수 있음 |
| 헬스 DOWN인데 캐시·주문은 정상 | `INFO`(클러스터면 `CLUSTER INFO`) 거부 | 헬스 상세의 예외 메시지, `ACL LOG`의 `object=info`(클러스터면 `cluster\|info`) | `+info`(클러스터면 `+cluster\|info`). `INFO`는 서버 정보 노출이 대가 |
| 재고가 일부만 반영된 상태로 남음 | 스크립트 안 명령이 거부되어 중간에 멈춤 | 로그의 `ACL failure in script`, `ACL LOG`의 `context=lua` | 스크립트가 부르는 명령을 모두 열기. 이미 남은 상태는 보상 처리 필요 |
| 앱이 Redis에 연결하지 못함 | 비밀번호·사용자 이름 오류, 사용자 `off`, `SELECT` 거부 | 원인 예외의 `WRONGPASS`·`NOPERM` 메시지. 인증 실패는 `ACL LOG`의 `reason=auth`, `SELECT` 거부는 `reason=command`·`object=select` | 자격 증명 수정 또는 `+select`. 네트워크 장애와 혼동하지 않기 |
| 앱이 시작하지 못함(빈 초기화 실패) | 시작 때 보내는 `CONFIG GET` 등이 거부됨 | 시작 로그의 원인 예외 | 서버 설정을 운영자가 미리 하고, 앱이 그 명령을 보내지 않게 설정 |
| `acl_access_denied_cmd`가 새 연결마다 계속 늘어남(연결 하나에 `CLIENT SETINFO` 두 번이라 2씩) | 막아 둔 `CLIENT SETINFO` | `ACL LOG`의 `object=client\|setinfo` | `+client\|setinfo`를 열거나 무시. 무시하면 다른 거부를 찾기 어려움 |

## 말로 설명하기

### 30초 버전

Spring 앱의 Redis 계정은 앱 전용 사용자를 만들고, 앱이 실제로 보내는 명령과 키 접두사만 여는 허용 목록으로 정합니다. 목록에는 직접 쓴 코드뿐 아니라 Lettuce의 연결 명령, 캐시 비우기의 `KEYS`, 헬스 체크의 `INFO`가 들어가므로, 스테이징에서 운영과 같은 ACL로 돌리며 `ACL LOG`로 빠진 것을 찾습니다. 분류 단위로 막으면 `INFO`·`KEYS`까지 막히고, 분류로 열면 `FLUSHALL`이 다시 열리므로 위험한 명령은 명령 단위로 다룹니다. 키 패턴은 `FLUSHALL`과 `KEYS *`를 막지 못한다는 점도 함께 기억합니다.

### 2분 버전

Redis는 명령을 실행하기 전에 연결의 사용자 권한으로 명령, 키, 채널을 차례로 검사하고, 거부하면 아무것도 바꾸지 않고 `NOPERM`을 돌려줍니다. 최소 권한은 이 규칙을 "앱에 필요한 일만"으로 맞추는 것이고, 어려운 부분은 필요한 일의 목록입니다. 상품 서비스라면 Lettuce가 연결 때 보내는 `HELLO`와 `CLIENT SETINFO`, 캐시의 `GET`·`SET`·`DEL`과 비우기용 `KEYS`, 재고 스크립트의 `EVALSHA`·`EVAL`과 그 안의 `GET`·`DECR`, 헬스 체크의 `INFO`가 목록입니다. 설정에 따라 바뀝니다. 비우기를 `SCAN`으로 바꾸면 `SCAN`이, 잠금 작성기를 쓰면 `product~lock` 키가 필요합니다. 목록은 앱 쪽 Lettuce 명령 지표와 앱 하나만 붙인 스테이징의 `INFO commandstats`로 모으고, 그 규칙을 스테이징에 걸어 드문 경로까지 돌린 뒤 `ACL LOG`를 봅니다.

규칙은 `-@all`에서 시작해 명령을 하나씩 여는 방식이 앱 전용 계정에 맞습니다. 분류는 앱의 필요와 맞지 않습니다. `@dangerous`에는 `INFO`와 `KEYS`가 함께 있고, `@write`와 `@keyspace`에는 `FLUSHALL`이 있습니다. 목록을 모을 수 없다면 `+@all -@admin -flushall -flushdb -swapdb`가 차선이고, 빼는 규칙은 반드시 뒤에 둡니다. 키 패턴은 키를 인자로 받는 명령만 제한합니다. 그래서 `FLUSHALL`은 명령으로 막아야 하고, `KEYS *`는 다른 팀 키 이름까지 보여 줍니다. Lua 스크립트는 실행 전에 선언한 키를, 실행 중에 안의 명령을 하나씩 검사하므로 중간에 막히면 앞의 쓰기가 남습니다. 거부는 일시적 오류가 아니니 재시도하지 않습니다. 스크립트 안의 거부는 `ERR ACL failure in script`로, `SELECT` 거부는 연결 실패로 보인다는 점도 놓치지 않습니다.

### 예상 질문과 꼬리 질문

**Q. 앱 계정에 `+@all -@dangerous`를 주면 안 되나요? 위험한 건 다 막히잖아요.**
헬스 체크의 `INFO`와 캐시 비우기의 `KEYS`가 `@dangerous`에 들어 있어서, 데이터 명령은 멀쩡한데 헬스가 DOWN이 되고 캐시 비우기가 실패합니다. 분류는 명령의 성격으로 묶은 것이라 앱에 필요한 명령과 위험한 명령이 섞여 있습니다. 위험한 명령은 명령 단위로 빼고, `@admin`처럼 앱이 정말 쓰지 않는 분류만 분류째 뺍니다. (본문: "분류는 서버가 정한 묶음이고, 앱의 필요와 맞지 않습니다")

- **꼬리: 그럼 `+@read +@write`로 여는 건요? 관리 명령은 안 들어가잖아요.**
  `@write`에 `FLUSHALL`, `FLUSHDB`, `SWAPDB`가 들어 있습니다. 키 공간 전체를 쓰는 명령이라 쓰기 분류에 속합니다. 분류로 열었다면 맨 뒤에 `-flushall -flushdb -swapdb`를 붙여야 하고, 그래도 `RESTORE`·`MIGRATE`처럼 앱이 쓰지 않는 쓰기 명령이 남습니다.
- **꼬리: `+@all`은 앞으로 생길 명령도 포함하나요?**
  공식 문서는 `+@all`이 모듈로 나중에 올라오는 명령까지 포함한다고 설명합니다. 그래서 `+@all`에서 빼는 방식은 "모르는 명령은 허용"이 되고, `-@all`에서 여는 방식은 "모르는 명령은 거부"가 됩니다. 앱 계정에는 뒤쪽이 맞습니다.

**Q. 키 패턴을 `~product::*`로 좁혔으니 `FLUSHALL`은 상품 키만 지우지 않나요?**
아닙니다. 키 패턴은 명령 인자 가운데 키로 정의된 인자만 검사합니다. `FLUSHALL`은 키 인자가 없으므로 키 패턴의 영향을 받지 않고 DB 전체를 지웁니다. 명령 규칙으로만 막을 수 있습니다. (본문: "못 막는 것: 키를 인자로 받지 않는 명령")

- **꼬리: `KEYS`는요? 패턴을 넘기니까 걸러지나요?**
  `KEYS product::*`의 인자는 키가 아니라 검색 패턴이라 검사하지 않습니다. 그래서 `KEYS *`를 보내면 다른 팀 키의 이름까지 모두 받습니다. 값은 못 읽지만 키 이름에 개인 정보가 들어 있다면 유출입니다. `SCAN`도 같습니다.
- **꼬리: 그러면 캐시 비우기용 `KEYS`를 열지 말아야 하나요?**
  키 이름이 민감하지 않으면 여는 것이 현실적입니다. 다만 `KEYS`는 서버를 막으므로 `SCAN` 방식으로 바꾸고 `+scan`을 여는 편이 낫습니다. 키 이름이 민감하다면 그 데이터를 다른 Redis로 나누는 것이 확실한 대응입니다.

**Q. 앱이 쓰는 명령은 코드를 검색하면 다 나오지 않나요?**
직접 쓴 코드 밖에서 나가는 명령이 많습니다. Lettuce는 연결마다 `HELLO`와 `CLIENT SETINFO`를, 0이 아닌 DB 번호를 정했으면 `SELECT`를 보냅니다. Spring Data Redis 캐시는 비우기에 `KEYS`를, 잠금을 쓰면 `product~lock` 키를, TTL 갱신을 켜면 `GETEX`를 씁니다. Actuator는 `INFO`를 보냅니다. 그래서 스테이징에서 앱 쪽 명령 지표와 서버의 `INFO commandstats`로 관찰합니다. (본문: "앱이 실제로 보내는 명령은 직접 쓴 코드보다 많습니다")

- **꼬리: `INFO commandstats`를 운영 Redis에서 보면 안 되나요?**
  서버 전체 숫자라 다른 앱의 명령이 섞여 누구의 명령인지 모릅니다. 앱 하나만 붙인 스테이징에서 `CONFIG RESETSTAT` 뒤에 봐야 의미가 있습니다. 앱 쪽 `lettuce.command.completion`은 그 앱 것만 세지만, 명령 이름 단위라 하위 명령을 구분하지 못합니다.
- **꼬리: 관찰로 다 잡을 수 있나요?**
  테스트가 지나간 경로만 보입니다. 캐시 비우기 같은 관리 기능, 앱 시작 때의 초기화, 재연결, 스크립트 캐시가 비워진 뒤의 `EVAL` 재전송은 일부러 지나가게 해야 합니다. 놓친 경로는 배포 뒤 `NOPERM`(스크립트 안이면 `ERR ACL failure in script`)으로 드러나므로, 적용 직후 거부 지표를 지켜봅니다.

**Q. Lua 스크립트를 쓰는 계정에는 `+eval`만 열면 되나요?**
안 됩니다. 서버는 `EVAL` 자체와 선언한 키를 실행 전에 검사하고, 스크립트 안의 `redis.call`도 명령마다 같은 사용자 권한으로 다시 검사합니다. 예시라면 `+evalsha +eval`과 함께 안에서 쓰는 `+get +decr`이 필요합니다. Spring의 스크립트 실행은 먼저 `EVALSHA`를 보내고 `NOSCRIPT`면 `EVAL`로 다시 보내므로 둘 다 엽니다. (본문: "Lua: 실행 전에 선언한 키, 실행 중에 명령마다")

- **꼬리: 안의 명령이 막히면 어떻게 되나요?**
  스크립트가 그 자리에서 멈추고 앞의 쓰기는 남습니다. 앱은 `NOPERM`이 아니라 `ERR ACL failure in script`로 시작하는 오류를 받습니다. 그래서 로그에서 `NOPERM`만 찾으면 놓칩니다.
- **꼬리: `ACL DRYRUN`으로 미리 확인하면 되지 않나요?**
  `ACL DRYRUN`은 명령 하나의 바깥만 판정합니다. `EVAL`이면 `EVAL`과 선언한 키만 보고 스크립트를 실행하지 않으므로, 안의 명령은 확인되지 않습니다. 스크립트는 스테이징에서 실제로 돌려 `ACL LOG`의 `context=lua` 항목을 봅니다.
- **꼬리: 읽기만 하는 스크립트라면 읽기 권한(`%R~`)만 주면 되나요?**
  `EVAL`·`EVALSHA`는 선언한 키에 읽기와 쓰기를 모두 요구합니다. 실행 전에는 스크립트가 무엇을 할지 모르기 때문입니다. 읽기 전용 권한으로 돌리려면 `EVAL_RO`를 씁니다.

**Q. 권한이 모자라면 앱에서는 어떻게 보이나요?**
일반 명령은 `NOPERM`을 받고, Lettuce의 `RedisCommandExecutionException`을 Spring Data Redis가 `RedisSystemException`으로 감쌉니다. 캐시라면 캐시 에러 처리기로 가서 기본으로는 API가 실패합니다. 헬스 체크의 `INFO`가 막히면 헬스가 DOWN이 됩니다. 비밀번호가 틀리거나 연결 직후 `SELECT`가 막히면 연결 자체가 실패해 `RedisConnectionFailureException`이 됩니다. (본문: "권한이 모자라면 앱에는 이렇게 드러납니다")

- **꼬리: 연결 실패로 보이면 네트워크 문제와 어떻게 구분하나요?**
  원인 예외의 메시지에 `WRONGPASS`나 `NOPERM`이 있는지 봅니다. 서버 쪽에서는 인증 실패면 `ACL LOG`의 `reason=auth`와 `INFO stats`의 `acl_access_denied_auth`, `SELECT` 거부면 `reason=command`·`object=select`와 `acl_access_denied_cmd`로 확인합니다.
- **꼬리: 재시도 정책은 어떻게 하나요?**
  권한 거부는 같은 명령에 늘 같은 결과이므로 재시도 대상에서 뺍니다. Spring Data Redis는 이 오류를 별도 예외 종류 없이 `RedisSystemException`으로 감싸므로, 원인 예외의 메시지(`NOPERM`, `ACL failure in script`)로 구분해야 합니다.

**Q. 키 거부가 났는데 어느 키인지 앱 로그에 없습니다.**
일반 응답은 "No permissions to access a key"로 키 이름을 말하지 않습니다. 서버의 `ACL LOG`에 `reason=key`와 함께 `object`로 키가 남으므로 거기서 봅니다. 흔한 원인은 키 접두사가 패턴과 다른 경우입니다. 예를 들어 잠금 캐시 작성기의 잠금 키 `product~lock`은 `~product::*`에 맞지 않습니다. (본문: "`ACL LOG`")

- **꼬리: `ACL LOG`는 얼마나 남나요?**
  기본 128개 항목까지 두고 오래된 것부터 버립니다. 같은 거부가 짧은 간격으로 반복되면 보통 새 항목 대신 `count`가 늘어나지만, 늘 묶이지는 않습니다. 그래서 오래된 원인을 찾을 때는 남아 있지 않을 수 있고, 누적 추세는 `acl_access_denied_*` 지표로 봅니다.

**Q. 운영에서 권한을 좁히면 앱을 재시작해야 하나요?**
이미 붙은 연결에도 곧바로 적용된다고 봅니다(소스에서 추론, 재현 안 함). 재시작은 필요 없지만, 잘못 좁히면 바로 깨집니다. 규칙 하나라도 틀리면 아무것도 바뀌지 않으므로 반쯤 적용된 상태는 생기지 않습니다. 적용 직후 거부 지표를 보고, 문제가 생기면 규칙 한 줄로 되돌립니다. (본문: "운영에 적용할 때")

- **꼬리: 기존 사용자에게 좁은 규칙을 보냈는데 여전히 다 됩니다.**
  `ACL SETUSER`는 기존 규칙 위에 덧붙입니다. 이전의 `~*`나 `+@all`이 남아 있으면 그대로 열려 있습니다. 규칙 앞에 `resetkeys resetchannels -@all`을 넣어 기본 규칙의 키 패턴·채널·명령 허용을 지우고, 괄호로 더한 셀렉터가 있었다면 `clearselectors`로 함께 지운 뒤 `ACL GETUSER`로 결과를 확인합니다.

**Q. 키 만료 이벤트를 받는 리스너를 쓰려는데 앱이 뜨지 않습니다.**
Spring Data Redis의 키 만료 리스너는 초기화 때 `CONFIG GET notify-keyspace-events`를 보내고, 비어 있으면 `CONFIG SET`을 보냅니다. `CONFIG`는 관리 명령이라 앱 계정에서 막혀 있으면 빈 초기화가 실패합니다. 운영자가 서버에서 설정을 켜 두고, 리스너의 설정 문자열을 비워 이 명령을 보내지 않게 합니다. (본문: "앱이 실제로 보내는 명령은 직접 쓴 코드보다 많습니다")

- **꼬리: 그다음엔 무엇이 더 필요한가요?**
  구독 권한입니다. 새 사용자는 채널이 모두 막혀 있고, 리스너는 `PSUBSCRIBE __keyevent@*__:expired`를 보냅니다. 패턴 구독은 ACL의 채널 패턴과 글자 그대로 같아야 하므로 `+psubscribe &__keyevent@*__:expired`를 똑같이 적습니다. 더 넓은 `&__keyevent@*`로는 통과하지 않습니다. (본문: "Pub/Sub 채널도 따로 엽니다")

**Q. 여러 앱이 한 계정을 같이 쓰고 있어서 명령 목록을 모을 수가 없습니다. 어떻게 하죠?**
당장은 `~* +@all -@admin -flushall -flushdb -swapdb`로 서버 설정·계정·종료와 사고 명령을 막는 것이 차선입니다. 다만 이 수준은 모든 데이터 명령과 모든 키가 열려 있어 다른 앱의 데이터를 지킬 수 없습니다. 앱별 사용자로 나눠야 `ACL LOG`의 `username`으로 누가 무엇을 쓰는지 알 수 있고, 그래야 앱별 허용 목록으로 좁힐 수 있습니다. (본문: "세 가지 규칙 수준")
