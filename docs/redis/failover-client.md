# Redis 장애 조치와 클라이언트: 주 서버가 죽으면 Spring 앱은 몇 초 동안 무엇을 겪는가

> **핵심 질문**: Redis 주 서버가 죽고 복제본이 새 주 서버로 올라가는 동안, Spring 앱은 초 단위로 무엇을 겪는가? 장애가 몇 초 이어질지는 무엇이 정하고, 무엇을 잃는가? 개발자는 어떤 설정을 고르고 무엇을 관찰해야 하는가?
>
> **범위**: Redis Sentinel이 장애를 판정하고 복제본을 승격하는 절차, Spring Boot + Lettuce 앱이 새 주 서버를 알아내는 방법, 장애 순간에 보낸 명령의 운명, 옛 주 서버가 돌아올 때의 문제를 다룹니다. Redis Cluster는 클라이언트 쪽에서 장애 조치를 따라가는 부분만 다룹니다. 비동기 복제가 쓰기를 잃는 원리는 [Redis 복제](./replication.md)에, 클러스터의 지도 갱신 설정은 Redis Cluster에 있습니다. Sentinel 배포 절차, 인증·TLS, Docker·NAT 환경의 주소 문제, 복제본 읽기는 다루지 않습니다.

## 배경 지식

### 이 문서의 예시: 재고 주문, 서버 세 대와 Sentinel 세 개

[Redis 복제](./replication.md)의 재고 장면을 서버 세 대로 넓혀 씁니다. 문서 끝까지 이 장면 하나를 씁니다.

```text
           박스 1                     박스 2                     박스 3
   ┌──────────────────┐      ┌──────────────────┐      ┌──────────────────┐
   │ 주 서버 M         │      │ 복제본 R2         │      │ 복제본 R3         │
   │ Sentinel S1      │      │ Sentinel S2      │      │ Sentinel S3      │
   └──────────────────┘      └──────────────────┘      └──────────────────┘
            ▲  M ──복제──▶ R2,  M ──복제──▶ R3
            │
   주문 API (Spring Boot + Lettuce, 공유 연결 하나로 M에 DECR stock:item:1)
```

- **Redis**: 주 서버 M, 복제본 R2·R3입니다. 주문 API는 M에만 씁니다. 처음 재고는 `stock:item:1 = 10`이고, 주문 A, B, C의 `DECR`로 7이 됩니다.
- **Sentinel**: 세 박스마다 Sentinel이 하나씩 있습니다. Sentinel은 Redis 서버를 감시하다가 주 서버가 죽으면 복제본 하나를 새 주 서버로 올리는 별도 프로세스입니다(아래 "Sentinel은 어떻게 판정하고 승격하는가"). 감시 대상의 이름은 `mymaster`, `quorum`은 2이고(주 서버가 죽었다고 판정하는 데 필요한 Sentinel 동의 수입니다. 장애 조치를 실제로 진행하는 데 필요한 표 수와는 다르며, 아래 "선거와 승격: 과반의 표를 얻은 Sentinel 하나가 진행합니다"에서 구분합니다), 나머지 설정은 기본값입니다(`down-after-milliseconds` 30,000, `failover-timeout` 180,000, `parallel-syncs` 1).
- **주문 API**: Spring Boot 앱이고 Redis 설정은 다음뿐입니다. `spring.data.redis.sentinel.master=mymaster`, `spring.data.redis.sentinel.nodes=s1:26379,s2:26379,s3:26379`, `spring.data.redis.timeout=100ms`. 명령 타임아웃 100ms는 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에서 고른 값입니다. 복제본 읽기 설정(`spring.data.redis.lettuce.read-from`)은 두지 않았습니다. 주문은 초당 100건 들어옵니다.
- **장애**: 주문 C가 응답을 받은 직후 M의 프로세스가 갑자기 죽습니다. 박스 1의 운영체제와 S1은 살아 있습니다. 이 순간을 **0초**로 둡니다. 이때 R2는 주문 B까지(재고 8), R3는 주문 A까지(재고 9) 받았다고 합시다. 복제가 비동기라 복제본마다 받은 양이 다를 수 있습니다.

`quorum` 2, 초당 100건, 복제본별로 받은 양은 계산을 보여 주기 위한 **가상의 값**입니다. 기본값과 동작은 `근거` 절의 Redis 8.2.10, Lettuce 6.6.0.RELEASE, Spring Data Redis 3.5.13, Spring Boot 3.5.16 소스로 확인했습니다. 이 문서의 초 단위 시각은 소스의 주기·기본값으로 계산한 추정이며, 장애를 재현해 잰 값이 아닙니다.

장애는 두 갈래로 나눠 봅니다. 위처럼 **프로세스만 죽으면** 박스 1의 운영체제가 남아 연결 종료를 알려 주므로, 주문 API가 연결이 끊긴 것을 곧바로 압니다. 반면 **박스 1의 전원이나 네트워크가 통째로 사라지면** 아무도 종료를 알려 주지 않습니다. 이 차이가 클라이언트 쪽 장애 시간을 크게 바꾸므로, 본문은 앞의 경우로 시작하고 뒤의 경우를 "연결이 조용히 죽으면 다시 연결 자체가 일어나지 않습니다"에서 따로 봅니다.

### 장애 감지는 '응답이 없다'에서 출발한 추측입니다

- **한 줄**: 네트워크 너머의 서버가 죽었는지는 직접 알 수 없고, "정해 둔 시간 동안 응답이 없다"로 추측할 뿐입니다.
- **비유**: 전화를 걸었는데 상대가 받지 않습니다. 상대가 쓰러졌을 수도, 회의 중일 수도, 통신망이 끊겼을 수도 있습니다. 30초를 기다려도 안 받으면 "무슨 일이 생겼다"고 판단하고 다른 사람에게 일을 넘기기로 정해 둔 것과 같습니다.
- **실제로는**: 감시하는 쪽은 주기적으로 `PING`을 보내고, 마지막 정상 응답 뒤로 정해 둔 시간이 지나면 죽었다고 표시합니다. 이 판단에는 두 가지 오류가 있습니다. 살아 있지만 느린 서버(긴 명령으로 메인 스레드가 막힌 경우 등)를 죽었다고 볼 수 있고, 반대로 죽었는데도 그 시간이 지나기 전에는 모릅니다. 기다리는 시간을 줄이면 앞의 오류가, 늘리면 뒤의 대가(알아채기까지의 시간)가 커집니다. 기다림과 타임아웃의 일반 원리는 [타임아웃](../cs/timeout.md)에서 다룹니다.
- **이 주제에서는**: Sentinel의 `down-after-milliseconds`가 이 기다리는 시간이고, 기본값 30초가 예시의 장애 시간 대부분을 차지합니다.

### 과반: 둘로 갈라져도 한쪽만 결정하게 하는 수

- **한 줄**: 전체 N개 중 절반보다 많은 수(N/2를 넘는 가장 작은 정수)입니다. 3개면 2, 5개면 3입니다.
- **비유**: 위원 세 명이 두 방으로 흩어져 서로 연락이 끊겼습니다. "두 명 이상이 찬성해야 결정한다"고 정해 두면, 두 방 중 두 명이 있는 방만 결정할 수 있고 한 명뿐인 방은 결정하지 못합니다. 두 방이 서로 다른 결정을 내리는 일이 없습니다.
- **실제로는**: 네트워크가 갈리면 각 쪽은 상대가 죽었는지 갈라졌을 뿐인지 구분하지 못합니다. 그래서 "과반의 동의를 얻은 쪽만 행동한다"는 규칙으로 동시에 두 결정이 나오는 것을 막습니다. 대가는 과반에 못 미치는 쪽이 아무것도 못 한다는 것입니다. 3개 중 2개가 죽으면 남은 하나는 옳은 판단을 하고 있어도 결정하지 못합니다. 또 과반 규칙은 **결정하는 쪽**(Sentinel)을 하나로 만들 뿐, **결정을 따르지 않는 쪽**(갈라진 옛 주 서버, 그 서버에 붙어 있는 클라이언트)을 멈추지는 못합니다.
- **이 주제에서는**: Sentinel은 과반의 표를 얻은 하나만 장애 조치를 진행합니다. 예시의 Sentinel 세 개 중 두 개가 살아 있으면 장애 조치를 할 수 있습니다. 갈라진 옛 주 서버가 계속 쓰기를 받는 문제는 "옛 주 서버가 돌아오면: 두 주 서버와 버려지는 쓰기"에서 다룹니다.

### 비동기 복제: 응답한 쓰기가 복제본에 없을 수 있습니다

- **한 줄**: 주 서버는 복제본이 쓰기를 받았다는 확인을 기다리지 않고 클라이언트에 응답합니다. 그래서 주 서버가 죽는 순간 복제본은 일부 쓰기를 아직 못 받았을 수 있습니다. 원리와 유실 범위는 [Redis 복제](./replication.md)의 "장애 조치 때 무엇을 잃는가"에서 다룹니다.
- **이 주제에서는**: 예시에서 R2는 재고 8, R3는 9입니다. 어느 쪽이 새 주 서버가 되든 주문 C의 `DECR`은 사라집니다. 이 문서는 그 손실이 "얼마나 오래, 누구에게 어떻게 보이는가"를 따라갑니다.

### 명령 타임아웃과 모르는 결과

- **한 줄**: 명령 타임아웃은 클라이언트가 기다림을 그만둘 뿐 서버의 실행을 취소하지 않으므로, 타임아웃이 난 쓰기는 실행됐는지 알 수 없습니다. 원리는 [타임아웃](../cs/timeout.md)의 "타임아웃은 기다림을 그만둘 뿐, 일을 취소하지 않습니다"에서 다룹니다.
- **이 주제에서는**: 장애 순간에 나가 있던 `DECR`은 M에서 실행됐는지, 실행됐다면 R2에 복제됐는지 모두 모릅니다. 다시 보내도 되는지가 "장애 순간의 명령: 실패했는지, 실행됐는지 모릅니다"의 질문입니다.

### Lettuce의 자동 재연결과 끊긴 동안의 버퍼

- **한 줄**: Spring Boot의 기본 클라이언트 Lettuce는 연결이 끊긴 것을 알면 스스로 다시 연결하고, 그동안 들어온 명령과 응답을 못 받은 명령을 버리지 않고 쌓아 두었다가 다시 연결되면 보냅니다. 자세한 동작은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "연결이 끊기면 Lettuce가 응답 못 받은 명령을 다시 보냅니다"에서 다룹니다.
- **이 주제에서는**: Lettuce가 새 주 서버를 알게 되는 계기도 이 재연결입니다(Sentinel 구성에 `read-from`이 없을 때). 그래서 "언제 다시 연결을 시도하는가"가 앱이 겪는 장애 시간을 정합니다.

## 장애 시간은 세 구간의 합입니다

앱 입장에서 장애는 "쓰기가 실패하기 시작한 때부터 다시 성공할 때까지"입니다. 이 시간은 세 구간으로 나뉘고, 구간마다 정하는 주체가 다릅니다.

| 구간 | 무엇을 기다리는가 | 정하는 것 | 예시의 길이(추정) |
| --- | --- | --- | --- |
| ① 감지 | Sentinel들이 M을 죽었다고 판정 | `down-after-milliseconds`, `quorum` | 약 30초 |
| ② 승격 | 선거, 복제본 선택, 새 주 서버로 전환 확인 | Sentinel 내부 주기(대부분 1초) | 약 1~2초 |
| ③ 클라이언트 반영 | 앱이 새 주 서버 주소를 알고 연결 | Lettuce 재연결 간격, `read-from` 여부, 연결이 끊긴 것을 아는가 | 0~30초 |

예시를 초 단위로 따라가면 다음과 같습니다.

