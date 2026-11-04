# Redis 복제: 복제본은 주 서버를 어떻게 따라가고, 무엇을 놓치는가

> **핵심 질문**: 복제본은 주 서버와 같은 데이터를 어떻게 갖게 되는가? 연결이 끊겼다 다시 붙으면 무엇을 다시 받으며, 처음부터 다시 받아야 할 때 주 서버와 복제본은 무엇을 치르는가? 복제가 비동기라는 것은 서비스에 어떤 결과(늦은 읽기, 장애 조치 때 잃는 쓰기)를 남기며, 개발자는 무엇을 선택하고 관찰해야 하는가?
>
> **범위**: 주 서버 하나와 복제본으로 이루어진 Redis 기본 복제를 다룹니다. Sentinel과 Redis Cluster는 "장애가 나면 복제본 하나를 새 주 서버로 올린다"는 결과만 쓰고, 장애를 판정하고 복제본을 고르는 절차는 다루지 않습니다. Sentinel의 판정·승격 절차와 그때 Spring 앱이 겪는 일은 [Redis 장애 조치와 클라이언트](./failover-client.md)에서 다룹니다. 복제본을 다시 복제하는 연쇄 구성, 복제 연결의 TLS·인증, 모듈이 복제에 끼치는 영향도 다루지 않습니다. 운영체제 동작은 Linux를 기준으로 합니다.

## 배경 지식

### 이 문서의 예시: 재고 10개, 주문 3건, 서버 두 대

[Redis 영속성](./persistence.md)의 재고 장면을 서버 두 대로 넓혀 씁니다. 문서 끝까지 이 장면 하나를 씁니다.

- 주문 API가 쓰는 Redis는 **주 서버 M** 하나와 **복제본 R** 하나입니다. 주문 API는 M에만 씁니다.
- 처음 상태에서 M과 R 모두 `stock:item:1 = 10`이고, 둘 다 복제 흐름의 1000번째 바이트까지 같은 내용을 가졌다고 합시다(이 "몇 번째 바이트"가 뒤에서 설명할 **오프셋**입니다. 1000은 계산을 쉽게 하려고 정한 가상의 값입니다).
- 주문 A, B, C가 차례로 `DECR stock:item:1`을 보내 재고가 9, 8, 7이 됩니다. 셋 다 "주문 완료" 응답을 받았습니다.

| 시각(가상) | 일어난 일 | M의 재고 | M의 오프셋 | R의 재고 | R이 처리한 오프셋 |
| --- | --- | --- | --- | --- | --- |
| 10:00:00.0 | 시작 | 10 | 1000 | 10 | 1000 |
| 10:00:01.2 | 주문 A: `DECR` → 9, 응답 | 9 | 1033 | 10 → 곧 9 | 1000 → 곧 1033 |
| 10:00:01.7 | 주문 B: `DECR` → 8, 응답 | 8 | 1066 | 9 → 곧 8 | 1033 → 곧 1066 |
| 10:00:02.4 | 주문 C: `DECR` → 7, 응답 | 7 | 1099 | **8** | **1066** |

- `DECR stock:item:1` 하나가 복제 흐름에서 33바이트를 차지합니다. Redis가 명령을 주고받는 형식(RESP)으로 적으면 `*2\r\n$4\r\nDECR\r\n$12\r\nstock:item:1\r\n`이고, 각 부분이 4 + 4 + 6 + 5 + 14바이트입니다. 그래서 오프셋이 33씩 늘어납니다. 데이터베이스 번호를 바꾸는 `SELECT`는 이미 보냈다고 가정해 셈에서 뺐습니다.
- 마지막 줄은 "C가 응답을 받은 순간, R에는 아직 C의 `DECR`이 닿지 않았다"는 장면입니다. 이 한 줄이 문서 전체의 질문을 만듭니다. 이 순간 R에서 재고를 읽으면 얼마가 나오는가? 이 순간 M이 사라지고 R이 새 주 서버가 되면 재고는 얼마인가?

```text
 주문 API                주 서버 M (재고 7, 오프셋 1099)                  복제본 R (재고 8, 오프셋 1066)
 ─────────              ──────────────────────────────────              ────────────────────────────────
 DECR ────①──────────▶  명령 실행
       ◀───②─ 응답 7     복제 버퍼에 C의 DECR(33바이트) 덧붙임
                         ────────────③ 복제 연결로 전송 ──────────────▶  (아직 도착 전)
                         ◀───────────④ "1066까지 처리함"(1초마다) ─────
```

화살표는 데이터(바이트)가 이동하는 방향이고, 번호는 이 장면에서 일어난 순서의 한 예입니다. ②의 응답이 ③보다 먼저일 수 있다는 점, 그리고 ④가 ③과 따로 1초마다 온다는 점이 이 문서의 핵심입니다.

### 프로세스, 소켓, 연결

- **한 줄**: Redis 서버 하나는 운영체제 위의 프로세스 하나이고, 프로세스끼리는 서로의 메모리를 볼 수 없어 소켓으로 메시지를 주고받습니다. 원리는 [프로세스와 스레드](../cs/process-and-thread.md)에서 다룹니다.
- **이 주제에서는**: M과 R은 서로 다른 프로세스(보통 서로 다른 서버)입니다. R이 M의 데이터를 갖는 방법은 M이 소켓으로 보내 주는 바이트를 R이 받아 자기 메모리에 반영하는 것뿐입니다. 이 문서에서 **복제 연결**은 R이 M에 연 소켓 연결을 말합니다. M 쪽에서 보면 R은 이 연결로 붙은 특별한 클라이언트 하나입니다.

### fork와 copy-on-write

- **한 줄**: `fork`는 프로세스를 복제해 자식 프로세스를 만들고, 자식은 `fork` 순간의 메모리를 봅니다. Linux는 메모리를 페이지라는 고정 크기 단위로 관리하는데, Redis 데이터처럼 프로세스가 혼자 쓰던 일반 메모리는 `fork` 때 바로 복사하지 않고, 아직 부모와 자식이 함께 쓰는 페이지에 어느 한쪽이 처음 쓸 때 그 페이지만 복사합니다. 대가는 둘입니다. `fork` 자체도 "어느 페이지가 어디 있는가"를 적은 표(페이지 테이블)는 복사하므로 대체로 쓰는 메모리가 클수록 오래 걸리고, 자식이 사는 동안 바뀐 페이지만큼 메모리가 늘어납니다. 1바이트만 바꿔도 그 페이지 전체가 복사됩니다. 원리는 [fork와 copy-on-write](../cs/fork-and-copy-on-write.md)에서 다룹니다.
- **이 주제에서는**: R이 처음 붙거나 너무 오래 끊겨 있었으면, M은 `fork`한 자식에게 그 순간의 전체 데이터를 RDB 형식으로 만들게 해 R에 보냅니다. RDB는 Redis가 메모리의 데이터 전체를 한 시점으로 떠 둔 파일 형식입니다([Redis 영속성](./persistence.md)). 복제본을 붙이는 일이 영속성 설정과 상관없이 `fork`의 대가를 부를 수 있다는 뜻입니다.

### 동기와 비동기: 응답하기 전에 누구를 기다리는가

- **한 줄**: 동기 복제는 쓰기를 복제본이 받았다는 확인을 받은 뒤 클라이언트에 응답하고, 비동기 복제는 그 확인을 기다리지 않고 응답합니다.
- **비유**: 은행 창구 직원이 입금을 처리하고 장부 사본을 다른 지점에 팩스로 보낸다고 합시다. 동기는 "다른 지점이 사본을 받았다"는 전화를 받은 뒤 고객에게 영수증을 주고, 비동기는 영수증을 먼저 주고 팩스는 따로 보냅니다. 비동기면 고객은 빨리 떠나지만, 팩스가 가는 도중 이 지점에 불이 나면 다른 지점의 장부에는 그 입금이 없습니다.
- **실제로는**:
  - Redis 복제는 기본으로 비동기입니다. M은 명령을 실행하고 응답을 보내며, R이 그 명령을 받았는지 매번 기다리지 않습니다(공식 문서 replication, "Important facts about Redis replication").
  - 비유와 달리 "응답이 먼저, 복제가 나중"이라는 순서가 정해져 있지는 않습니다. 응답과 복제 흐름은 둘 다 먼저 메모리 버퍼에 담겼다가 각자의 소켓으로 나가며, 어느 바이트가 먼저 상대에게 닿는지는 정해져 있지 않습니다. 비동기의 핵심은 순서가 아니라 **응답이 복제본의 확인을 기다리지 않는다**는 점입니다.
  - 대신 R은 "어디까지 처리했는지"를 따로, 주기적으로 M에 알립니다. 그래서 M은 기다리지는 않지만 나중에 확인할 수는 있고, 이것으로 필요한 쓰기에만 "확인될 때까지 기다리기"(`WAIT`)를 붙일 수 있습니다. 이 부분은 "장애 조치 때 무엇을 잃는가"에서 다룹니다.
  - 비유와 다른 점: 팩스는 보낼 때마다 따로 가지만, Redis는 모든 쓰기를 한 줄의 흐름으로 순서대로 보냅니다. 그래서 R은 "어떤 쓰기는 받고 그 앞의 쓰기는 못 받은" 상태가 되지 않습니다. 뒤쪽 일부만 아직 못 받은 상태가 될 뿐입니다.
  - Kotlin 코드에 빗대면, 동기는 `repository.save()`가 끝난 뒤 응답하는 것이고, 비동기는 저장 작업을 다른 스레드에 넘기고(`executor.submit { ... }`) 바로 응답하는 것과 비슷합니다. 다른 점은, Redis에서는 넘긴 작업이 다른 **서버**에서 실행되므로 이쪽 서버가 죽어도 이미 보낸 바이트는 저쪽에 남을 수 있고, 아직 보내지 못한 바이트는 이쪽과 함께 사라진다는 것입니다.
- **이 주제에서는**: 예시의 마지막 줄이 비동기의 결과입니다. C는 응답(재고 7)을 받았지만 R은 아직 8입니다. 이 차이를 **복제 지연**이라고 하고, 이 문서는 이 차이가 서비스에 무엇을 남기는지 따라갑니다.

### 이 문서에 나오는 실행 단위

복제에는 Redis의 여러 실행 단위가 나오므로 구분해 둡니다. 실행 단위의 일반 원리는 [프로세스와 스레드](../cs/process-and-thread.md)에서 다룹니다.

| 실행 단위 | 복제에서 하는 일 |
| --- | --- |
| M의 메인 스레드 | 명령을 실행하고, 실행한 쓰기를 복제 버퍼에 덧붙이고, 복제 연결로 보냅니다. 복제본이 보낸 "어디까지 처리했다"를 받아 기록합니다. 전체 동기화가 필요하면 `fork`를 부릅니다 |
| M의 자식 프로세스 | 전체 동기화 때 `fork` 순간의 데이터를 RDB 형식으로 만들어 복제본에 보냅니다 |
| R의 메인 스레드 | M이 보낸 명령을 받아 실행하고, 1초마다 "어디까지 처리했다"를 보냅니다. 전체 동기화 때는 옛 데이터를 비우고 받은 RDB를 읽습니다 |
| I/O 스레드 | `io-threads`를 2 이상으로 켜도 복제 연결은 맡지 않습니다. 복제본 연결과 주 서버 연결은 메인 스레드에 남습니다 |

## 복제는 무엇을 해결하고, 무엇을 해결하지 않는가

Redis 한 대만 있으면 그 서버가 사라질 때 데이터와 서비스가 함께 사라집니다. 영속성(RDB: 전체 데이터를 한 시점으로 떠 둔 파일, AOF: 실행한 쓰기 명령을 차례로 덧붙인 파일)은 프로세스나 전원 장애 뒤 같은 서버에서 다시 시작할 때를 대비하지만, 디스크나 서버 자체를 잃으면 소용이 없습니다([Redis 영속성](./persistence.md)). 복제는 이 빈자리를 채웁니다.

| 복제가 주는 것 | 예시에서 | 대가와 조건 |
| --- | --- | --- |
| 다른 서버에 있는 사본 | M의 서버가 통째로 사라져도 R에 재고가 남습니다 | 비동기라 마지막 일부(예시에서 C)는 없을 수 있습니다 |
| 장애 조치의 바탕 | Sentinel이나 Cluster가 R을 새 주 서버로 올릴 수 있습니다 | 올리는 판단과 절차는 복제가 아니라 그 도구가 맡습니다 |
| 읽기 분산 | 재고 조회를 R에 보내 M의 부담을 덜 수 있습니다 | R의 값은 M보다 늦을 수 있습니다 |

반대로 복제가 해결하지 **않는** 것도 처음부터 분명히 해 둡니다.

