# Redis 이벤트 루프: 명령 하나가 왜 모든 요청을 멈추는가

> **핵심 질문**: Redis 서버는 연결 수천 개의 명령을 어떤 순서로 처리하는가? 그 구조에서 왜 명령 하나가 관련 없는 다른 요청까지 기다리게 하며, 백엔드 개발자는 무엇을 피하고 무엇을 관찰해야 하는가?
>
> **범위**: 단일 Redis 서버의 메인 스레드가 명령을 처리하는 순서와, 그 순서가 지연으로 이어지는 경로를 다룹니다. 원자성은 [Redis 원자성](./atomicity.md), `fork`와 디스크 쓰기의 세부는 [Redis 영속성](./persistence.md)에서 다룹니다. 이벤트 루프 함수의 호출 순서와 I/O 스레드의 내부 동작은 판단에 필요한 만큼만 씁니다.

## 배경 지식

### 이 문서의 예시: 연결 셋, 큰 해시 하나

문서 끝까지 다음 장면 하나를 씁니다.

- Redis 서버 한 대가 기본 설정으로 돌고 있습니다. I/O 스레드(뒤에서 설명)는 꺼져 있습니다.
- 서버에는 필드가 100만 개인 해시 키 `user:all`이 있습니다. 해시는 필드와 값의 쌍을 모은 자료형으로, Kotlin의 `Map<String, String>` 하나를 키 하나에 담은 것과 비슷합니다.
- 거의 같은 순간에 세 연결이 명령을 보냅니다.

| 연결 | 보내는 쪽 | 명령 | 하는 일 |
| --- | --- | --- | --- |
| A | 주문 API | `GET stock:1` | 재고 값 하나 읽기 |
| B | 관리자 배치 | `HGETALL user:all` | 해시의 필드 100만 개를 모두 읽기 |
| C | 로그인 API | `GET session:42` | 세션 값 하나 읽기 |

```text
주문 API   ── 연결 A: GET stock:1 ──────┐
관리자 배치 ── 연결 B: HGETALL user:all ─┼──▶ Redis 서버 프로세스 ── 메인 스레드
로그인 API ── 연결 C: GET session:42 ───┘
```

화살표는 명령이 네트워크로 이동하는 방향입니다. 연결 풀, 복제본, 디스크 영속화, Redis의 보조 스레드는 그림에서 뺐습니다. 보조 스레드 중 판단에 영향을 주는 것은 아래 "I/O 스레드를 켜도 명령 실행은 한 줄입니다"와 "무엇이 메인 스레드를 오래 잡는가"에서 다룹니다.

이 문서의 시간 숫자는 흐름을 보여 주기 위한 **가상의 숫자**입니다. `HGETALL user:all`이 실제로 얼마나 걸리는지는 값 크기, CPU, 설정에 따라 다르며, 이 문서에서 재지 않았습니다.

### 프로세스·스레드와 단일 스레드 처리

- **한 줄**: 프로세스는 운영체제가 실행 중인 프로그램에 나눠 준 자기만의 메모리 공간이고, 스레드는 그 안에서 명령어를 차례로 실행하는 흐름입니다. "단일 스레드 처리"는 핵심 데이터를 바꾸는 일을 한 스레드가 차례로 맡는다는 뜻입니다.
- **이 주제에서는**: Redis 서버는 프로세스 하나이고, 키와 값을 읽고 바꾸는 명령은 그 안의 **메인 스레드** 하나가 실행합니다. 예시의 A, B, C 명령도 모두 같은 메인 스레드가 하나씩 실행합니다. 자세한 원리와 대가는 [프로세스와 스레드](../cs/process-and-thread.md)에 있습니다.

### I/O 다중화와 이벤트 루프

- **한 줄**: I/O 다중화는 여러 연결을 운영체제에 등록해 두고 데이터가 도착한 연결만 골라 받는 방식이고, 이벤트 루프는 "준비된 연결이 생길 때까지 기다리기 → 준비된 연결을 차례로 처리하기 → 시각이 된 정기 작업 실행하기"를 한 스레드가 반복하는 구조입니다. 이벤트 루프는 처리 중인 일을 중간에 멈추고 다른 일을 먼저 하지 않으므로, 한 처리가 길면 그 루프의 모든 연결이 기다립니다.
- **이 주제에서는**: Redis 메인 스레드가 바로 이 이벤트 루프를 돕니다. 그래서 연결이 수천 개여도 스레드 하나로 맡을 수 있고, 같은 이유로 `HGETALL user:all` 하나가 A와 C를 기다리게 합니다. 운영체제가 메인 스레드를 잠깐 멈추는 일(선점)이 있어도 다른 연결의 명령이 대신 실행되지는 않습니다. 그 명령들을 실행할 스레드가 메인 스레드뿐이기 때문입니다. 원리는 [I/O 다중화와 이벤트 루프](../cs/io-multiplexing-and-event-loop.md)에 있습니다.

### Redis에서 '클라이언트'는 연결 하나입니다

Redis가 말하는 클라이언트는 서버에 맺어진 연결 하나입니다. 애플리케이션 서버 한 대가 연결 여러 개를 열면 Redis에게는 클라이언트 여러 개입니다. 예시의 A, B, C는 클라이언트 셋입니다. 서버는 연결마다 "받았지만 아직 처리하지 않은 바이트"를 담는 입력 버퍼와 "보낼 응답"을 담는 응답 버퍼를 따로 둡니다.

### 시간은 세 가지로 나눠 셉니다

이 문서에서 가장 헷갈리기 쉬운 지점이라 처음에 나눠 둡니다.

| 이름 | 무엇을 세는가 | 예시에서 C의 경우 |
| --- | --- | --- |
| 실행 시간 | 메인 스레드가 그 명령 자체를 실행한 시간 | `GET session:42`는 아주 짧습니다 |
| 기다린 시간 | 명령이 도착한 뒤 메인 스레드가 다른 일을 하느라 손대지 못한 시간 | B의 `HGETALL`이 끝날 때까지 |
| 클라이언트가 본 지연 | 애플리케이션이 명령을 보내고 응답을 받기까지의 전체 시간 | 네트워크 왕복 + 기다린 시간 + 읽기·해석 + 실행 시간 + 응답 쓰기 |

애플리케이션이 Redis 호출 시간을 재는 위치에 따라, 클라이언트가 본 지연에는 명령을 보내기 **전**의 대기도 들어갑니다. 연결 풀에서 연결을 빌리려고 기다린 시간, 클라이언트 라이브러리 안에서 차례를 기다린 시간이 그렇습니다. 이 대기는 Redis 서버가 알 수 없으므로 서버 지표에는 나오지 않습니다.

C의 실행 시간은 짧지만, 클라이언트가 본 지연은 B 때문에 깁니다. **느려진 요청의 실행 시간이 짧다고 원인이 Redis 밖에 있는 것은 아닙니다.** 아래 "실무 포인트"의 확인 순서가 이 구분에서 출발합니다.

### 복잡도 표기 O(N)

- **한 줄**: O(N)은 "일의 양이 N에 비례해 늘어난다"는 표기입니다. N이 무엇을 세는지는 명령마다 다르며, Redis 공식 문서가 명령마다 적어 둡니다.
- **비유**: 서랍 하나를 여는 일(O(1))과 서랍 N개를 모두 여는 일(O(N))의 차이입니다. 서랍이 두 배가 되면 뒤의 일도 대략 두 배가 됩니다.
- **실제로는**: O(N)은 늘어나는 비율만 말하고 실제 시간은 말하지 않습니다. 같은 O(N)이라도 원소 하나를 처리하는 비용, 값의 크기, CPU에 따라 시간이 다릅니다. 또 N이 작으면 O(N) 명령도 충분히 빠릅니다.
- **이 주제에서는**: `HGETALL`의 공식 복잡도는 "O(N) where N is the size of the hash"입니다. 필드가 10개인 해시라면 문제가 없지만, 예시의 `user:all`은 N이 100만입니다. 문제는 명령 이름이 아니라 **그 명령이 만나는 N**입니다.