| 시각 | Redis·Sentinel | 주문 API |
| --- | --- | --- |
| 0초 | M 프로세스가 죽습니다. 박스 1의 운영체제가 M의 연결들을 닫습니다 | 공유 연결이 끊긴 것을 곧바로 압니다. 재연결을 시작합니다. Sentinel S1에 주 서버를 물으면 아직 M의 주소를 답하고, M의 포트는 연결을 거부합니다 |
| 0~30초 | Sentinel들은 대략 1초마다 M에 `PING`을 보내고 정상 응답을 못 받습니다 | 모든 주문의 `DECR`이 Lettuce 안에서 대기하다 100ms 뒤 `QueryTimeoutException`으로 실패합니다. 재연결은 1, 2, 4, 8, …밀리초 간격으로 늘어나며 계속 실패합니다 |
| 약 30초 | 각 Sentinel이 M을 `+sdown`으로 표시하고, 서로 묻고 답해 두 개가 동의하면 `+odown` | 계속 실패 |
| 약 30~31초 | 선거에서 과반의 표를 얻은 Sentinel(예: S2)이 R2를 고르고 `REPLICAOF NO ONE`을 보냅니다 | 계속 실패 |
| 약 31~32초 | S2가 R2의 역할 변화를 확인합니다. 이때부터 Sentinel은 주 서버 주소로 R2를 답합니다. 다른 Sentinel도 새 구성을 받아 `+switch-master`를 알립니다 | 계속 실패 |
| 약 32.8초 | R3를 R2에 붙이는 작업이 진행됩니다 | 15번째 재연결 시도. Sentinel이 R2를 답하면 연결되고 주문이 다시 성공합니다. 이 시도가 승격보다 먼저 일어났다면 다음 시도는 약 62.8초입니다 |

이 장애에서 앱이 잃은 것과 겪은 것은 다음과 같습니다.

- **잃은 쓰기**: 주문 C의 `DECR`입니다. 새 주 서버 R2의 재고는 8이라, C는 "주문 완료"를 받았는데 재고 하나가 되살아났습니다. 원인은 비동기 복제이고, 장애 조치 설정으로는 줄일 수만 있습니다.
- **실패한 주문**: 약 33초(늦으면 63초) 동안의 주문 약 3,300건(늦으면 6,300건)이 예외로 끝났습니다. 명령 타임아웃 100ms 덕에 이들은 대부분 서버에 가기 전에 실패 처리됐으므로 실행되지 않았습니다. 잃은 것이 아니라 거절된 것입니다. 예외는 재연결 직전 100ms 안에 들어온 주문으로, 아직 타임아웃 전이라 재연결 때 R2로 나가 정상 처리됩니다.
- **모르는 결과**: 0초에 나가 있던 `DECR`(있었다면)은 결과를 모릅니다. 아래 "장애 순간의 명령: 실패했는지, 실행됐는지 모릅니다"에서 다룹니다.

이 표에서 판단에 쓰는 결론은 둘입니다. **기본값 그대로면 Sentinel 쪽만으로 30초 넘게 걸리고, 클라이언트가 거기에 최대 30초를 더할 수 있습니다.** 그리고 **박스가 통째로 사라지는 경우는 이 표보다 훨씬 길어질 수 있습니다.** 아래 절들이 각 구간을 설명합니다.

## Sentinel은 어떻게 판정하고 승격하는가

### SDOWN: Sentinel 하나가 정해 둔 시간 동안 정상 응답을 못 받으면

각 Sentinel은 감시하는 서버마다 대략 1초에 한 번 `PING`을 보냅니다(`down-after-milliseconds`가 1초보다 작으면 그 주기로). 정상 응답이 없는 상태가 `down-after-milliseconds` 동안 이어지면 그 서버를 **SDOWN**(주관적 다운, Subjectively Down)으로 표시하고 `+sdown` 이벤트를 냅니다. "이 Sentinel 혼자 보기에 죽었다"는 뜻입니다.

정상 응답으로 치는 것은 `PONG`, `LOADING` 오류, `MASTERDOWN` 오류입니다. 그 밖의 응답이나 무응답은 정상이 아닙니다. 주의할 점이 두 가지입니다.

- **바쁜 주 서버도 죽은 것으로 봅니다.** Redis는 메인 스레드 하나가 명령을 차례로 실행하므로, 오래 걸리는 명령이 실행되는 동안에는 `PING`에도 답하지 못합니다([Redis 이벤트 루프](./event-loop.md)). 그 시간이 `down-after-milliseconds`보다 길면 살아 있는 주 서버가 장애 조치를 당합니다. Lua 스크립트가 오래 돌아 `BUSY`를 답하는 경우에는 Sentinel이 `SCRIPT KILL`을 먼저 시도하지만, 쓰기를 한 스크립트는 멈출 수 없어 결국 장애 조치로 갑니다.
- **응답이 간헐적으로 오면 SDOWN이 되지 않습니다.** 30초 기준이라면 29초마다 한 번씩만 정상 응답이 와도 정상으로 봅니다. 네트워크가 심하게 불안정해 대부분의 요청이 실패하는 상태도 장애 조치를 부르지 않을 수 있습니다.

예시에서는 M이 0초에 죽었으므로, 각 Sentinel은 30초 안팎에 SDOWN을 표시합니다.

### ODOWN: quorum만큼의 Sentinel이 동의하면

SDOWN만으로는 장애 조치를 하지 않습니다. 그 Sentinel만 M에 닿지 못하는 것일 수 있기 때문입니다. SDOWN을 표시한 Sentinel은 1초마다 다른 Sentinel들에게 "너도 M이 죽었다고 보는가"(`SENTINEL is-master-down-by-addr`)를 묻습니다. 자기 자신을 포함해 죽었다고 답한 수가 `quorum` 이상이면 M을 **ODOWN**(객관적 다운, Objectively Down)으로 표시하고 `+odown`을 냅니다. 최근에 받은 답만 세므로, "최근에 그만큼이 동의했다"는 느슨한 합의입니다. 같은 순간에 모두가 동의했다는 보장은 없습니다.

예시에서는 `quorum`이 2이므로, Sentinel 두 개가 SDOWN을 표시하면 곧 ODOWN이 됩니다. ODOWN은 주 서버에만 쓰는 표시입니다. 복제본이나 다른 Sentinel이 죽어도 SDOWN까지만 표시하고 장애 조치는 하지 않습니다.

### 선거와 승격: 과반의 표를 얻은 Sentinel 하나가 진행합니다

`quorum`은 **감지**의 기준입니다. ODOWN이 됐다고 바로 장애 조치를 하지는 않고, 진행하려면 선거를 거쳐야 합니다. 선거에서도 `quorum`은 필요한 표 수의 하한으로 다시 쓰입니다.

1. ODOWN을 표시한 Sentinel이 "장애 조치 번호"(에폭)를 하나 올리고(`+new-epoch`, `+try-failover`), 다른 Sentinel들에게 자기에게 표를 달라고 요청합니다. 각 Sentinel은 한 에폭에 한 번만, 먼저 요청한 쪽에 투표합니다.
2. 아는 Sentinel 전체(자신 포함)의 과반이면서 `quorum` 이상의 표를 얻은 Sentinel이 리더가 됩니다(`+elected-leader`). 예시는 Sentinel 세 개라 과반 2, `quorum` 2이므로 2표가 필요합니다. `quorum`을 과반보다 크게 잡으면 그만큼 더 많은 표가 필요합니다.
3. 리더는 승격할 복제본을 고릅니다(`+selected-slave`). 끊겨 있거나 SDOWN인 복제본, 최근에 응답하지 않은 복제본, 주 서버와 너무 오래 끊겨 있던 복제본(`down-after-milliseconds`의 10배에 주 서버가 SDOWN인 시간을 더한 것보다 오래), `replica-priority`가 0인 복제본은 빼고, 남은 것 중 `replica-priority`가 작은 것, 같으면 **복제 오프셋이 큰 것**(주 서버에서 더 많이 받은 것)을 고릅니다. 예시에서는 우선순위가 같고 R2가 R3보다 많이 받았으므로 R2가 뽑힙니다.
4. 리더는 R2에 `REPLICAOF NO ONE`(주 서버가 되라)을 보냅니다. 이때 설정 파일 다시 쓰기와, R2에 붙어 있던 일반 클라이언트와 Pub/Sub 클라이언트의 연결 끊기를 한 트랜잭션으로 묶어 보냅니다. 연결을 끊는 이유는 클라이언트들이 다시 연결하면서 Sentinel에 주 서버를 다시 묻게 하려는 것입니다(공식 문서 sentinel-clients, "Sentinel failover disconnection").
5. 리더는 R2의 `INFO` 출력에서 역할이 주 서버로 바뀐 것을 확인합니다(`+promoted-slave`). 장애 조치 중에는 복제본에 `INFO`를 1초마다 보내므로, 확인까지 최대 1초쯤 걸립니다. 이 순간부터 장애 조치는 되돌릴 수 없는 단계로 넘어갑니다.
6. 리더는 나머지 복제본(R3)에 `REPLICAOF R2`를 보내 새 주 서버를 따르게 합니다. `parallel-syncs`(기본 1)가 한 번에 다시 붙이는 복제본 수입니다. 모든 복제본이 붙으면 `+failover-end`입니다.

선거가 실패하는 경우도 있습니다. 여러 Sentinel이 거의 동시에 출마해 표가 갈리면 아무도 과반을 얻지 못합니다. 리더가 되지 못한 Sentinel은 10초(또는 `failover-timeout`이 더 짧으면 그 값) 뒤 시도를 접고, 같은 주 서버에 대한 다음 시도는 `failover-timeout`의 2배가 지나야 합니다. 다른 Sentinel에게 투표한 Sentinel도 같은 시간 동안 스스로 출마하지 않습니다. 기본값이면 6분입니다. 시작 시각에 무작위 지연을 섞는 장치가 있지만 표가 얼마나 자주 갈리는지는 이 문서가 확인하지 않았습니다. **장애 조치 시간은 한 값이 아니라 꼬리가 긴 분포**라는 점을 기억해 둡니다. 이 부분은 소스로 확인한 규칙이며 얼마나 자주 일어나는지는 재지 않았습니다.

### 새 주 서버의 주소는 두 가지 방법으로 알려집니다

클라이언트가 새 주 서버를 알 수 있는 경로는 둘입니다.

- **묻기**: `SENTINEL GET-MASTER-ADDR-BY-NAME mymaster`. 리더는 5번(승격 확인) 직후부터 R2의 주소를 답합니다. 리더는 이때 새 구성을 에폭 번호와 함께 다른 Sentinel들에게 곧바로 알리고(Sentinel끼리는 감시하는 Redis 서버의 `__sentinel__:hello` 채널로 구성을 주고받습니다), 번호가 더 큰 구성을 받은 Sentinel은 자기 구성을 바꿉니다. 그래서 대체로 승격 확인 직후부터는 어느 Sentinel에 물어도 R2를 답합니다.
- **듣기**: Sentinel에 `SUBSCRIBE`하면 `+sdown`, `+odown`, `+switch-master` 같은 이벤트를 받습니다. 공식 문서는 `+switch-master <이름> <옛 IP> <옛 포트> <새 IP> <새 포트>`를 외부 사용자가 가장 관심 가질 메시지로 꼽습니다(공식 문서 sentinel, "Pub/Sub messages"). 다만 이 이벤트를 내는 시점은 Sentinel마다 다릅니다. 새 구성을 전해 받은 Sentinel은 받는 즉시 내지만, 리더 자신은 6번까지(다른 복제본을 다 붙이거나 `failover-timeout`이 지날 때까지) 끝나야 냅니다.

예시에서 리더 S2가 승격을 확인하는 31~32초쯤부터 Sentinel에 물으면 R2를 얻습니다. 주문 API가 그 뒤 언제 묻느냐가 다음 절의 주제입니다.

### 기본값 그대로면 감지에만 30초가 걸립니다

`down-after-milliseconds`를 줄이면 ① 감지 구간이 그대로 줄어듭니다. 5,000으로 두면 예시는 약 5초에 SDOWN, 약 6~7초에 승격이 끝납니다. 대가는 앞의 "장애 감지는 '응답이 없다'에서 출발한 추측입니다"의 첫 번째 오류입니다.

- 5초 넘게 메인 스레드를 붙잡는 명령(큰 키의 동기 삭제, `KEYS`, 큰 컬렉션 전체 조회 등, [Hot Key와 Big Key](./hot-key-big-key.md))이 실행되면 멀쩡한 주 서버가 장애 조치를 당합니다. 장애 조치는 그 자체로 쓰기 유실(복제 지연만큼)과 수십 초의 클라이언트 장애를 부르므로, 원래 문제보다 피해가 클 수 있습니다.
- Sentinel과 Redis 사이의 짧은 네트워크 지연도 같은 결과를 낼 수 있습니다.

그래서 값은 "주 서버가 정상인데도 응답이 멈출 수 있는 최장 시간"보다 길고, "서비스가 Redis 쓰기 없이 버틸 수 있는 시간"보다 짧게 잡습니다. 앞의 시간은 `SLOWLOG`와 지연 기록(`LATENCY`)으로 재 볼 수 있습니다. 두 조건을 함께 만족하는 값이 없다면, 값을 줄이기보다 메인 스레드를 오래 붙잡는 명령부터 없애는 것이 먼저입니다.

## Lettuce는 새 주 서버를 어떻게 알게 되는가

