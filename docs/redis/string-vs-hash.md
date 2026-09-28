# 객체 하나를 String에 담을까, Hash에 담을까

> **핵심 질문**: 세션처럼 필드 몇 개로 된 객체를 Redis에 둘 때, JSON으로 직렬화해 String 하나에 담는 것과 Hash의 필드로 나눠 담는 것은 무엇이 다른가? 어떤 접근 패턴에서 무엇을 고르고, 그 선택은 데이터가 커질 때 어떤 비용으로 돌아오는가?
>
> **범위**: 객체 하나를 키 하나에 담는 경우의 String과 Hash를 비교합니다. List, Set, Sorted Set, Stream을 고르는 기준과, 모듈로 제공되는 JSON 자료형은 다루지 않습니다. 내부 표현은 판단에 필요한 만큼만 씁니다.

## 배경 지식

### 이 문서의 예시: 로그인 세션 `session:42`

문서 끝까지 다음 장면 하나를 씁니다.

- 로그인한 사용자의 세션을 키 `session:42`에 담습니다. 세션에는 필드 네 개가 있습니다.

| 필드 | 값 | 바뀌는 때 |
| --- | --- | --- |
| `userId` | `1001` | 바뀌지 않음 |
| `nickname` | `neo` | 사용자가 닉네임을 바꿀 때 |
| `cartCount` | `3` | 장바구니에 상품을 담을 때 |
| `lastAccess` | `1761800000000` | 요청마다(밀리초 단위 시각) |

- 세션은 30분 동안 요청이 없으면 사라져야 하므로 키에 TTL(유효 기간)을 겁니다.
- 담는 방법은 두 가지입니다.

```text
방법 ① String: 객체를 JSON 한 줄로 직렬화해 값 하나로 담기

  session:42 ──▶ {"userId":1001,"nickname":"neo","cartCount":3,"lastAccess":1761800000000}
                 (73바이트 문자열 하나. Redis는 안쪽이 JSON인지 모름)

방법 ② Hash: 필드마다 이름과 값을 나눠 담기

  session:42 ──▶ userId     → 1001
                 nickname   → neo
                 cartCount  → 3
                 lastAccess → 1761800000000
```

화살표는 "이 키의 값이 이것이다"라는 대응입니다. 데이터가 이동하는 방향이 아닙니다. 그림은 논리적인 모습이며, Redis 안에서 실제로 메모리에 어떻게 놓이는지는 아래 "Redis 안에서는 String은 한 덩어리, Hash는 크기에 따라 두 모습입니다"에서 다룹니다. 키의 TTL은 두 그림 모두에서 뺐습니다. TTL은 값이 아니라 키에 붙는 정보입니다.

### 세 층을 구분합니다: 자료형, 내부 표현, 직렬화

이 문서에서 가장 헷갈리기 쉬운 지점이라 처음에 나눠 둡니다. "Hash로 저장한다"는 말은 세 층 중 하나만 말합니다.

| 층 | 누가 정하는가 | 무엇을 정하는가 | 예시에서 |
| --- | --- | --- | --- |
| **논리 자료형** | 개발자가 명령으로 고름 | 어떤 명령으로 다룰 수 있는가 | `SET`/`GET`을 쓰면 String, `HSET`/`HGET`을 쓰면 Hash |
| **내부 표현(인코딩)** | Redis가 크기를 보고 고름 | Redis 메모리 안에 어떻게 놓이는가 | Hash가 작으면 `listpack`, 커지면 `hashtable` |
| **직렬화 형식** | 애플리케이션(Spring 설정)이 고름 | 값 칸에 들어가는 바이트가 무엇인가 | JSON 문자열, Java 직렬화 바이트, 숫자를 적은 문자열 |

- 논리 자료형은 명령이 정합니다. String 키에 `HGET`을 보내면 Redis는 `WRONGTYPE` 에러로 거부합니다.
- 내부 표현은 개발자가 명령으로 고르지 않습니다. 같은 Hash라도 필드 수와 값 길이에 따라 Redis가 바꿉니다. `OBJECT ENCODING session:42`로 지금 어떤 표현인지 볼 수 있습니다.
- 직렬화 형식은 Redis가 모릅니다. Redis에게 String 값과 Hash의 필드 값은 그냥 바이트열입니다. 그런데 이 바이트열의 **길이**가 내부 표현을 바꿀 수 있습니다. 이 연결이 아래 "Spring Session은 Hash를 쓰지만, 기본 직렬화 때문에 hashtable이 됩니다"의 핵심입니다.

### 직렬화

- **한 줄**: 직렬화는 메모리 안의 객체를 네트워크로 보내거나 저장할 수 있는 바이트열로 바꾸는 일이고, 역직렬화는 그 반대입니다.
- **비유**: 조립된 가구를 분해해 납작한 상자에 넣는 일입니다. 상자째 옮기기는 쉽지만, 의자 다리 하나만 바꾸려면 상자를 뜯어 조립하고, 고친 뒤 다시 포장해야 합니다.
- **실제로는**:
  - JSON은 객체를 사람이 읽을 수 있는 문자열로 적는 형식입니다. 예시의 세션은 73바이트 문자열이 됩니다.
  - Java 직렬화(`ObjectOutputStream`)는 값과 함께 클래스 이름, 버전 같은 클래스 설명을 바이트에 적습니다. 그래서 값이 작아도 결과가 깁니다. JDK 25에서 `Long` 값 하나를 직렬화해 보면 82바이트였습니다(이 문서를 쓰며 잰 값, 근거 절 참고).
  - 비유와 다른 점은, 상자 안을 일부만 고치는 방법이 아예 없지는 않다는 것입니다. 바이트 위치를 알면 일부만 바꿀 수 있지만, JSON처럼 길이가 제각각인 형식에서는 위치를 알려면 결국 앞에서부터 해석해야 합니다.
- **이 주제에서는**: 방법 ①은 세션 전체가 상자 하나입니다. `cartCount` 하나를 바꾸려면 애플리케이션이 상자 전체를 받아 풀고, 고치고, 다시 싸서 보내야 합니다. 방법 ②는 필드마다 작은 상자가 따로 있어, 그 필드의 상자만 바꿔 보냅니다.

### 해시 테이블

- **한 줄**: 해시 테이블은 키를 숫자로 바꾸는 함수(해시 함수)로 저장 위치를 계산해, 원소가 많아도 원하는 키를 거의 한 번에 찾는 자료구조입니다.
- **비유**: 이름의 첫 글자로 칸을 나눈 서류함입니다. "neo"를 찾을 때 서류함 전체를 뒤지지 않고 "n" 칸만 엽니다.
- **실제로는**:
  - 칸(버킷)을 담은 배열이 하나 있고, 각 원소는 따로 할당한 조각에 담겨 해당 칸에 매달립니다. 서로 다른 키가 같은 칸으로 계산되면(충돌) 그 칸에 여러 원소가 이어 매달리고, 찾을 때 그 몇 개를 비교합니다.
  - 그래서 찾기는 평균적으로 원소 수와 상관없이 일정한 시간이 들지만, 원소마다 조각과 그 조각을 가리키는 주소가 따로 붙습니다. 원소가 늘면 배열을 키우고 원소들을 새 칸으로 옮기는 일도 생깁니다.
  - Kotlin의 `HashMap`과 같은 구조입니다. `HashMap`도 키의 `hashCode()`로 칸을 정하고, 원소마다 노드 객체를 따로 만듭니다. 다른 점은, JVM에서는 원소마다 붙는 비용이 객체 머리말과 GC가 따라갈 객체 수로 나타나고, Redis에서는 C 할당기의 조각과 해제 호출로 나타난다는 것입니다.