## 메인 스레드는 한 바퀴에 무엇을 하는가

Redis 8.2.10 기본 설정에서 메인 스레드는 다음을 반복합니다.

```text
┌─▶ ① 다음 바퀴 전에 할 일: 쓰기 대기 목록에 오른 연결의 응답을 소켓에 씀,
│      빠른 만료 정리, AOF 버퍼를 파일에 씀 등
│   ② 기다림: 데이터가 도착한 연결이 생기거나, 다음 정기 작업 시각이 될 때까지
│   ③ 준비된 연결마다: 소켓에서 읽기 → 완성된 명령을 차례로 해석·실행
│      → 응답은 그 연결의 응답 버퍼에 담고 쓰기 대기 목록에 올림
└── ④ 시각이 된 정기 작업: 만료 키 정리, 통계 갱신 등
```

화살표는 실행 순서입니다. 그림의 용어 두 가지를 먼저 풀어 둡니다. **소켓에 쓴다**는 것은 Redis가 자기 응답 버퍼의 바이트를 운영체제가 연결마다 둔 보내기 버퍼로 넘긴다는 뜻입니다. 실제 전송은 그 뒤 운영체제가 하므로, 소켓에 썼다고 클라이언트가 받았다는 뜻은 아닙니다. **AOF**는 실행한 쓰기 명령을 파일에 덧붙여 재시작 때 되살리는 영속화 방식으로, 켜 두면 명령이 남긴 기록을 ①에서 파일에 씁니다. 네 단계 모두 같은 메인 스레드가 합니다. 그래서 ③에서 명령 하나를 실행하는 동안에는 ①, ②, ④ 어느 것도 돌지 않습니다.

각 단계에서 판단에 필요한 점은 다음과 같습니다.

- **②는 운영체제의 I/O 다중화 호출입니다.** Linux에서는 epoll을 씁니다. 기다리는 최대 시간은 다음 정기 작업까지 남은 시간이라, 연결이 조용해도 정기 작업이 돕니다.
- **③에서 한 연결이 보낸 명령 여러 개는 이어서 실행됩니다.** 클라이언트가 응답을 기다리지 않고 명령을 여러 개 이어 보내면(파이프라이닝), 한 번에 읽힌 완성된 명령들을 차례로 실행한 뒤 다음 연결로 넘어갑니다. 그 연결이 `BLPOP` 같은 기다리는 명령으로 멈추거나 닫히면 남은 명령을 두고 넘어갑니다(아래 "기다리게 하는 명령은 서버를 멈추지 않습니다"). 짧은 명령이라도 한 연결이 한꺼번에 많이 보내면, 그만큼 다른 연결이 기다립니다.
- **③에서 응답은 바로 보내지 않습니다.** 명령을 실행하면 응답은 그 연결의 응답 버퍼에 담기고, 연결은 쓰기 대기 목록에 오릅니다. 실제로 소켓에 쓰는 것은 다음 바퀴의 ①입니다. 그 연결에 앞서 다 보내지 못한 응답이 남아 있던 경우만 예외로 더 일찍 나갈 수 있고, 보통의 짧은 응답은 이 순서를 따릅니다. 이 순서가 바로 아래 절의 두 번째 결과를 만듭니다.
- **④는 기본 초당 10번 돕니다.** 설정 `hz`의 기본값이 10입니다. 다만 `dynamic-hz`가 기본으로 켜져 있어, 연결 수가 많으면 서버가 실제 주기를 두 배씩 올립니다. 정해진 시각이 지나도 ③이 끝나지 않으면 ④는 그만큼 늦게 돕니다.

## 명령 하나가 모두를 멈추는 과정

예시의 세 명령이 같은 바퀴에서 A, B, C 순서로 처리된다고 해 봅시다. 같은 바퀴의 연결을 어떤 순서로 처리할지는 운영체제가 돌려준 순서에 달려 있어 정해져 있지 않습니다. 순서가 달라도 결론은 같습니다.

```text
시간 →  (가상의 숫자)

메인 스레드 │②대기│③A 실행│③B: HGETALL user:all 실행 ··············· 800ms │③C 실행│④│①A·B·C 응답 쓰기│
            │      │ 짧음  │                                              │ 짧음  │  │                 │
연결 A      │      │실행 끝, 응답은 버퍼에서 대기 ··································│응답 나감
연결 C      │      │도착했지만 차례를 기다림 ·······································│실행·응답 나감
```

그림의 칸 길이는 시간을 비율대로 나타내지 않습니다. 이 한 바퀴에서 세 가지 일이 생깁니다.

1. **C는 B가 끝날 때까지 기다립니다.** C의 `GET`은 아주 짧지만, 메인 스레드가 B의 `HGETALL`을 실행하는 800ms 동안 손대지 못합니다. C가 본 지연은 800ms 이상입니다.
2. **A도 기다립니다. 먼저 실행됐는데도 그렇습니다.** A의 응답은 실행 직후 응답 버퍼에 담겼지만, 소켓에 쓰는 것은 다음 바퀴의 ①입니다. 그래서 A의 응답도 B가 끝난 뒤에야 나갑니다. "B보다 먼저 처리된 명령은 영향이 없다"는 생각은 기본 설정의 8.2.10에서 보통은 맞지 않습니다. A에 앞서 다 보내지 못한 응답이 남아 있었다면 앞 절의 예외 경로로 먼저 나갈 수도 있습니다.
3. **서버의 다른 일도 멈춥니다.** 새 연결 수락, 만료 키 정리, 정기 작업, 복제본으로 보낼 쓰기 전송도 모두 메인 스레드의 일이라 함께 밀립니다.

이것은 [Redis 원자성](./atomicity.md)에서 본 "명령 하나는 다른 명령과 섞이지 않는다"는 성질의 뒷면입니다. 섞이지 않는다는 약속은, 실행 중인 명령이 끝날 때까지 다른 명령이 끼어들 수 없다는 뜻이기도 합니다. Redis 공식 문서도 단일 스레드의 결과로 "when a request is slow to serve all the other clients will wait for this request to be served"라고 적고 있습니다(공식 문서 latency, "Latency generated by slow commands").

A와 C 입장에서 보면 자기 명령에는 아무 문제가 없습니다. 그래서 A와 C를 보내는 API의 로그만 보면 "Redis가 갑자기 느려졌다"로 보이고, 원인인 B는 다른 서비스나 배치에 있을 수 있습니다.

## 무엇이 메인 스레드를 오래 잡는가

메인 스레드를 오래 잡는 일은 크게 두 종류입니다. 원소 수에 비례하는 명령, 그리고 명령이 아니어도 메인 스레드에서 도는 서버 작업입니다.

### 원소 수에 비례하는 명령