- **사람의 실수**: 복제본은 주 서버를 "그대로" 따라가므로, `FLUSHALL` 같은 실수도 그대로 따라 합니다. 공식 문서는 복제본이 주 서버에 무슨 일이 생기든 정확한 사본이 되려 한다고 설명합니다(공식 문서 replication 첫 문단). 실수 대비는 서버 밖 백업이 맡습니다([Redis 영속성](./persistence.md)의 "사람의 실수와 백업").
- **응답한 쓰기의 보존**: 비동기라 응답한 쓰기도 복제본에 없을 수 있습니다. 뒤의 "장애 조치 때 무엇을 잃는가"에서 다룹니다.
- **쓰기 처리량**: 쓰기는 여전히 주 서버 한 대가 모두 받습니다. 복제본을 늘리면 오히려 주 서버가 보낼 곳이 늘어납니다. 쓰기를 여러 서버로 나누는 것은 Redis Cluster의 샤딩이며 이 문서의 범위 밖입니다. 언제 클러스터로 가야 하는지는 [클러스터 도입 판단](./cluster-adoption.md)에서 다룹니다.

## 연결된 동안: 주 서버는 실행한 쓰기를 하나의 흐름으로 보냅니다

### 무엇을 보내는가: 복제본에서 같은 결과가 나오는 명령

M은 데이터를 바꾼 명령을 실행한 뒤, 그 명령을 Redis 프로토콜 형식 그대로 복제 흐름에 덧붙여 R에 보냅니다. R은 받은 명령을 자기 메모리에서 다시 실행합니다(공식 문서 replication, "This system works using three main mechanisms"의 1번). 예시에서 M이 C의 `DECR`을 실행하면, R에도 `DECR stock:item:1`이 그대로 갑니다.

"받은 명령을 다시 실행한다"가 성립하려면 같은 명령이 M과 R에서 같은 결과를 내야 합니다. 그렇지 않은 명령은 M이 결과가 확정된 다른 명령으로 바꿔 보냅니다.

| M에서 실행한 명령 | 복제 흐름에 실리는 명령 | 바꾸는 이유 |
| --- | --- | --- |
| `EXPIRE key 60`(지금부터 60초) | `PEXPIREAT key <절대 시각>` | R이 받는 시점의 "지금"은 M과 다릅니다 |
| Lua 스크립트 | 스크립트가 실행한 쓰기 명령들(둘 이상이면 `MULTI`/`EXEC`로 감쌈. `MULTI`와 `EXEC` 사이의 명령은 다른 명령이 끼어들지 않고 한 번에 실행되므로, R에서도 스크립트의 쓰기가 반쯤만 반영된 상태가 보이지 않습니다) | 스크립트 안의 판단을 R에서 다시 하지 않습니다([Redis 원자성](./atomicity.md)의 "스크립트의 쓰기는 복제본에도 한 덩어리로 전달됩니다") |

클라이언트가 보내지 않은 변경도 흐름에 실립니다. M에서 키가 만료되거나 메모리 한도 때문에 쫓겨나면(eviction), M이 `DEL`을 만들어 R에 보냅니다(공식 문서 replication, "How Redis replication deals with expires on keys"). 기본 설정 기준이며, `lazyfree-lazy-expire`나 `lazyfree-lazy-eviction`을 켜면 `UNLINK`로 보냅니다. R이 스스로 키를 지우지 않는 이유는 "복제본에서 읽으면"에서 다룹니다.

### 오프셋: 흐름의 몇 번째 바이트까지 처리했는가

M은 복제 흐름에 보낸 바이트 수를 계속 셉니다. 이 누적 바이트 수가 **복제 오프셋**입니다. R도 자기가 받아 처리한 바이트 수를 같은 방식으로 셉니다(공식 문서 replication, "How Redis replication works").

- **오프셋은 명령 수가 아니라 바이트 수입니다.** 예시에서 `DECR` 세 번은 오프셋을 3이 아니라 99(33 × 3) 늘립니다. 값이 큰 `SET` 하나는 오프셋을 수천 바이트 늘릴 수 있습니다. 그래서 "오프셋 차이 33"은 "명령 33개가 밀렸다"가 아니라 "33바이트가 밀렸다"이고, 그것이 몇 개의 명령인지는 명령 크기에 달렸습니다.
- **복제본이 끊겨 있어도 늘어납니다.** 복제본이 모두 끊겨도 백로그(다음 절)가 남아 있는 동안은 흐름을 백로그에 계속 쌓으며 바이트만큼 오프셋을 올립니다. 백로그까지 풀린 뒤에는 흐름을 만들지 않으므로, 그때의 오프셋 변화는 복제 바이트 수가 아닙니다.
- 주 서버가 연결이 조용할 때 보내는 `PING`도 흐름에 실리므로 오프셋을 올립니다. M은 `repl-ping-replica-period`(기본 10초) 동안 흐름에 아무것도 보내지 않았으면 `PING`을 보냅니다. 그래서 쓰기가 없어도 오프셋은 조금씩 늘 수 있습니다.

### 복제본은 1초마다 어디까지 처리했는지 알립니다

R은 1초마다 `REPLCONF ACK <처리한 오프셋>`을 M에 보냅니다. 여기서 오프셋은 R이 **받아서 실행까지 마친** 위치입니다. 디스크에 남겼는지와는 상관이 없습니다.

M은 이 보고를 두 가지에 씁니다.

1. 오래 보고가 없는 복제본을 끊긴 것으로 판단합니다(`repl-timeout`, 기본 60초).
2. `WAIT`처럼 "몇 대가 이 쓰기를 처리했는가"를 확인하는 기능과, 최근에 보고한 복제본 수를 세는 `min-replicas-*`의 근거로 씁니다.

끊겼다 다시 붙을 때 어디부터 다시 보낼지는 이 보고가 아니라, 복제본이 다시 붙으며 보내는 자기 오프셋으로 정합니다("끊겼다 다시 붙으면"에서 다룹니다).

예시의 10:00:02.4에 M이 아는 R의 위치는 "마지막 보고 시점의 오프셋"입니다. R이 실제로는 1066까지 처리했어도, 마지막 보고가 10:00:01.5에 1033이었다면 M은 아직 1033으로 압니다. 보고는 따라가는 속도를 늦추지 않습니다. 보고와 상관없이 R은 받는 대로 처리합니다.

**혼동 주의: `INFO replication`의 `lag`는 밀린 데이터의 양이 아닙니다.** M의 `INFO replication`은 복제본마다 `slave0:...,offset=1033,lag=1`처럼 보여 주는데, `lag`는 **마지막 보고를 받은 지 몇 초가 지났는가**입니다. 보고가 1초마다 오므로 정상이면 0이나 1입니다. 복제본이 얼마나 뒤처졌는지는 M의 `master_repl_offset`과 그 복제본의 `offset`을 빼서 **바이트**로 봅니다. 예시에서 마지막 보고가 1033이었다면 1099 − 1033 = 66바이트, `DECR` 두 개 분량입니다.

### 기다리지 않고 응답합니다

M은 C의 `DECR`을 실행하면서 응답을 응답 버퍼에 담고, 명령이 끝나면 같은 명령을 복제 버퍼에 덧붙입니다. 둘은 각자의 버퍼를 거쳐 각자의 소켓으로 나갑니다. 소켓에 쓴 뒤에도 바이트가 네트워크를 건너 R에 도착하고 R이 실행하기까지 시간이 더 걸립니다. M은 이 중 어느 것도 기다리지 않습니다. 그래서 쓰기 직후에는 R이 잠시 뒤처지고(따라잡고 나면 오프셋 차이가 0인 순간도 있습니다), R이 바쁘거나 네트워크가 막히면 뒤처지는 시간이 길어집니다. 정상일 때 얼마나 짧은지는 네트워크와 부하에 달려 있어 이 문서에서 재지 않았습니다.

이 설계를 고른 이유는 지연입니다. 공식 문서는 비동기 복제를 지연이 낮고 성능이 좋아 대부분의 Redis 쓰임에 맞는 방식이라고 설명합니다(공식 문서 replication). 동기였다면 예시의 모든 `DECR`이 R까지의 왕복을 더 기다렸을 것이고, R이 느려지거나 끊기면 M의 쓰기 전체가 멈췄을 것입니다. 비동기의 대가는 "응답했지만 복제본에는 없는 쓰기"가 항상 조금 있을 수 있다는 점이고, 이것이 이 문서 뒤쪽의 주제입니다.

## 끊겼다 다시 붙으면: 놓친 만큼만 받는 부분 재동기화

네트워크가 잠깐 끊기거나 R이 잠깐 멈추면 복제 연결이 끊깁니다. R은 사람이 개입하지 않아도 스스로 다시 연결합니다. 다시 붙을 때 R이 원하는 것은 "놓친 부분만" 받는 것입니다. 이것이 **부분 재동기화**입니다.

### 복제 ID와 오프셋 한 쌍이 "어느 역사의 어디까지"를 가리킵니다

오프셋만으로는 부족합니다. 서로 다른 주 서버의 오프셋 1066은 서로 다른 데이터를 가리키기 때문입니다. 그래서 주 서버마다 **복제 ID**가 있습니다. 복제 ID는 무작위로 만든 긴 문자열로, "이 데이터가 겪어 온 변경의 역사 하나"에 붙은 이름입니다. 공식 문서는 (복제 ID, 오프셋) 한 쌍이 그 주 서버 데이터의 정확한 한 버전을 가리킨다고 설명합니다(공식 문서 replication, "How Redis replication works", "Replication ID explained").

- 주 서버가 처음부터(빈 상태나 영속성 파일로) 다시 시작해 새 역사를 시작하거나, 복제본이 주 서버로 올라가면 새 복제 ID를 만듭니다.
- 복제본은 주 서버에 붙을 때 그 주 서버의 복제 ID를 이어받습니다. 예시에서 R의 복제 ID는 M과 같습니다.

### 백로그: 주 서버는 최근에 보낸 흐름을 정해 둔 크기만큼 들고 있습니다

R이 다시 붙으면 `PSYNC <복제 ID> <다음에 받을 오프셋>`을 보냅니다. M이 그 위치부터의 흐름을 아직 가지고 있으면 `+CONTINUE`로 답하고 그 뒤를 보냅니다. M이 최근 흐름을 들고 있는 곳이 **복제 백로그**입니다.

- **한 줄**: 복제 백로그는 주 서버가 최근에 보낸 복제 흐름을 정해 둔 크기만큼 메모리에 남겨 두는 버퍼입니다(흐름이 그만큼 쌓이기 전에는 쌓인 만큼만 있습니다). 크기를 넘으면 가장 오래된 부분부터 버립니다.
- **비유**: 단체 대화방이 최근 1,000개 메시지만 보관한다고 합시다. 잠깐 자리를 비운 사람은 놓친 메시지를 스크롤해서 읽으면 되지만, 1,000개 넘게 쌓인 뒤 돌아오면 앞부분이 이미 지워져 처음부터 다시 받아야 합니다.
- **실제로는**:
  - 크기는 `repl-backlog-size`이고 기본값은 1MB입니다. 메시지 개수가 아니라 바이트로 정합니다.
  - 보통 첫 복제본이 붙을 때 만들어지고(RDB로 다시 시작하는 주 서버는 읽는 동안 미리 만들기도 합니다), 주 서버에 복제본이 하나도 없는 상태가 `repl-backlog-ttl`(기본 3600초) 동안 이어지면 풀어 줍니다. 복제본은 나중에 주 서버로 올라갈 수 있으므로 백로그를 시간이 지났다고 풀지 않습니다.
  - 비유와 다른 점: 대화방은 메시지마다 지우지만, 백로그는 수 KiB 이상의 블록 단위로 앞을 잘라 내고, 남은 양이 설정 크기보다 작아지게는 자르지 않습니다. 연결된 복제본이 아직 보내지 못한 블록도 자르지 않으므로 실제로 들고 있는 양은 설정보다 클 수 있습니다. 그래서 다시 붙을 때의 판단은 설정 크기가 아니라 "요청한 복제 ID가 현재 ID이거나 유효 범위 안의 두 번째 ID이고(두 번째 ID는 복제본이 주 서버로 올라갈 때 기억해 두는 옛 주 서버의 ID로, 그 ID로 이어 줄 수 있는 오프셋에 상한이 있습니다. 자세한 것은 "장애 조치 때 무엇을 잃는가"에서 다룹니다), 요청한 오프셋이 백로그가 실제로 들고 있는 범위 안인가"입니다.
- **이 주제에서는**: R이 오프셋 1033까지 처리한 직후(A까지 받음) 연결이 끊겼고, 그동안 B와 C가 실행됐다고 합시다. R은 다시 붙으며 "M의 복제 ID, 1034부터"를 요청합니다. M의 백로그는 1MB를 들고 있고 그사이 쌓인 흐름은 66바이트뿐이므로 범위 안입니다. M은 `+CONTINUE`와 함께 B와 C의 `DECR` 66바이트를 보내고, R은 재고 7, 오프셋 1099가 됩니다. 데이터 전체를 다시 보내지 않습니다.

### 버티는 시간은 백로그 크기 ÷ 복제 흐름 속도입니다

백로그가 지켜 주는 것은 "몇 초"가 아니라 "몇 바이트"입니다. 끊긴 동안 쌓인 흐름이 백로그 크기를 넘으면, 다시 붙어도 놓친 앞부분이 이미 지워져 부분 재동기화를 할 수 없습니다.