- **이 주제에서는**: Hash가 커지면 Redis는 필드들을 해시 테이블에 담습니다(`hashtable`). 필드를 찾는 시간은 필드 수와 거의 상관없어지지만, 필드마다 따로 할당한 조각이 생깁니다.

### 메모리 할당: 한 덩어리와 원소마다 따로

- **한 줄**: 할당은 프로그램이 할당기(메모리를 나눠 주는 라이브러리)에게 "이만큼 쓸 메모리를 달라"고 해서 연속된 조각 하나를 받는 일이고, 해제는 그 조각을 돌려주는 일입니다. 조각을 다시 찾아가려면 그 조각을 가리키는 포인터를 어딘가에 저장해야 합니다. 또 할당기는 요청을 미리 정해 둔 크기 단계 중 하나로 올려 내주므로, 요청 크기가 단계와 딱 맞지 않으면 조각 안에 남는 칸이 생깁니다. 그래서 데이터를 조각 하나에 이어 담으면 이 비용이 한 번만 들고, 원소마다 조각을 따로 받으면 원소 수만큼 포인터와 할당·해제 호출이 붙고 남는 칸도 조각마다 생길 수 있습니다. 대신 한 덩어리는 안쪽을 바꾸거나 늘릴 때 옮기고 복사해야 합니다.
- **이 주제에서는**: String 값과 작은 Hash(`listpack`)는 한 덩어리이고, 큰 Hash(`hashtable`)는 필드마다 따로입니다. 그래서 같은 세션 데이터라도 내부 표현에 따라 메모리 사용량과 지우는 비용이 달라집니다. 원리와 계산은 [메모리 할당](../cs/memory-allocation.md)에 있습니다.

### 경쟁 상태와 원자성

- **한 줄**: 여러 실행 흐름이 같은 데이터를 "읽고 → 판단하고 → 쓰는" 사이에 끼어들면, 먼저 읽은 값이 쓸 때는 이미 낡아 결과가 틀어집니다(경쟁 상태). 원자성은 그 중간에 끼어들 수 없게 만드는 성질입니다.
- **이 주제에서는**: Redis는 명령 하나를 다른 명령과 섞지 않고 끝까지 실행합니다. 그래서 `HSET` 하나로 끝나는 변경은 끼어들 틈이 없고, `GET`과 `SET` 두 명령에 걸친 변경에는 틈이 생깁니다. 원리는 [경쟁 상태와 원자성](../cs/race-condition-and-atomicity.md), Redis에서의 대책은 [Redis 원자성](./atomicity.md)에 있습니다.

### 복잡도 표기 O(1)과 O(N)

- **한 줄**: O(1)은 "일의 양이 데이터 크기와 상관없이 일정하다", O(N)은 "N에 비례해 늘어난다"는 표기입니다. N이 무엇을 세는지는 명령마다 다르며 공식 문서가 적어 둡니다.
- **이 주제에서는**: `GET`, `SET`, `HGET`은 공식 복잡도가 O(1)이고, `HGETALL`은 "N is the size of the hash"인 O(N)입니다. 단, 공식 복잡도는 명령의 대표 경로를 말할 뿐 내부 표현마다의 실제 경로를 다 말하지 않습니다. 작은 Hash에서 `HGET`이 실제로 하는 일은 아래 "Redis 안에서는 String은 한 덩어리, Hash는 크기에 따라 두 모습입니다"에서 바로잡습니다.

## 필드 하나를 바꾸는 비용이 가장 크게 갈립니다

두 방법의 차이는 읽을 때보다 **일부를 바꿀 때** 가장 크게 드러납니다. 사용자가 장바구니에 상품을 하나 담아 `cartCount`를 3에서 4로 바꾼다고 합시다.

| 단계 | 방법 ① String | 방법 ② Hash |
| --- | --- | --- |
| 보내는 명령 | `GET session:42` → 애플리케이션에서 역직렬화, `cartCount` 수정, 다시 직렬화 → `SET session:42 {...}` | `HINCRBY session:42 cartCount 1` (또는 `HSET session:42 cartCount 4`) |
| 왕복 | 2번 | 1번 |
| 오가는 값 데이터(키·명령 이름·프로토콜 정보 제외) | 73바이트 전체를 받고 73바이트 전체를 보냄 | 필드 이름과 새 값(`HINCRBY`는 증분 `1`)만 |
| Redis 안에서 하는 일 | 값 전체를 새 값으로 교체 | 그 필드 하나만 교체 |
| 다른 요청이 끼어들 틈 | `GET`과 `SET` 사이 | 없음(명령 하나) |

`HINCRBY`는 Redis가 필드 값을 정수로 읽을 수 있을 때만 동작합니다. Redis는 필드 값을 바이트열로만 저장하므로, `"3"`처럼 숫자를 글자로 적은 값이어야 합니다. Kotlin에서 `Long`이었다는 사실은 상관없습니다. 그 값을 Java 직렬화 같은 형식으로 저장했다면 `HINCRBY`는 "hash value is not an integer" 에러를 돌려줍니다.

### 두 요청이 서로 다른 필드를 바꾸면, String은 한쪽을 잃습니다

같은 사용자가 거의 동시에 두 요청을 보냈다고 합시다. 요청 X는 장바구니에 상품을 담고(`cartCount` 3 → 4), 요청 Y는 닉네임을 바꿉니다(`nickname` neo → trinity). 두 요청은 서로 다른 필드를 바꾸므로 둘 다 반영되어야 합니다.

```text
시간 →

요청 X │GET: cartCount=3, nickname=neo│ cartCount=4로 고침 │SET {cartCount:4, nickname:neo}│
요청 Y │GET: cartCount=3, nickname=neo│ nickname=trinity로 고침 │                 │SET {cartCount:3, nickname:trinity}│

결과   session:42 = {cartCount:3, nickname:trinity}   ← X가 담은 장바구니 수가 사라짐
```

화살표 방향은 시간입니다. 두 요청 모두 자기가 읽은 **옛 세션 전체**를 고쳐서 덮어씁니다. 나중에 쓴 Y가 X의 변경을 모르는 채로 `cartCount=3`을 다시 써 넣습니다. 이를 갱신 손실(lost update)이라고 합니다. 각 명령은 원자적이지만, `GET`과 `SET`이라는 원자적 명령 두 개를 이어 붙였기 때문에 그 사이에 틈이 생긴 것입니다.

Hash라면 X는 `HINCRBY session:42 cartCount 1`, Y는 `HSET session:42 nickname trinity`를 보냅니다. 각자 자기 필드만 바꾸므로 순서가 어떻든 둘 다 남습니다. 두 요청이 **같은 필드**를 바꾸면 Hash에서도 나중에 쓴 쪽이 이깁니다. 다만 숫자를 더하는 변경이라면 `HINCRBY`가 읽기와 더하기와 쓰기를 명령 하나로 하므로 잃지 않습니다.