### read-from 없이 Sentinel을 쓰면: 다시 연결할 때만 Sentinel에 묻습니다

Spring Boot에 `spring.data.redis.sentinel.*`만 설정하면, Spring Data Redis는 Sentinel 주소들을 담은 접속 정보로 Lettuce 연결을 만듭니다. Lettuce는 **연결을 맺을 때마다** Sentinel에 주 서버 주소를 묻고(`SENTINEL GET-MASTER-ADDR-BY-NAME`), 받은 주소로 연결합니다. Sentinel은 설정에 적은 순서대로 연결을 시도해 처음 연결된 하나에게만 묻습니다.

여기서 중요한 점은 **연결이 살아 있는 동안에는 Sentinel에 다시 묻지 않는다**는 것입니다. Sentinel 이벤트를 구독하지도 않습니다. 새 주 서버를 알게 되는 계기는 연결이 끊겨 다시 연결하는 순간뿐입니다. 예시는 M의 프로세스가 죽어 연결이 곧바로 끊겼으므로 재연결이 일어나고, 재연결 때마다 Sentinel에 묻습니다.

또 Lettuce 6.6.0의 이 경로에서는 연결한 뒤 그 서버의 역할을 확인하는 코드를 찾지 못했습니다. 공식 클라이언트 지침은 받은 주소에 연결한 뒤 `ROLE`로 정말 주 서버인지 확인하라고 권하지만(공식 문서 sentinel-clients, "Step 3"), 이 경로는 Sentinel의 답을 그대로 믿는 것으로 보입니다. 낡은 정보를 가진 Sentinel이 옛 주 서버를 답하면 그대로 붙습니다.

이 연결 방식의 두 가지 약점이 이어지는 두 절입니다.

### 재연결 간격이 늘어나, 승격 뒤에도 최대 30초를 더 기다릴 수 있습니다

Lettuce의 재연결 간격은 기본으로 지수적으로 늘어납니다. n번째 시도 전에 2의 (n−1)제곱 밀리초를 기다리고, 상한은 30초입니다. 시도할 때마다 Sentinel에 묻고 연결해 보는 데 드는 시간을 무시하면, 15번째 시도까지는 n번째 시도가 0초에서 약 (2의 n제곱 − 1)밀리초에 일어나고, 16번째부터는 간격이 상한 30초에 걸려 30초씩 늘어납니다.

| 시도 | 기다린 간격 | 0초부터 누적 |
| --- | --- | --- |
| 1 | 1ms | 1ms |
| 10 | 512ms | 약 1.0초 |
| 13 | 약 4.1초 | 약 8.2초 |
| 14 | 약 8.2초 | 약 16.4초 |
| 15 | 약 16.4초 | 약 32.8초 |
| 16 | 30초(상한) | 약 62.8초 |
| 17 이후 | 30초 | 30초씩 |

예시의 승격 확인이 약 31~32초이므로, 15번째 시도(약 32.8초)가 R2를 받으면 장애는 약 33초입니다. 그런데 선거가 조금 늦어 승격이 33초에 끝나면, 15번째 시도는 아직 M의 주소를 받아 실패하고 다음 시도는 약 62.8초입니다. **Sentinel은 33초에 일을 끝냈는데 앱은 30초를 더 실패합니다.** 이 계산은 위 공식에서 끌어낸 것이며, 시도마다 걸리는 시간만큼 실제 시각은 뒤로 밀립니다. 재현해 잰 값이 아닙니다.

`down-after-milliseconds`를 5,000으로 줄인 경우도 같은 방식으로 보면, 승격이 약 6~7초에 끝나고 13번째 시도(약 8.2초)에서 연결됩니다. 감지를 줄이면 클라이언트 쪽 대기도 함께 짧아지는 이유는, 재연결 간격이 장애가 길어질수록 커지기 때문입니다.

재연결 간격의 상한은 Spring Boot의 `ClientResourcesBuilderCustomizer` 빈에서 Lettuce `ClientResources`의 `reconnectDelay`를 바꿔 낮출 수 있습니다. 예를 들어 상한을 1초로 두면 시도 사이의 간격이 1초를 넘지 않으므로, 승격 뒤 다음 시도까지의 대기가 1초 남짓으로 줄어듭니다(시도 자체에 걸리는 시간은 별도). 대가는 장애 동안의 시도 횟수입니다. 앱 인스턴스마다, 연결마다 시도할 때 Sentinel 연결 하나와 Redis 연결 시도 하나를 만듭니다. 인스턴스가 200대이고 상한이 1초라면(가상 예시) 장애 동안 Sentinel은 초당 수백 개의 새 연결을 받습니다. 인스턴스 수가 많다면 상한을 너무 낮추지 말고, 무작위 지연을 섞는 방식(Lettuce `Delay`의 지터 계열)을 검토합니다.

### 연결이 조용히 죽으면 다시 연결 자체가 일어나지 않습니다

이번에는 0초에 박스 1의 전원이 꺼졌다고 합시다. M과 S1이 함께 사라지고, 연결을 닫는다는 패킷을 보낼 운영체제도 없습니다.

1. 주문 API의 운영체제와 Lettuce는 공유 연결이 살아 있다고 믿습니다. `DECR`은 계속 M의 옛 연결로 나가고, 100ms마다 타임아웃으로 실패합니다. **명령 타임아웃은 연결을 끊지 않으므로** 재연결이 일어나지 않습니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "조용히 끊긴 연결: 명령마다 실패하는데 다시 연결하지 않습니다").
2. Sentinel S2, S3는 약 30초에 승격을 끝냅니다. 하지만 주문 API는 Sentinel에 묻지 않으므로 이 사실을 모릅니다.
3. 운영체제가 재전송을 포기해 연결을 끊어야 Lettuce가 재연결을 시작합니다. Linux 기본값에서 이 시간은 15분을 넘을 수 있습니다([타임아웃](../cs/timeout.md)의 "조용히 끊긴 연결은 타임아웃 없이는 오래 남습니다").
4. 재연결을 시작해도 한 번 더 걸립니다. Sentinel 목록의 첫 번째가 사라진 S1이라, S1로의 연결 시도가 실패해야 S2로 넘어갑니다. 그 대기는 연결 타임아웃(`spring.data.redis.connect-timeout`, 적지 않으면 Lettuce 기본 10초)과 연결 초기화 제한 중 먼저 끝나는 쪽입니다. Spring Data Redis는 명령 타임아웃(`spring.data.redis.timeout`)을 Sentinel 접속 정보에도 넣고, Lettuce는 이 값을 연결을 맺기 시작할 때부터 재는 초기화 제한으로 씁니다. 그래서 예시(100ms)에서는 약 0.1초면 넘어가지만, 두 값을 모두 적지 않은 기본 설정에서는 10초를 기다립니다. 이 부분은 Lettuce의 Sentinel 연결 코드(목록 순서대로 시도하고 실패하면 다음으로)에서 끌어낸 추론이며 재현하지 않았습니다.

그래서 **Sentinel이 30초 만에 승격을 끝내도 앱은 15분 넘게 Redis에 쓰지 못할 수 있습니다.** 서버가 통째로 사라지는 장애(전원, 하이퍼바이저, 네트워크 장비)는 운영에서 드물지 않으므로, 이 경우를 기준으로 설정을 점검해야 합니다. 대응은 둘 중 하나 또는 둘 다입니다.

- **연결의 무응답을 운영체제가 감지하게 합니다.** Lettuce `SocketOptions`의 TCP user timeout이나 keepalive를 켭니다. 둘은 맡는 상황이 다릅니다. 이 장면처럼 명령을 계속 보내는 연결은 보낸 데이터의 확인(운영체제끼리 주고받는 TCP 수신 확인이며, Redis의 명령 응답과는 다릅니다)을 기다리는 연결이라 TCP user timeout이 맡고, keepalive는 한동안 주고받은 것이 없는 조용한 연결만 확인합니다([타임아웃](../cs/timeout.md)). 요청이 끊이지 않는 서비스에서 keepalive만 켜면 이 장면을 놓칩니다. 적용 조건(네이티브 전송 필요 등)과 값 정하는 법은 위 링크의 절에 있습니다. 감지되면 연결이 끊기고, 재연결하면서 Sentinel에 묻습니다.
- **Sentinel 이벤트를 듣게 합니다.** 다음 절의 `read-from` 방식입니다.

Sentinel 목록의 순서도 점검합니다. 앱 인스턴스마다 같은 순서로 적으면 모두가 첫 번째 Sentinel에 묻고, 그 Sentinel이 주 서버와 같은 박스에 있다면 박스 장애 때 모두가 위의 대기를 한 번씩 치릅니다. `connect-timeout`이나 명령 타임아웃 중 하나라도 짧으면(예: 수백 밀리초~1초) 이 대기도 짧아집니다.

### read-from을 주면: Sentinel 이벤트를 듣고 구성을 다시 읽습니다

`spring.data.redis.lettuce.read-from`을 설정하면 연결 방식이 바뀝니다. Spring Data Redis는 Lettuce의 주 서버·복제본 연결(`MasterReplica`)을 Sentinel 주소로 만들고, Lettuce는 다음처럼 동작합니다.

- 모든 Sentinel에 Pub/Sub 연결을 열고 모든 채널을 구독합니다(`PSUBSCRIBE *`).
- 감시 대상(`mymaster`)에 관한 이벤트를 받으면, **그 뒤 5초를 모았다가 한 번** Sentinel에 주 서버와 복제본 목록을 다시 묻습니다. 5초 안에 들어온 이벤트는 같은 갱신에 합쳐집니다. 갱신을 일으키는 이벤트는 `mymaster`의 `+elected-leader`·`+switch-master`, 그리고 `mymaster`에 딸린 복제본에 관한 `+sdown`·`+role-change`·`+convert-to-slave` 등입니다. 주 서버 M 자신의 `+sdown`은 메시지 형식이 달라 여기에 걸리지 않습니다.
- 새 목록에 없는 노드로의 연결은 닫고, 쓰기는 새 주 서버로의 연결로 보냅니다.

그래서 이 방식은 **연결이 끊겼는지와 상관없이** Sentinel이 알려 주면 따라갑니다. 앞 절의 박스 장애에서도, S2·S3가 보내는 이벤트를 받아 몇 초 안에 R2로 옮겨 갈 수 있습니다. 예시라면 약 30~31초의 `+elected-leader`가 첫 계기가 되어 약 35~36초에 구성을 다시 읽습니다. 그때까지 승격이 끝나 있지 않으면 옛 구성을 다시 읽고, 다음 이벤트로 한 번 더 갱신해야 합니다. 정확히 몇 초에 옮겨 가는지는 이벤트 순서와 어느 Sentinel에 묻느냐에 따라 달라지며, 이 문서는 재현하지 않았습니다.

이 방식은 원래 복제본에서 읽기 위한 것이라, 읽기를 어디로 보낼지가 함께 정해집니다. 복제본에서 읽으면 늦은 값을 읽을 수 있으므로([Redis 복제](./replication.md)의 "복제본에서 읽으면: 늦은 값, 만료, 쓰기 금지"), **장애 조치를 빨리 따라가는 것만 원한다면 `read-from`을 `upstream`(주 서버에서만 읽기)으로 두는 선택**이 있습니다. Spring Boot의 설정값이 Lettuce `ReadFrom.valueOf`로 넘어가고, `read-from`이 있기만 하면 Spring Data Redis가 위 연결 방식을 쓰므로, 읽기 위치를 바꾸지 않고 이벤트 구독만 얻을 수 있다는 판단입니다. 이는 소스의 코드 경로로 판단한 것이며 실행해 보지 않았습니다. 대가는 앱 인스턴스마다 Sentinel 수만큼 Pub/Sub 연결이 늘고, 구성 갱신 때 Sentinel에 질의가 몰린다는 것입니다.

## 장애 순간의 명령: 실패했는지, 실행됐는지 모릅니다

0초 직전, 주문 C 뒤에 주문 D의 `DECR`이 M에 나가 있었다고 합시다. M이 죽은 순간 이 명령은 다음 중 하나입니다. 예시는 R2가 주문 B까지 받았다고 정했으므로 실제로는 앞의 두 경우지만, 앱은 그 사실을 모릅니다. 복제가 더 빨랐다면 R2가 C와 D까지 받았을 수 있으므로 세 번째 경우도 함께 봅니다.