가상 예시로 계산해 봅니다. 주문이 몰려 `DECR`가 초당 1,000번 실행되면 복제 흐름은 초당 33,000바이트(약 32KiB)입니다. 기본 백로그 1MB(1,048,576바이트)는 약 32초 분량입니다. 이 계산은 끊기기 직전에 복제본이 따라잡아 있었고 흐름 속도가 일정하다는 전제의 용량 환산입니다. 끊기기 전에 이미 0.5MiB 뒤처져 있었다면 같은 속도에서 더 버틸 수 있는 시간은 약 16초뿐입니다. 같은 백로그라도 초당 10MB를 쓰는 서비스라면 0.1초 분량입니다. 그래서 백로그 크기는 "우리 서비스의 복제 흐름 속도 × 견디고 싶은 끊김 시간"으로 정합니다. 흐름 속도는 `master_repl_offset`의 증가 속도로 잴 수 있습니다.

### 부분 재동기화를 할 수 없는 경우

| 경우 | 왜 안 되는가 |
| --- | --- |
| 처음 붙는 복제본 | 받은 것이 없으므로 이어받을 위치가 없습니다(공식 문서 replication) |
| 끊긴 동안의 흐름이 백로그보다 큼 | 놓친 앞부분이 지워졌습니다 |
| 복제본이 요청한 복제 ID가 주 서버의 현재 ID도, 유효 범위 안의 두 번째 ID도 아님(예: 주 서버가 영속성 없이 재시작해 새 역사 시작) | 복제본이 아는 역사와 다른 역사입니다 |
| 복제본이 AOF를 켠 채 재시작 | AOF로 데이터를 읽을 때는 오프셋 숫자만 맞추고, 이어받기에 필요한 복제 ID와 주 서버 정보는 되살리지 않습니다(공식 문서 replication, "Partial sync after restarts and failovers") |

반대로, 복제본이 RDB로 저장하고 내려갔다가 다시 시작하면 RDB 안에 복제 ID와 오프셋이 함께 저장되어 있어 부분 재동기화를 시도할 수 있습니다. 공식 문서는 업그레이드처럼 복제본을 일부러 내릴 때 `SHUTDOWN`으로 저장하고 내리라고 권합니다(공식 문서 같은 절).

## 따라잡을 수 없으면: 처음부터 다시 받는 전체 동기화

부분 재동기화를 할 수 없으면 R은 데이터 전체를 처음부터 받습니다. 이것이 **전체 동기화**입니다. 복제에서 가장 비싼 일이므로 과정과 비용을 나눠 봅니다.

### 과정: 한 시점의 전체 사본과, 그 뒤의 변경을 함께 보냅니다

전체 동기화가 풀어야 하는 문제는 [fork와 copy-on-write](../cs/fork-and-copy-on-write.md)가 다루는 문제와 같습니다. M은 요청을 계속 받으면서 전체 데이터의 일관된 사본을 R에 보내야 합니다. 그래서 두 가지를 보냅니다.

1. `fork` 순간의 전체 데이터(RDB 형식)
2. `fork` 이후에 실행된 쓰기 흐름

R이 1을 읽고 2를 이어서 실행하면, M과 같은 데이터가 됩니다. 기본 설정에서의 순서는 다음과 같습니다.

```text
  주 서버 M                                                     복제본 R
  ─────────                                                     ─────────
  ① R의 PSYNC 요청을 받았지만 이어받을 수 없음
  ② 다른 복제본이 붙기를 5초 기다림(repl-diskless-sync-delay. 다른 자식 프로세스가 일하는 중이면 더 늦어짐)
  ③ fork → 자식이 RDB를 만들어 소켓으로 바로 전송 ── RDB ──▶  임시 파일에 받아 디스크에 씀
                                                               (받는 동안 옛 데이터로 읽기 응답)
  ④ 메인 스레드는 그동안의 쓰기 흐름을 보냄 ── 흐름 ──▶       받은 흐름을 메모리에 쌓아 둠
                                                            ⑤ 옛 데이터를 비우고 RDB를 읽음
                                                               (이 동안 LOADING 오류로 응답)
                                                            ⑥ 쌓아 둔 흐름을 실행 → 연결 완료
```

번호는 일어나는 순서이고, 화살표는 데이터가 이동하는 방향입니다. ③과 ④는 동시에 일어납니다. 복제본이 연결할 때 거치는 인증과 기능 교환(`AUTH`, `REPLCONF`) 단계는 이 문서의 판단에 영향을 주지 않아 그림에서 뺐습니다. 각 단계의 근거는 다음과 같습니다.

- **②③ 디스크를 거치지 않는 전송**: `repl-diskless-sync` 기본값 `yes`에서 M의 자식은 RDB를 디스크 파일로 쓰지 않고 복제본에 바로 보냅니다. 전송이 시작되면 그 뒤에 온 복제본은 다음 전송을 기다려야 하므로, 여러 대가 함께 받도록 `repl-diskless-sync-delay`(기본 5초)만큼 기다렸다가 시작합니다. 그래서 기본 설정에서 전체 동기화는 최소 몇 초 늦게 시작합니다. `repl-diskless-sync-max-replicas`를 정해 두면 그 수만큼 모였을 때 일찍 시작합니다.
- **③④ 두 연결로 동시에 보내기**: 전체 동기화 때 복제본이 연결을 하나 더 엽니다. 새 연결(RDB 채널)로는 M의 자식이 RDB를 보내고, 원래 연결로는 M의 메인 스레드가 `fork` 이후의 쓰기 흐름을 동시에 보냅니다. R은 RDB를 읽는 동안 흐름을 자기 메모리에 쌓아 두었다가 다 읽은 뒤 실행합니다. 이 방식은 M의 `repl-diskless-sync`가 켜져 있고 양쪽이 기능을 지원할 때 쓰입니다(숨은 설정 `repl-rdb-channel` 기본값 `yes`). 이 방식이 쓰이지 않으면 M이 RDB를 다 보낼 때까지 그동안의 흐름을 자기 쪽 버퍼에 쌓아 둡니다(공식 문서 replication, "How Redis replication works").
- **③ 복제본이 디스크에 먼저 받기**: 복제본의 `repl-diskless-load` 기본값 `disabled`에서는 R이 받은 RDB를 디스크의 임시 파일에 다 받은 뒤 자신의 RDB 파일 이름(`dbfilename`, 기본 `dump.rdb`)으로 바꾸고 나서 읽습니다. 복제본이 영속성을 켜 두었다면 원래 있던 `dump.rdb`가 이때 주 서버에서 받은 것으로 바뀝니다.
- **⑤ 비우고 읽기**: 받은 RDB를 읽기 전에 R은 옛 데이터를 모두 비웁니다. 비우는 동안과 읽는 동안 R은 대부분의 명령에 `LOADING` 오류로 답합니다. 공식 문서도 초기 동기화 뒤 옛 데이터를 지우고 새 데이터를 읽는 동안은 복제본이 연결을 막으며, 큰 데이터면 여러 초가 걸릴 수 있다고 설명합니다(공식 문서 replication, "Important facts about Redis replication").

예시로 돌아오면, R이 처음 붙는 순간 M은 재고 7을 담은 RDB를 만들고, 그 사이 들어온 쓰기(예: 주문 D의 `DECR`)를 흐름으로 따로 보냅니다. R은 RDB를 읽어 재고 7이 된 뒤 쌓아 둔 D의 `DECR`을 실행해 6이 되고, 그 뒤로는 "연결된 동안"의 방식으로 따라갑니다.

### 주 서버가 치르는 비용

전체 동기화는 M에 영속성의 RDB 저장과 같은 비용을 지웁니다. 복제를 위해 영속성을 꺼 두었어도 마찬가지입니다.

| 비용 | 무엇이 늘어나는가 | 누가 언제 치르는가 |
| --- | --- | --- |
| `fork` 시간 | 메모리가 클수록 긴 `fork` 호출 | M의 메인 스레드. 그동안 모든 클라이언트의 명령이 멈춥니다 |
| copy-on-write 메모리 | 자식이 RDB를 보내는 동안 M이 쓴 페이지만큼 | M 서버의 메모리. 쓰기가 많고 흩어져 있으면 두 배 가까이 될 수 있습니다 |
| 네트워크 | 데이터 전체 크기의 전송 | 복제 연결. 같은 네트워크를 쓰는 다른 트래픽에도 영향을 줍니다 |
| 동기화 동안의 흐름 버퍼 | RDB를 보내는 동안 실행된 쓰기 | RDB 채널을 쓰면 주로 R이, 아니면 M이 메모리에 쌓습니다 |

`fork`와 copy-on-write의 원리와 조건은 [fork와 copy-on-write](../cs/fork-and-copy-on-write.md)에 있습니다. 공식 문서는 M이 전체 동기화 중에도 요청 처리를 멈추지 않는다고 설명하는데(공식 문서 replication, "Redis replication is non-blocking on the master side"), 이는 RDB를 만드는 일을 자식이 맡기 때문입니다. `fork` 호출 자체가 메인 스레드를 잠깐 멈추는 것까지 없애 주지는 않습니다.

### 복제본이 치르는 비용: 비우고 읽는 동안은 응답하지 않습니다

R에서 가장 눈에 띄는 비용은 ⑤입니다. 데이터가 크면 옛 데이터를 비우고 새 RDB를 읽는 데 여러 초가 걸리고, 그동안 R에 보낸 읽기는 `LOADING` 오류를 받습니다. 읽기를 R에 나눠 보내던 서비스라면 이 시간 동안 그 읽기가 실패합니다.

- **옛 데이터를 비우는 시간**: `replica-lazy-flush` 기본값 `no`에서는 메인 스레드가 직접 비웁니다. `yes`면 비우기를 백그라운드 스레드에 넘겨 이 시간을 줄일 수 있습니다. 대신 옛 데이터가 다 풀리기 전까지 새 데이터와 메모리를 함께 차지합니다.
- **읽기를 계속 받는 방법**: `repl-diskless-load swapdb`는 소켓에서 바로 읽으면서 옛 데이터를 그대로 두고 다 읽은 뒤에 바꿔 끼웁니다. 같은 복제 역사의 데이터라면 읽는 동안에도 옛 데이터로 응답할 수 있지만, 옛 데이터와 새 데이터를 동시에 메모리에 두므로 메모리가 모자라면 강제 종료될 위험이 있습니다.
- **받는 동안 옛 데이터로 답할지**: ③에서 RDB를 받는 동안, 또는 연결이 끊겨 있는 동안 R이 읽기에 어떻게 답할지는 `replica-serve-stale-data`가 정합니다. 기본값 `yes`면 옛 데이터(처음 동기화라면 빈 데이터)로 답하고, `no`면 `MASTERDOWN` 오류로 답합니다.
- **AOF를 켠 복제본의 디스크**: R이 AOF를 켜 두었다면 ⑤ 직전에 AOF를 끄고, 다 읽은 뒤 AOF를 처음부터 다시 만듭니다. 새 AOF가 완성되기 전까지 디스크의 AOF는 동기화 전 데이터를 가리킵니다. 이 구간의 재시작, `WAITAOF`, 장애 조치는 [AOF를 켠 복제본의 전체 동기화](./replica-aof-full-sync.md)에서 다룹니다.

### 버퍼 한도를 넘으면 끊기고, 처음부터 다시 시작할 수 있습니다

전체 동기화 중 쌓이는 흐름에는 한도가 있습니다.

- M 쪽: 복제본 연결로 보낼 데이터가 `client-output-buffer-limit replica`를 넘으면 M은 그 연결을 끊습니다. 기본값은 `256mb 64mb 60`으로, 256MB를 넘거나 64MB 이상이 60초 동안 이어지면 끊습니다.
- R 쪽(RDB 채널을 쓸 때): R이 쌓아 두는 흐름은 `replica-full-sync-buffer-limit`까지입니다. 기본값 0은 R 자신의 `client-output-buffer-limit replica` 한도(기본 256MB)를 그대로 쓴다는 뜻입니다. 한도에 닿으면 R은 더 읽지 않고, 그만큼 M 쪽 버퍼에 쌓이기 시작합니다.

RDB를 다 받기 전에 연결이 끊기면 R은 다시 붙어 전체 동기화를 처음부터 시작합니다. RDB 채널 방식에서 RDB를 이미 디스크에 다 받았다면, R은 그것을 끝까지 읽은 뒤 확보한 오프셋으로 부분 재동기화를 시도하므로 처음부터 다시 하지 않을 수 있습니다. 데이터가 커서 RDB 전송이 오래 걸리고 그동안의 쓰기가 많으면, RDB 전송 도중 한도를 넘어 끊기고 다시 시작하는 일이 되풀이될 수 있습니다. 매번 M은 `fork`와 copy-on-write를 치르고, R은 끝내 따라잡지 못합니다. 이때 확인할 것은 M의 `INFO stats`의 `sync_full`이 계속 늘어나는지, 로그에 출력 버퍼 한도로 연결을 닫았다는 줄("scheduled to be closed ASAP for overcoming of output buffer limits")이 있는지입니다. 한도를 올리면 끊김은 멈출 수 있지만 그만큼 메모리를 더 쓰므로, 쓰기가 몰리는 시간을 피해 동기화하거나 데이터 크기 자체를 줄이는 방안도 함께 봅니다.