String을 유지하면서 갱신 손실을 막는 방법도 있습니다. 둘 다 [Redis 원자성](./atomicity.md)에서 다룹니다.

- **`WATCH`**: `GET` 전에 `WATCH`로 키를 감시합니다. 그다음 `MULTI`로 쓰기 명령(`SET`)을 바로 실행하지 않고 모아 두고, `EXEC`으로 모은 명령을 실행합니다. `EXEC`은 감시를 시작한 뒤 그 키를 누가 바꿨으면 모은 명령을 하나도 실행하지 않고 실패를 돌려주므로, 애플리케이션은 처음부터 다시 합니다. `WATCH`만 걸고 `MULTI`/`EXEC` 없이 `SET`을 보내면 보호되지 않습니다. 대가는 재시도 코드이고, 같은 세션에 요청이 몰릴수록 재시도가 늘어납니다.
- **Lua 스크립트**: 서버 안에서 JSON을 해석해 고친 뒤 다시 씁니다. Redis의 Lua 환경에는 JSON을 다루는 `cjson` 라이브러리가 들어 있습니다. 대가는 스크립트가 도는 동안 다른 모든 명령이 기다린다는 점이고, 값이 클수록 해석 시간도 깁니다.

어느 쪽이든 "필드 하나를 바꾼다"는 일에 비해 장치가 큽니다. 필드 단위 변경이 잦다면 그 자체가 Hash를 고를 이유입니다.

### String을 덮어쓰면 TTL이 사라집니다

일부만 바꾸는 경로에는 TTL 함정도 하나 있습니다. `SET`은 값을 바꾸면서 키의 기존 TTL을 지웁니다. `KEEPTTL` 옵션으로 기존 TTL을 그대로 두거나, `EX`·`PX`·`EXAT`·`PXAT`로 새 만료를 함께 줄 때만 키에 만료가 남습니다. 예시에서 `GET` → 수정 → `SET session:42 {...}`를 옵션 없이 보내면, 30분 뒤 사라져야 할 세션이 **만료 없는 키**가 되어 남습니다.

Spring에서도 같습니다. `opsForValue().set(key, value)`는 옵션 없는 `SET`을 보냅니다. 만료를 남기려면 `set(key, value, timeout)`처럼 만료를 함께 주는 메서드를 써야 합니다. 이 메서드는 남아 있던 만료 시간을 이어 가는 `KEEPTTL`과 달리, 덮어쓰는 순간부터 `timeout`만큼의 새 기한을 겁니다.

Hash의 `HSET`과 `HINCRBY`는 이미 있는 키의 필드만 바꾸므로 키의 TTL을 건드리지 않습니다. 지우지도 않지만 늘리지도 않습니다. 세션처럼 "마지막 접근부터 30분"인 데이터라면 TTL을 **지키는 것**과 **늘리는 것**은 다른 일입니다. Hash는 접근할 때마다 `EXPIRE`를 따로 보내 늘려야 하고, String은 `SET`에 `EX`를 붙이면 덮어쓰기와 늘리기를 명령 하나로 합니다.

## 통째로 읽을 때는 두 방법이 비슷합니다

세션은 요청마다 대부분 통째로 읽습니다. 이 경우 두 방법 모두 명령 한 번에 객체 전체를 받습니다.

| 읽기 | 방법 ① String | 방법 ② Hash |
| --- | --- | --- |
| 전체 | `GET` 한 번, 73바이트를 받아 역직렬화 | `HGETALL` 한 번, 필드 이름과 값 쌍 네 개를 받아 객체로 조립 |
| 일부(`nickname`만) | 결국 `GET`으로 전체를 받아 역직렬화 | `HGET` 또는 `HMGET`으로 그 필드만 |

`HGETALL`은 공식 복잡도가 O(N)이지만, 예시의 N은 필드 수 4입니다. [Redis 이벤트 루프](./event-loop.md)에서 본 위험한 `HGETALL`은 필드가 수십만, 수백만 개인 Hash에 보내는 경우입니다. 문제는 명령 이름이 아니라 그 명령이 만나는 N입니다.

일부만 읽는 일이 잦으면 Hash가 받는 바이트를 줄입니다. 객체가 작다면 이 이점도 작습니다. 반대로 값이 수 KB인 객체에서 한두 필드만 자주 읽는다면 차이가 커집니다.

## Redis 안에서는 String은 한 덩어리, Hash는 크기에 따라 두 모습입니다

논리 자료형이 정해지면, 실제 메모리 배치는 Redis가 정합니다.

### String: 값 바이트가 한 덩어리입니다

예시의 73바이트 JSON을 `SET`하면 `OBJECT ENCODING`은 `raw`를 돌려줍니다. 짧은 정수는 `int`, 짧은 문자열은 `embstr`로 표시되지만, 어느 쪽이든 값은 한 덩어리로 담기고 Redis는 그 안쪽 구조를 모릅니다. 그래서 String에서는 내부 표현이 선택을 바꾸는 일이 거의 없습니다. 지울 때도 해제할 조각이 한두 개뿐이라, 원소 수만큼 해제를 반복하는 일이 없습니다.

### Hash: 작을 때는 `listpack`, 조건을 넘으면 `hashtable`

Hash는 두 표현 중 하나입니다.

```text
listpack (한 덩어리, 이어 적기)

  [ userId | 1001 | nickname | neo | cartCount | 3 | lastAccess | 1761800000000 ]
     필드      값     필드     값      필드     값      필드          값

hashtable (필드마다 따로)

  칸 배열 [ ○ | ○ | ○ | ○ ]
            │   │   │   │
            ▼   ▼   ▼   ▼
         항목  항목  항목  항목     ← 항목마다: 필드 이름 조각, 값 조각, 둘을 잇는 항목 조각
```

`listpack` 그림의 칸은 순서대로 이어 적힌 원소이고, 각 원소 앞에 붙는 길이 정보는 뺐습니다. `hashtable` 그림의 화살표는 "그 칸이 이 항목을 가리킨다"는 주소 참조이며, 충돌로 한 칸에 여러 항목이 이어 매달리는 경우와 칸 배열의 크기는 뺐습니다. 도형 폭은 실제 크기에 비례하지 않습니다.

8.2.10에서 Hash는 처음에 `listpack`으로 만들어지고, 필드를 쓰는 명령이 다음 기준을 넘기면 `hashtable`로 바뀝니다.

| 설정 | 기본값 | 무엇을 세는가 |
| --- | --- | --- |
| `hash-max-listpack-entries` | 512 | 필드 수(필드·값 쌍의 수) |
| `hash-max-listpack-value` | 64 | 필드 이름 하나 또는 값 하나의 바이트 길이 |

필드가 512개를 넘거나, 64바이트를 넘는 필드 이름이나 값이 **하나라도** 들어오면 그 자리에서 `hashtable`로 바뀝니다. 반대 방향은 없어서, 지금 기준 안에 있다고 해서 지금 `listpack`이라는 보장은 없습니다(아래 "한 번 바뀌면 돌아오지 않습니다"). 필드 만료를 쓰면 `listpackex`라는 변형도 있습니다. 예시의 세션은 필드 4개, 가장 긴 값이 13바이트인 채로 처음부터 저장했으므로 `listpack`입니다.

두 표현의 대가는 다음과 같습니다.