| 상태 | 새 주 서버 R2의 재고 | 주문 API가 보는 것 |
| --- | --- | --- |
| M에 닿기 전 | 8(반영 안 됨) | 타임아웃 예외 |
| M이 실행했지만 R2에 복제되기 전 | 8(반영 안 됨). M과 함께 사라짐 | 타임아웃 예외 |
| M이 실행했고 R2에도 복제됨 | 6(C와 D 모두 반영됨) | 타임아웃 예외 |

세 경우 모두 앱은 같은 예외(`QueryTimeoutException`)만 봅니다. 그런데 결과는 둘로 갈립니다. **재고가 줄었는지 아닌지 앱은 알 수 없습니다.**

### Lettuce가 저절로 다시 보내는 경우

Lettuce는 응답을 못 받은 명령을 버리지 않고 쌓아 두었다가, 다시 연결되면 아직 타임아웃이 나지 않은 명령을 보냅니다. 장애 조치에서는 다시 연결되는 곳이 **새 주 서버**입니다.

- 예시처럼 명령 타임아웃이 100ms면, 0초에 나가 있던 주문 D의 `DECR`은 0.1초에 타임아웃으로 끝나 다시 보내지지 않습니다. 장애 동안 들어온 주문도 대부분 그렇게 끝납니다. 재연결 때 R2로 나가는 것은 재연결 직전 100ms 안에 들어와 아직 살아 있는 명령뿐인데, 이들은 처음 보내는 명령이라 중복이 아닙니다. 다만 이 명령이 R2로 나간 뒤 응답 전에 타임아웃이 나면 다시 결과를 모르는 명령이 됩니다.
- 명령 타임아웃이 기본값 60초라면, 0초부터 33초 사이에 쌓인 명령이 아직 살아 있다가 33초에 R2로 한꺼번에 나갑니다. 위 표의 세 번째 경우였던 주문 D의 `DECR`은 R2에서 **두 번 반영**됩니다. 두 번째 경우였다면 우연히 한 번만 반영됩니다. 또 33초 동안 쌓인 주문 수천 건이 한꺼번에 R2로 쏟아집니다. 그동안 요청 스레드는 묶여 있습니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "예시의 세 사건에 대입하면").

즉 명령 타임아웃을 장애 시간보다 짧게 두면 Lettuce의 자동 재전송은 거의 일어나지 않습니다. 그래도 재연결이 타임아웃보다 빨리 일어나는 짧은 장애에서는 재전송이 일어나므로, 이것으로 중복을 막았다고 볼 수는 없습니다.

### 앱이 다시 보내는 경우

주문 API가 `QueryTimeoutException`을 보고 `DECR`을 다시 보내면, 위 표의 세 번째 경우에서 재고가 두 번 줄어듭니다. 장애 조치 중에는 재시도도 33초 동안 계속 실패하므로, 짧은 재시도 몇 번으로는 장애를 넘기지도 못합니다. 판단은 다음과 같습니다.

- **값을 바꾸는 쓰기는 멱등하게 만든 뒤에만 다시 보냅니다.** 주문 ID를 처리 기록으로 남기고 "이미 처리한 주문이면 차감하지 않는다"를 Lua 스크립트 하나로 묶습니다([Redis 멱등 키](./idempotency-key.md)). Lua 스크립트는 앱이 아니라 Redis 서버가 메인 스레드에서 끝까지 실행하는 처리 단위입니다. 스크립트 하나가 쓰기를 여러 개 하면, 주 서버는 그 쓰기들을 `MULTI`/`EXEC`로 감싸 복제 흐름에 한 덩어리로 넣습니다. 다만 장애 조치는 그 처리 기록도 함께 잃을 수 있습니다. 처리 기록과 차감이 같은 주 서버에 있으므로, 복제되지 않은 쓰기는 둘 다 사라져 "처리한 적 없음"으로 돌아갑니다. 이 경우 다시 보낸 요청은 한 번 반영되므로 결과는 맞습니다. 반대로 둘 다 복제됐다면 기록이 남아 중복을 막습니다. 멱등 처리가 장애 조치에서도 일관된 이유는 기록과 차감을 한 스크립트로 묶어 복제 흐름에 한 덩어리로 들어가게 했기 때문입니다. 복제본은 그 덩어리를 통째로 받거나 못 받으므로, 둘은 함께 남거나 함께 사라집니다. 스크립트가 중간에 오류로 멈춘 경우에 앞의 쓰기까지 되돌린다는 뜻은 아닙니다([Redis 원자성](./atomicity.md)).
- **재시도는 장애 시간을 넘길 만큼 버티게 하지 않습니다.** 33초 동안 재시도를 쌓으면 요청 스레드가 묶입니다. 짧게 실패시키고, 주문 실패를 사용자에게 알리거나 나중에 처리할 큐로 넘기는 편이 서비스 전체를 지킵니다.
- **재고처럼 최종 판단이 필요한 데이터는 DB 기준으로 다시 맞춥니다.** 장애 조치 뒤에는 C의 `DECR`처럼 응답까지 받은 쓰기도 사라질 수 있으므로, 어떤 재시도 정책으로도 Redis 값만으로 정확성을 지킬 수 없습니다.

## 옛 주 서버가 돌아오면: 두 주 서버와 버려지는 쓰기

### 장애 조치보다 먼저 돌아오면: 장애 조치는 일어나지 않습니다

M의 프로세스를 자동 재시작 도구가 5초 만에 다시 띄웠다고 합시다. Sentinel은 30초 동안 정상 응답을 못 받아야 SDOWN을 표시하는데, 다시 뜬 M은 데이터를 읽는 동안에도 `PING`에 `LOADING`을 답하고 Sentinel은 이를 정상으로 칩니다. 그래서 장애 조치가 일어나지 않습니다.

주문 API는 재연결 때 Sentinel에 묻고 그대로 M의 주소를 받아 붙습니다. 데이터를 다 읽을 때까지 명령은 `LOADING` 오류로 실패합니다. 다 읽은 뒤의 데이터는 M의 영속성 설정이 정합니다. 영속성을 껐다면 M은 빈 상태로 시작하고, R2·R3까지 M을 따라 비워집니다([Redis 복제](./replication.md)의 "영속성을 끈 주 서버가 다시 시작하면 복제본까지 비워집니다"). **장애 조치보다 빠른 자동 재시작은 장애 시간을 줄이는 대신, 영속성 설정이 잃는 범위를 그대로 결정하게 합니다.**

### 장애 조치 뒤에 돌아오면: Sentinel이 복제본으로 돌려놓습니다

장애 조치가 끝난 뒤 M이 다시 뜨면, M은 자기 설정대로 주 서버로 시작합니다. Sentinel은 장애 조치 때 M을 R2의 복제본 목록에 넣어 두었으므로, M이 "나는 주 서버"라고 답하는 것을 보면 `REPLICAOF R2`를 보내 복제본으로 돌립니다(`+convert-to-slave`). 다만 그 역할을 본 지 적어도 구성 알림 주기의 4배(약 8초)가 지나야 하고, 이 조건은 M의 `INFO` 응답을 받을 때 검사합니다. 평소 `INFO`는 10초 간격이므로 실제로는 M이 뜬 뒤 수 초에서 십수 초가 걸립니다. 이때도 M에 붙은 클라이언트 연결을 모두 끊습니다. M은 R2에서 전체 데이터를 다시 받아 R2와 같아집니다. M에만 있던 쓰기는 버려집니다([Redis 복제](./replication.md)의 "옛 주 서버가 돌아오면 그 쓰기는 버려집니다").

그 몇 초 동안 M에 쓰는 클라이언트가 있으면 그 쓰기도 버려집니다. 정상이라면 Sentinel에 묻는 클라이언트는 R2를 받으므로 M에 붙지 않습니다. 하지만 재연결 때 Sentinel에 닿지 못하면 Lettuce는 **마지막으로 알던 주소로 다시 붙습니다.** 그 주소가 M이면, M이 복제본으로 바뀌기 전에는 쓰기가 성공했다가 버려지고, 바뀐 뒤에는 `READONLY` 오류를 받습니다. Sentinel을 여러 대 두고 앱에서 그 Sentinel들에 닿을 수 있는지를 함께 확인하는 이유입니다.

### 네트워크가 갈리면: 두 주 서버가 동시에 쓰기를 받습니다

이번에는 박스 1이 박스 2·3과의 네트워크만 끊기고, 주문 API는 여전히 박스 1에 닿는다고 합시다. 흔히 스플릿 브레인(split brain)이라 부르는 상황입니다.

1. S2와 S3는 M에 닿지 못하므로 약 30초에 ODOWN, 선거(2표, 과반), R2 승격을 진행합니다. S1은 M에 닿으므로 SDOWN을 표시하지 않고, 혼자서는 과반이 아니라 아무것도 결정하지 못합니다.
2. 주문 API의 연결은 M과 멀쩡히 이어져 있습니다. 연결이 끊기지 않았으므로 재연결도, Sentinel 질의도 일어나지 않습니다. 주문은 계속 M에서 성공합니다.
3. 이제 주 서버가 둘입니다. 박스 2·3 쪽에서 R2에 붙은 다른 앱이 있다면 그쪽 주문은 R2에 쓰입니다. 같은 재고를 두 곳에서 따로 차감합니다.
4. 네트워크가 회복되면 S1이 더 큰 에폭의 구성을 받아 M을 복제본으로 돌립니다. M이 분할 동안 받은 모든 주문의 쓰기가 버려지고, 연결이 끊긴 주문 API는 다시 연결하며 R2를 찾아갑니다.

과반 규칙은 Sentinel이 두 번 장애 조치하는 것은 막지만, 갈라진 쪽에 남은 M과 그 클라이언트를 멈추지는 못합니다. 앞의 "과반: 둘로 갈라져도 한쪽만 결정하게 하는 수"에서 본 한계입니다. 공식 문서도 Sentinel과 Redis 전체를 "마지막 장애 조치가 이기는" 최종 일관성 시스템으로 설명하며, 확인 응답을 받은 쓰기를 잃는 구간이 항상 있다고 밝힙니다(공식 문서 sentinel, "Consistency under partitions").

### min-replicas-to-write를 켜면 잃는 대신 실패합니다

M에 `min-replicas-to-write 1`, `min-replicas-max-lag 10`을 두면, M은 10초 넘게 복제본의 보고를 받지 못했을 때 쓰기를 `NOREPLICAS` 오류로 거부합니다. 설정의 원리는 [Redis 복제](./replication.md)의 "WAIT은 기다려서, min-replicas는 거부해서 잃는 구간을 줄입니다"에 있습니다. 예시에 대입하면 다음과 같습니다.

- **네트워크 분할**: M은 분할 뒤 약 10초부터 주문을 거부합니다. 버려지는 쓰기가 "분할 전체 시간"에서 "약 10초"로 줄어듭니다.
- **장애 조치 뒤 돌아온 M**: 다시 뜬 M에는 붙은 복제본이 없으므로 처음부터 쓰기를 거부합니다. Sentinel이 복제본으로 돌려놓기 전의 몇 초 동안 받을 쓰기가 사라집니다.

대가는 두 가지입니다.

- **잃는 대신 실패로 바뀝니다.** 분할 동안 주문 API는 M에서 `NOREPLICAS` 오류를 계속 받습니다. 이 오류는 명령 오류일 뿐 연결을 끊지 않으므로, `read-from` 없는 Lettuce는 R2를 찾아가지 않습니다. 분할이 풀려 Sentinel이 M의 연결을 끊을 때까지 주문은 실패합니다. 거부가 시작된 뒤의 쓰기는 잃지 않지만, 그 시간 동안의 가용성은 내줍니다. 거부가 시작되기 전 약 10초의 쓰기는 여전히 버려집니다.
- **복제본이 모두 내려가면 쓰기가 멈춥니다.** R2·R3를 함께 점검하거나 둘 다 전체 동기화로 M이 RDB를 만들고 보내는 중이면, M이 멀쩡해도 주문을 받지 못합니다. 예시처럼 복제본이 둘이면 `min-replicas-to-write 1`은 하나를 점검하는 동안에도 쓰기를 유지합니다.

Redis를 캐시로만 쓴다면 분할 중의 쓰기를 잃어도 괜찮을 수 있고, 이 설정의 대가가 더 클 수 있습니다. 재고처럼 Redis 값이 판단에 쓰인다면 켜는 쪽을 검토합니다. 공식 문서도 같은 기준을 제시합니다(공식 문서 sentinel, "Consistency under partitions").

## Redis Cluster에서는 클라이언트가 지도를 다시 받아야 합니다