백로그와 복제본 연결의 버퍼는 따로 복사되지 않습니다. Redis는 복제 흐름을 버퍼 하나에 두고 백로그와 각 복제본 연결이 그 안의 위치만 가리킵니다. 그래서 복제본 수만큼 흐름이 복사되지는 않지만, 가장 뒤처진 복제본이 아직 보내지 못한 위치부터 최신까지는 풀 수 없으므로 한 대가 늦으면 버퍼 전체가 그만큼 커집니다. 출력 버퍼 한도도 이 공유 버퍼에서 그 복제본이 아직 보내지 못한 양을 블록 단위로(블록 관리 비용 포함) 계산합니다.

## 복제본에서 읽으면: 늦은 값, 만료, 쓰기 금지

### 방금 쓴 값이 안 보일 수 있습니다

예시에서 주문 C의 API가 `DECR`로 7을 받은 직후, 다른 화면이 재고를 R에서 읽으면 8이 나올 수 있습니다. 같은 사용자가 방금 바꾼 값을 바로 다시 읽는 경우에도 마찬가지입니다. 비동기 복제에서 R의 값은 M보다 **조금 전의 M**입니다. 이 차이가 서비스에서 문제인지는 그 읽기로 무엇을 판단하느냐에 달렸습니다.

| 읽기의 용도 | R에서 읽어도 되는가 | 이유 |
| --- | --- | --- |
| 상품 목록의 "재고 있음" 표시 | 대개 괜찮습니다 | 잠깐 틀려도 주문 단계에서 다시 판단합니다 |
| 재고를 읽고 그 값으로 차감 여부 결정 | 안 됩니다 | 이미 팔린 재고를 남은 것으로 보고 판단합니다. 판단은 M에서, 가능하면 명령 하나나 스크립트로 합니다([Redis 원자성](./atomicity.md)) |
| 방금 저장한 세션·설정을 바로 다시 읽기 | 조심해야 합니다 | 쓴 사람에게 자기 쓰기가 안 보이는 현상이 생깁니다. 그 읽기만 M으로 보냅니다 |

`WAIT`로 복제본이 받을 때까지 기다린 뒤 R에서 읽으면 늦은 값을 피할 수 있을 것 같지만, `WAIT`은 "몇 대가 받았는가"를 셀 뿐 **어느** 복제본인지는 알려 주지 않습니다(공식 문서 wait). 복제본이 여럿이면 읽기가 간 복제본이 그 쓰기를 받았다는 보장이 없습니다. 방금 쓴 값을 읽어야 하는 요청은 M에서 읽는 것이 단순합니다.

### 복제본은 키를 스스로 만료시키지 않습니다

R은 TTL이 지난 키를 스스로 지우지 않고, M이 보내는 `DEL`을 기다립니다. 두 서버의 시계가 정확히 같을 수 없으므로, 지우는 판단을 M 한 곳에 모아 두 서버의 데이터가 갈라지지 않게 하는 것입니다(공식 문서 replication, "How Redis replication deals with expires on keys"). M은 만료된 키를 누군가 접근하거나 주기적으로 훑을 때 지우므로, 그 전까지 R에는 만료 시각이 지난 키가 남아 있을 수 있습니다.

그래도 R에서 읽는 쪽이 지난 값을 보지 않도록, R은 읽기 명령에 한해 자기 시계로 만료 여부를 판정해 "없음"으로 답합니다. 키 자체는 지우지 않습니다. 그래서 다음이 함께 성립합니다.

- R에서 `GET`으로 만료된 키를 읽으면 없다고 나옵니다.
- 하지만 R의 키 개수(`DBSIZE`)와 메모리에는 그 키가 아직 포함될 수 있습니다.
- R의 시계가 M보다 앞서 있으면, M에서는 아직 살아 있는 키를 R은 이미 없다고 답할 수 있습니다(위 판정 방식에서 나오는 추론). 만료 시각은 절대 시각으로 복제되므로(`PEXPIREAT`) 시계 차이가 그대로 판정 차이가 됩니다.
- 복제본은 기본으로 `maxmemory`도 무시하고, 메모리 한도에 따른 쫓아내기도 M의 `DEL`을 따릅니다(`replica-ignore-maxmemory` 기본값 `yes`; 공식 문서 replication, "Maxmemory on replicas"). 그래서 R이 M보다 메모리를 더 쓸 수 있으므로, R 서버의 메모리는 M의 `maxmemory`보다 넉넉히 잡습니다.

### 복제본에는 쓰지 않습니다

`replica-read-only` 기본값 `yes`에서 R은 쓰기 명령을 `READONLY` 오류로 거부합니다. 이 설정을 끄면 R에 쓸 수 있지만 공식 문서는 권하지 않습니다. R에 쓴 키와 M에서 오는 명령이 부딪치면(예: R에서 지운 키에 M의 `INCR`이 도착) 결과가 M과 달라지고, 그 쓰기는 다음 전체 동기화 때 사라지기 때문입니다(공식 문서 replication, "Read-only replica"). 스크립트를 R에서 돌리려면 `EVAL` 대신 읽기 전용인 `EVAL_RO`를 씁니다(같은 절).

## 장애 조치 때 무엇을 잃는가

### 새 주 서버가 가진 것까지가 새 기준입니다

예시의 10:00:02.4 직후 M의 서버가 통째로 사라졌다고 합시다. Sentinel이나 Cluster가 R을 새 주 서버로 올립니다. R의 재고는 **8**입니다. C는 "주문 완료"를 받았지만 그 `DECR`은 R에 닿지 않았으므로, 새 기준에서 재고 하나가 되살아났습니다. 이 재고는 다른 고객에게 한 번 더 팔릴 수 있습니다.

이 손실은 M의 영속성 설정과 상관이 없습니다. M이 `appendfsync always`로 C의 `DECR`을 디스크에 남겼어도, 그 디스크는 새 주 서버가 아닙니다([Redis 영속성](./persistence.md)의 "장애 조치로 응답한 쓰기를 잃는 것은 영속성 설정과 별개입니다"). 공식 문서도 비동기 복제에는 항상 데이터를 잃는 구간이 있다고 밝힙니다(공식 문서 replication, "Allow writes only with N attached replicas").

새 주 서버가 된 R은 두 가지를 합니다(공식 문서 replication, "Replication ID explained").

- 옛 복제 ID(M의 ID)를 **두 번째 복제 ID**로 기억하고, 그 ID로 받아 줄 수 있는 `PSYNC` 요청 위치의 상한을 자기 오프셋 + 1(예시에서 1067)로 적어 둡니다. R은 1066까지 처리했으므로, 옛 ID로 "1067부터"를 요청하는 복제본은 R과 같은 역사를 가졌고 다음 바이트를 원하는 것입니다. 그래서 M의 다른 복제본들이 옛 ID로 `PSYNC`해 와도 부분 재동기화를 해 줄 수 있습니다.
- 자기 이름으로는 새 복제 ID를 만듭니다. 옛 M이 네트워크 분리로 아직 살아서 쓰기를 받고 있을 수 있으므로, 같은 ID와 같은 오프셋이 서로 다른 데이터를 가리키는 일을 막기 위해서입니다.

### 옛 주 서버가 돌아오면 그 쓰기는 버려집니다

옛 M이 프로세스와 메모리를 그대로 가진 채(예: 네트워크 분리가 풀려서) 새 주 서버 R의 복제본으로 붙는다고 합시다. M이 그사이 재시작했다면 어떤 ID로 요청할지는 영속성 파일에 따라 달라지지만, 어느 경우든 R이 모르는 쓰기가 R에 합쳐지지는 않습니다. M은 자기 기록대로 "옛 ID, 1100부터"를 요청합니다. 그런데 R이 옛 ID로 받아 줄 수 있는 것은 1067까지입니다. M은 R이 모르는 33바이트(C의 `DECR`)를 더 가지고 있으므로 역사가 갈라졌고, 부분 재동기화는 거절됩니다. M은 전체 동기화를 받아 R의 데이터(재고 8)로 덮어쓰이고, C의 `DECR`은 어디에도 남지 않습니다.

즉 장애 조치에서 잃은 쓰기는 옛 주 서버가 돌아와도 되살아나지 않습니다. 이 결론은 소스 조건에서 끌어낸 것이며, 장애 조치를 재현해 확인하지는 않았습니다.

### 영속성을 끈 주 서버가 다시 시작하면 복제본까지 비워집니다

같은 "복제본은 주 서버를 따라간다"가 반대 방향으로도 작동합니다. 영속성을 끈 M이 죽고 자동 재시작 도구가 M을 장애 조치보다 먼저 다시 띄우면, M은 빈 상태로 새 복제 ID를 만들어 시작합니다. R은 다시 붙으며 부분 재동기화를 요청하지만 복제 ID가 달라 거절되고, 전체 동기화로 빈 데이터를 받아 자기 데이터까지 비웁니다(공식 문서 replication, "Safety of replication when master has persistence turned off"). 자세한 경로와 권고는 [Redis 영속성](./persistence.md)의 "영속성을 끈 주 서버가 자동으로 다시 시작하면"에서 다룹니다.

영속성을 켰더라도 비슷한 일이 작게 일어날 수 있습니다. M이 RDB 스냅숏으로 다시 시작하면, 스냅숏 이후의 쓰기를 이미 받은 R은 M보다 앞선 오프셋을 요청하게 됩니다. M이 되살린 복제 정보로는 스냅숏 시점까지만 이어 줄 수 있으므로 부분 재동기화가 거절되고, R은 전체 동기화로 M의 더 오래된 데이터를 받습니다. 이 경로도 소스 조건에서 끌어낸 해석이며 재현하지 않았습니다. 주 서버의 재시작이 장애 조치보다 빠를 수 있는 구성이라면, 복제본이 더 최신이어도 주 서버의 시점으로 돌아갈 수 있다고 보고 판단합니다.

### WAIT은 기다려서, min-replicas는 거부해서 잃는 구간을 줄입니다

비동기의 손실 구간을 줄이는 장치가 둘 있습니다. 둘 다 구간을 없애지는 못합니다.

**`WAIT <복제본 수> <제한 시간 밀리초>`**: 이 연결이 지금까지 보낸 쓰기를 지정한 수의 복제본이 처리했다고 보고할 때까지 기다립니다. M은 연결마다 마지막 쓰기의 오프셋을 기억하고, 그 오프셋 이상을 보고한 복제본 수를 셉니다(공식 문서 wait, "Implementation"). 1초 주기 보고를 기다리지 않도록, `WAIT`이 걸리면 M이 복제본들에게 곧바로 보고를 요청합니다. 제한 시간 0은 무한히 기다린다는 뜻이고, `MULTI` 안처럼 기다릴 수 없는 곳에서 보낸 `WAIT`은 기다리지 않고 지금까지 확인된 수를 바로 돌려줍니다(공식 문서 wait의 2·3번 설명).

- 예시에서 주문 C의 API가 `DECR` 뒤 같은 연결로 `WAIT 1 100`을 보내면, R이 1099까지 처리했다고 보고할 때까지 최대 100밀리초 기다립니다. 1이 오면 C의 `DECR`은 R에 있습니다. 0이 오면 R에 없을 수 있다는 뜻이고, 이미 실행된 `DECR`을 되돌려 주지는 않습니다. 앱이 반환값을 요구한 수와 비교해 대응을 정해야 합니다(공식 문서 wait의 4번 설명).
- "처리했다"는 R의 메모리에 반영했다는 뜻입니다. R의 디스크에 남았는지는 따로 `WAITAOF`가 확인합니다([Redis 영속성](./persistence.md)).
- 공식 문서는 `WAIT`이 Redis를 강한 일관성 저장소로 만들지 않는다고 밝힙니다. 이 문서의 맥락에서 그 뜻은 "`WAIT`으로 확인된 쓰기도 장애 조치 뒤 남는다고 보장되지 않는다"입니다. 장애 조치 도구는 가장 나은 복제본을 고르려고 최선을 다할 뿐이어서, 여러 복제본에 전달된 쓰기도 잃을 수 있습니다(공식 문서 wait, "Consistency and WAIT").
- 대가는 그 쓰기의 응답 지연입니다. 주문 확정처럼 잃으면 안 되는 쓰기에만 붙이는 식으로 쓰기별로 고를 수 있습니다.

**`min-replicas-to-write N`과 `min-replicas-max-lag M초`**: 마지막 보고가 M초 이내인 복제본이 N대 미만이면, 주 서버가 쓰기 명령을 `NOREPLICAS` 오류로 거부합니다. 기본값은 `min-replicas-to-write 0`(꺼짐), `min-replicas-max-lag 10`입니다.