| 원인 | 왜 오래 걸리는가 | 대안 | 대안의 대가 |
| --- | --- | --- | --- |
| `KEYS 패턴` | 데이터베이스의 키 전체를 훑습니다. 공식 복잡도는 O(N), N은 데이터베이스의 키 수입니다 | `SCAN`으로 조금씩 나눠 훑기 | 아래 "SCAN 계열의 대가" 참고 |
| 큰 컬렉션 전체 읽기: `HGETALL`, `SMEMBERS`, `LRANGE 0 -1`, `ZRANGE 0 -1` 등 | 원소마다 응답을 만듭니다. 예시의 `HGETALL user:all`이 여기에 해당합니다 | `HSCAN`·`SSCAN`·`ZSCAN`으로 나눠 읽기, 범위를 지정해 일부만 읽기, 필요한 필드만 읽기(`HMGET`), 키를 작게 나누기 | 여러 번 왕복해야 하고, 한 번에 일관된 전체를 보지 못합니다. 키를 나누면 전체를 다루는 코드가 복잡해집니다 |
| 원소가 많은 키 `DEL` | 원소마다 메모리를 해제합니다. 공식 복잡도도 문자열이 아닌 값은 원소 수 M에 비례한다고 적습니다 | `UNLINK`, 또는 `lazyfree-lazy-user-del yes`로 `DEL`도 같은 방식으로 지우기 | 메모리가 바로 돌아오지 않고 보조 스레드가 나중에 해제합니다 |
| 큰 집합 연산이나 정렬: `SUNION`, `SINTER`, `SORT` 등 | 공식 복잡도가 입력 집합의 크기에 따라 늘어나, 입력이 클수록 깁니다. 공식 문서도 큰 집합의 교집합을 오래 걸리는 예로 듭니다 | 입력 크기를 제한하거나, 결과를 미리 계산해 두기 | 미리 계산한 결과를 최신으로 유지하는 일이 생깁니다 |
| 오래 도는 Lua 스크립트, Functions | 스크립트 전체가 명령 하나로 실행됩니다 | 스크립트를 짧게 유지 | [Redis 원자성](./atomicity.md)의 "대가 1: 스크립트가 도는 동안 다른 모든 명령이 기다립니다" 참고 |

`UNLINK`가 빠른 이유는 일을 없애서가 아니라 옮겨서입니다. `UNLINK`는 키를 키 공간에서 떼어 내는 일만 메인 스레드에서 하고, 원소를 해제하는 일은 백그라운드 스레드(bio)에 넘깁니다. 공식 복잡도도 "O(1) for each key removed regardless of its size. Then the command does O(N) work in a different thread"입니다. 해제할 것이 적은 값은 넘기는 비용이 더 크므로, 해제할 일이 적은 값은 `UNLINK`여도 메인 스레드가 바로 해제합니다. 예시의 `user:all`처럼 원소가 많은 키만 넘깁니다. 8.2.10 기본값에서는 `DEL`이 이 방식을 쓰지 않습니다(`lazyfree-lazy-user-del no`).

### SCAN 계열의 대가

`SCAN`은 한 번에 일부만 훑고 다음 위치(커서)를 돌려줍니다. 호출 한 번은 짧으므로 그 사이에 다른 연결의 명령이 실행될 수 있습니다. 대신 공식 문서가 밝히는 다음 대가가 있습니다(공식 문서 scan, "Scan guarantees").

- 처음부터 끝까지 계속 있던 원소는 반드시 한 번 이상 돌려줍니다.
- **같은 원소를 여러 번 돌려줄 수 있습니다.** 애플리케이션이 중복을 처리해야 합니다.
- 훑는 도중에 추가되거나 지워진 원소는 돌려줄 수도, 아닐 수도 있습니다.
- 호출 한 번이 돌려주는 개수는 정해져 있지 않습니다. 커서가 0으로 돌아와야 끝난 것입니다.

전체 작업량(O(N))은 줄지 않습니다. 줄어드는 것은 **한 번에 메인 스레드를 차지하는 시간**입니다. 단, 호출 한 번의 일감은 `COUNT` 옵션이 정하는데 이는 힌트일 뿐 시간 상한이 아닙니다. `COUNT`를 크게 주면 호출 한 번도 그만큼 길어지고, 작은 컬렉션을 한 덩어리로 담은 키에는 `HSCAN` 등이 한 번에 전체를 돌려줄 수도 있습니다(공식 문서 scan, "The COUNT option"). 그래서 `SCAN`은 "모두를 한 번에 멈추게 하던 일"을 "잘게 나눠 다른 요청 사이에 끼워 넣는 일"로 바꿉니다.

### 응답이 크면 실행 뒤에도 비용이 남습니다

`HGETALL user:all`은 실행 중에 필드 100만 개의 응답을 그 연결의 응답 버퍼에 모두 만듭니다. 실행이 끝나도 이 응답이 소켓으로 다 나갈 때까지 서버 메모리를 차지합니다. 8.2.10 기본 설정에서 일반 클라이언트의 응답 버퍼에는 한도가 없습니다(`redis.conf`의 `client-output-buffer-limit normal 0 0 0`).

보내는 쪽은 한 연결이 루프를 독점하지 않게, 큰 응답을 여러 바퀴에 나눠 보냅니다. 그러니 큰 응답은 **응답을 만드는 실행 단계**에서 한 번에 메인 스레드를 오래 잡고, 보내는 단계에서는 나뉜 쓰기 비용과 다 나갈 때까지의 메모리 비용을 남깁니다. 두 단계 중 어느 쪽이 더 큰지는 응답 크기와 네트워크에 달려 있어 이 문서에서 재지 않았습니다.

### 명령이 아니어도 메인 스레드에서 도는 일

| 작업 | 언제 생기는가 | 메인 스레드에 남는 비용 | 더 볼 곳 |
| --- | --- | --- | --- |
| 만료 키 정리 | TTL이 지난 키를 주기적으로 찾아 지움 | 아래 "만료 정리도 메인 스레드가 합니다" | 이 문서 |
| 큰 키의 만료·메모리 한도 삭제 | 원소가 많은 키가 만료되거나 메모리 한도로 밀려날 때 | 8.2.10 기본값(`lazyfree-lazy-expire no`, `lazyfree-lazy-eviction no`)에서는 메인 스레드가 원소를 직접 해제 | `redis.conf`의 LAZY FREEING 절 |
| `fork` 호출 | 백그라운드 RDB 저장(데이터 전체를 파일 하나로 떠 두기), AOF 다시 쓰기(계속 커진 AOF를 지금 데이터 기준의 새 파일로 바꾸기), 복제본 전체 동기화(복제본이 데이터 전체를 처음부터 받기). 셋 다 자식 프로세스를 만들어 일을 맡깁니다. 다만 전체 동기화는 이미 진행 중인 저장에 합류하거나 기다릴 수 있어, 요청 시각과 `fork` 시각이 다를 수 있습니다. `SAVE` 명령은 자식 없이 메인 스레드가 직접 저장하므로 저장하는 내내 멈춥니다 | 자식 프로세스를 만드는 호출 자체가 메인 스레드를 멈춤. 메모리가 클수록 길어짐 | [Redis 영속성](./persistence.md), [fork와 copy-on-write](../cs/fork-and-copy-on-write.md) |
| AOF 쓰기 | AOF를 켰을 때 ①에서 | 디스크가 느리면 쓰기와 동기화가 밀림 | [Redis 영속성](./persistence.md) |

### 만료 정리도 메인 스레드가 합니다

TTL은 키에 붙이는 유효 기간입니다. 시간이 지나면 키는 논리적으로 없는 것이 되지만, 메모리에서 실제로 지우는 일은 따로 일어납니다. TTL이 지난 키는 두 경로로 지워집니다. 명령이 그 키에 접근할 때 지우는 경로와, 메인 스레드가 주기적으로 TTL이 있는 키를 표본으로 골라 지운 키를 찾는 경로입니다. 뒤의 경로는 ④의 정기 작업과 ①에서 돌며, 8.2.10은 이 일이 메인 스레드를 무한정 잡지 않게 시간 한도를 둡니다.