| | `listpack` | `hashtable` |
| --- | --- | --- |
| 메모리 | 조각 하나. 필드마다 붙는 포인터와 조각 비용이 없음 | 필드마다 필드 이름, 값, 항목 조각이 따로 붙음 |
| 필드 찾기 | 앞에서부터 차례로 비교 | 해시로 칸을 계산해 거의 바로 찾음 |
| 필드 바꾸기 | 덩어리 안을 당기거나 늘림. 크기 단계를 넘으면 옮겨 복사 | 그 필드의 조각만 바꿈 |
| 키 지우기 | 해제 한 번 | 필드 수만큼 해제 |

여기서 공식 복잡도와 실제 경로가 갈립니다. `HGET`의 공식 복잡도는 O(1)이지만, `listpack`에서는 필드를 앞에서부터 차례로 비교해 찾습니다. 이 경로의 비용은 필드 수에 비례합니다. 다만 `listpack`은 필드 512개, 값 64바이트 기준 안에서만 쓰이므로 훑는 길이에 상한이 있고, 필드가 늘어도 이 비용이 끝없이 커지지는 않습니다. 공식 문서가 O(1)이라고 쓴 것은 그 상한 안의 비용을 상수로 보는 셈입니다. 상한 안에서 실제로 얼마나 걸리는지는 이 문서에서 재지 않았고, 상한을 크게 올리면 이 전제가 약해집니다.

### 한 번 바뀌면 돌아오지 않습니다

필드를 지워 다시 작아져도 이미 `hashtable`인 Hash는 `listpack`으로 돌아가지 않습니다. 8.2.10에는 있는 Hash를 `hashtable`에서 `listpack`으로 바꾸는 경로가 없습니다.

예외는 Hash를 저장된 바이트에서 **새로 만들 때**입니다. RDB는 메모리의 데이터를 한 시점에 통째로 파일로 적은 스냅샷인데, 서버를 재시작하며 이 파일을 읽으면 Redis는 키마다 객체를 처음부터 다시 만듭니다. 실행 중에 `DUMP`로 떠 둔 값을 `RESTORE`로 되살릴 때도 같은 형식에서 다시 만듭니다. 이때는 그 시점의 필드 수와 길이로 표현을 새로 정하므로 `listpack`이 될 수 있습니다.

그래서 "가끔 긴 값이 한 번 들어오는" Hash는 그 한 번 때문에 계속 `hashtable`로 남습니다. 예를 들어 세션에 긴 리다이렉트 URL을 잠깐 넣었다 지워도, 그 세션 키는 만료될 때까지 `hashtable`입니다.

## Spring Session은 Hash를 쓰지만, 기본 직렬화 때문에 hashtable이 됩니다

Spring Session Data Redis(3.5.7)의 `RedisSessionRepository`는 세션을 방법 ②로 저장합니다. 이 구현을 보면 앞의 판단들이 실제로 어떻게 쓰이는지 알 수 있습니다.

- **키와 필드**: 키는 `spring:session:sessions:<세션 ID>`이고, 필드는 `creationTime`, `lastAccessedTime`, `maxInactiveInterval`, 그리고 세션 속성마다 `sessionAttr:<속성 이름>`입니다.
- **읽기**: 세션을 찾을 때 `opsForHash().entries()`, 곧 `HGETALL`로 통째로 읽습니다.
- **쓰기**: 저장할 필드를 따로 모아 두었다가, 그 필드들만 `opsForHash().putAll()`로 보내고 키의 만료 시각을 다시 겁니다. `putAll()`은 여러 필드를 한 번에 쓰는 `HMSET`을 보냅니다.

어떤 필드를 모아 두는지는 저장 모드(`SaveMode`)가 정합니다. 기본값 `ON_SET_ATTRIBUTE`에서는 요청 처리 중 `setAttribute`로 넣은 속성과 접근 시각 같은 메타데이터만 모읍니다. 그래서 앞의 요청 X와 Y처럼 서로 다른 속성을 바꾸는 동시 요청이 서로의 변경을 덮어쓰지 않습니다. 이것이 String이 아니라 Hash를 쓴 실질적인 이유입니다. 같은 속성을 동시에 바꾸면 나중에 저장한 쪽이 이깁니다.

이 추적은 JPA의 변경 감지처럼 객체 안을 비교하지 않습니다. 세션에서 꺼낸 Kotlin 객체나 컬렉션의 안쪽만 고치고 `setAttribute`를 다시 부르지 않으면, 기본 모드에서는 그 변경이 저장되지 않습니다. 반대로 저장 모드를 `ALWAYS`(모든 속성을 매번 저장)나 `ON_GET_ATTRIBUTE`(읽은 속성도 저장)로 바꾸면, 다른 요청이 바꾼 속성의 옛 값을 함께 써서 String과 같은 갱신 손실이 다시 생길 수 있습니다.

문제는 직렬화 층에서 생깁니다. 별도 설정이 없으면 Spring Session은 필드 값을 `RedisTemplate`의 기본 직렬화기, 곧 Java 직렬화(`JdkSerializationRedisSerializer`)로 저장합니다. 필드 이름은 문자열 그대로 저장합니다. 앞의 배경 지식에서 본 것처럼 Java 직렬화는 `Long` 하나도 82바이트로 만듭니다. `lastAccessedTime`의 값만으로 `hash-max-listpack-value` 64를 넘으므로, **기본 설정의 Spring Session 세션 키는 처음 저장할 때부터 `hashtable`입니다.**

세션 하나만 보면 필드가 몇 개뿐이라, 지울 때 해제할 조각도 그만큼만 늘어납니다. 다만 세션이 수백만 개라면 세션마다 필드 수만큼 조각 비용을 내므로 메모리 추정이 어긋납니다. 대응은 두 가지가 있고, 둘 다 대가가 있습니다.

- **값을 짧게 만드는 직렬화기를 고릅니다.** `springSessionDefaultRedisSerializer`라는 이름의 빈을 두면 Spring Session이 그 직렬화기를 씁니다. 다만 JSON 직렬화기도 타입 정보를 함께 적으면 값이 길어질 수 있고, 세션 속성에 큰 객체를 넣으면 어차피 64바이트를 넘습니다. 바꾼 뒤 `OBJECT ENCODING`으로 실제 표현을 확인해야 합니다. 이미 저장된 세션과 형식이 달라지므로 배포 중 옛 형식을 읽는 문제도 함께 풀어야 합니다.
- **`hash-max-listpack-value`를 올립니다.** 서버 설정이라 그 서버의 **모든 Hash**에 적용됩니다. `listpack`은 필드를 앞에서부터 훑으므로, 값이 긴 Hash에서는 필드 하나를 찾거나 바꾸는 비용이 커집니다. 근거 없이 올리지 말고 실제 데이터로 메모리와 지연을 함께 재고 정합니다.

## 데이터가 늘면 무엇이 달라지는가