- 이 설정은 "복제본과 연락이 끊긴 주 서버가 혼자 쓰기를 계속 받는" 시간을 M초 안팎으로 묶습니다. 예를 들어 네트워크가 갈라져 M이 R과 끊긴 채 클라이언트의 쓰기만 받고 있다면, 그사이 R이 새 주 서버로 올라가면 M이 받은 쓰기는 모두 버려집니다. 이 설정을 켜 두면 M은 보고가 끊긴 지 M초 뒤부터 쓰기를 거부하므로, 버려질 쓰기의 양이 줄어듭니다.
- 공식 문서는 이 기능을 "주어진 쓰기의 일관성은 보장하지 않지만, 잃는 구간을 정해진 초 수로 제한하는 최선의 노력"으로 설명합니다(공식 문서 replication, "Allow writes only with N attached replicas"). 이 설정이 보는 것은 "마지막 보고를 언제 받았나"이지 "보고한 오프셋이 얼마나 최신인가"가 아닙니다. 복제본이 1초마다 보고는 하지만 처리가 크게 뒤처져 있으면 조건은 만족되므로, 잃는 쓰기의 양을 시간으로 묶어 주지는 못합니다. 두 설정 중 하나라도 0이면 꺼집니다.
- 대가는 가용성입니다. 복제본이 모두 내려가거나 점검 중이면 M이 살아 있어도 쓰기가 실패합니다. 예시의 구성(복제본 1대)에서 N을 1로 두면, R을 재시작하는 동안 주문을 받을 수 없습니다.

## 데이터와 쓰기가 늘면 무엇이 커지는가

| 늘어나는 것 | 커지는 비용 | 누가 언제 치르는가 | 판단 |
| --- | --- | --- | --- |
| 데이터 크기 | 전체 동기화의 `fork` 시간, RDB 전송량, 복제본의 비우기·읽기 시간 | M의 메인 스레드(`fork`), 네트워크, R의 메인 스레드(`LOADING` 동안 응답 못 함) | 전체 동기화가 일어나는 횟수 자체를 줄이는 쪽(백로그 크기, 계획된 재시작 때 RDB 저장)이 먼저입니다 |
| 쓰기량(바이트/초) | 같은 백로그가 버티는 시간이 짧아짐, 전체 동기화 중 쌓이는 흐름 | 끊긴 R이 다시 붙을 때, 전체 동기화 동안의 M·R 메모리 | 백로그 = 흐름 속도 × 견딜 끊김 시간. 출력 버퍼 한도를 넘는 동기화 반복을 감시합니다 |
| 큰 값 하나의 쓰기 | 그 값 크기만큼의 흐름이 한 번에 실림 | 네트워크, R의 실행 시간 | 큰 값을 자주 덮어쓰면 작은 변경도 흐름에서는 큽니다(예: 큰 JSON 문자열을 통째로 `SET`). 필드 단위로 바꾸는 자료형을 검토합니다 |
| 복제본 수 | M이 보내는 바이트 수, 동시 전체 동기화 | M의 네트워크, 전체 동기화가 겹칠 때 | 흐름 버퍼는 공유되므로 메모리는 복제본 수에 비례해 늘지 않지만, 가장 늦은 복제본이 버퍼 크기를 정합니다 |
| 읽기량 | 복제본으로 나눌 수 있음 | R | 늦은 값을 허용하는 읽기만 보냅니다. R이 전체 동기화에서 옛 데이터를 비우고 새 데이터를 읽는 동안 그 읽기가 `LOADING`으로 실패할 수 있음을 대비합니다 |

## 흔한 오해 모아 보기

| 오해 | 실제 | 처음 다룬 곳 |
| --- | --- | --- |
| 복제본이 있으면 응답한 쓰기는 잃지 않는다 | 비동기라 응답한 쓰기가 복제본에 없을 수 있고, 장애 조치 뒤 새 주 서버에는 그 쓰기가 없습니다 | 동기와 비동기, 장애 조치 때 무엇을 잃는가 |
| 주 서버에 `appendfsync always`를 켜면 장애 조치에도 안전하다 | 주 서버의 디스크는 새 주 서버가 아닙니다 | 새 주 서버가 가진 것까지가 새 기준입니다 |
| 복제본은 실수로 지운 데이터의 백업이다 | `FLUSHALL`도 그대로 따라 합니다 | 복제는 무엇을 해결하고 |
| 오프셋은 명령 개수다 | 복제 흐름의 누적 바이트 수입니다 | 오프셋: 흐름의 몇 번째 바이트까지 |
| `INFO replication`의 `lag`는 밀린 데이터 양이다 | 마지막 보고 이후 지난 초입니다. 밀린 양은 오프셋 차이(바이트)로 봅니다 | 복제본은 1초마다 |
| 백로그 1MB면 몇 분은 끊겨도 된다 | 버티는 시간은 대략 백로그 크기 ÷ 흐름 속도입니다 | 버티는 시간은 |
| 영속성을 끄면 `fork`도 없다 | 전체 동기화마다 주 서버가 `fork`합니다 | 주 서버가 치르는 비용 |
| 복제본에서 읽으면 주 서버와 같은 값이다 | 조금 전의 값입니다. 방금 쓴 값이 안 보일 수 있습니다 | 방금 쓴 값이 안 보일 수 있습니다 |
| `WAIT`이 1을 돌려주면 내가 읽을 복제본에도 있다 | 몇 대인지만 셀 뿐, 어느 복제본인지는 모릅니다 | 방금 쓴 값이 안 보일 수 있습니다 |
| `WAIT`이 실패하면 쓰기가 취소된다 | 쓰기는 이미 실행됐습니다. 반환값을 보고 앱이 대응합니다 | WAIT은 기다려서 |
| 복제본도 TTL이 지나면 키를 지운다 | 지우지 않고 읽기에만 "없음"으로 답합니다. 개수와 메모리에는 남을 수 있습니다 | 복제본은 키를 스스로 만료시키지 않습니다 |
| 옛 주 서버가 돌아오면 못 넘어간 쓰기가 합쳐진다 | 역사가 갈라져 전체 동기화로 덮어쓰입니다 | 옛 주 서버가 돌아오면 |
| 복제본은 항상 최신이라 주 서버보다 뒤로 가지 않는다 | 주 서버가 더 오래된 데이터로 다시 시작하면 복제본도 그 시점으로 돌아갑니다 | 영속성을 끈 주 서버가 다시 시작하면 |

## 근거

**Redis 8.2.10 소스**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`). 본문에서 버전 없이 적은 소스 파일은 모두 이 태그입니다.

- [`src/replication.c`](https://github.com/redis/redis/blob/8.2.10/src/replication.c): `replicationFeedSlaves`(실행한 쓰기를 복제 흐름에 덧붙임, 복제본이 모두 끊겨도 백로그가 있으면 흐름을 쌓아 오프셋을 올림), `feedReplicationBuffer`(백로그와 복제본이 공유하는 버퍼, 각자 그 안의 위치만 가리킴), `incrementalTrimReplicationBacklog`(백로그를 블록 단위로 앞에서 자르되 설정 크기보다 작아지게 자르지 않고, 복제본이 아직 보내지 못한 블록도 남김), `createReplicationBacklog`, `createReplicationBacklogIfNeeded`(RDB로 다시 시작하는 주 서버는 읽는 동안 백로그를 미리 만듦), `startBgsaveForReplication`(전체 동기화용 RDB 생성 시작), `syncCommand`(부분 재동기화 시도 후 전체 동기화, RDB 채널이면 `+RDBCHANNELSYNC`, 진행 중인 저장에 붙기와 diskless 지연), `masterTryPartialResynchronization`(복제본이 `PSYNC`로 보낸 오프셋으로만 판단하고 주기 보고의 오프셋은 쓰지 않음, 복제 ID·두 번째 ID와 `second_replid_offset`, 백로그 범위 확인, `+CONTINUE`, 거절 경로 "Replication ID mismatch"·"Requested offset for second ID"·"lack of backlog"), `replconfCommand`(`ack`가 보고 오프셋과 보고 시각을 갱신, `capa rdb-channel-repl`은 `repl_diskless_sync`일 때만), `shiftReplicationId`(두 번째 ID와 `master_repl_offset + 1`), 복제본이 `PSYNC`에 `reploff + 1`을 보냄, `readSyncBulkPayload`(임시 파일, `fsync`, `rename`, `swapdb`의 `asyncLoading` 조건), `rdbLoadEmptyDbFunc`(받은 RDB를 읽기 전에 옛 데이터를 비움, `replica-lazy-flush`에 따라 `EMPTYDB_ASYNC`를 고름), `useDisklessLoad`, `replicationCron`(타임아웃, `replicationSendAck`, `repl-ping-replica-period`의 `PING`), `replicationSendAck`(`reploff` 보고), `waitCommand`(그 연결의 쓰기 오프셋 `woff` 이상을 보고한 복제본 수를 셈, `CLIENT_DENY_BLOCKING`이면 기다리지 않음), `replicationRequestAckFromSlaves`(`WAIT`이 걸리면 복제본에 곧바로 보고 요청), `refreshGoodSlavesCount`(`online` 상태 복제본의 보고 시각만 봄), `checkGoodReplicasStatus`(`min-replicas-*` 미달이면 쓰기 거부), "Rdb channel for full sync" 주석(두 연결로 RDB와 흐름을 동시에 보냄, 자식이 소켓에 직접 쓰거나 파이프로 메인 스레드에 넘김), `rdbChannelBufferReplData`(`replica-full-sync-buffer-limit`, 0이면 복제본 출력 버퍼 hard limit), `rdbChannelAbort`(RDB를 디스크에 다 받았으면 끝까지 읽은 뒤 부분 재동기화 시도)
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c): `serverCron`(`replicationCron`을 1000밀리초마다, 장애 조치 중에는 100밀리초마다), `processCommand`(`NOREPLICAS`, `MASTERDOWN`, `LOADING` 거부), `shouldPropagate`(백로그까지 풀리면 흐름을 만들지 않음), `loadDataFromDisk`(AOF로 읽으면 `aof.c`의 `updateReplOffsetAndResetEndOffset`로 오프셋 숫자만 설정하고 복제 ID와 캐시된 주 서버는 되살리지 않음, RDB면 복제본은 캐시된 주 서버, 주 서버는 RDB의 오프셋 + 1까지만 유효한 두 번째 ID로 복원), `genRedisInfoString`(`master_repl_offset`, `slaveN`의 `offset`과 `lag`(`time(NULL) - repl_ack_time`), `sync_full`, `sync_partial_ok`, `sync_partial_err`, `master_link_status`, `repl_backlog_*`), `shared.roslaveerr`, `shared.noreplicaserr`, `commandTimeSnapshot`
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `repl-diskless-sync` `yes`, `repl-rdb-channel` `yes`(숨은 설정), `repl-diskless-sync-delay` 5, `repl-diskless-sync-max-replicas` 0, `repl-diskless-load` `disabled`, `replica-lazy-flush` `no`, `replica-serve-stale-data` `yes`, `replica-read-only` `yes`, `replica-ignore-maxmemory` `yes`, `repl-backlog-size` 1MB, `repl-backlog-ttl` 3600, `repl-timeout` 60, `repl-ping-replica-period` 10, `min-replicas-to-write` 0, `min-replicas-max-lag` 10, `replica-full-sync-buffer-limit` 0, `clientBufferLimitsDefaults`(replica 256MB/64MB/60초)
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `getClientOutputBufferMemoryUsage`(복제본은 공유 버퍼에서 그 복제본이 가리키는 블록부터 끝까지를 블록 관리 비용과 함께 계산), `catClientInfoString`(`CLIENT LIST`의 `omem`이 이 값을 `getClientMemoryUsage`로 받아 보여 줌), `checkClientOutputBufferLimits`, 한도 초과 로그 "scheduled to be closed ASAP for overcoming of output buffer limits"
- [`src/sentinel.c`](https://github.com/redis/redis/blob/8.2.10/src/sentinel.c): `sentinelSendSlaveOf`(역할을 바꿀 때 `SLAVEOF`, `CONFIG REWRITE`와 함께 `CLIENT KILL TYPE normal`·`pubsub`을 한 트랜잭션으로 보냄)
- [`src/rdb.c`](https://github.com/redis/redis/blob/8.2.10/src/rdb.c): `rdbSaveToSlavesSockets`(RDB 채널이면 자식이 복제본 연결들에 직접 쓰고, 아니면 파이프로 부모에 넘김)
- [`src/iothread.c`](https://github.com/redis/redis/blob/8.2.10/src/iothread.c): `isClientMustHandledByMainThread`(주 서버·복제본 연결은 메인 스레드가 처리)
- [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c): `expireIfNeeded`(복제본은 `KEY_EXPIRED`를 돌려주되 지우지 않음, 주 서버가 보낸 명령을 실행할 때는 만료로 보지 않음), `keyIsExpired`(서버 자신의 시각으로 비교), `propagateDeletion`(만료·축출 삭제를 `DEL`로, `lazyfree-lazy-expire`·`lazyfree-lazy-eviction`이면 `UNLINK`로 전파)
- [`src/expire.c`](https://github.com/redis/redis/blob/8.2.10/src/expire.c): `expireGenericCommand`(상대 만료를 `PEXPIREAT`으로 바꿔 전파)
- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): REPLICATION 절(3번 항목: 복제본은 끊기면 스스로 다시 연결; 복제본은 백로그를 시간이 지났다고 풀지 않음; `replica-serve-stale-data`, `repl-diskless-sync`, `repl-diskless-load`, `repl-backlog-size`, `repl-backlog-ttl`, `replica-full-sync-buffer-limit`, `min-replicas-*`), CLIENTS 절의 `client-output-buffer-limit`

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`. 특정 Redis 버전 전용 문서가 아니므로 소스와 다르면 소스를 따릅니다)