- ④에서 도는 정리는 기본 설정(`hz` 10)에서 한 번에 약 25ms까지 씁니다. `dynamic-hz`로 실제 주기가 올라가면 한 번의 한도는 줄고 더 자주 돕니다.
- ①에서 도는 정리는 필요하다고 판단할 때만 돌고, 한 번에 약 1ms까지 씁니다.
- 두 한도 모두 정확한 상한이 아닙니다. 한도는 키 몇 개를 처리할 때마다 확인하므로 조금 넘을 수 있습니다. 특히 그 사이에 원소가 많은 키를 메인 스레드에서 해제하면, 그 해제가 끝날 때까지 한도 확인이 돌아오지 않습니다.

그래서 **많은 키에 같은 TTL을 주어 한꺼번에 만료되게 하면**, 아무도 명령을 보내지 않았는데도 정기 작업이 돌 때마다 수십 ms씩 다른 명령이 기다릴 수 있습니다. 한도에 닿은 횟수는 `INFO stats`의 `expired_time_cap_reached_count`로 셉니다.

## 기다리게 하는 명령은 서버를 멈추지 않습니다

`BLPOP`처럼 "데이터가 생길 때까지 기다리는" 명령은 이름과 달리 서버를 멈추지 않습니다. 공식 문서는 `BLPOP`이 "blocks the connection"이라고 씁니다(공식 문서 blpop). 멈추는 것은 **그 명령을 보낸 연결 하나**입니다. 메인 스레드는 그 연결을 "기다리는 중"으로 표시해 두고 다른 연결의 명령을 계속 처리하다가, 다른 연결이 그 리스트에 원소를 넣으면 그때 기다리던 연결에 응답합니다. 기다리는 연결 수는 `INFO clients`의 `blocked_clients`로 볼 수 있습니다.

| 구분 | 무엇이 멈추는가 | 예 |
| --- | --- | --- |
| 오래 실행되는 명령 | 메인 스레드, 곧 모든 연결 | `HGETALL user:all`, `KEYS *` |
| 기다리는 명령 | 그 명령을 보낸 연결 하나 | `BLPOP`, `XREAD BLOCK` |

다만 기다리는 연결은 그동안 다음 명령을 처리받지 못합니다. 바이트를 보내는 것 자체는 가능하지만, 서버는 기다림이 끝날 때까지 그 연결의 다음 명령을 해석하지 않습니다. 그래서 애플리케이션은 기다리는 명령에 연결을 따로 써야 합니다. Spring Data Redis의 `LettuceConnectionFactory`는 기본적으로 연결 하나를 여러 스레드가 공유하되, 기다리는 명령과 트랜잭션에는 공유하지 않는 별도 연결을 쓴다고 설명합니다(Spring Data Redis 3.5.13 `LettuceConnectionFactory` 문서 주석).

## I/O 스레드를 켜도 명령 실행은 한 줄입니다

`io-threads`를 2 이상으로 켜면 소켓 읽기·쓰기와 요청 해석을 다른 스레드가 나눠 맡을 수 있습니다. 8.2.10 기본값은 1, 곧 꺼짐입니다. 켜더라도 명령 실행은 여전히 메인 스레드가 합니다. 자세한 근거는 [Redis 원자성](./atomicity.md)의 "Redis는 명령을 메인 스레드 하나에서 하나씩 끝까지 실행합니다"에 있습니다.

그래서 I/O 스레드는 "읽고 쓰는 일이 메인 스레드를 차지하는 시간"을 줄여 줄 수 있지만, 예시의 `HGETALL user:all`처럼 **실행 자체가 긴 명령의 문제는 풀지 못합니다.** 응답을 만드는 일은 실행 단계에 있기 때문입니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

- **원소 수가 늘면, 코드를 바꾸지 않아도 지연이 늘어납니다.** `user:all`이 처음에는 필드 1,000개였다가 서비스가 커지며 100만 개가 되면, 같은 `HGETALL` 호출이 메인 스레드를 차지하는 시간도 대략 비례해 늘어납니다. 이 비용은 그 호출을 보낸 배치만이 아니라 같은 서버를 쓰는 모든 연결이 나눠 냅니다. 개발 환경의 작은 데이터로는 드러나지 않는 이유입니다.
- **요청 수가 늘면, 짧은 명령끼리도 기다립니다.** 명령마다 실행이 짧아도 메인 스레드는 한 번에 하나만 처리하므로, 도착하는 속도가 처리 속도에 가까워지면 기다린 시간이 늘어납니다. 메인 스레드의 처리량은 코어 하나의 속도를 넘지 못합니다.
- **클러스터로 나누면 다른 키끼리는 서로 덜 기다립니다. 같은 키의 문제는 그대로입니다.** Redis 클러스터는 키를 여러 서버(노드)에 나눠 담으므로, 한 노드의 긴 명령은 그 노드를 쓰는 요청만 기다리게 합니다. 하지만 `user:all`은 키 하나라 한 노드에 통째로 있고, 그 노드의 메인 스레드에서 똑같이 오래 걸립니다.
- **느린 조회를 복제본으로 보내는 방법에도 대가가 있습니다.** 공식 문서는 느린 명령을 피할 수 없으면 복제본에서 실행하라고 제안합니다(공식 문서 latency, "Latency generated by slow commands"). 그러면 주 서버의 다른 요청은 기다리지 않습니다. 대신 복제본도 메인 스레드 하나로 주 서버가 보낸 쓰기를 적용하므로, 긴 명령이 도는 동안 복제가 밀리고 그 복제본에서 읽는 다른 요청도 기다립니다. 복제의 지연과 읽기 일관성은 [Redis 복제](./replication.md)에서 다룹니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 바로잡은 곳 |
| --- | --- | --- |
| 내 명령이 짧으면 내 요청은 빠르다 | 앞에서 메인 스레드를 차지한 명령만큼 기다립니다. 같은 바퀴에서 먼저 실행된 명령도 응답이 늦게 나갈 수 있습니다 | 명령 하나가 모두를 멈추는 과정 |
| 느린 명령은 특정 명령 이름이다 | 문제는 명령이 만나는 N입니다. 필드 10개짜리 `HGETALL`은 괜찮고, 100만 개짜리는 위험합니다 | 배경 지식 |
| `SCAN`은 `KEYS`보다 일을 적게 한다 | 전체 일은 줄지 않고, 호출이 나뉘는 만큼 호출마다의 처리가 더해집니다. 한 번에 메인 스레드를 차지하는 시간이 짧을 뿐이고, 중복을 돌려줄 수 있습니다 | SCAN 계열의 대가 |
| `UNLINK`는 메모리를 바로 돌려준다 | 키만 바로 떼어 내고, 메모리는 보조 스레드가 나중에 해제합니다 | 원소 수에 비례하는 명령 |
| `BLPOP`은 서버를 막는다 | 그 명령을 보낸 연결만 기다립니다 | 기다리게 하는 명령은 서버를 멈추지 않습니다 |
| I/O 스레드를 켜면 긴 명령의 문제가 풀린다 | 실행은 여전히 메인 스레드가 합니다 | I/O 스레드를 켜도 명령 실행은 한 줄입니다 |
| 아무도 명령을 안 보내면 지연이 없다 | 만료 정리와 `fork`처럼 명령이 아닌 일도 메인 스레드에서 돕니다 | 명령이 아니어도 메인 스레드에서 도는 일 |
| SLOWLOG에 없으면 Redis 쪽 원인이 아니다 | SLOWLOG는 명령의 실행 시간만 기록합니다. 기다린 시간과, 정기 작업이 하는 만료 정리나 자동 저장의 `fork`처럼 명령 밖에서 도는 일은 들어가지 않습니다 | 실무 포인트 |