- **필드가 몇 개 늘어나는 것은 문제가 아닙니다.** 세션 속성이 4개에서 20개가 되어도 `listpack` 조건 안이면 한 덩어리이고, `hashtable`이 되어도 키 하나의 비용은 작습니다.
- **필드 수에 상한이 없는 Hash는 다른 문제입니다.** "사용자 전체를 Hash 하나에", "상품별 조회 수를 Hash 하나에"처럼 필드가 계속 늘어나는 설계는 객체 하나를 담는 용도가 아닙니다. 필드가 512개를 넘으면 `hashtable`이 되고, 그 뒤로는 `HGETALL` 한 번이 필드 수만큼 일합니다. 기본 설정(`lazyfree-lazy-user-del no`)의 `DEL`도 필드 수만큼 해제하는 동안 다른 명령을 기다리게 합니다. `UNLINK`나 그 설정을 켠 `DEL`은 해제를 백그라운드 스레드로 넘길 뿐, 해제할 양 자체는 같습니다. 이 비용과 대책은 [Redis 이벤트 루프](./event-loop.md)에서 다룹니다.
- **String 값이 커지면 일부 변경의 비용이 값 크기에 비례합니다.** 필드 하나를 바꿀 때마다 전체를 받고 보내므로, 값이 수십 KB인 객체를 자주 고치면 네트워크와 직렬화 비용이 요청마다 붙습니다. 경쟁 상태의 틈도 역직렬화와 직렬화에 걸리는 시간만큼 길어집니다.
- **같은 객체를 담는 키가 많아지면 내부 표현의 차이가 곱해집니다.** 키 하나에서는 작던 `listpack`과 `hashtable`의 차이가 키 수백만 개에서는 메모리 사용량의 차이로 드러납니다. 이 차이는 키 수 × 필드 수에 비례합니다.

## 어떤 조건에서 무엇을 고르는가

| 접근 패턴 | 고를 것 | 이유 | 대가 |
| --- | --- | --- | --- |
| 항상 통째로 읽고 통째로 바꿈. 예: 외부 API 응답 캐시 | String | 명령 하나로 끝나고, 값이 덩어리 하나라 지울 때 원소 수만큼 해제를 반복하지 않음 | 일부만 바꿀 일이 생기면 위의 비용을 모두 냄 |
| 필드 단위로 자주 바꿈, 특히 여러 요청이 서로 다른 필드를 바꿈. 예: 세션, 장바구니 요약 | Hash | 필드 단위 명령 하나로 바꿔, 서로 다른 필드를 바꾸는 요청끼리는 갱신 손실이 없음 | 기본 직렬화에 따라 `hashtable`이 되어 키당 메모리가 늘 수 있음 |
| 필드 중 숫자를 원자적으로 더함. 예: 조회 수, 남은 횟수 | Hash(`HINCRBY`) | 읽고 더하고 쓰기를 명령 하나로 함 | 그 필드는 숫자를 글자로 적은 값이어야 하므로, 직렬화기를 그 형식에 맞춰야 함 |
| 객체 안에 객체·배열이 중첩됨 | String(JSON) | Hash는 한 단계뿐이라 필드 값에 다시 직렬화한 덩어리를 넣어야 함 | 중첩된 부분의 일부 변경은 String과 같은 비용 |
| 필드마다 다른 시점에 만료되어야 함 | Hash와 필드 만료(`HEXPIRE` 등) | 키를 나누지 않고 필드 단위로 만료를 걸 수 있음 | 작은 Hash는 필드 만료를 처음 걸 때 필드마다 만료 칸을 더한 `listpackex`로 바뀌고, 만료 관련 명령 다수는 O(N) |
| 필드 수가 계속 늘어남 | 어느 쪽도 아님. 키를 나눔 | 키 하나가 커지면 그 키의 명령 하나가 모든 요청을 기다리게 함 | 전체를 다루는 연산이 여러 키에 걸침 |

통째로 바꾸는 객체를 Hash에 담으면 함정이 하나 더 있습니다. `HSET`과 `HMSET`(Spring의 `opsForHash().putAll()`)은 넘겨준 필드만 쓰고, 넘기지 않은 기존 필드는 지우지 않습니다. 예시의 세션에서 `nickname`을 뺀 객체를 `putAll()`로 다시 저장해도 `session:42`에는 `nickname → neo`가 그대로 남습니다. 없앤 필드는 `HDEL`로 따로 지워야 합니다. 키를 `DEL`로 지우고 새로 쓰면 남는 필드는 없지만, 키의 TTL도 함께 사라지므로 만료를 다시 걸어야 합니다. 통째 교체가 잦은 객체에서 String이 단순한 이유 중 하나입니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 바로잡은 곳 |
| --- | --- | --- |
| Hash로 저장하면 항상 메모리를 아낀다 | Hash도 작을 때(`listpack`)만 한 덩어리이고, 64바이트를 넘는 값이 하나라도 있으면 필드마다 조각이 붙는 `hashtable`입니다. 같은 객체를 담은 String과 Hash 중 어느 쪽이 작은지는 같은 데이터로 재 봐야 합니다 | Redis 안에서는 String은 한 덩어리, Hash는 크기에 따라 두 모습입니다 |
| `HGET`은 O(1)이니 언제나 바로 찾는다 | `listpack`에서는 앞에서부터 훑습니다. 훑는 길이에 상한이 있어 상수로 볼 뿐입니다 | 같은 절 |
| 필드를 지우면 다시 `listpack`이 된다 | 있는 Hash는 돌아오지 않습니다. RDB 적재나 `RESTORE`로 새로 만들 때만 다시 정해집니다 | 한 번 바뀌면 돌아오지 않습니다 |
| `SET`으로 값만 바꾸면 TTL은 그대로다 | 옵션 없는 `SET`은 TTL을 지웁니다 | String을 덮어쓰면 TTL이 사라집니다 |
| 명령마다 원자적이니 `GET` 후 `SET`도 안전하다 | 두 명령 사이에 다른 요청이 끼어들어 갱신 손실이 생깁니다 | 두 요청이 서로 다른 필드를 바꾸면, String은 한쪽을 잃습니다 |
| Spring Session은 세션 객체 안의 변경을 알아서 저장한다 | 기본 모드는 `setAttribute`로 넣은 속성만 저장합니다. 꺼낸 객체 안쪽만 고치면 저장되지 않습니다 | Spring Session은 Hash를 쓰지만, 기본 직렬화 때문에 hashtable이 됩니다 |
| Spring Session은 Hash를 쓰니 세션이 작게 저장된다 | 기본 Java 직렬화로 값이 64바이트를 넘어 처음부터 `hashtable`입니다 | Spring Session은 Hash를 쓰지만, 기본 직렬화 때문에 hashtable이 됩니다 |

## 근거