- [replication](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/replication.md): 세 가지 동작 방식, 비동기 복제, "Important facts about Redis replication", "Safety of replication when master has persistence turned off", "How Redis replication works", "Replication ID explained", "Read-only replica", "Allow writes only with N attached replicas", "How Redis replication deals with expires on keys", "Partial sync after restarts and failovers", "Maxmemory on replicas"
- [wait](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/wait.md): 반환값, "Consistency and WAIT", "Implementation". "Implementation"은 복제본의 주기 보고가 쓰이는 곳으로 타임아웃 판단, 끊긴 뒤의 부분 재동기화, `WAIT`을 듭니다. 8.2.10 소스에서 부분 재동기화의 위치는 보고가 아니라 `PSYNC` 요청의 오프셋으로 정하므로(위 `masterTryPartialResynchronization`), 본문은 소스를 따랐습니다

**Spring Data Redis와 Lettuce**

- Spring Data Redis 3.5.13 [`LettuceConnectionFactory.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionFactory.java): 설정 종류에 따라 `StaticMasterReplicaConnectionProvider` 또는 `StandaloneConnectionProvider`에 `readFrom`을 넘김, `createSentinelClient`(Sentinel 주소로 `RedisClient` 생성)
- Spring Data Redis 3.5.13 [`StandaloneConnectionProvider.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/StandaloneConnectionProvider.java): `readFrom`이 있으면 `MasterReplica.connect` 후 `setReadFrom`, 없으면 연결한 노드 하나로 보냄
- Spring Data Redis 3.5.13 [`LettuceClientConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClientConfiguration.java): `readFrom(ReadFrom)`
- Spring Data Redis 3.5.13 [`pom.xml`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/pom.xml): Lettuce 6.6.0.RELEASE
- Lettuce 6.6.0.RELEASE [`ReadFrom.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ReadFrom.java): `MASTER`/`UPSTREAM`, `REPLICA_PREFERRED`, `REPLICA`, `ANY` 등
- Lettuce 6.6.0.RELEASE [`MasterReplicaChannelWriter.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/masterreplica/MasterReplicaChannelWriter.java): `getIntent`에서 읽기 전용 명령은 `READ` 의도, 그 밖의 명령과 트랜잭션 안의 명령은 `WRITE` 의도로 보냄
- Lettuce 6.6.0.RELEASE [`RedisClient.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisClient.java): `getSocketAddress`(Sentinel 주소면 `lookupRedis`로 주 서버 주소를 물음)
- Lettuce 6.6.0.RELEASE [`ConnectionWatchdog.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/ConnectionWatchdog.java): 주소 공급자가 실패하면 `onErrorResume`으로 캐시해 둔 마지막 주소를 다시 씀
- Lettuce 6.6.0.RELEASE [`ReconnectionHandler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/ReconnectionHandler.java): 다시 연결할 때마다 주소 공급자(`socketAddressSupplier`)를 새로 구독
- Lettuce 6.6.0.RELEASE [`ReadOnlyCommands.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/ReadOnlyCommands.java): 읽기 전용 명령 목록(`EVAL_RO`는 있고 `EVAL`은 없음)
- Lettuce 6.6.0.RELEASE [`MasterReplica.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/masterreplica/MasterReplica.java): 단독 구성은 연결할 때 한 번 `INFO REPLICATION`으로 구성을 찾고 그 뒤로 고정, 장애 조치나 구성 변경 뒤에는 연결을 다시 만들어야 함. Sentinel 구성은 Sentinel 이벤트로 갱신

## 실무 포인트

### 어떤 구성을 고를까

먼저 **복제를 무엇에 쓰는가**를 정합니다. 목적마다 확인할 것이 다릅니다.

| 목적 | 고를 것 | 남는 위험과 대가 |
| --- | --- | --- |
| 서버 소실 대비(데이터 사본) | 복제본 1대 이상 + 영속성 | 장애 조치 때 복제본에 아직 닿지 않은 쓰기를 잃습니다. 양은 그 순간의 복제 지연에 달려 있어 정해진 상한이 없습니다. 실수(`FLUSHALL`)는 대비하지 못하므로 서버 밖 백업도 둡니다 |
| 주 서버 장애 때 자동 전환 | Sentinel 또는 Cluster | 전환 판단은 그 도구가 합니다. 클라이언트가 새 주 서버를 찾아가야 합니다(아래 Spring 절) |
| 읽기 분산 | 늦은 값을 허용하는 읽기만 복제본으로 | 방금 쓴 값이 안 보일 수 있고, 복제본이 전체 동기화에서 옛 데이터를 비우고 새 데이터를 읽는 동안 그 읽기가 `LOADING`으로 실패할 수 있습니다 |
| 잃으면 안 되는 일부 쓰기 | 그 쓰기에만 `WAIT`(필요하면 `WAITAOF`) | 그 쓰기의 지연이 늘고, 반환값을 확인하는 코드가 필요합니다. 그래도 보장이 아닌 확률 감소입니다 |
| 주 서버가 혼자 쓰기를 받는 시간 제한 | `min-replicas-to-write`, `min-replicas-max-lag` | 복제본이 모두 내려가면 쓰기가 멈춥니다. 복제본 수와 점검 절차를 함께 설계합니다 |

복제 지연이 사업적으로 허용되지 않는 데이터(예: 결제 확정, 재고의 최종 판단)라면, Redis 복제의 설정으로 해결하려 하기보다 원본을 DB에 두고 Redis를 그 앞의 빠른 판단 장치로 쓰는지부터 검토합니다. 재고라면 "남은 재고 + 확정 주문 수 = 처음 재고"를 DB 기준으로 맞춰 보는 절차가 장애 조치 뒤의 마지막 방어선입니다.

### 중요도별 점검 항목

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | 장애 조치 때 잃을 수 있는 쓰기와 그 결과를 데이터별로 적기 | "복제가 있으니 안전하다"로는 판단할 수 없습니다. 예시처럼 재고 하나가 되살아나는 결과가 허용되는지 정해야 합니다 |
| 필수 | 복제본에서 읽는 요청을 목록으로 관리하기 | 판단에 쓰는 읽기가 복제본으로 가면 늦은 값으로 잘못 판단합니다 |
| 필수 | 영속성을 끈 주 서버의 자동 재시작 금지 | 빈 주 서버가 복제본을 비웁니다 |
| 필수 | 주 서버와 복제본 서버 모두 메모리를 데이터 크기 + 동기화 여유로 잡기 | 주 서버는 전체 동기화 때 copy-on-write를, 복제본은 `maxmemory`를 무시하고 동기화 중 흐름을 쌓으므로 둘 다 데이터보다 많이 씁니다 |
| 알면 유리 | `repl-backlog-size`를 흐름 속도 × 견딜 끊김 시간으로 정하기 | 기본 1MB는 쓰기가 많은 서비스에서 몇 초만 버팁니다. 짧은 네트워크 문제가 매번 전체 동기화가 됩니다 |
| 알면 유리 | 복제본을 계획적으로 내릴 때 RDB로 저장하고 내리기 | 다시 시작한 뒤 부분 재동기화를 할 수 있습니다. AOF로 다시 시작하면 전체 동기화입니다 |
| 알면 유리 | `client-output-buffer-limit replica`와 동기화 시간 비교 | 데이터가 커서 전송이 오래 걸리면 한도를 넘어 동기화가 반복될 수 있습니다 |
| 알면 유리 | `min-replicas-*` 사용 여부 결정 | 네트워크 분리 때 버려질 쓰기를 줄이는 대신 쓰기 가용성을 내줍니다 |
| 참고 | `repl-diskless-load swapdb`, `replica-lazy-flush` | 복제본의 `LOADING` 시간을 줄이는 대신 그동안 메모리를 더 씁니다 |
| 참고 | 복제본의 시계 동기화 | 복제본은 읽을 때 자기 시계로 만료를 판정하므로, 시계가 어긋나면 만료 판정이 주 서버와 달라집니다 |

### 관찰할 것

- **주 서버의 `INFO replication`**: `connected_slaves`, 복제본마다 `state`(정상이면 `online`), `offset`, `lag`. 밀린 양은 `master_repl_offset − offset`(바이트)으로 봅니다. `lag`는 마지막 보고 이후의 초이므로 1을 넘어 계속 늘면 보고가 끊긴 것입니다. `repl_backlog_histlen`과 `repl_backlog_size`는 백로그가 실제로 얼마나 차 있는지를 보여 줍니다.
- **복제본의 `INFO replication`**: `master_link_status`(`up`/`down`), `master_last_io_seconds_ago`, `master_sync_in_progress`(1이면 전체 동기화 중), `slave_repl_offset`. 끊겨 있다면 `master_link_down_since_seconds`가 끊긴 시간입니다.
- **`INFO stats`**: `sync_full`(전체 동기화 횟수), `sync_partial_ok`, `sync_partial_err`. 짧은 끊김 뒤에 `sync_partial_err`와 `sync_full`이 함께 늘면 부분 재동기화가 거절된 것입니다. 원인이 백로그 부족인지 복제 ID 불일치인지는 아래 로그로 구분합니다.
- **로그**: 주 서버의 "Partial resynchronization not accepted"(복제 ID 불일치나 두 번째 ID 범위 초과), "Unable to partial resync with replica … for lack of backlog", 출력 버퍼 한도로 연결을 닫았다는 줄. 복제본의 "MASTER <-> REPLICA sync" 단계별 줄("Loading DB in memory", "Flushing old data", "Finished with success")은 동기화의 진행을 보여 줍니다. `LOADING` 구간 자체는 복제본의 `INFO persistence`의 `loading`(1이면 읽는 중)으로 확인합니다.
- **흐름 속도**: `master_repl_offset`을 일정 간격으로 읽어 차이를 시간으로 나누면 초당 복제 바이트입니다. 백로그 크기를 정하는 근거입니다.

### 증상과 대응

| 증상 | 의심할 원인 | 확인 | 대응과 부작용 |
| --- | --- | --- | --- |
| 장애 조치 뒤 재고가 늘어 있음, 주문은 있는데 Redis에 흔적이 없음 | 비동기 복제로 응답한 쓰기가 새 주 서버에 없음 | 장애 조치 시각, 옛 주 서버 로그, 주문 DB | DB 기준으로 다시 계산합니다. 중요한 쓰기에 `WAIT`을 붙이면 지연이 늘고, 그래도 0은 아닙니다 |
| 네트워크가 잠깐 끊길 때마다 복제본이 오래 `LOADING` | 부분 재동기화 실패 → 전체 동기화 | `sync_partial_err`, `sync_full`, "lack of backlog" 로그 | `repl-backlog-size`를 늘립니다. 주 서버 메모리를 그만큼 더 씁니다 |
| 전체 동기화가 끝나지 않고 반복됨 | 동기화 중 흐름이 출력 버퍼 한도를 넘음 | 한도 초과 로그, `sync_full` 증가, 주 서버 메모리 급증 | 한도 상향(메모리 증가), 쓰기가 적은 시간에 동기화, RDB 채널 사용 확인. 데이터 크기를 줄이는 것이 근본 대응입니다 |
| 복제본을 붙이거나 재시작할 때 주 서버 지연이 튐 | 전체 동기화의 `fork`와 copy-on-write | 주 서버 `latest_fork_usec`, 동기화 시각과 지연 시각 비교 | 복제본 재시작은 RDB 저장 후 해 부분 재동기화를 노립니다. 여러 복제본은 함께 붙여 `fork` 한 번으로 받게 합니다(`repl-diskless-sync-delay`) |
| 복제본에서 읽은 값이 방금 쓴 값과 다름 | 복제 지연 | 주 서버 `master_repl_offset`과 복제본 `offset` 차이 | 그 읽기를 주 서버로 보냅니다. 주 서버의 읽기 부하가 늘어납니다 |
| 연결은 `online`인데 오프셋 차이가 계속 커짐 | 복제본이 흐름을 받는 속도나 처리하는 속도보다 쓰기 흐름이 빠름 | ① `master_repl_offset` 증가 속도가 평소보다 커졌는지(쓰기 흐름 증가) ② 주 서버 `CLIENT LIST TYPE replica`의 `omem`(그 복제본 연결의 출력 버퍼 메모리. 공유 버퍼에서 아직 다 보내지 못한 블록부터 끝까지를 블록 관리 비용과 함께 잰 값)이 계속 커지는지(보내는 쪽 적체의 단서) ③ 복제본의 `SLOWLOG`·CPU(복제본에 보낸 무거운 읽기가 메인 스레드를 잡는지. 명령 하나가 메인 스레드를 오래 잡으면 1초 보고도 늦어져 `lag`가 1을 넘습니다) ④ 두 서버 사이 네트워크 대역폭 | 흐름이 원인이면 큰 값 덮어쓰기를 필드 단위 변경으로 줄입니다(코드 변경). 복제본이 바쁘면 무거운 읽기를 줄이거나 다른 복제본·주 서버로 옮깁니다(옮긴 곳의 부하 증가). 이 차이는 장애 조치 때 잃을 수 있는 범위의 지표이고, 이 상태에서 끊기면 백로그를 넘기 쉽습니다 |
| 복제본의 키 수가 주 서버보다 많거나, 메모리를 더 씀 | 키 수: 주 서버가 지운 키의 `DEL`이 아직 닿지 않음(복제 지연). 메모리: 복제본은 `maxmemory`를 무시하고, 동기화 버퍼처럼 데이터 밖의 메모리도 씀 | 키 수와 메모리를 나눠 봅니다. 키 수는 `DBSIZE` 비교와 오프셋 차이로, 메모리는 `INFO memory`로 봅니다 | 키 수 차이는 복제가 따라잡으면 줄어듭니다. 줄지 않으면 복제 연결 상태부터 봅니다. 메모리는 복제본 서버의 여유를 늘립니다 |
| 주 서버가 쓰기를 `NOREPLICAS`로 거부 | `min-replicas-*` 조건 미달(복제본 다운, 보고 끊김) | 주 서버 `INFO replication`의 복제본 `lag`와 `state` | 복제본을 복구합니다. 설정을 급히 끄면 쓰기는 되지만 보호도 사라집니다 |
| 복제본 데이터가 통째로 비거나 과거로 돌아감 | 영속성 없는(또는 오래된 스냅숏의) 주 서버가 장애 조치보다 먼저 재시작 | 주 서버 시작 로그, 복제 ID 변경, 복제본의 "Flushing old data" 로그 | 자동 재시작을 끄거나 영속성을 켭니다. 복구는 서버 밖 백업에서 합니다 |
| 복제본에 쓰려다 `READONLY` 오류 | 클라이언트가 장애 조치 뒤 옛 주 서버(이제 복제본)에 계속 씀 | 클라이언트가 가리키는 노드, Sentinel 이벤트 | 클라이언트가 새 주 서버를 찾도록 구성합니다(Sentinel 구성 사용 등) |