## 근거

**소스**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10))

- [`src/ae.c`](https://github.com/redis/redis/blob/8.2.10/src/ae.c): 플랫폼별 evport·epoll·kqueue·select 선택, `aeProcessEvents`(beforesleep 호출 → 가장 가까운 타이머까지 기다리는 `aeApiPoll` → 준비된 소켓의 핸들러 호출 → `processTimeEvents`), `aeMain`
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `readQueryFromClient`, `processInputBuffer`(연결이 기다리는 명령으로 멈추거나 닫히면 남은 입력을 두고 반복을 멈춤. 그 밖에는 입력 버퍼의 완성된 명령을 차례로 `processCommandAndResetClient`), `_prepareClientToWrite`·`putClientInPendingWriteQueue`(앞서 못 보낸 응답이 없으면 바로 쓰지 않고 쓰기 대기 목록에 올림), `handleClientsWithPendingWrites`, 큰 응답을 여러 바퀴에 나눠 보냄: 일반 클라이언트는 한 번에 쓴 양이 `NET_MAX_WRITES_PER_EVENT`를 넘으면 멈춤(쓴 뒤에 확인하므로 정확한 상한은 아님), `maxmemory`를 넘은 상태에서는 이 제한을 두지 않음
- [`src/server.h`](https://github.com/redis/redis/blob/8.2.10/src/server.h): `NET_MAX_WRITES_PER_EVENT (1024*64)`, `CONFIG_DEFAULT_HZ 10`
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c): `beforeSleep`(빠른 만료 주기, `handleClientsWithPendingWrites`), `serverCron` 등록, `databasesCron`(느린 만료 주기), `call`(명령 함수 실행 시간만 재어 `slowlogPushCurrentCommand`·`latencyAddSampleIfNeeded`의 `command`/`fast-command`에 기록), `INFO`의 `blocked_clients`, `expired_time_cap_reached_count`, `eventloop_cycles`, `eventloop_duration_sum`, `eventloop_duration_cmd_sum`, `instantaneous_eventloop_duration_usec`, Debug 절의 `eventloop_duration_max`
- [`src/expire.c`](https://github.com/redis/redis/blob/8.2.10/src/expire.c): `activeExpireCycle`. 느린 주기의 한도는 정기 작업 한 주기의 25%이고 CPU 시간이 아니라 흐른 시간으로 잼(`ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25`, 한도 `25*1000000/hz/100`마이크로초, 기본 `active-expire-effort 1` 기준이라 `hz` 10이면 약 25ms). 빠른 주기는 약 1ms(`ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000`마이크로초). 16회 반복마다 한도 확인, `stat_expired_time_cap_reached_count`, `expire-cycle` 지연 기록
- [`src/rdb.c`](https://github.com/redis/redis/blob/8.2.10/src/rdb.c): `saveCommand`는 자식 없이 `rdbSave`를 직접 실행, `rdbSaveBackground`는 자식을 만듦
- [`src/replication.c`](https://github.com/redis/redis/blob/8.2.10/src/replication.c): `syncCommand`(디스크 대상 BGSAVE가 진행 중이면 그 저장에 합류할 수 있음), `startBgsaveForReplication`
- [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c): `delCommand`(`lazyfree_lazy_user_del`), `unlinkCommand`(항상 `dbAsyncDelete` 경로), `delGenericCommand`
- [`src/lazyfree.c`](https://github.com/redis/redis/blob/8.2.10/src/lazyfree.c): 해제할 일이 적은 값은 `UNLINK`여도 바로 해제함. `LAZYFREE_THRESHOLD 64`, `lazyfreeGetFreeEffort`(바이트가 아닌 작업량 추정. 원소마다 따로 담는 해시·집합·정렬 집합은 원소 수, 리스트는 노드 수, 한 덩어리로 담은 작은 컬렉션과 문자열은 1), `freeObjAsync`(작업량이 64 이하이면 바로 해제)
- [`src/script.c`](https://github.com/redis/redis/blob/8.2.10/src/script.c): `scriptKill`(쓰기를 실행한 스크립트와 복제로 받은 스크립트는 `UNKILLABLE`), `scriptInterrupt`
- [`src/slowlog.c`](https://github.com/redis/redis/blob/8.2.10/src/slowlog.c): 항목마다 클라이언트 주소(`peerid`)와 이름(`cname`)을 함께 저장
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `hz`(기본 10), `dynamic-hz`(기본 켜짐, `serverCron`에서 연결 수에 따라 주기를 올림), `io-threads`(기본 1), `slowlog-log-slower-than`(기본 10000마이크로초), `slowlog-max-len`(기본 128), `latency-monitor-threshold`(기본 0), `lazyfree-lazy-user-del`·`lazyfree-lazy-expire`·`lazyfree-lazy-eviction`(기본 no), `active-expire-effort`(기본 1)
- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): LAZY FREEING 절, `client-output-buffer-limit normal 0 0 0`
- [`src/commands/hgetall.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hgetall.json), [`keys.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/keys.json), [`del.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/del.json), [`unlink.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/unlink.json), [`scan.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/scan.json), [`sunion.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/sunion.json), [`sinter.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/sinter.json), [`sort.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/sort.json): 공식 복잡도

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`. 특정 Redis 버전 전용 문서가 아니므로 소스와 다르면 소스를 따릅니다)

- [latency](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/optimization/latency.md): "Single threaded nature of Redis", "Latency generated by slow commands"(느린 명령을 복제본에서 실행하라는 제안, `KEYS` 경고), "Latency generated by fork", "Latency generated by expires"(시간 한도 없이 "25% 넘게 만료되면 반복한다"는 옛 설명을 적고 있어 8.2.10 소스와 맞지 않음. 이 문서는 소스를 따름)
- [latency-monitor](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/optimization/latency-monitor.md): 기본 꺼짐(threshold 0), 켜는 방법
- [slowlog](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/slowlog.md): 실행 시간에 클라이언트와 주고받는 I/O가 들어가지 않음
- [scan](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/scan.md): "Scan guarantees", 호출당 반환 개수, "The COUNT option"(작업량 힌트, 작은 표현은 한 번에 전체 반환)
- [blpop](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/blpop.md): 연결을 기다리게 함
- [info](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/info.md): `eventloop_*` 지표의 뜻

**Spring Boot, Spring Data Redis와 Lettuce**

- Spring Boot 태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16)
  - [`RedisProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisProperties.java): `clientName`(`spring.data.redis.client-name`). 연결에 붙일 이름. 실제로 보내는 명령은 프로토콜에 따라 다름(Lettuce 6.6.0.RELEASE는 RESP3 연결에서 `HELLO … SETNAME`, RESP2 연결에서 `CLIENT SETNAME`)

- Spring Data Redis 태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13)
  - [`LettuceConnectionFactory.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionFactory.java): `shareNativeConnection` 기본 true, 기다리는 명령과 트랜잭션은 공유하지 않는 연결 사용(클래스 주석)
  - [`LettuceClientConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClientConfiguration.java): 명령 타임아웃 기본값 `RedisURI.DEFAULT_TIMEOUT`(60초)
  - [`RedisTemplate.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisTemplate.java): `keys`는 `KEYS`, `scan`은 `SCAN`
  - [`DefaultHashOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/DefaultHashOperations.java): `entries`는 `HGETALL`
- Lettuce 태그 [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE)(Spring Data Redis 3.5.13의 `pom.xml`이 지정한 버전)
  - [`RedisURI.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisURI.java): `DEFAULT_TIMEOUT = 60`
  - [`RedisHandshake.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisHandshake.java), [`RedisCommandBuilder.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisCommandBuilder.java): RESP3 연결은 `HELLO`에 `SETNAME`을 실어 이름을 붙이고, RESP2 연결은 연결 메타데이터 단계에서 따로 붙임
  - [`CommandExpiryWriter.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/CommandExpiryWriter.java): 타임아웃이 되면 클라이언트 쪽 명령 객체만 예외로 완료하고, 서버에는 아무것도 보내지 않음

## 실무 포인트

### 무엇을 피하고 무엇으로 바꿀까

| 중요도 | 항목 | 이유와 선택 |
| --- | --- | --- |
| 필수 | 운영 서버에서 `KEYS` 쓰지 않기 | 키 전체를 훑는 동안 모든 연결이 기다립니다. Spring의 `RedisTemplate.keys()`도 `KEYS`를 보냅니다. 키 목록이 필요하면 `scan()`을 쓰고, 중복을 견디는 코드로 처리합니다. 자주 필요하다면 키 목록을 별도 Set으로 관리하는 설계도 검토합니다. 이때는 Set과 실제 키를 함께 맞추는 일이 생깁니다. |
| 필수 | 원소 수가 계속 느는 키에 전체 읽기를 쓰지 않기 | `opsForHash().entries()`는 `HGETALL`입니다. 원소 수의 상한이 없는 키라면 처음부터 나눠 읽기나 필요한 필드만 읽기로 짭니다. 데이터가 커진 뒤에 바꾸려면 호출하는 쪽을 모두 고쳐야 합니다. |
| 필수 | 큰 키는 `UNLINK`로 지우기 | `DEL`은 8.2.10 기본값에서 원소를 메인 스레드가 해제합니다. 대가는 메모리가 늦게 돌아온다는 것이라, 메모리 한도 근처에서 큰 키를 연달아 지우면 여유가 생각보다 늦게 생깁니다. |
| 필수 | 키 하나가 담을 원소 수에 상한 두기 | 위 세 항목의 근본 대책입니다. 상한을 넘을 수 있으면 키를 나눕니다. 나누면 전체를 다루는 연산이 여러 키에 걸치므로, 원자성이 필요하면 [Redis 원자성](./atomicity.md)의 방법을 다시 골라야 합니다. |
| 알면 유리 | 배치·관리 작업에 연결 이름을 붙이고, 호출 한 번의 일감을 작게 두기 | 이름을 붙이면 SLOWLOG에 남은 긴 명령을 누가 보냈는지 바로 찾습니다. 다만 SLOWLOG는 명령 하나가 기준 시간을 넘을 때만 남기므로, 짧은 명령을 대량으로 보내 생긴 지연은 이름을 붙여도 SLOWLOG에 나오지 않을 수 있습니다. 그래서 일감 자체를 작게 둡니다. `SCAN`의 `COUNT`, 한 번에 지우는 키 수, 파이프라인 한 묶음의 명령 수를 작게 두고 묶음 사이에 간격을 둡니다. 짧은 명령이라도 한 연결이 한꺼번에 많이 보내면 이어서 실행되어 다른 연결이 기다리기 때문입니다. 대가는 배치가 늦게 끝난다는 것입니다. |
| 알면 유리 | 한꺼번에 만료되지 않게 TTL을 흩기 | 같은 시각에 대량으로 만료되면 정기 작업마다 수십 ms씩 다른 명령이 기다릴 수 있습니다. 대가는 만료 시각이 정확히 맞지 않는다는 것이라, 정확한 시각이 필요한 데이터에는 쓰지 않습니다. |
| 알면 유리 | 큰 키에 TTL이 있으면 `lazyfree-lazy-expire` 검토 | 기본값에서는 만료된 큰 키를 메인 스레드가 해제합니다. 켜면 해제를 보조 스레드로 넘기고, 대가는 `UNLINK`와 같습니다. |
| 참고 | `io-threads` | 읽기·쓰기 부담이 큰 경우에만 의미가 있습니다. 긴 명령의 문제는 풀지 않습니다. |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 어떤 명령을 보낼지(전체 읽기 대신 나눠 읽기) | 키마다 원소 수가 어떻게 자라는지 |
| 키 하나에 담을 원소 수의 상한, 키를 나누는 방식 | SLOWLOG에 쌓이는 명령과 그 인자 |
| TTL을 정하는 방식 | 정기 작업 시각에 맞춘 주기적인 지연이 있는지 |
| `lazyfree-*` 설정, 지연 모니터 켜기 | 메인 스레드의 CPU 사용률(코어 하나 기준) |
| 클라이언트 타임아웃과 재시도 정책 | 클라이언트가 본 지연과 서버 실행 시간의 차이 |

### 증상과 확인 순서

**증상**: "서로 관련 없는 여러 API가 같은 시각에 Redis 타임아웃을 냈다."

같은 시각에 함께 느려졌다는 점이 메인 스레드를 차지한 무언가를 가리킵니다. 다음 순서로 좁힙니다.

1. **`SLOWLOG GET`으로 그 시각의 긴 명령을 찾습니다.** 기본값으로 10ms(`slowlog-log-slower-than 10000`마이크로초) 이상 걸린 명령이 최근 128개까지 남습니다. 명령과 인자, 보낸 클라이언트의 주소와 이름이 함께 남으므로 원인인 B를 찾을 수 있습니다. 이름은 클라이언트가 연결에 붙여 둔 경우에만 남습니다(Spring Boot는 `spring.data.redis.client-name`). **SLOWLOG는 명령의 실행 시간만 잽니다.** 공식 문서도 "The execution time does not include I/O operations like talking with the client"라고 적습니다. 그래서 A와 C처럼 기다리기만 한 명령은 SLOWLOG에 나오지 않습니다.
2. **SLOWLOG에 없으면 명령이 아닌 일을 봅니다.** 지연 모니터는 기본으로 꺼져 있습니다(`latency-monitor-threshold 0`). 미리 켜 두면 `LATENCY LATEST`로 `command`, `expire-cycle`, `fork`, AOF 관련 이벤트처럼 메인 스레드를 오래 잡은 일을 종류별로 볼 수 있습니다. 장애 뒤에 켜면 그 장애는 기록되지 않으므로, 평소에 서비스가 견딜 지연 기준으로 켜 둡니다.
3. **`INFO`로 이벤트 루프 전체를 봅니다.** `INFO stats`의 `eventloop_duration_sum`(루프가 ②의 기다림을 뺀 나머지 일을 한 시간의 누적, 마이크로초)과 `eventloop_duration_cmd_sum`(그중 명령 실행 시간의 누적)의 증가 폭을 비교하면, 메인 스레드가 바빴던 시간이 명령 실행에서 왔는지 다른 일에서 왔는지 가늠할 수 있습니다. 누적값이므로 개별 요청의 지연을 나눠 주지는 않습니다. `expired_time_cap_reached_count`가 늘었다면 만료 정리가 시간 한도까지 돈 것입니다.
4. **메인 스레드의 CPU 사용률을 코어 하나 기준으로 봅니다.** 요청 수가 평소와 같은데 높다면 긴 명령이 섞였을 가능성이 큽니다. 공식 문서도 같은 판단을 제안합니다(latency, "Latency generated by slow commands").
5. **서버가 그 시각에 바쁘지 않았다면 클라이언트와 네트워크를 봅니다.** 명령이 서버에서 기다린 시간은 Redis의 지표와 로그 어디에도 요청마다 나오지 않습니다. 그래서 "기다리지 않았다"를 직접 확인할 수는 없고, 그 시각에 메인 스레드가 바빴는지로 판단합니다. 3·4번에서 이벤트 루프 지표의 증가 폭과 메인 스레드 CPU 사용률이 평소와 같았는데 클라이언트가 본 지연만 길다면, 원인은 서버 밖일 가능성이 큽니다. 이때는 명령을 보내기 전의 대기(연결 획득, 요청 스레드 부족)와 네트워크가 남습니다. 다만 평소와 같았다는 것이 기다림이 없었다는 증명은 아닙니다. 지연 모니터를 켜 두지 않았거나 지표를 모으는 간격이 길면, 짧은 멈춤이 언제 얼마나 몰렸는지 구분되지 않습니다. 그 경우 서버 원인을 배제하지 말고 다음 장애에 대비해 관측부터 켭니다. 앱 안 대기와 네트워크까지 가르는 전체 순서는 [Redis 지연 진단](./latency-diagnosis.md)에서 다룹니다.

**대응과 부작용**: 원인 명령을 찾았다면 위의 "무엇을 피하고 무엇으로 바꿀까"로 바꿉니다. 급하게 `CLIENT KILL`로 원인 연결을 끊어도 **이미 실행 중인 명령은 멈추지 않습니다.** 메인 스레드가 그 명령을 끝내야 끊는 처리도 할 수 있기 때문입니다. 예외는 긴 Lua 스크립트와 Functions입니다. 실행이 `busy-reply-threshold`를 넘으면 서버가 실행 도중 이벤트를 처리해 `SCRIPT KILL`(Functions는 `FUNCTION KILL`)을 받을 수 있습니다. 다만 이미 쓰기 명령을 실행한 스크립트와 주 서버가 복제로 보낸 스크립트는 멈출 수 없습니다([Redis 원자성](./atomicity.md) 참고).

### Spring에서 확인할 것

- **명령 타임아웃의 기본값은 60초입니다.** Spring Data Redis 3.5.13의 `LettuceClientConfiguration`은 따로 정하지 않으면 Lettuce의 기본값 60초를 씁니다. 값을 정하는 기준과 Spring Boot 설정은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에서 다룹니다. 60초 그대로라면 예시의 C를 기다리는 요청 스레드가 Tomcat 스레드 풀을 오래 묶어, Redis의 지연이 애플리케이션 전체 지연으로 번질 수 있습니다.
- **타임아웃은 서버의 명령을 취소하지 않습니다.** Lettuce는 타임아웃이 되면 클라이언트 쪽에서 예외로 끝낼 뿐, 서버에는 아무것도 보내지 않습니다. B의 `HGETALL`은 서버에서 끝까지 실행됩니다. 이때 B가 타임아웃을 보고 같은 명령을 재시도하면, 같은 긴 명령이 다시 줄을 섭니다. 긴 명령에는 재시도보다 명령 자체를 바꾸는 것이 먼저입니다.
- **기다리는 명령은 연결을 따로 씁니다.** 기본 설정의 `LettuceConnectionFactory`는 일반 명령에 연결 하나를 공유하고, `BLPOP` 같은 기다리는 명령과 트랜잭션에는 공유하지 않는 연결을 씁니다. 그래서 `BLPOP` 때문에 공유 연결의 다른 명령이 멈추지는 않습니다. 대신 기다리는 명령이 동시에 많으면 그만큼 연결이 필요합니다.

## 말로 설명하기

### 30초 버전

Redis는 메인 스레드 하나가 이벤트 루프를 돌며, 데이터가 도착한 연결을 골라 명령을 하나씩 끝까지 실행합니다. 그래서 연결이 많아도 락 없이 처리할 수 있지만, 명령 하나가 오래 걸리면 그동안 다른 모든 연결이 기다립니다. 문제는 명령 이름이 아니라 그 명령이 만나는 원소 수입니다. `KEYS`나 큰 해시의 `HGETALL`, 큰 키의 `DEL`을 피하고 `SCAN`, 나눠 읽기, `UNLINK`로 바꾸며, 원인은 SLOWLOG로 찾습니다.

### 2분 버전

Redis 서버에서 키를 읽고 바꾸는 명령은 메인 스레드 하나가 실행합니다. 메인 스레드는 이벤트 루프를 돕니다. 운영체제에 데이터가 도착한 연결을 묻고, 준비된 연결의 명령을 차례로 실행해 응답을 버퍼에 담고, 다음 바퀴 전에 응답을 한꺼번에 소켓에 씁니다. 만료 키 정리 같은 정기 작업도 같은 스레드가 합니다.

이 구조에서는 명령 실행 중에 메인 스레드의 다른 일이 하나도 돌지 않습니다. 필드 100만 개짜리 해시에 `HGETALL`을 보내면, 그 실행이 끝날 때까지 다른 연결의 짧은 `GET`도 기다립니다. 같은 바퀴에서 먼저 실행된 `GET`도 응답을 다음 바퀴에 쓰므로 함께 늦어집니다. 그래서 여러 API가 같은 시각에 함께 느려졌다면, 각 API의 명령보다 그 시각에 메인 스레드를 차지한 하나를 의심해야 합니다.

대책은 한 번에 메인 스레드를 차지하는 시간을 줄이는 것입니다. `KEYS` 대신 `SCAN`, 전체 읽기 대신 나눠 읽기, `DEL` 대신 `UNLINK`를 씁니다. 전체 일의 양은 줄지 않거나 다른 스레드로 옮겨질 뿐이고, `SCAN`은 중복을 돌려줄 수 있으며 `UNLINK`는 메모리를 늦게 돌려줍니다. 근본적으로는 키 하나의 원소 수에 상한을 둡니다. 원인을 찾을 때는 SLOWLOG가 실행 시간만 잰다는 점에 주의합니다. 기다리기만 한 명령은 나오지 않고, 정기 작업의 만료 정리나 자동 저장의 `fork`는 지연 모니터를 켜 두어야 보입니다.

### 예상 질문과 꼬리 질문

**Q. Redis는 단일 스레드인데 어떻게 연결 수천 개를 처리하나요?**
메인 스레드가 연결마다 기다리지 않고, I/O 다중화로 데이터가 도착한 연결만 골라 처리하기 때문입니다. 조용한 연결은 스레드를 묶지 않습니다. 각 명령이 짧으면 많은 연결을 번갈아 처리할 수 있습니다. 반대로 명령 하나가 길면 그 이점이 그대로 약점이 됩니다. (본문: "메인 스레드는 한 바퀴에 무엇을 하는가")

- **꼬리: 그럼 스레드가 정말 하나뿐인가요?**
  아닙니다. 큰 값의 메모리 해제를 맡는 백그라운드 스레드, 켜면 읽기·쓰기를 맡는 I/O 스레드가 있고, 영속화에는 자식 프로세스도 씁니다. 중요한 것은 명령 실행을 메인 스레드 하나가 한다는 점입니다.

**Q. 내 API는 `GET` 하나만 보내는데 왜 Redis 타임아웃이 나나요?**
`GET`의 실행 시간은 짧아도, 앞에서 다른 연결의 긴 명령이 메인 스레드를 차지하면 그만큼 기다립니다. 같은 바퀴에서 먼저 실행됐더라도 응답은 다음 바퀴에 쓰므로 함께 늦어집니다. 원인은 같은 서버를 쓰는 다른 서비스나 배치에 있을 수 있습니다. (본문: "명령 하나가 모두를 멈추는 과정")

- **꼬리: 그 원인을 어떻게 찾나요?**
  `SLOWLOG GET`으로 그 시각에 오래 실행된 명령과 보낸 클라이언트를 찾습니다. 내 `GET`은 기다리기만 했으므로 SLOWLOG에 나오지 않습니다. SLOWLOG에 아무것도 없으면 지연 모니터와 `INFO`의 이벤트 루프 지표로 만료 정리나 `fork` 같은 명령이 아닌 일을 봅니다.
- **꼬리: SLOWLOG 기준을 낮추면 다 잡히지 않나요?**
  명령의 실행 시간은 더 잘게 잡히지만, 기다린 시간과 명령이 아닌 일은 여전히 들어가지 않습니다. SLOWLOG는 무엇을 재는지가 정해져 있는 도구입니다.

**Q. `KEYS` 대신 `SCAN`을 쓰면 일이 줄어드나요?**
전체 일은 줄지 않습니다. 한 번에 일부만 훑으므로 메인 스레드를 한 번에 차지하는 시간이 줄어, 그 사이에 다른 요청이 실행될 수 있습니다. 대가로 같은 키를 여러 번 돌려줄 수 있고, 훑는 도중 바뀐 키는 나올 수도 안 나올 수도 있습니다. 그래서 결과를 쓰는 코드가 중복을 견뎌야 합니다. (본문: "SCAN 계열의 대가")

- **꼬리: 그럼 `SCAN`으로 훑는 동안 서버 부하는 그대로인가요?**
  전체 일의 양은 줄지 않고, 호출이 여러 번으로 나뉘는 만큼 왕복과 호출마다의 처리가 더해집니다. 달라지는 것은 지연의 모양입니다. 한 번에 모두를 오래 멈추게 하던 일이, 짧은 조각으로 다른 요청 사이에 끼어듭니다.

**Q. `DEL` 대신 `UNLINK`를 쓰면 무엇을 얻고 무엇을 잃나요?**
얻는 것은 메인 스레드가 원소를 하나하나 해제하는 시간입니다. `UNLINK`는 키를 떼어 내는 일만 메인 스레드에서 하고, 해제는 백그라운드 스레드에 넘깁니다. 잃는 것은 메모리를 바로 돌려받지 못한다는 점입니다. 해제가 끝날 때까지 메모리를 차지하므로, 메모리 한도 근처라면 여유가 늦게 생깁니다. 해제 작업량이 작은 값은 넘기는 비용이 더 커서 바로 해제합니다. (본문: "원소 수에 비례하는 명령")

- **꼬리: 그럼 모든 `DEL`을 `UNLINK`처럼 동작하게 하면 되지 않나요?**
  `lazyfree-lazy-user-del yes`로 그렇게 할 수 있습니다. 작은 값은 어차피 바로 해제하므로 손해가 작습니다. 다만 메모리 반환이 늦어지는 대가는 같으므로, 큰 키를 자주 지우고 메모리가 빠듯한 서버라면 해제가 따라오는지 메모리 지표를 함께 봐야 합니다.

**Q. I/O 스레드를 켜면 긴 명령 문제가 사라지나요?**
아닙니다. I/O 스레드는 소켓 읽기·쓰기와 요청 해석을 나눠 맡을 뿐이고, 명령 실행은 메인 스레드가 합니다. `HGETALL user:all`이 응답을 만드는 일은 실행 단계에 있으므로 그대로 깁니다. (본문: "I/O 스레드를 켜도 명령 실행은 한 줄입니다")

**Q. `BLPOP`은 데이터가 올 때까지 막는다는데, 서버 전체가 막히나요?**
그 명령을 보낸 연결 하나만 기다립니다. 메인 스레드는 그 연결을 기다리는 중으로 표시해 두고 다른 명령을 계속 처리합니다. 다만 기다리는 동안 그 연결의 다음 명령은 처리되지 않으므로, Spring Data Redis는 이런 명령에 공유하지 않는 연결을 씁니다. (본문: "기다리게 하는 명령은 서버를 멈추지 않습니다")

**Q. 아무도 무거운 명령을 보내지 않았는데 주기적으로 지연이 튑니다. 무엇을 의심하나요?**
명령이 아닌 일입니다. 많은 키가 같은 시각에 만료되면 정기 작업의 만료 정리가 돌 때마다 시간 한도까지 메인 스레드를 씁니다. 8.2.10 기본 설정에서 실제 주기가 `hz` 10이면 한 번에 약 25ms입니다. 원소가 많은 키가 만료되면 해제 시간이 더해집니다. RDB 저장이나 AOF 다시 쓰기가 시작되는 시각이라면 `fork`도 의심합니다. `expired_time_cap_reached_count`와 지연 모니터의 `expire-cycle`, `fork` 이벤트로 나눠 봅니다. (본문: "만료 정리도 메인 스레드가 합니다")

- **꼬리: 만료 정리가 25ms면 큰 문제가 아니지 않나요?**
  한 번은 짧지만, 대량 만료가 이어지는 동안 정기 작업마다 되풀이됩니다. 응답 시간 목표가 수 ms인 서비스라면 그 시각의 요청들이 목표를 넘습니다. 또 시간 한도는 키 몇 개마다 확인하므로, 그 사이 큰 키를 해제하면 한도를 넘을 수 있습니다.

**Q. 클러스터로 늘리면 이 문제가 해결되나요?**
다른 키끼리는 서로 덜 기다리게 됩니다. 키가 여러 노드에 나뉘므로, 한 노드의 긴 명령은 그 노드를 쓰는 요청만 기다리게 합니다. 하지만 큰 키 하나는 한 노드에 통째로 있으므로, 그 키에 대한 긴 명령은 그 노드에서 똑같이 깁니다. 클러스터는 키를 나누는 것이지 키 하나를 나누지 않습니다. (본문: "데이터와 요청이 늘면 무엇이 달라지는가")

**Q. 긴 조회를 복제본에서 실행하면 되지 않나요?**
주 서버의 다른 요청은 기다리지 않게 됩니다. 대신 복제본도 메인 스레드 하나로 주 서버의 쓰기를 적용하므로, 긴 조회가 도는 동안 복제가 밀리고 그 복제본에서 읽는 다른 요청도 기다립니다. 긴 조회 전용 복제본을 따로 두는 것이 낫지만, 근본 대책은 명령을 바꾸는 것입니다.

**Q. Spring에서 Redis 타임아웃이 나서 재시도를 붙였습니다. 괜찮은가요?**
원인이 긴 명령이라면 오히려 나빠질 수 있습니다. Lettuce의 타임아웃은 클라이언트 쪽에서 기다리기를 그만둘 뿐 서버의 명령을 취소하지 않습니다. 원인 명령을 보낸 쪽이 재시도하면 같은 긴 명령이 다시 실행되고, 기다리기만 한 쪽이 재시도하면 이미 밀린 줄에 요청을 더 얹습니다. 재시도 전에 원인 명령을 찾아 바꿔야 합니다. 또 기본 타임아웃 60초는 요청 스레드를 오래 묶으므로, 서비스의 응답 시간 목표에 맞는 값으로 정합니다. (본문: "Spring에서 확인할 것")

- **꼬리: 원인 연결을 `CLIENT KILL`로 끊으면 바로 풀리나요?**
  이미 실행 중인 명령은 끝까지 돕니다. 끊는 처리도 메인 스레드가 그 명령을 끝낸 뒤에야 할 수 있기 때문입니다. 끊는 것은 같은 명령이 다시 오지 않게 하는 효과만 있습니다. 긴 Lua 스크립트는 아직 쓰기를 하지 않았다면 `SCRIPT KILL`로 멈출 수 있습니다.