**소스**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c), [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): `hash-max-listpack-entries` 기본 512, `hash-max-listpack-value` 기본 64
- [`src/t_hash.c`](https://github.com/redis/redis/blob/8.2.10/src/t_hash.c):
  - `hashTypeTryConversion`: 명령에 들어온 필드·값 쌍의 수(같은 필드가 겹쳐도 그대로 셈)가 `hash-max-listpack-entries`를 넘거나, 인자 중 하나라도 `hash-max-listpack-value`보다 길면 `hashtable`로 변환
  - `hashTypeSet`: `listpack`에서 필드를 `lpFind`로 앞에서부터 찾아 바꾸고, 필드 수가 기준을 넘으면 변환. 길이 검사는 `HINCRBY` 경로도 덮음
  - `hashTypeGetFromListpack`: `listpack`에서 `lpFind`로 차례로 찾음
  - `hashTypeConvert`: 표현이 `hashtable`이면 `serverPanic("Not implemented")`, 곧 실행 중 `hashtable`에서 다른 표현으로 바꾸는 경로가 없음
  - `hashTypeConvertListpack`과 `mstrHashDictType`: `hashtable`은 필드마다 필드 이름(`hfield`)과 값(`sds`)을 따로 두고 각각 소멸자로 해제
  - `hsetCommand`: `HSET`과 `HMSET`이 같은 함수로 처리되며 변환 검사를 거침. 인자로 받은 필드·값 쌍만 `hashTypeSet`으로 쓰고 다른 필드는 건드리지 않음(넘기지 않은 기존 필드는 남음)
  - "listpackEx functions" 절 주석: `listpack` Hash에 필드 만료 명령을 처음 쓰면 `listpackex`로 바꾸고 필드마다 만료 칸을 더함
- [`src/dict.c`](https://github.com/redis/redis/blob/8.2.10/src/dict.c): `dictInsertKeyAtLink`에서 항목(`dictEntry`)마다 `zmalloc`
- [`src/rdb.c`](https://github.com/redis/redis/blob/8.2.10/src/rdb.c): `rdbLoadObject`의 Hash 적재. 필드 수가 기준 이하면 `listpack`으로 만들고, 긴 값을 만나면 그때 변환
- [`src/cluster.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster.c): `restoreCommand`도 `rdbLoadObject`로 객체를 새로 만듦
- [`src/object.c`](https://github.com/redis/redis/blob/8.2.10/src/object.c): `tryObjectEncodingEx`(정수로 읽히는 20바이트 이하 문자열은 `int` 후보, `OBJ_ENCODING_EMBSTR_SIZE_LIMIT` 44바이트 이하는 `embstr` 후보, 그 밖은 `raw`로 둠. 73바이트 JSON은 `raw`), `kvobjSet`(`embstr`은 키·만료 정보와 합친 크기가 `CACHE_LINE_SIZE` 이하일 때만 한 조각에 담음. [`src/config.h`](https://github.com/redis/redis/blob/8.2.10/src/config.h)에서 Apple ARM 빌드는 128, 그 밖은 64), `strEncoding`(`OBJECT ENCODING`이 돌려주는 이름: `raw`, `int`, `embstr`, `listpack`, `listpackex`, `hashtable` 등)
- [`src/lazyfree.c`](https://github.com/redis/redis/blob/8.2.10/src/lazyfree.c): `lazyfreeGetFreeEffort`. `hashtable` 표현의 Hash는 필드 수, 그 밖(`listpack` Hash, String)은 1
- `t_hash.c`의 `hincrbyCommand`: 필드 값을 정수로 읽지 못하면 "hash value is not an integer" 에러
- [`src/t_string.c`](https://github.com/redis/redis/blob/8.2.10/src/t_string.c)의 `setGenericCommand`(`KEEPTTL`이나 만료 옵션 `EX`·`PX`·`EXAT`·`PXAT`이 있을 때만 `SETKEY_KEEPTTL`), [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c)의 `setKeyByLink` 주석("The expire time of the key is reset … unless 'SETKEY_KEEPTTL'"): 옵션 없는 `SET`은 TTL을 지움
- `db.c`의 `dbGenericDelete`: 키를 지울 때 만료 정보(`db->expires`의 항목)도 함께 지움. `DEL` 뒤 새로 만든 키에는 TTL이 없음
- [`src/script_lua.c`](https://github.com/redis/redis/blob/8.2.10/src/script_lua.c): Lua 환경에 `cjson` 라이브러리를 적재
- [`src/commands/get.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/get.json), [`set.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/set.json), [`hget.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hget.json), [`hincrby.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hincrby.json): 공식 복잡도 O(1). [`hgetall.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hgetall.json): "O(N) where N is the size of the hash". [`hmget.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hmget.json): 요청한 필드 수 N에 O(N). [`hexpire.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hexpire.json): 지정한 필드 수 N에 O(N). [`hmset.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hmset.json): `hsetCommand`로 처리

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`. 특정 Redis 버전 전용 문서가 아니므로 소스와 다르면 소스를 따릅니다)

- [hashes](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/data-types/hashes.md): "Field expiration"(`HEXPIRE` 등 필드 단위 만료), "Performance"("Most Redis hash commands are O(1)", `HGETALL`과 만료 관련 명령 다수는 O(n))
- [memory-optimization](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/optimization/memory-optimization.md): "Special encoding of small aggregate data types"(작은 Hash의 특별한 인코딩과 `hash-max-listpack-*` 설정)

**Spring**

- Spring Session 태그 [`3.5.7`](https://github.com/spring-projects/spring-session/tree/3.5.7)(커밋 `bf21f5a75e430026a64f1e399b5bf63ea8cbd3e6`)
  - [`RedisSessionRepository.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/RedisSessionRepository.java): 키 `spring:session:sessions:<id>`(`DEFAULT_KEY_NAMESPACE`, `getSessionKey`), `findById`는 `opsForHash().entries()`, `saveDelta`는 모아 둔 필드(`delta`)만 `opsForHash().putAll()`로 쓰고 `expireAt`으로 만료 시각을 다시 걺, 기본 `saveMode`는 `SaveMode.ON_SET_ATTRIBUTE`, `RedisSession` 생성자(`ALWAYS`면 모든 속성을 `delta`에), `getAttribute`(`ON_GET_ATTRIBUTE`면 읽은 속성을 `delta`에), `setAttribute`(넣은 속성을 `delta`에)
  - [`RedisSessionMapper.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/RedisSessionMapper.java): 필드 이름 `creationTime`, `lastAccessedTime`, `maxInactiveInterval`, `sessionAttr:`
  - [`AbstractRedisHttpSessionConfiguration.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/config/annotation/web/http/AbstractRedisHttpSessionConfiguration.java): `createRedisTemplate`(키와 필드 이름은 문자열 직렬화, `springSessionDefaultRedisSerializer` 빈이 있으면 그것을 기본 직렬화기로)
- Spring Data Redis 태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13)
  - [`RedisTemplate.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisTemplate.java): 기본 직렬화기를 정하지 않으면 `JdkSerializationRedisSerializer`
  - [`DefaultHashOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/DefaultHashOperations.java): `putAll`은 `hMSet`, `entries`는 `hGetAll`, `increment`는 `hIncrBy`
  - [`LettuceHashCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceHashCommands.java): `hMSet`은 `HMSET`, `hGetAll`은 `HGETALL`, `hIncrBy`는 `HINCRBY`
  - [`DefaultValueOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/DefaultValueOperations.java): `set(key, value)`는 옵션 없는 `set`, `set(key, value, timeout, unit)`은 `Expiration.from(timeout, unit)`으로 새 만료를 붙인 `set`
  - [`GenericJackson2JsonRedisSerializer.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/serializer/GenericJackson2JsonRedisSerializer.java): `createDefaultTypeResolverBuilder`가 클래스 이름을 속성(기본 `@class`)으로 함께 적도록 설정. 타입 정보를 적는 JSON 직렬화기의 예

**실측**(이 문서를 쓰며 한 번 잰 값. 서버 동작이 아니라 직렬화 결과의 길이입니다)

- JDK 25.0.2(Temurin)에서 `ObjectOutputStream`으로 `Long` 값 하나를 쓰면 82바이트, `Integer`는 81바이트. Java 직렬화 형식이 값과 함께 클래스 설명을 적기 때문입니다.

## 실무 포인트

### 고르기 전에 답할 질문

| 중요도 | 질문 | 왜 |
| --- | --- | --- |
| 필수 | 이 객체를 일부만 바꾸는가, 통째로 바꾸는가? | 일부를 자주 바꾸면 String은 매번 전체 왕복과 갱신 손실 위험을 냅니다. 이 질문 하나로 대부분 정해집니다 |
| 필수 | 여러 요청이 같은 객체를 동시에 바꿀 수 있는가? | String이라면 `WATCH`나 Lua가 필요하고, Hash라면 필드가 다를 때는 필요 없습니다 |
| 필수 | 값을 덮어쓰는 코드가 TTL을 함께 거는가? | 옵션 없는 `SET`은 TTL을 지워 만료되지 않는 키를 남깁니다 |
| 필수 | 필드 수에 상한이 있는가? | 상한이 없으면 객체가 아니라 큰 키입니다. 자료형보다 키를 나누는 설계가 먼저입니다 |
| 알면 유리 | 필드 값이 직렬화 후 64바이트를 넘는가? | 넘으면 `hashtable`이 되어 키 수 × 필드 수만큼 메모리 비용이 붙습니다 |
| 알면 유리 | 객체가 중첩되어 있는가? | Hash는 한 단계뿐이라 중첩된 부분은 결국 String과 같은 비용을 냅니다 |
| 참고 | `hash-max-listpack-*` 설정 조정 | 서버의 모든 Hash에 적용되고, 올리면 필드 찾기·바꾸기가 길어집니다. 실측 없이 바꾸지 않습니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 논리 자료형(어떤 명령을 쓸지) | `OBJECT ENCODING <키>`: 실제 내부 표현 |
| 직렬화기(Spring의 `RedisSerializer`, `springSessionDefaultRedisSerializer`) | `MEMORY USAGE <키>`: 키 하나의 메모리 추정치 |
| 값을 쓸 때 TTL을 함께 거는지(`set(key, value, timeout)`, `KEEPTTL`) | `TTL <키>`: 덮어쓴 뒤 -1(만료 없음)이 되지 않았는지 |
| 키 하나에 담을 필드 수의 상한 | 필드 수가 시간에 따라 어떻게 자라는지(`HLEN`) |

### 증상과 확인 순서

**증상: "세션 키 수는 예상대로인데 Redis 메모리가 추정치보다 훨씬 크다."**

1. 표본 세션 키에 `OBJECT ENCODING`을 봅니다. `hashtable`이면 필드마다 조각 비용을 내고 있는 것입니다.
2. `HGETALL`로 필드 값을 보고, 어느 값이 64바이트를 넘는지 찾습니다. 값이 `\xac\xed`로 시작하면 Java 직렬화 형식입니다. 이것만으로는 기본값을 그대로 쓴 것인지, 누군가 Java 직렬화기를 직접 지정한 것인지 알 수 없으니, 애플리케이션의 `springSessionDefaultRedisSerializer` 설정을 함께 확인합니다.
3. `MEMORY USAGE`로 표본 키의 크기를 재어 키 수를 곱해 봅니다. 추정이 맞으면 직렬화기 교체를 검토합니다. 교체의 대가는 위 "Spring Session은 Hash를 쓰지만, 기본 직렬화 때문에 hashtable이 됩니다"에 있습니다.

**증상: "가끔 장바구니 수나 설정 값이 이전 값으로 돌아간다."**

1. 해당 객체를 String(JSON)으로 저장하고 `GET` → 수정 → `SET`으로 바꾸는 코드가 있는지 찾습니다.
2. 같은 객체를 동시에 바꾸는 요청이 있는지 봅니다. 같은 사용자의 탭 여러 개, 재시도, 비동기 작업이 흔한 원인입니다.
3. 필드 단위로 바꿔도 되면 Hash로 옮기고, String을 유지해야 하면 `WATCH`로 감싸거나 Lua로 서버에서 고칩니다.

**증상: "만료되어야 할 키가 사라지지 않고 쌓인다."**

1. 표본 키에 `TTL`을 봅니다. -1이면 만료가 없는 키입니다.
2. 그 키를 쓰는 코드에서 만료 없이 `SET`(Spring의 `set(key, value)`)하는 곳을 찾습니다. 처음에 TTL을 걸었더라도 이후 덮어쓰기가 지웁니다.

## 말로 설명하기

### 30초 버전

객체를 JSON으로 String 하나에 담으면 통째로 읽고 쓸 때 단순하지만, 필드 하나를 바꾸려면 전체를 받아 고쳐 다시 보내야 하고, 그 사이 다른 요청이 끼어들면 변경을 잃습니다. Hash는 필드 단위 명령 하나로 바꿀 수 있어, 서로 다른 필드를 바꾸는 요청끼리는 이 문제가 없습니다. 대신 Hash는 작을 때만 한 덩어리로 담기고, 값 하나라도 64바이트를 넘으면 필드마다 메모리 조각이 붙는 표현으로 바뀝니다. 그래서 일부 변경이 잦으면 Hash, 통째로만 다루면 String을 고르고, 직렬화 결과의 길이까지 확인합니다.

### 2분 버전

객체 하나를 Redis에 둘 때 먼저 볼 것은 접근 패턴입니다. 항상 통째로 읽고 쓰는 캐시라면 String이 단순합니다. 명령 하나로 끝나고, 값이 덩어리 하나라 지울 때 원소 수만큼 해제를 반복하지 않습니다. 반대로 세션처럼 요청마다 필드 몇 개만 바뀌면, String은 `GET`으로 전체를 받아 고쳐 `SET`으로 다시 보내야 합니다. 왕복이 두 번이고, 두 명령 사이에 다른 요청이 같은 세션을 덮어쓰면 한쪽 변경이 사라집니다. 옵션 없는 `SET`이 TTL을 지운다는 함정도 있습니다. Hash는 `HSET`이나 `HINCRBY` 하나로 그 필드만 바꾸므로, 서로 다른 필드를 바꾸는 요청끼리 덮어쓰지 않습니다.

여기서 논리 자료형과 내부 표현을 구분해야 합니다. Redis는 Hash를 처음에 `listpack`이라는 한 덩어리로 담고, 필드가 512개를 넘거나 64바이트를 넘는 필드 이름·값이 들어오면 필드마다 조각을 따로 받는 `hashtable`로 바꿉니다. 한 번 바뀌면 그 Hash는 돌아오지 않습니다. 직렬화 형식도 여기에 영향을 줍니다. Spring Session은 기본 저장 모드에서 `setAttribute`로 바꾼 필드만 Hash에 쓰는 좋은 구조이지만, 기본 Java 직렬화로는 `Long` 하나도 64바이트를 넘어 세션마다 처음부터 `hashtable`이 됩니다. 세션이 수백만 개면 메모리 추정이 어긋나므로, `OBJECT ENCODING`과 `MEMORY USAGE`로 확인하고 필요하면 직렬화기를 바꿉니다. 필드 수에 상한이 없는 데이터라면 자료형보다 키를 나누는 설계가 먼저입니다.

### 예상 질문과 꼬리 질문

**Q. 세션을 JSON String이 아니라 Hash로 저장한 이유는 무엇인가요?**
세션은 요청마다 일부 필드만 바뀌기 때문입니다. String이면 필드 하나를 바꿀 때 `GET`으로 전체를 받아 고쳐 `SET`으로 다시 보내야 하고, 그 사이 같은 세션을 바꾸는 다른 요청이 있으면 한쪽 변경이 사라집니다. Hash는 필드 단위 명령 하나로 바꾸므로 서로 다른 필드를 바꾸는 요청끼리 덮어쓰지 않습니다. (본문: "필드 하나를 바꾸는 비용이 가장 크게 갈립니다")

- **꼬리: 같은 필드를 동시에 바꾸면요?**
  Hash에서도 나중에 쓴 쪽이 이깁니다. 덮어쓰는 값이라면 그것이 원하는 의미일 수 있고, 숫자를 더하는 변경이라면 `HINCRBY`로 읽기·더하기·쓰기를 명령 하나로 해서 잃지 않습니다.
- **꼬리: String을 유지하면서 갱신 손실을 막을 수는 없나요?**
  `WATCH`로 그사이 바뀌었으면 실패시키고 다시 하거나, Lua 스크립트로 서버 안에서 JSON을 고칠 수 있습니다. `WATCH`는 재시도 코드가 필요하고 같은 키에 경합이 많을수록 재시도가 늘어납니다. Lua는 도는 동안 다른 모든 명령을 기다리게 합니다. 필드 하나를 바꾸는 일에 비해 장치가 큽니다.

**Q. 그럼 객체는 항상 Hash에 담는 게 낫나요?**
아닙니다. 통째로 읽고 통째로 바꾸는 데이터라면 String이 명령 하나로 끝나 더 단순합니다. 객체가 중첩되어 있으면 Hash는 한 단계뿐이라 중첩된 부분을 다시 직렬화해 필드 값에 넣어야 하므로, 그 부분은 String과 같은 비용을 냅니다. (본문: "어떤 조건에서 무엇을 고르는가")

**Q. Hash가 String보다 메모리를 아낀다는 말은 맞나요?**
그대로는 말할 수 없습니다. 확실한 것은 Hash 안에서의 차이입니다. 작은 Hash는 `listpack`이라는 한 덩어리로 담겨 필드마다 붙는 조각 비용이 없고, 필드가 512개를 넘거나 64바이트를 넘는 이름이나 값이 하나라도 들어오면 `hashtable`이 되어 필드마다 이름, 값, 항목 조각을 따로 받습니다(8.2.10 기본 설정값). 같은 객체를 JSON String 하나로 담을 때와 비교해 어느 쪽이 작은지는 직렬화 길이와 할당 크기에 달려 있어, 같은 데이터로 `MEMORY USAGE`를 재 봐야 합니다. 이 문서는 그 비교를 재지 않았습니다. (본문: "Hash: 작을 때는 listpack, 조건을 넘으면 hashtable")

- **꼬리: 그 기준을 올리면 되지 않나요?**
  서버의 모든 Hash에 적용되는 설정입니다. `listpack`은 필드를 앞에서부터 훑어 찾으므로, 기준을 올리면 필드 하나를 찾거나 바꾸는 비용이 커집니다. 실제 데이터로 메모리와 지연을 함께 재고 정해야 합니다.
- **꼬리: 필드를 지워 다시 작아지면 `listpack`으로 돌아오나요?**
  있는 Hash는 돌아오지 않습니다. 재시작하며 RDB 스냅샷을 읽거나 `RESTORE`로 되살려 객체를 새로 만들 때만 그 시점의 크기로 다시 정합니다.

**Q. `HGET`은 O(1)이라고 했는데 `listpack`에서 앞에서부터 훑는다면 O(N) 아닌가요?**
그 경로만 보면 필드 수에 비례합니다. 공식 복잡도는 명령의 대표 경로를 말하고, `listpack`은 필드 512개와 값 64바이트 이하로 묶여 있어 훑는 길이에 상한이 있습니다. 그래서 실무에서는 상수로 봐도 됩니다. 상한을 올리면 이 전제가 약해집니다. (본문: "Hash: 작을 때는 listpack, 조건을 넘으면 hashtable")

**Q. Spring Session을 쓰는데 세션 메모리가 예상보다 큽니다. 왜 그럴까요?**
기본 설정의 Spring Session은 필드 값을 Java 직렬화로 저장합니다. Java 직렬화는 클래스 설명을 함께 적어 `Long` 하나도 64바이트를 넘으므로, 세션 Hash가 처음부터 `hashtable`이 되어 필드마다 조각 비용을 냅니다. `OBJECT ENCODING`으로 확인할 수 있습니다. (본문: "Spring Session은 Hash를 쓰지만, 기본 직렬화 때문에 hashtable이 됩니다")

- **꼬리: 직렬화기를 JSON으로 바꾸면 해결되나요?**
  값이 짧아지면 `listpack`에 머물 수 있지만 보장은 아닙니다. 타입 정보를 함께 적는 JSON 직렬화기는 값이 길어질 수 있고, 큰 객체를 속성으로 넣으면 어차피 64바이트를 넘습니다. 바꾼 뒤 `OBJECT ENCODING`으로 확인하고, 이미 저장된 옛 형식의 세션을 읽는 문제도 함께 풀어야 합니다.
- **꼬리: 그 정도 차이가 장애로 이어지나요?**
  세션 하나만 보면 필드가 몇 개뿐이라 늘어나는 조각도 몇 개입니다. 다만 세션 수 × 필드 수에 비례해 메모리 차이가 커지므로, 메모리 한도와 축출 정책을 정할 때 추정이 어긋납니다.

**Q. String 세션을 수정했더니 세션이 만료되지 않습니다. 왜 그런가요?**
옵션 없는 `SET`은 키의 기존 TTL을 지우기 때문입니다. 처음 저장할 때 TTL을 걸었어도, `GET` → 수정 → `SET`으로 덮어쓰면 만료 없는 키가 됩니다. Spring의 `set(key, value)`가 이 경우입니다. `set(key, value, timeout)`이나 `KEEPTTL`을 써야 합니다. Hash의 `HSET`은 필드만 바꾸므로 키의 TTL을 건드리지 않습니다. (본문: "String을 덮어쓰면 TTL이 사라집니다")

**Q. 모든 사용자 정보를 Hash 하나에 필드로 넣으면 키 수가 줄어 좋지 않나요?**
필드 수에 상한이 없는 Hash는 객체가 아니라 큰 키입니다. 512개를 넘으면 `hashtable`이 되고, 그 뒤로는 `HGETALL` 한 번이나 기본 설정의 `DEL` 한 번이 필드 수만큼 일하는 동안 다른 모든 요청이 기다립니다. 사용자마다 키를 나누는 것이 먼저입니다. (본문: "데이터가 늘면 무엇이 달라지는가")

- **꼬리: 키를 나누면 무엇을 잃나요?**
  전체를 한 번에 다루는 연산이 여러 키에 걸칩니다. 전체 목록이 필요하면 `SCAN`으로 훑거나 별도 목록을 관리해야 하고, 여러 키를 원자적으로 바꿔야 하면 조건이 하나 붙습니다. Redis 클러스터는 키 이름으로 계산한 번호(슬롯)에 따라 키를 여러 서버에 나눠 두는데, 여러 키를 한 명령이나 트랜잭션으로 다루려면 그 키들이 같은 슬롯에 있어야 합니다.