### Spring에서 확인할 것

- **복제본 읽기는 클라이언트 설정입니다.** Spring Data Redis의 Lettuce 연동에서는 `LettuceClientConfiguration.builder().readFrom(ReadFrom.REPLICA_PREFERRED)`처럼 읽을 노드를 정합니다. 그러면 읽기 전용 명령은 복제본을 우선해(쓸 수 있는 복제본이 없으면 주 서버로), 그 밖의 명령과 트랜잭션(`MULTI`) 안의 명령은 주 서버로 갑니다. 읽기와 쓰기가 섞인 명령 묶음은 통째로 주 서버로 갈 수 있습니다. `readFrom`을 정하지 않으면 모든 명령이 연결한 노드 하나로 갑니다.
- **읽기를 복제본으로 보내는 순간, 늦은 값이 앱 전체에 퍼집니다.** `readFrom`은 연결 팩토리 단위 설정이라, 그 연결로 나가는 모든 읽기가 영향을 받습니다. 재고 판단처럼 최신 값이 필요한 읽기가 같은 `RedisTemplate`을 쓰면 복제본에서 읽습니다. 최신 값이 필요한 읽기를 Lua 스크립트(`EVAL`은 쓰기로 분류되어 주 서버로 감)나 주 서버 전용 연결 팩토리로 분리하는 방법을 검토합니다.
- **장애 조치를 따라가려면 Sentinel 구성을 씁니다.** 단독 주소(`RedisStandaloneConfiguration`)와 `readFrom`을 함께 쓰면, Lettuce는 연결할 때 한 번 `INFO REPLICATION`으로 구성을 찾고 그 뒤로 고정합니다. 장애 조치 뒤에는 연결을 다시 만들어야 합니다. `RedisStaticMasterReplicaConfiguration`도 주어진 노드만 쓰고 구성 변경을 따라가지 않습니다. Sentinel 주소를 담은 `RedisSentinelConfiguration`과 `readFrom`을 정한 클라이언트 설정을 함께 연결 팩토리에 주면 Spring Data Redis가 Sentinel 주소로 Lettuce의 주 서버·복제본 연결을 만들고, Lettuce는 Sentinel 이벤트를 받아 구성을 갱신합니다. `readFrom` 없이 Sentinel 구성을 쓰면, Lettuce는 연결이 끊겨 다시 연결할 때마다 Sentinel에 주 서버 주소를 다시 묻습니다. Sentinel은 서버의 역할을 바꿀 때 그 서버의 일반 클라이언트 연결을 끊으므로, 장애 조치 뒤 옛 주 서버에 붙어 있던 연결도 끊기고 다시 연결하며 새 주 서버를 찾게 됩니다. 다만 다시 연결할 때 Sentinel에 묻는 일이 실패하면 Lettuce는 마지막으로 알던 주소로 다시 붙으므로, 그 순간 Sentinel에 닿지 못하면 옛 주 서버로 돌아갈 수 있습니다. Sentinel을 여러 대 두고 앱에서 닿을 수 있는지를 함께 확인하는 이유입니다. 고정 구성에서는 장애 조치 뒤 쓰기가 옛 주 서버로 가 `READONLY` 오류가 날 수 있습니다.
- **`WAIT`은 같은 연결에서 보내야 의미가 있습니다.** Redis가 기억하는 "이 연결"은 서버에 열린 소켓 연결 하나입니다. `RedisTemplate`, Spring의 `RedisConnection`, Lettuce의 연결 객체는 이것과 같은 단위가 아니어서, 어떤 호출 경로를 쓰면 쓰기와 `WAIT`이 같은 소켓으로 나가는지는 트랜잭션·파이프라인 여부와 연결 공유 설정에 따라 달라집니다. 호출 경로별 결과와 권하는 구성은 [Spring에서 쓰기와 WAIT을 같은 연결로 보내기](./spring-wait-connection.md)에서 다룹니다. 쓰기와 `WAIT`이 다른 소켓으로 나가면, `WAIT`은 확인하려던 쓰기가 아니라 자기가 실행된 연결이 마지막으로 보낸 쓰기의 위치를 기준으로 세므로 그 쓰기의 복제 확인이 되지 않습니다. `MULTI` 안에 넣으면 기다리지 않습니다. `WAIT`은 스크립트 안에서도 기다리지 않으므로(공식 문서 wait의 2번 설명) 쓰기와 확인을 한 명령으로 묶을 수도 없습니다.
- **재시도는 멱등해야 합니다.** 장애 조치 중 응답을 받지 못한 `DECR`을 다시 보내면, 재시도가 성공했다고 할 때, 처음 것이 새 주 서버에 남아 있었다면 두 번 반영되고 사라졌다면 한 번 반영됩니다. 앱은 둘 중 어느 쪽인지 알 수 없습니다. [Redis 원자성](./atomicity.md)의 "원자성이 보장하지 않는 것"과 같은 문제입니다.

## 말로 설명하기

### 30초 버전

Redis 복제는 주 서버가 실행한 쓰기를 바이트 흐름으로 복제본에 보내고, 복제본이 그대로 다시 실행해 같은 데이터를 갖게 하는 방식입니다. 기본은 비동기라 주 서버는 복제본을 기다리지 않고 응답하므로 빠르지만, 응답한 쓰기가 복제본에 없는 순간이 항상 있습니다. 그래서 복제본에서 읽으면 조금 전 값이 보일 수 있고, 장애 조치 때는 그 마지막 쓰기를 잃습니다. 끊겼다 붙으면 백로그로 놓친 부분만 받지만, 백로그를 넘으면 `fork`로 전체를 다시 보내야 해서 주 서버와 복제본 모두 큰 비용을 치릅니다.

### 2분 버전

복제는 세 가지 방식으로 동작합니다. 연결된 동안에는 주 서버가 데이터를 바꾼 명령을 흐름으로 보냅니다. 같은 결과가 나와야 하므로 `EXPIRE`는 절대 시각의 `PEXPIREAT`으로 바꿔 보내고, 만료나 메모리 한도로 지운 키는 (기본 설정에서) `DEL`로 보냅니다. 흐름의 누적 바이트 수가 오프셋이고, 복제본은 1초마다 처리한 오프셋을 보고합니다. 주 서버는 이 보고로 끊긴 복제본을 판단하고 `WAIT`을 구현하지만, 응답 전에 보고를 기다리지는 않습니다.

연결이 끊겼다 붙으면 복제본은 (복제 ID, 다음 오프셋)으로 이어받기를 요청합니다. 주 서버가 그 위치를 백로그(기본 1MB)에 들고 있으면 놓친 바이트만 보냅니다. 백로그가 버티는 시간은 대략 크기를 흐름 속도로 나눈 값이라, 쓰기가 많으면 몇 초일 수 있습니다. 이어받을 수 없으면 전체 동기화입니다. 주 서버가 `fork`해 자식이 RDB를 디스크를 거치지 않고 보내고, 그동안의 쓰기 흐름도 함께 보냅니다. 이 흐름을 복제본이 쌓아 두는 RDB 채널 방식이 기본입니다. 복제본은 옛 데이터를 비우고 RDB를 읽는 동안 `LOADING`으로 답합니다. 동기화 중 쌓이는 흐름이 출력 버퍼 한도를 넘으면 끊기고 다시 시작하는 반복에 빠질 수 있습니다.

비동기의 결과는 두 가지입니다. 복제본 읽기는 늦을 수 있으므로 판단에 쓰는 읽기는 주 서버에서 합니다. 그리고 장애 조치 때 응답한 쓰기를 잃으며, 옛 주 서버가 돌아와도 역사가 갈라져 그 쓰기는 덮어쓰입니다. `WAIT`은 확인될 때까지 기다려서, `min-replicas-to-write`는 복제본과 끊긴 주 서버의 쓰기를 거부해서 이 구간을 줄이지만 없애지는 못합니다. 또 복제본은 주 서버를 그대로 따라가므로, 영속성 없이 재시작한 빈 주 서버나 실수로 보낸 `FLUSHALL`도 따라 합니다.

### 예상 질문과 꼬리 질문

**Q. Redis 복제는 무엇을 보내나요? 데이터를 보내나요, 명령을 보내나요?**
연결된 동안에는 명령을 보냅니다. 주 서버가 데이터를 바꾼 명령을 실행한 뒤 같은 형식으로 흐름에 덧붙이고, 복제본이 다시 실행합니다. 다만 다시 실행해도 같은 결과가 나오도록, 실행 시점에 따라 결과가 달라질 수 있는 명령(예: 상대 시간으로 만료를 거는 `EXPIRE`)은 결과가 확정된 명령으로 바꿔 보내고, 스크립트는 스크립트가 실행한 쓰기 명령을 보냅니다. 처음 붙거나 이어받을 수 없을 때만 데이터 전체(RDB)를 보냅니다. (본문: "무엇을 보내는가", "따라잡을 수 없으면")

- **꼬리: 만료된 키는 어떻게 복제본에서 사라지나요?**
  복제본은 스스로 지우지 않고 주 서버가 만든 `DEL`을 기다립니다. 두 서버의 시계가 같다고 기댈 수 없어서 지우는 판단을 주 서버 한 곳에 둡니다. 그 사이 읽기에는 복제본이 자기 시계로 판정해 "없음"으로 답하지만, 키 수와 메모리에는 남아 있을 수 있습니다.
- **꼬리: 그럼 복제본의 시계가 틀리면요?**
  읽기 판정이 주 서버와 달라질 수 있습니다. 만료 시각은 절대 시각으로 복제되므로, 복제본 시계가 앞서면 주 서버에서는 살아 있는 키를 복제본은 없다고 답할 수 있습니다. 이 부분은 판정 방식에서 끌어낸 추론입니다.

**Q. 복제가 비동기라는 것은 정확히 무슨 뜻인가요?**
주 서버가 쓰기를 실행하고 응답을 보낼 때, 복제본이 그 쓰기를 받았는지 기다리지 않는다는 뜻입니다. 흐름은 응답과 따로 보내지고, 복제본의 처리 보고도 1초마다 따로 옵니다. 그래서 응답했지만 복제본에는 없는 쓰기가 항상 조금 있을 수 있습니다. 대신 모든 쓰기가 복제본 왕복을 기다리지 않고, 복제본이 느려져도 주 서버의 쓰기가 멈추지 않습니다. (본문: "동기와 비동기", "기다리지 않고 응답합니다")

- **꼬리: 그 "조금"은 얼마인가요?**
  정해진 값이 없습니다. 네트워크와 부하에 달렸고, 복제본이 바쁘거나 네트워크가 막히면 늘어납니다. 주 서버의 `master_repl_offset`과 복제본 `offset`의 차이로 바이트 단위로 봅니다. `INFO`의 `lag`는 마지막 보고 이후의 초라서 밀린 양이 아닙니다.
- **꼬리: 흐름이 순서대로 가니까, 복제본이 중간 쓰기를 빼먹을 수도 있나요?**
  흐름은 한 줄로 순서대로 보내고 받으므로, 앞의 쓰기를 빼고 뒤의 쓰기만 가진 상태는 생기지 않습니다. 뒤쪽 일부를 아직 못 받은 상태가 될 뿐입니다. 끊겼다 붙을 때도 오프셋으로 정확히 그다음 바이트부터 받습니다.