Redis Cluster는 Sentinel 없이 노드들끼리 장애를 판정하고 복제본을 승격합니다. 클러스터는 키마다 16,384개 해시 슬롯 중 하나를 정하고, 슬롯마다 맡을 주 서버를 정합니다. 클라이언트는 "어느 슬롯은 어느 노드"라는 대응표(슬롯 지도)를 들고 명령마다 키의 슬롯으로 보낼 노드를 고릅니다. 그래서 주 서버가 바뀌면 지도를 고쳐야 새 주 서버로 보냅니다. 자세한 내용은 Redis Cluster에 있습니다. 여기서는 장애 조치 때 시간이 어떻게 쌓이는지만 봅니다.

예시를 클러스터로 옮겨, 주 서버 셋(A, B, C) 중 `stock:item:1`의 슬롯을 맡은 C가 프로세스 장애로 죽었다고 합시다.

| 구간 | 클러스터 | 정하는 것 |
| --- | --- | --- |
| 감지 | 다른 노드들이 C를 `cluster-node-timeout`(기본 15,000ms) 동안 응답 없음으로 보고 PFAIL로 표시합니다. 주 서버 과반의 보고가 모이면 FAIL로 표시하고 모두에게 알립니다 | `cluster-node-timeout` |
| 승격 | C의 복제본이 짧은 지연(데이터를 덜 받은 복제본일수록 더 길게)을 기다린 뒤 주 서버들에게 표를 요청하고, 과반의 표를 얻으면 스스로 주 서버가 됩니다 | 복제본의 순위 |
| 클라이언트 반영 | Lettuce가 지도를 다시 받아야 새 주 서버로 보냅니다 | Lettuce의 토폴로지 갱신 설정 |

감지와 승격은 대략 `cluster-node-timeout` + 1~2초입니다. Sentinel과 달리 쓰기를 받던 노드가 죽어도 옛 노드가 `MOVED`로 새 주인을 알려 주지 않으므로, 클라이언트는 지도를 다시 받아야만 따라갑니다. 지도 갱신을 켜지 않은 Lettuce는 앱을 재시작하기 전까지 C의 슬롯에 계속 실패한다는 점은 Redis Cluster의 "장애 조치는 지도가 바뀌는 또 하나의 경우입니다"에 있습니다.

갱신을 켜도 시간이 더 붙을 수 있습니다. `spring.data.redis.lettuce.cluster.refresh.adaptive=true`로 사건 기반 갱신을 켜면, 같은 노드에 다시 연결하기를 5번 이상 반복한 것도 갱신의 계기가 됩니다. 그런데 이 갱신은 **한 번 실행하면 30초 동안 다음 계기를 버립니다.**

- C가 죽은 직후 C로의 재연결이 1, 2, 4, 8, 16밀리초 간격으로 거듭 실패하다가, 다섯 번째 시도를 시작할 때 갱신이 한 번 실행됩니다. 설정된 간격만 더하면 약 0.03초이고, 실제로는 시도마다 걸린 시간만큼 늦지만 승격(십수 초)보다는 훨씬 이릅니다. 이때는 아직 승격 전이라 C를 주인으로 적은 지도를 다시 받습니다.
- 약 16~17초에 승격이 끝나도, 30초가 지나기 전에는 새 계기가 버려집니다.
- 30초가 지난 뒤 다음 재연결 시도(앞의 재연결 간격 표로 약 32.8초)가 갱신을 일으켜야 새 지도를 받습니다.

결국 클러스터의 장애 조치는 16~17초에 끝났는데 앱은 약 33초까지 C의 슬롯에서 실패할 수 있습니다. 이 흐름은 Lettuce의 갱신 제한 코드(먼저 실행하고 30초 동안 버림)와 재연결 간격에서 끌어낸 추론이며 재현하지 않았습니다. 주기 갱신(`spring.data.redis.lettuce.cluster.refresh.period`)을 함께 켜 두면, 사건 기반 갱신이 막혀 있는 동안에도 다음 주기에 새 지도를 받습니다. 예를 들어 주기를 10초로 두면 승격 뒤 다음 갱신 시도까지 최대 10초이고, 실제 반영에는 지도를 받아 고치는 시간이 더해집니다(갱신이 실패하면 그다음 주기까지 밀립니다). 주기 갱신의 비용은 Redis Cluster의 "Lettuce는 지도를 언제 받고, 언제 고치는가"에 있습니다.

쓰기 유실은 Sentinel 구성과 같습니다. 비동기 복제로 승격된 복제본에 없는 쓰기는 사라지고, 네트워크가 갈리면 소수 쪽 주 서버가 `cluster-node-timeout` 안팎 동안 받은 쓰기도 버려집니다(Redis Cluster의 "쓰기 유실: 비동기 복제에 네트워크 분할이 더해집니다").

## 데이터와 요청이 늘면 무엇이 달라지는가

- **쓰기가 많을수록 잃는 쓰기가 늘어납니다.** 장애 순간 복제본이 못 받은 양은 복제 지연(바이트)이고, 같은 지연 시간이라도 쓰기 속도가 빠르면 더 많은 명령이 들어 있습니다.
- **요청이 많을수록 같은 장애 시간에 실패하는 요청이 늘어납니다.** 예시의 초당 100건이면 33초에 약 3,300건이고, 초당 1만 건이면 33만 건입니다. 장애 시간을 30초 줄이는 설정의 가치가 요청량에 비례해 커집니다.
- **명령 타임아웃이 길수록 쌓이는 명령과 묶이는 스레드가 늘어납니다.** 끊긴 동안의 버퍼에는 크기 상한이 사실상 없으므로, 긴 타임아웃과 많은 요청이 겹치면 앱의 메모리도 늘어납니다. 다시 연결되는 순간 쌓인 명령이 새 주 서버로 한꺼번에 쏟아집니다.
- **앱 인스턴스가 많을수록 장애 동안 Sentinel이 받는 연결이 늘어납니다.** `read-from` 없는 방식은 재연결 시도마다 Sentinel에 새 연결을 열어 묻고, `read-from` 방식은 평소에도 인스턴스마다 Sentinel 수만큼 구독 연결을 유지합니다.
- **데이터가 클수록 장애 조치 뒤 복제본이 다시 붙는 데 오래 걸립니다.** R3는 새 주 서버의 역사가 자기 역사를 이어받았으므로 보통 부분 재동기화로 붙지만, 돌아온 M처럼 역사가 갈라진 서버는 전체 동기화를 받습니다. 그동안 새 주 서버는 전체 데이터를 한 시점으로 떠서 보내는 비용(그 작업을 맡을 자식 프로세스를 만드는 데 드는 메모리·CPU와 네트워크 전송)을 치르고, 복제본이 하나 줄어든 상태가 이어집니다([Redis 복제](./replication.md)의 "따라잡을 수 없으면: 처음부터 다시 받는 전체 동기화").

## 흔한 오해 모아 보기

| 오해 | 실제 | 바로잡은 곳 |
| --- | --- | --- |
| Sentinel을 쓰면 장애 조치가 몇 초 안에 끝난다 | 기본 `down-after-milliseconds` 30초가 감지에만 걸립니다 | 장애 시간은 세 구간의 합입니다 |
| Sentinel이 승격을 끝내면 앱도 곧 새 주 서버로 간다 | `read-from` 없는 Lettuce는 재연결 때만 묻고, 재연결 간격 때문에 최대 30초를 더 기다릴 수 있습니다. 연결이 조용히 죽으면 15분을 넘길 수 있습니다 | Lettuce는 새 주 서버를 어떻게 알게 되는가 |
| `quorum`만큼 동의하면 장애 조치가 진행된다 | `quorum`은 ODOWN 판정의 기준이고, 진행에는 Sentinel 전체의 과반이면서 `quorum` 이상인 표가 필요합니다 | 선거와 승격: 과반의 표를 얻은 Sentinel 하나가 진행합니다 |
| Sentinel이 과반으로 결정하니 스플릿 브레인이 없다 | Sentinel의 결정은 하나지만, 갈라진 옛 주 서버와 그 클라이언트는 계속 씁니다 | 네트워크가 갈리면: 두 주 서버가 동시에 쓰기를 받습니다 |
| 옛 주 서버가 돌아오면 거기에 쓴 데이터가 합쳐진다 | 복제본으로 돌아가며 새 주 서버의 데이터로 덮입니다 | 장애 조치 뒤에 돌아오면: Sentinel이 복제본으로 돌려놓습니다 |
| 타임아웃이 난 쓰기는 실패한 것이니 다시 보내면 된다 | 새 주 서버에 이미 반영됐을 수 있어 두 번 반영될 수 있습니다 | 장애 순간의 명령: 실패했는지, 실행됐는지 모릅니다 |
| `down-after-milliseconds`는 짧을수록 좋다 | 바쁜 주 서버를 죽었다고 보고 장애 조치를 일으킵니다 | 기본값 그대로면 감지에만 30초가 걸립니다 |
| 클러스터는 사건 기반 갱신만 켜면 장애 조치를 바로 따라간다 | 장애 직후의 갱신이 30초 제한을 써 버려 승격을 늦게 반영할 수 있습니다 | Redis Cluster에서는 클라이언트가 지도를 다시 받아야 합니다 |

## 근거

**Redis 8.2.10 소스**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/sentinel.c`](https://github.com/redis/redis/blob/8.2.10/src/sentinel.c):
  - 시간 상수: `sentinel_ping_period` 1000, `sentinel_info_period` 10000, `sentinel_ask_period` 1000, `sentinel_publish_period` 2000, `sentinel_default_down_after` 30000, `sentinel_election_timeout` 10000, `sentinel_default_failover_timeout` 180000, `SENTINEL_DEFAULT_PARALLEL_SYNCS` 1, `SENTINEL_DEFAULT_SLAVE_PRIORITY` 100
  - `sentinelSendPeriodicCommands`: `PING` 주기는 `down-after-milliseconds`와 1초 중 작은 값, 주 서버가 ODOWN이거나 장애 조치 중이면 복제본에 `INFO`를 1초마다
  - `sentinelPingReplyCallback`: `PONG`, `LOADING`, `MASTERDOWN`을 정상 응답으로 기록, `BUSY`면 `SCRIPT KILL`
  - `sentinelCheckSubjectivelyDown`: 정상 응답이 없는 시간(응답을 기다리는 `PING`이 있으면 그 전송 시각부터, 연결이 끊겨 있으면 마지막 정상 응답부터)이 `down_after_period`를 넘으면 `+sdown`
  - `sentinelEvent`: `%@` 형식에서 주 서버는 `master <이름> <IP> <포트>`, 복제본·Sentinel은 뒤에 `@ <주 서버 이름> …`을 붙임
  - `sentinelCheckObjectivelyDown`: 자신과 주 서버 다운을 알린 Sentinel 수가 `quorum` 이상이면 `+odown`
  - `sentinelAskMasterStateToOtherSentinels`: SDOWN이면 `is-master-down-by-addr`를 1초마다, 받은 답이 `sentinel_ask_period*5`(5초)보다 오래되면 버림
  - `sentinelStartFailover`, `sentinelStartFailoverIfNeeded`: 에폭 증가, `+try-failover`, 직전 시도 뒤 `failover_timeout*2`가 지나야 다시 시도
  - `sentinelVoteLeader`: 에폭마다 한 번 투표, 다른 Sentinel에 투표하면 자기 장애 조치 시작 시각을 늦춤
  - `sentinelGetLeader`: 과반(`voters/2+1`)과 `quorum`을 모두 넘는 표를 얻어야 리더
  - `sentinelFailoverWaitStart`: 리더가 아니면 선거 제한 시간(10초와 `failover_timeout` 중 작은 값) 뒤 `-failover-abort-not-elected`. `SENTINEL FAILOVER`로 켠 `SRI_FORCE_FAILOVER`면 리더 조건을 건너뜀
  - `sentinelSelectSlave`, `compareSlavesForPromotion`: 제외 조건(SDOWN, 끊김, 응답 5초 이상 없음, 주 서버와 끊긴 시간 > `down_after_period*10` + 주 서버 SDOWN 시간, 우선순위 0), 정렬 순서(우선순위, 복제 오프셋, 실행 ID)
  - `sentinelSendSlaveOf`: `MULTI`, `SLAVEOF`, `CONFIG REWRITE`, `CLIENT KILL TYPE normal`, `CLIENT KILL TYPE pubsub`, `EXEC`
  - `sentinelRefreshInstanceInfo`: 승격된 복제본이 주 서버로 보고하면 `+promoted-slave`와 `RECONF_SLAVES` 상태, 새 구성 알림 강제(`sentinelForceHelloUpdateForMaster`). 장애 조치 대상이 아닌 서버가 주 서버로 보고하면, `INFO` 응답을 처리할 때 그 역할을 본 지 `sentinel_publish_period*4`가 지났고 새 주 서버가 정상인지(`sentinelMasterLooksSane`) 확인한 뒤 `+convert-to-slave`
  - `sentinelGetCurrentMasterAddress`, `SENTINEL GET-MASTER-ADDR-BY-NAME` 처리: `RECONF_SLAVES` 이후에는 승격된 복제본의 주소를 답함
  - `sentinelProcessHelloMessage`: 더 큰 구성 에폭을 받으면 `+config-update-from`, `+switch-master` 후 주소 변경
  - `sentinelFailoverReconfNextSlave`, `sentinelFailoverDetectEnd`, `sentinelFailoverSwitchToPromotedSlave`: `parallel_syncs`만큼 복제본을 다시 붙이고, 모두 끝나거나 `failover_timeout`이 지나면 `+failover-end` 후 리더가 `+switch-master`
- [`sentinel.conf`](https://github.com/redis/redis/blob/8.2.10/sentinel.conf): `sentinel down-after-milliseconds mymaster 30000`, `sentinel failover-timeout mymaster 180000` 예시와 설명
- [`src/replication.c`](https://github.com/redis/redis/blob/8.2.10/src/replication.c): `checkGoodReplicasStatus`(주 서버이고 `min-replicas-*`가 켜져 있으면 보고가 유효한 복제본 수가 부족할 때 쓰기 거부), `refreshGoodSlavesCount`(ONLINE 상태이고 마지막 보고가 `min-replicas-max-lag`초 이내인 복제본만 셈), `updateSlavesWaitingBgsave`·`replicaPutOnline`(RDB 채널 방식은 RDB 전송을 마치면 복제본이 아직 읽는 중이어도 ONLINE으로 바꾸고 마지막 보고 시각을 현재로 채움), `replicationCron`(복제본은 주 서버 연결 객체가 생긴 뒤에만 주기 보고를 보냄)
- [`src/cluster_legacy.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster_legacy.c): `clusterCron`(응답 지연이 `cluster_node_timeout`을 넘으면 PFAIL), `markNodeAsFailingIfNeeded`(주 서버 과반의 보고로 FAIL, `clusterSendFail`로 알림), `clusterHandleSlaveFailover`(선거 지연 500ms + `random() % 500` + 순위 × 1000ms, 표 대기 `cluster_node_timeout*2`(최소 2초))
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `cluster-node-timeout` 기본 15000
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c): `propagatePendingCommands`(한 실행 단위(스크립트 등)가 여러 쓰기를 전파하면 `MULTI`/`EXEC`로 감싸 복제 흐름과 AOF에 넣음)