**Q. 장애 조치 때 정말 응답한 쓰기를 잃나요? 어떤 경우인가요?**
잃을 수 있습니다. 주 서버가 응답한 뒤 그 쓰기가 복제본에 닿기 전에 주 서버가 사라지고 그 복제본이 새 주 서버가 되면, 새 기준에는 그 쓰기가 없습니다. 예시에서 주문 C는 "완료"를 받았지만 새 주 서버의 재고는 8이라 하나가 다시 팔릴 수 있습니다. 주 서버의 AOF `always`도 소용이 없습니다. 주 서버의 디스크는 새 주 서버가 아니기 때문입니다. (본문: "새 주 서버가 가진 것까지가 새 기준입니다")

- **꼬리: 옛 주 서버가 돌아오면 그 쓰기를 되살릴 수 있지 않나요?**
  자동으로는 아닙니다. 옛 주 서버가 메모리와 옛 복제 ID를 그대로 가진 채 복제본으로 붙는 경우라면, 새 주 서버가 모르는 바이트를 더 가지고 있어 이어받기 범위(새 주 서버가 옛 ID로 받아 줄 수 있는 오프셋)를 넘으므로, 부분 재동기화가 거절되고 전체 동기화로 덮어쓰입니다. 그 쓰기를 찾으려면 옛 주 서버가 새 주 서버에 붙기 전에 그 서버의 AOF 같은 파일에서 따로 꺼내야 합니다. 이 결론은 소스 조건에서 끌어낸 것이고 재현하지는 않았습니다.
- **꼬리: 그럼 `WAIT`을 붙이면 안전한가요?**
  확률을 크게 줄일 뿐입니다. `WAIT`은 지정한 수의 복제본이 그 쓰기를 메모리에 반영했다고 보고할 때까지 기다리지만, 장애 조치 도구가 그 복제본을 고른다는 보장은 없습니다. 공식 문서도 여러 복제본에 전달된 쓰기도 잃을 수 있다고 밝힙니다. 또 시간이 다 되면 0이나 부족한 수를 돌려줄 뿐 쓰기를 취소하지 않으므로, 앱이 반환값을 보고 대응해야 합니다.
- **꼬리: 반환값이 0이면 어떻게 대응하나요?**
  0은 "아직 확인되지 않았다"는 뜻이지 쓰기가 사라졌다는 뜻이 아닙니다. 쓰기는 이미 주 서버에 실행됐습니다. 그래서 쓰기를 다시 보내지 않고, 같은 연결로 `WAIT`만 다시 보내거나 그 쓰기를 "복제 미확인"으로 기록합니다. 확인된 쓰기도 장애 조치 뒤 사라질 수 있으므로, 최종 방어는 재고 같은 값을 DB의 주문 기록으로 다시 맞출 수 있게 설계하는 것입니다. 호출 경로와 대응의 상세는 [Spring에서 쓰기와 WAIT을 같은 연결로 보내기](./spring-wait-connection.md)에서 다룹니다.

**Q. `min-replicas-to-write`는 무엇을 막나요?**
복제본과 연락이 끊긴 주 서버가 혼자 쓰기를 계속 받는 상황을 막습니다. 마지막 보고가 `min-replicas-max-lag`초 이내인 복제본이 지정한 수보다 적으면 쓰기를 `NOREPLICAS`로 거부합니다. 네트워크가 갈라져 다른 쪽에서 복제본이 새 주 서버가 되면 옛 주 서버가 받은 쓰기는 모두 버려지는데, 이 설정은 그 시간을 몇 초로 묶습니다. (본문: "WAIT은 기다려서, min-replicas는 거부해서")

- **꼬리: 그럼 켜 두면 잃는 것이 없나요?**
  아닙니다. 이 설정은 보고가 최근에 왔는지만 볼 뿐 보고한 위치가 최신인지는 보지 않으므로, 조건을 만족하는 동안에도 복제본이 뒤처진 만큼의 쓰기는 복제본에 없을 수 있습니다. 공식 문서도 일관성 보장이 아니라 잃는 구간을 제한하는 최선의 노력으로 설명합니다.
- **꼬리: 대가는요?**
  쓰기 가용성입니다. 복제본이 1대인데 1로 두면, 그 복제본을 재시작하는 동안 주 서버가 살아 있어도 모든 쓰기가 실패합니다. 복제본 수와 점검 절차를 함께 정해야 합니다.

**Q. 복제본이 잠깐 끊겼다 다시 붙으면 데이터 전체를 다시 받나요?**
놓친 부분이 주 서버의 백로그 안에 있으면 그 부분만 받습니다. 복제본이 (복제 ID, 다음 오프셋)을 보내고, 주 서버는 그 ID가 자기 현재 ID이거나 유효 범위 안의 두 번째 ID(장애 조치 전 주 서버의 ID)이고, 오프셋이 백로그가 들고 있는 범위 안이면 그 뒤만 보냅니다. 그렇지 않으면 전체 동기화입니다. (본문: "끊겼다 다시 붙으면")

- **꼬리: 백로그 기본 1MB면 충분한가요?**
  흐름 속도에 달렸습니다. 버티는 시간은 대략 백로그 크기 ÷ 초당 복제 바이트입니다. 끊기기 전에 이미 뒤처져 있었다면 그만큼 줄어듭니다. 가상 예시로 33바이트 `DECR`이 초당 1,000번이면 약 32초지만, 초당 10MB를 쓰면 0.1초입니다. `master_repl_offset`의 증가 속도를 재고, 견디고 싶은 끊김 시간을 곱해 정합니다.
- **꼬리: 백로그를 크게 잡으면 단점은요?**
  그 크기만큼 주 서버 메모리를 씁니다. 복제본도 나중에 주 서버가 될 수 있어 백로그를 유지하므로 복제본 쪽 메모리도 봅니다.
- **꼬리: 복제본을 재시작해도 이어받을 수 있나요?**
  RDB로 저장하고 내렸다면 RDB에 복제 ID와 오프셋이 들어 있어 시도할 수 있습니다. AOF를 켠 채 재시작하면 복제 정보를 되살리지 않아 전체 동기화입니다. 그래서 공식 문서는 업그레이드처럼 일부러 내릴 때 `SHUTDOWN`으로 저장하고 내리라고 권합니다.

**Q. 전체 동기화는 왜 비싼가요?**
주 서버는 `fork`로 그 순간의 데이터를 떠서 보내야 하므로, `fork` 동안 메인 스레드가 멈추고 전송 동안 copy-on-write로 메모리가 늘어납니다. 영속성을 꺼도 피할 수 없습니다. 데이터 전체가 네트워크를 건너고, 복제본은 옛 데이터를 비우고 새 데이터를 읽는 동안 `LOADING`으로 답해 그 복제본에 보낸 읽기가 실패합니다. (본문: "주 서버가 치르는 비용", "복제본이 치르는 비용")

- **꼬리: 전체 동기화가 계속 반복되면 무엇을 의심하나요?**
  동기화하는 동안의 쓰기가 출력 버퍼 한도(기본 256MB, 또는 64MB 이상 60초)를 넘어 연결이 끊기는지 봅니다. `sync_full`이 계속 늘고 한도 초과 로그가 있으면 그 경우입니다. 한도를 올리면 메모리를 더 쓰므로, 쓰기가 적은 시간에 동기화하거나 데이터를 줄이는 것도 함께 검토합니다.
- **꼬리: RDB 채널 방식이면 그 문제가 없어지나요?**
  줄었을 뿐입니다. 기본으로 쓰이는 RDB 채널 방식은 동기화 중의 흐름을 복제본이 쌓아 두므로 주 서버 쪽 부담이 줄지만, 복제본 쪽 한도(`replica-full-sync-buffer-limit`, 기본은 출력 버퍼 hard limit)에 닿으면 다시 주 서버 쪽에 쌓입니다. 또 주 서버의 `repl-diskless-sync`가 꺼져 있으면 이 방식이 쓰이지 않습니다.
- **꼬리: 복제본 `LOADING` 시간을 줄일 수는 없나요?**
  `replica-lazy-flush yes`로 옛 데이터 비우기를 백그라운드 스레드에 넘기거나, `repl-diskless-load swapdb`로 옛 데이터를 둔 채 읽어 마지막에 바꿔 끼울 수 있습니다. 둘 다 그동안 옛 데이터와 새 데이터가 함께 메모리를 차지하므로, 메모리가 모자라면 강제 종료될 수 있습니다.

**Q. 읽기를 복제본으로 보내면 무엇이 문제인가요?**
복제본의 값은 조금 전의 주 서버 값이라, 방금 쓴 값이 안 보일 수 있습니다. 화면 표시처럼 잠깐 틀려도 되는 읽기는 괜찮지만, 재고를 읽고 그 값으로 차감을 결정하는 식의 판단에 쓰면 팔린 재고를 남은 것으로 봅니다. 또 복제본이 전체 동기화에서 옛 데이터를 비우고 새 데이터를 읽는 동안에는 그 읽기가 `LOADING`으로 실패합니다(RDB를 받는 동안에는 기본 설정에서 옛 데이터로 답합니다). (본문: "복제본에서 읽으면")

- **꼬리: `WAIT` 뒤에 복제본에서 읽으면 되지 않나요?**
  `WAIT`은 몇 대가 받았는지만 셀 뿐, 어느 복제본인지는 알려 주지 않습니다. 복제본이 여럿이면 읽기가 간 복제본이 그 쓰기를 받았다는 보장이 없습니다. 방금 쓴 값이 필요한 읽기는 주 서버에서 합니다.
- **꼬리: Spring에서 일부 읽기만 주 서버로 보내려면요?**
  Lettuce의 `readFrom`은 연결 팩토리 단위라, 그 연결로 나가는 모든 읽기 전용 명령에 적용됩니다. 최신 값이 필요한 읽기는 주 서버 전용 연결 팩토리로 분리하거나, 판단과 변경을 Lua 스크립트 하나로 묶어 주 서버에서 실행합니다. `EVAL`은 쓰기 명령으로 분류되어 주 서버로 갑니다.

**Q. 복제본이 있으니 주 서버의 영속성은 꺼도 되지 않나요?**
조건부로만 가능합니다. 영속성을 끈 주 서버가 죽고 자동 재시작 도구가 장애 조치보다 먼저 띄우면, 빈 주 서버가 새 복제 ID로 시작하고 복제본은 전체 동기화로 빈 데이터를 받아 자기 데이터까지 비웁니다. 공식 문서는 이 구성이면 자동 재시작을 끄라고 권합니다. 또 영속성을 꺼도 전체 동기화의 `fork`는 피할 수 없습니다. (본문: "영속성을 끈 주 서버가 다시 시작하면")

- **꼬리: 영속성을 켜면 이 문제는 사라지나요?**
  작아질 뿐입니다. 주 서버가 오래된 RDB 스냅숏으로 다시 시작하면, 그 뒤의 쓰기를 이미 받은 복제본은 이어받기가 거절되어 주 서버의 더 오래된 데이터로 돌아갈 수 있습니다. 소스 조건에서 끌어낸 해석이며 재현하지 않았습니다. 주 서버의 재시작과 장애 조치 중 무엇이 먼저 일어나는지를 운영 구성에서 정해 두어야 합니다.

**Q. 복제본에 쓰기를 허용하면 안 되나요?**
기본은 쓰기 금지(`replica-read-only yes`)이고, 공식 문서는 쓰기 가능한 복제본을 권하지 않습니다. 복제본에 쓴 키와 주 서버에서 오는 명령이 부딪치면 결과가 주 서버와 달라지고, 그 쓰기는 다음 전체 동기화 때 사라집니다. 예전에 복제본에 임시 결과를 저장하던 용도는 `SUNION`, `SORT_RO`, `EVAL_RO` 같은 읽기 전용 명령으로 대신할 수 있습니다. (본문: "복제본에는 쓰지 않습니다")

**Q. 장애 조치 뒤 앱에서 `READONLY` 오류가 나면 무엇이 문제인가요?**
앱이 아직 옛 주 서버를 가리키고 있는데, 그 서버가 돌아와 새 주 서버의 복제본이 된 경우입니다. 단독 주소나 고정 노드 목록으로 Lettuce를 구성하면 구성 변경을 따라가지 않으므로 연결을 다시 만들어야 합니다. Sentinel 구성이라면 `readFrom`을 쓸 때는 Sentinel 이벤트로 구성을 갱신하고, `readFrom` 없이 쓸 때는 연결이 끊겨 다시 연결할 때마다 Sentinel에 주 서버 주소를 다시 묻습니다. (본문: "Spring에서 확인할 것")

- **꼬리: 그 사이 실패한 쓰기는 다시 보내도 되나요?**
  멱등하지 않은 쓰기(`DECR` 등)는 처음 시도가 반영됐는지 모르는 채 다시 보내면 두 번 반영될 수 있습니다. 주문 ID 같은 식별자로 이미 처리했는지 확인하는 방식으로 재시도를 멱등하게 만들어야 합니다.