**Redis 공식 문서**(저장소 [`redis/docs`](https://github.com/redis/docs/tree/e53775dde0ee7e39ebe327e7ffff583caa9fbd65), 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`. 특정 Redis 버전 전용 문서가 아니므로 소스와 다르면 소스를 따릅니다)

- [sentinel](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/sentinel.md): "Fundamental things to know about Sentinel before deploying"(비동기 복제로 확인 응답한 쓰기를 잃을 수 있음, 클라이언트 지원 필요), "Configuring Sentinel"(`quorum`은 감지 기준, 진행은 과반 승인), "Example 2: basic setup with three boxes"(옛 주 서버와 같은 쪽에 남은 클라이언트의 쓰기 유실, `min-replicas-*`로 완화), "Pub/Sub messages"(`+switch-master` 형식), "SDOWN and ODOWN failure state"(정상 응답의 종류, 간헐적 응답), "Sentinel reconfiguration of instances outside the failover procedure"(돌아온 주 서버를 복제본으로 재구성), "Replica selection and priority", "Quorum", "Configuration epochs"(다른 Sentinel에 투표하면 `2 * failover-timeout` 동안 같은 주 서버의 장애 조치를 시도하지 않음), "Configuration propagation", "Consistency under partitions"(마지막 장애 조치가 이기는 최종 일관성, 캐시와 저장소에서의 판단), "Handling of -BUSY state"
- [sentinel-clients](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/reference/sentinel-clients.md): "Step 3: call the ROLE command in the target instance", "Handle reconnections", "Sentinel failover disconnection"(역할을 바꿀 때 `CLIENT KILL`로 클라이언트가 주소를 다시 묻게 함), "Subscribe to Sentinel events to improve responsiveness"
- [cluster-spec](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/reference/cluster-spec.md): "Failure detection"(PFAIL, FAIL, 주 서버 과반), "Replica election and promotion", "Replica rank"(`DELAY = 500 milliseconds + random delay between 0 and 500 milliseconds + REPLICA_RANK * 1000 milliseconds`)

**Lettuce**(태그 [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE). Spring Boot 3.5.16이 지정한 버전)

- [`RedisClient.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisClient.java): `getSocketAddress`(Sentinel 주소면 `lookupRedis`), `lookupRedis`(`getMasterAddrByName`), `connectSentinelAsync`(Sentinel 목록을 순서대로 연결 시도, 실패하면 다음으로)
- [`ConnectionBuilder.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ConnectionBuilder.java), [`protocol/RedisHandshakeHandler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/RedisHandshakeHandler.java): 접속 정보의 타임아웃을 연결 초기화 제한으로 쓰고, `channelRegistered`부터 재어 넘으면 "Connection initialization timed out"으로 실패
- [`protocol/ConnectionWatchdog.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/ConnectionWatchdog.java): `scheduleReconnect`(시도 번호마다 `reconnectDelay.createDelay`), 연결되면 시도 번호를 0으로, 주소 공급자 실패 시 "reusing cached address", 재연결 시도 전 마지막으로 알던 주소를 찍는 "Reconnecting, last destination was" 로그
- [`protocol/ReconnectionHandler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/ReconnectionHandler.java): `reconnect0`(연결에 성공하면 연결한 주소로 "Reconnected to" 로그)
- [`resource/Delay.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/resource/Delay.java), [`resource/ExponentialDelay.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/resource/ExponentialDelay.java), [`resource/DefaultClientResources.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/resource/DefaultClientResources.java): 기본 재연결 간격 `Delay.exponential()`(하한 0, 상한 30초, 2의 거듭제곱, n번째는 `1 << (n - 1)`밀리초), `reconnectDelay` 설정, 지터 계열 `Delay`
- [`masterreplica/SentinelTopologyRefresh.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/masterreplica/SentinelTopologyRefresh.java): 모든 Sentinel에 `psubscribe("*")`, `TopologyRefreshMessagePredicate`(`+elected-leader`·`+reset-master`는 메시지가 `master <이름> `으로 시작할 때, `+slave`·`+sdown`·`-sdown`·`fix-slave-config`·`+convert-to-slave`·`+role-change`는 메시지에 `@ <이름> `이 있을 때, `+switch-master`는 `<이름> `으로 시작할 때, `failover-end`·`failover-end-for-timeout`), `TimedSemaphore`(5초 단위로 모아 한 번 실행)
- [`masterreplica/SentinelConnector.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/masterreplica/SentinelConnector.java), [`masterreplica/SentinelTopologyProvider.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/masterreplica/SentinelTopologyProvider.java), [`masterreplica/MasterReplicaConnectionProvider.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/masterreplica/MasterReplicaConnectionProvider.java): 갱신 때 Sentinel의 `master`·`replicas`로 구성을 읽고 `setKnownNodes` → `closeStaleConnections`
- [`ReadFrom.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ReadFrom.java): `UPSTREAM`, `valueOf`
- [`cluster/ClusterTopologyRefreshScheduler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterTopologyRefreshScheduler.java): `onReconnectAttempt`(재연결 시도를 시작할 때 시도 번호가 기준 이상이면 갱신 신호), 주기 갱신은 정한 간격마다 갱신 작업을 제출하고 이미 갱신 중이면 건너뜀, `indicateTopologyRefreshSignal`·`acquireTimeout`(제한 시간이 남아 있으면 신호를 버리고, 아니면 바로 갱신하고 새 제한 시간 시작)
- [`cluster/ClusterTopologyRefreshOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterTopologyRefreshOptions.java): 사건 기반 갱신 제한 30초, 재연결 계기 5번
- [`protocol/DefaultEndpoint.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/DefaultEndpoint.java), [`protocol/CommandExpiryWriter.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/CommandExpiryWriter.java): 응답 못 받은 명령을 끊긴 동안의 버퍼로 옮기고 다시 연결되면 끝나지 않은 명령만 보냄, 명령 타임아웃은 명령을 맡긴 순간부터
- [`SocketOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/SocketOptions.java): `DEFAULT_CONNECT_TIMEOUT` 10초, keepalive·TCP user timeout 기본 꺼짐

**Spring Data Redis**(태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13), 커밋 `42619b203148bae01235fe14f3cff3e171d93ebf`)

- [`LettuceConnectionFactory.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceConnectionFactory.java): `doCreateConnectionProvider`(Sentinel 구성은 `StandaloneConnectionProvider`), `createSentinelClient`, `getSentinelRedisURI`·`applyToAll`(주 서버 접속 정보와 각 Sentinel 접속 정보에 명령 타임아웃을 `setTimeout`)
- [`StandaloneConnectionProvider.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/StandaloneConnectionProvider.java): `readFrom`이 있으면 `MasterReplica.connect` 후 `setReadFrom`, 없으면 `client.connect`
- [`LettuceExceptionConverter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceExceptionConverter.java): 명령 타임아웃 → `QueryTimeoutException`

**Spring Boot**(태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16), 커밋 `0566f6933049aca6bc5ffc6d559fffade9cd2e0c`)

- [`RedisProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisProperties.java): `Sentinel`(`master`, `nodes`, `username`, `password`), `Lettuce.readFrom`
- [`RedisConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisConnectionConfiguration.java): `getSentinelConfig`(설정한 Sentinel 주소로 `RedisSentinelConfiguration`)
- [`LettuceConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/LettuceConnectionConfiguration.java): `lettuceClientResources`(`ClientResourcesBuilderCustomizer` 적용), `readFrom` 설정을 `ReadFrom.valueOf`로 변환, Sentinel 구성이면 `LettuceConnectionFactory(getSentinelConfig(), …)`
- [`ClientResourcesBuilderCustomizer.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/ClientResourcesBuilderCustomizer.java)

**이 문서가 계산·추론한 것**

- 예시의 초 단위 시각(SDOWN 약 30초, 승격 확인 약 31~32초, 재연결 약 32.8초·62.8초, 클러스터 승격 약 16~17초)은 위 소스의 주기와 기본값으로 계산한 추정이며, 장애를 재현해 재지 않았습니다. 재연결 시각은 시도마다 걸리는 시간을 0으로 둔 값입니다.
- `read-from` 없는 Lettuce Sentinel 연결이 연결 뒤 역할을 확인하지 않는다는 설명은 위 Lettuce 소스에서 그런 코드를 찾지 못했다는 판단입니다.
- 박스 장애 때 첫 Sentinel에서 연결 타임아웃과 명령 타임아웃 중 짧은 쪽만큼 기다린다는 설명, `read-from=upstream`으로 읽기 위치를 바꾸지 않고 이벤트 구독을 얻는다는 설명, 클러스터에서 사건 기반 갱신이 장애 직후에 쓰여 승격 반영이 30초 뒤로 밀린다는 설명은 코드 경로에서 끌어낸 추론이며 실행해 보지 않았습니다.

## 실무 포인트

### 장애 시간을 정하는 설정과 대가

| 설정 | 어디 | 기본 | 줄이면·켜면 얻는 것 | 대가 |
| --- | --- | --- | --- | --- |
| `down-after-milliseconds` | Sentinel | 30,000ms | 감지 구간이 그대로 줄어듦 | 긴 명령·짧은 네트워크 지연에도 장애 조치 |
| `quorum` | Sentinel | 설정 필수 | 작으면 민감하게 감지 | 작으면 일부 Sentinel만의 오판으로도 선거가 시작됨(진행은 과반이 필요) |
| `failover-timeout` | Sentinel | 180,000ms | 선거 실패 뒤 재시도(2배)가 빨라짐 | 복제본 재구성 단계의 제한 시간도 짧아짐 |
| `min-replicas-to-write`, `min-replicas-max-lag` | Redis 주 서버 | 0(꺼짐), 10초 | 분할·재시작 때 버려질 쓰기를 줄임 | 그동안 쓰기 실패, 복제본이 모두 내려가면 쓰기 멈춤 |
| `spring.data.redis.timeout` | 앱 | Lettuce 60초 | 장애 동안 스레드를 지키고 자동 재전송을 줄임 | 정상 꼬리 지연에서 실패가 늘 수 있음 |
| `spring.data.redis.connect-timeout` | 앱 | Lettuce 10초 | 죽은 Sentinel·서버에 대한 연결 시도를 빨리 포기 | 느린 네트워크에서 정상 연결도 실패 |
| `spring.data.redis.lettuce.read-from` | 앱 | 없음 | Sentinel 이벤트로 구성을 따라감(연결이 죽지 않아도) | 읽기 위치가 함께 정해짐(`upstream`이면 그대로), Sentinel 구독 연결 증가 |
| `reconnectDelay`(`ClientResourcesBuilderCustomizer`) | 앱 | 지수, 상한 30초 | 승격 뒤 대기를 줄임 | 장애 동안 Sentinel·Redis에 연결 시도 증가 |
| TCP user timeout·keepalive(`LettuceClientOptionsBuilderCustomizer`) | 앱 | 꺼짐 | 박스 장애 때 죽은 연결을 끊어 재연결을 일으킴(명령을 계속 보내는 연결은 TCP user timeout, 조용한 연결은 keepalive가 맡음) | 너무 짧으면 멀쩡한 연결도 끊음, 전송 방식에 따라 적용 안 됨 |
| `spring.data.redis.lettuce.cluster.refresh.period`·`adaptive` | 앱(클러스터) | 꺼짐 | 클러스터 장애 조치를 따라감 | 갱신 질의 증가 |

### 중요도별 점검 항목

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | 박스가 통째로 사라지는 장애에서 앱이 몇 초 만에 새 주 서버로 가는지 확인 | `read-from`도 TCP 감지도 없으면 15분 넘게 쓰지 못할 수 있습니다. 프로세스 장애만 시험하면 이 경우를 놓칩니다 |
| 필수 | 값을 바꾸는 쓰기의 재시도는 멱등 처리 뒤에만 | 장애 조치 중 타임아웃 난 쓰기는 새 주 서버에 반영됐을 수 있습니다 |
| 필수 | 명령 타임아웃을 장애 시간보다 훨씬 짧게 | 기본 60초면 장애 동안 스레드가 묶이고, 재연결 때 쌓인 명령이 새 주 서버로 다시 나갑니다 |
| 필수 | 장애 조치 때 잃을 수 있는 쓰기와 그 결과를 데이터별로 정하기 | 응답을 받은 쓰기도 사라집니다. 재고처럼 판단에 쓰는 값은 DB 기준으로 맞추는 절차가 필요합니다 |
| 알면 유리 | `down-after-milliseconds`를 메인 스레드가 멈출 수 있는 최장 시간과 비교 | 줄이면 장애 시간이 줄지만, 긴 명령이 장애 조치를 부릅니다 |
| 알면 유리 | 재연결 간격 상한 | 기본 30초 상한이 승격 뒤에도 최대 30초를 더합니다 |
| 알면 유리 | `min-replicas-*` 사용 여부 | 분할 때 버려질 쓰기를 실패로 바꿉니다. 캐시냐 판단용 데이터냐로 정합니다 |
| 알면 유리 | Sentinel 목록 순서와 `connect-timeout` | 첫 Sentinel이 주 서버와 함께 죽으면 모든 인스턴스가 연결 타임아웃과 명령 타임아웃 중 짧은 쪽만큼 기다립니다. 둘 다 기본값이면 10초입니다 |
| 참고 | 선거 실패 뒤 재시도 간격(`failover-timeout` × 2) | 빈도는 확인하지 않았지만, 장애 조치가 분 단위로 늦어질 수 있는 꼬리입니다 |

### 관찰할 것

- **Sentinel 이벤트**: Sentinel 로그나 구독으로 `+sdown`, `+odown`, `+try-failover`, `+elected-leader`, `+selected-slave`, `+promoted-slave`, `+switch-master`, `+failover-end`, `-failover-abort-*`의 시각을 남깁니다. 장애 회고에서 ①감지와 ②승격 구간의 실제 길이가 여기서 나옵니다.
- **Sentinel 상태**: `SENTINEL GET-MASTER-ADDR-BY-NAME mymaster`(지금 답하는 주 서버), `SENTINEL MASTER mymaster`(`flags`, `num-other-sentinels`, `quorum`), `SENTINEL CKQUORUM mymaster`(지금 과반과 `quorum`을 채울 수 있는가), `INFO sentinel`의 `sentinel_tilt`(Sentinel이 시계 이상으로 판단을 멈췄는가).
- **앱 쪽**: `QueryTimeoutException`·`RedisConnectionFailureException`의 시간별 수, Lettuce의 "Reconnecting, last destination was …" 로그(어느 주소로 재연결하는가)와 "reusing cached address" 경고(Sentinel에 묻지 못해 옛 주소를 쓰는가). ③클라이언트 반영 구간은 Sentinel의 승격 시각과 앱이 새 주 서버에 다시 연결된 시각의 차이입니다. 앱 오류가 멈춘 시각으로 어림할 수 있지만, **오류가 멈춘 것이 새 주 서버로 옮겨 갔다는 증거는 아닙니다.** 네트워크가 갈리면 옛 주 서버에 붙은 인스턴스는 오류 없이 계속 씁니다. "Reconnecting, last destination was …"는 재연결을 시도하기 전의 옛 주소를 찍을 뿐이므로, 연결에 성공한 주소는 "Reconnected to …" 로그로 봅니다. 인스턴스마다 이 로그가 새 주 서버를 가리키는지와, 옛 주 서버의 `CLIENT LIST`에 앱 연결이 남아 있지 않은지를 함께 확인합니다. 분할처럼 재연결이 없었던 인스턴스는 이 로그가 남지 않으므로 뒤의 확인이 유일한 단서입니다(연결을 가려낼 때의 NAT·`client-name` 주의는 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "증상과 확인 순서").
- **Redis 쪽**: 새 주 서버의 `INFO replication`(`role`, `connected_slaves`), 돌아온 옛 주 서버의 `role`과 `master_link_status`. `NOREPLICAS` 오류가 나면 `min-replicas-*` 조건 미달입니다.
- **장애 조치 연습**: `SENTINEL FAILOVER mymaster`는 주 서버가 살아 있어도 장애 조치를 일으킵니다. 감지와 과반 선거를 건너뛰므로 복제본 승격과 ③ 클라이언트 반영만 시험합니다. 프로세스 종료와 네트워크 차단(박스 장애 흉내)도 따로 시험해야 ① 감지, 선거, 조용한 연결 문제까지 봅니다. 공식 문서도 시험하지 않은 고가용성 구성은 안전하지 않다고 적습니다(공식 문서 sentinel, "Fundamental things to know about Sentinel before deploying").

### 증상과 확인 순서

| 증상 | 의심할 원인 | 확인 | 대응과 부작용 |
| --- | --- | --- | --- |
| 주 서버가 죽은 뒤 30초 넘게 쓰기 실패, 그 뒤 회복 | 기본 `down-after-milliseconds` + 재연결 간격 | Sentinel `+sdown`·`+promoted-slave` 시각과 앱 회복 시각 비교 | 감지 시간 단축(오탐 증가), 재연결 상한 단축(연결 시도 증가) |
| Sentinel은 승격을 끝냈는데 앱은 수 분째 타임아웃 | 옛 주 서버로의 연결이 조용히 죽어 재연결이 없음 | 앱이 연결한 주소, 재연결 로그가 없는지 | TCP user timeout(keepalive는 조용한 연결만 확인), `read-from` 구독 방식. 감지 시간을 너무 짧게 잡으면 멀쩡한 연결도 끊김 |
| 장애 조치 뒤 쓰기에 `READONLY` | 앱이 복제본이 된 옛 주 서버에 붙어 있음 | "reusing cached address" 경고, 앱에서 닿는 Sentinel | Sentinel 접근성 확보, 연결 재생성 |
| 장애가 없는데 주 서버가 바뀜 | 긴 명령으로 `PING` 무응답이 `down-after-milliseconds`를 넘음 | Sentinel `+sdown` 시각의 `SLOWLOG`, 지연 기록 | 긴 명령 제거가 우선. 감지 시간을 늘리면 실제 장애도 늦게 감지 |
| 장애 조치 뒤 재고가 늘어 있음 | 비동기 복제로 응답한 쓰기 유실, 또는 분할 동안 옛 주 서버에 쓴 것이 버려짐 | 장애·분할 시각, 옛 주 서버 로그, 주문 DB | DB 기준 재계산. `min-replicas-*`로 분할 손실을 줄이면 그동안 쓰기 실패 |
| 장애 조치 뒤 재고가 두 번 줄어 있음 | 타임아웃 난 쓰기를 앱이나 Lettuce가 새 주 서버로 다시 보냄 | 재시도 로그, 명령 타임아웃 값 | 멱등 처리, 명령 타임아웃 단축 |
| 쓰기가 `NOREPLICAS`로 계속 실패 | 복제본 보고 끊김(분할, 복제본 다운) | 주 서버 `INFO replication`의 복제본 `lag`·`state` | 복제본 복구. 설정을 끄면 쓰기는 되지만 보호도 사라짐 |
| 클러스터에서 장애 조치가 끝났는데 한 슬롯만 수십 초 실패 | 사건 기반 갱신이 장애 직후 쓰여 30초 제한에 걸림, 또는 갱신 꺼짐 | Lettuce 갱신 설정, 승격 시각과 회복 시각 | 주기 갱신 함께 켜기(갱신 질의 증가) |

## 말로 설명하기

### 30초 버전

Redis 주 서버가 죽으면 Sentinel이 `down-after-milliseconds`(기본 30초) 동안 응답이 없는 것을 보고, `quorum`만큼 동의하면 과반 투표로 리더를 뽑아 우선순위가 같다면 가장 많이 복제받은 복제본을 승격합니다. 앱이 겪는 장애는 이 감지 시간에 승격 1~2초, 그리고 클라이언트가 새 주소를 알아채는 시간을 더한 것입니다. Lettuce는 기본으로 재연결할 때만 Sentinel에 물어서, 재연결 간격 때문에 30초를 더 기다리거나 연결이 조용히 죽으면 15분 넘게 못 쓸 수도 있습니다. 잃는 것은 복제가 안 된 쓰기와 갈라진 옛 주 서버에 쓴 것이고, 타임아웃 난 쓰기는 결과를 모르니 멱등하게 만든 뒤에만 다시 보냅니다.

### 2분 버전

장애 시간은 세 구간입니다. 감지는 Sentinel마다 1초씩 `PING`을 보내다 `down-after-milliseconds` 동안 정상 응답이 없으면 SDOWN, `quorum`만큼 모이면 ODOWN입니다. 이 값은 짧을수록 빨리 감지하지만, 긴 명령으로 바쁜 주 서버도 죽었다고 봐 장애 조치를 일으킵니다. 승격은 Sentinel 과반의 표를 얻은 리더가 우선순위와 복제 오프셋으로 복제본을 고르고 `REPLICAOF NO ONE`을 보내 1초 간격의 `INFO`로 전환을 확인하는 데 1~2초쯤입니다. 이때부터 Sentinel은 새 주소를 답합니다.

클라이언트 반영이 가장 놓치기 쉽습니다. `read-from` 없이 Sentinel을 설정한 Lettuce는 연결을 맺을 때만 주소를 묻습니다. 프로세스만 죽었다면 연결이 끊겨 재연결이 계속되지만, 재연결 간격이 1밀리초부터 두 배씩 늘어 30초까지 가므로 승격 직후의 시도를 놓치면 30초를 더 기다립니다. 박스가 통째로 사라지면 연결이 끊긴 줄도 모르고 명령 타임아웃만 반복하므로 운영체제가 포기하는 15분 넘게 이어질 수 있습니다. TCP user timeout으로 죽은 연결을 끊게 하거나, `read-from`을 줘서 Sentinel 이벤트를 구독하게 합니다. 읽기를 바꾸고 싶지 않으면 `upstream`으로 둘 수 있다고 봅니다.

잃는 것은 둘입니다. 비동기 복제라 승격된 복제본에 없던 쓰기는 응답을 받았어도 사라지고, 네트워크가 갈리면 옛 주 서버에 쓰던 클라이언트의 쓰기가 분할 내내 쌓였다가 버려집니다. `min-replicas-to-write`는 이 분할 손실을 약 `max-lag`초로 줄이지만, 대신 그동안 쓰기가 실패합니다. 장애 순간의 쓰기는 실행 여부를 모르므로, 재시도는 멱등 처리를 한 뒤에만 하고 명령 타임아웃을 짧게 둬 Lettuce의 자동 재전송도 줄입니다.

### 예상 질문과 꼬리 질문

**Q. Sentinel의 `quorum`을 2로 두면 Sentinel 두 개만 동의하면 장애 조치가 되는 건가요?**
A. 아닙니다. `quorum`은 ODOWN 판정, 즉 "죽었다"는 감지의 기준입니다. 실제로 진행하려면 리더 선거에서 아는 Sentinel 전체의 과반이면서 `quorum` 이상의 표를 얻어야 합니다. Sentinel이 5개이고 `quorum`이 2면 두 개의 동의로 선거가 시작되지만, 진행에는 3표가 필요합니다(본문 "선거와 승격: 과반의 표를 얻은 Sentinel 하나가 진행합니다").

- **꼬리: 그럼 `quorum`을 작게 잡을 이유는 뭔가요?** 일부 Sentinel만 주 서버를 못 보는 상황에서도 선거를 시작하게 해 감지를 민감하게 만듭니다. 대신 그 일부 Sentinel이 네트워크 문제로 잘못 본 것이어도 선거가 시작되고, 과반이 동의하면 멀쩡한 주 서버가 장애 조치될 수 있습니다. 진행에 과반이 필요하다는 점은 변하지 않으므로, 소수 쪽 Sentinel만으로 장애 조치가 되지는 않습니다.
- **꼬리: Sentinel 세 개 중 두 개가 주 서버와 같은 박스에 있다면?** 그 박스가 죽으면 남은 Sentinel 하나는 과반이 아니라 장애 조치를 못 합니다. 공식 문서가 Sentinel을 독립적으로 죽는 서버 세 대 이상에 두라고 하는 이유입니다.

**Q. 장애 조치는 몇 초 걸리나요?**
A. 정해진 값이 아니라 세 구간의 합입니다. 기본값이면 감지만 약 30초, 승격 1~2초, 클라이언트 반영 0~30초라 앱 입장에서는 30초에서 1분 남짓으로 추정합니다. 이 숫자는 소스의 주기로 계산한 것이며 재현해 잰 값이 아닙니다(본문 "장애 시간은 세 구간의 합입니다").

- **꼬리: 그 추정이 틀리는 경우는요?** 세 가지가 있습니다. 표가 갈려 선거가 실패하면 다음 시도까지 `failover-timeout`의 2배(기본 6분)를 기다립니다. 박스가 통째로 사라져 연결이 조용히 죽으면 클라이언트 반영이 15분을 넘길 수 있습니다. Sentinel이 시계 이상으로 TILT 모드에 들어가 있으면 판단 자체를 멈춥니다.
- **꼬리: 그럼 `down-after-milliseconds`를 1초로 두면 되지 않나요?** 1초 넘게 메인 스레드를 붙잡는 명령 하나가 장애 조치를 부릅니다. 장애 조치는 복제 지연만큼의 쓰기 유실과 수십 초의 클라이언트 장애를 동반하므로, 원래의 1초 멈춤보다 피해가 커집니다. 먼저 주 서버가 정상인데도 멈출 수 있는 최장 시간을 재고 그보다 길게 잡습니다.

**Q. Sentinel이 새 주 서버를 정하면 Spring 앱은 어떻게 알게 되나요?**
A. 설정에 따라 다릅니다. `spring.data.redis.sentinel.*`만 있으면 Lettuce는 연결을 맺을 때 Sentinel에 주 서버 주소를 묻고, 연결이 살아 있는 동안에는 다시 묻지 않습니다. Sentinel은 역할을 바꾼 서버의 클라이언트 연결을 끊어 다시 묻게 하지만, 죽은 주 서버에는 그렇게 할 수 없습니다. `read-from`을 주면 Lettuce가 모든 Sentinel의 이벤트를 구독하고 이벤트 뒤 5초를 모아 구성을 다시 읽습니다(본문 "Lettuce는 새 주 서버를 어떻게 알게 되는가").

- **꼬리: 프로세스가 죽어서 연결이 바로 끊겼다면 문제없지 않나요?** 재연결이 일어나긴 하지만 간격이 지수적으로 늘어 30초 상한까지 갑니다. 30초쯤 승격이 끝나도 바로 앞의 시도가 승격 직전이었다면 다음 시도는 약 30초 뒤입니다. 재연결 간격 상한을 낮추면 줄지만, 인스턴스가 많으면 장애 동안 Sentinel이 받는 연결이 늘어납니다.
- **꼬리: 그럼 명령 타임아웃이 나면 Lettuce가 연결을 다시 맺지 않나요?** 맺지 않습니다. 명령 타임아웃은 그 명령만 실패시키고 연결은 그대로 둡니다. 그래서 박스가 통째로 사라져 연결이 조용히 죽으면 명령마다 타임아웃이 나는데도 재연결이 없고, 운영체제가 연결을 포기할 때까지 Sentinel에 묻지 않습니다.
- **꼬리: `read-from`을 쓰면 복제본에서 읽게 되잖아요. 재고 판단에는 위험하지 않나요?** 그래서 `upstream`으로 두면 읽기는 주 서버에 남기고 이벤트 구독만 얻는다고 판단합니다. 다만 이는 소스 코드 경로로 판단한 것이고 실행해 확인하지 않았으므로, 장애 조치 연습으로 실제 동작을 확인한 뒤 씁니다.

**Q. 장애 순간에 타임아웃이 난 `DECR`은 다시 보내도 되나요?**
A. 멱등하게 만들지 않았다면 안 됩니다. 그 `DECR`은 주 서버에 닿지 않았을 수도, 실행됐지만 복제 전에 사라졌을 수도, 복제까지 돼서 새 주 서버에 남았을 수도 있습니다. 마지막 경우에 다시 보내면 두 번 반영됩니다. 앱은 셋을 같은 예외로만 봅니다(본문 "장애 순간의 명령: 실패했는지, 실행됐는지 모릅니다").

- **꼬리: 멱등 기록도 Redis에 있으면 장애 조치로 같이 사라지지 않나요?** 사라질 수 있습니다. 하지만 기록과 차감을 한 Lua 스크립트로 묶었다면 둘은 같은 복제 흐름에 한 덩어리로 들어가므로 함께 남거나 함께 사라집니다. 함께 사라졌다면 다시 보낸 요청이 한 번 반영되고, 함께 남았다면 기록이 중복을 막습니다. 둘을 따로 보냈다면 이 성질이 없습니다.
- **꼬리: 앱이 재시도를 안 하면 중복은 없나요?** Lettuce가 응답 못 받은 명령을 재연결 뒤 다시 보낼 수 있습니다. 명령 타임아웃이 장애 시간보다 짧으면 장애 순간에 나가 있던 명령은 재연결 전에 타임아웃으로 끝나 다시 나가지 않지만, 기본값 60초에서는 30초 남짓의 장애 동안 쌓인 명령이 새 주 서버로 다시 나갑니다.

**Q. 장애 조치 뒤 옛 주 서버가 다시 살아나면 어떻게 되나요?**
A. Sentinel이 그 서버를 새 주 서버의 복제본으로 돌려놓습니다. 주 서버로 시작한 것을 본 뒤 적어도 약 8초가 지난 다음 `INFO` 확인 때(평소 10초 간격) `REPLICAOF`를 보내며, 그 서버에만 있던 쓰기는 전체 동기화로 덮여 사라집니다(본문 "장애 조치 뒤에 돌아오면: Sentinel이 복제본으로 돌려놓습니다").

- **꼬리: 그 몇 초 동안 옛 주 서버에 쓰는 클라이언트가 있을 수 있나요?** Sentinel에 묻는 클라이언트는 새 주소를 받으므로 보통은 없습니다. 하지만 재연결 때 Sentinel에 닿지 못한 Lettuce는 마지막으로 알던 주소로 붙으므로 옛 주 서버에 쓸 수 있습니다. `min-replicas-to-write`를 켜 두면 복제본이 없는 그 서버는 쓰기를 거부합니다.
- **꼬리: 장애 조치보다 먼저 자동 재시작되면요?** `LOADING`도 정상 응답이라 Sentinel은 장애 조치를 하지 않습니다. 영속성을 껐다면 빈 주 서버를 복제본들이 따라가 전부 비워집니다. 자동 재시작이 장애 조치보다 빠른 구성이라면 영속성 설정이 잃는 범위를 결정합니다.

**Q. Sentinel은 과반으로 결정하니까 주 서버가 둘이 되는 일은 없죠?**
A. Sentinel의 결정은 하나지만 주 서버는 둘이 될 수 있습니다. 네트워크가 갈려 옛 주 서버와 일부 앱이 소수 쪽에 남으면, 다수 쪽이 복제본을 승격하는 동안 옛 주 서버는 그 앱의 쓰기를 계속 받습니다. 연결이 끊기지 않았으니 앱은 Sentinel에 묻지도 않습니다. 분할이 풀리면 옛 주 서버의 쓰기는 모두 버려집니다(본문 "네트워크가 갈리면: 두 주 서버가 동시에 쓰기를 받습니다").

- **꼬리: `min-replicas-to-write`로 해결되나요?** 버려질 양을 약 `min-replicas-max-lag`초로 줄일 뿐 없애지 못합니다. 그리고 그 뒤로는 옛 주 서버가 `NOREPLICAS`로 쓰기를 거부하는데, 이 오류는 연결을 끊지 않으므로 `read-from` 없는 Lettuce는 분할이 풀릴 때까지 실패만 반복합니다. 잃는 것을 실패로 바꾸는 선택입니다.
- **꼬리: 그럼 손실을 없애려면?** 공식 문서는 동기 복제와 합의 알고리즘, 또는 버전을 합칠 수 있는 저장소만이 확인 응답한 쓰기를 잃지 않는다고 설명합니다. Redis 복제 구성은 둘 다 아니므로, 잃으면 안 되는 판단은 DB를 기준으로 두고 Redis는 그 앞의 빠른 판단 장치로 씁니다.

**Q. Redis Cluster에서는 Sentinel이 없는데 클라이언트는 어떻게 따라가나요?**
A. 노드들이 `cluster-node-timeout`(기본 15초)으로 장애를 판정하고 복제본이 표를 받아 스스로 승격합니다. 클라이언트는 슬롯 지도를 다시 받아야 새 주 서버로 보내는데, Lettuce의 지도 갱신은 기본으로 꺼져 있습니다(본문 "Redis Cluster에서는 클라이언트가 지도를 다시 받아야 합니다").

- **꼬리: 사건 기반 갱신을 켜면 충분한가요?** 장애 직후 재연결 실패가 갱신을 한 번 일으키는데, 그때는 승격 전이라 옛 지도를 받습니다. 그 뒤 30초 동안은 갱신 계기가 버려지므로 16초쯤 끝난 승격을 30초 넘어서야 반영할 수 있다고 봅니다. 주기 갱신을 함께 켜면 이 공백을 주기만큼으로 줄입니다. 이는 코드 경로에서 끌어낸 추론이며 재현하지 않았습니다.
- **꼬리: 리샤딩 때는 `MOVED`로 따라가는데 장애 조치는 왜 다른가요?** 리샤딩에서는 옛 노드가 살아서 `MOVED`로 새 주인을 알려 주지만, 장애 조치에서는 옛 노드가 죽어 있어 알려 줄 노드가 없습니다. 지도 갱신 말고는 새 주인을 알 길이 없습니다.

**Q. 장애 조치 설정이 맞는지 어떻게 검증하나요?**
A. 실제로 일으켜 봅니다. `SENTINEL FAILOVER`는 감지와 과반 선거를 건너뛰고 승격과 클라이언트 반영만 시험하므로, 주 서버 프로세스 종료와 네트워크 차단도 따로 시험합니다. 각 시험에서 Sentinel 이벤트 시각(`+sdown`, `+promoted-slave`)과 앱이 새 주 서버에 다시 연결된 시각을 비교해 세 구간의 실제 길이를 잽니다. 네트워크 차단 시험에서는 옛 주 서버에 남아 오류 없이 쓰는 인스턴스가 있을 수 있으므로, 오류가 멈춘 시각만 보지 않고 인스턴스마다 실제로 붙은 서버를 확인합니다(본문 "관찰할 것").

- **꼬리: 운영 환경에서 시험하기 부담스러우면요?** 개발 환경에서 같은 구성(Sentinel 배치, 앱의 Lettuce 설정, 전송 방식)으로 시험합니다. 다만 TCP user timeout처럼 운영체제와 네이티브 전송에 따라 적용 여부가 달라지는 설정은 운영과 같은 환경이어야 결과를 믿을 수 있습니다.
